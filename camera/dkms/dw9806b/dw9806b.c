// SPDX-License-Identifier: GPL-2.0
/*
 * Dongwoon DW9806B voice-coil motor driver
 *
 * The register layout and focus protocol match the mainline DW9807
 * driver.  The initialization sequence comes from Samsung's IMX241
 * driver for the Galaxy TabPro S.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>

#define DW9806B_NAME			"dw9806b"
#define DW9806B_NUM_SUPPLIES		2
#define DW9806B_MAX_FOCUS_POS		1023
#define DW9806B_FOCUS_STEPS		1
#define DW9806B_CTRL_STEPS		16
#define DW9806B_CTRL_DELAY_US		1000
#define DW9806B_MAX_RETRIES		10

#define DW9806B_INFO_REG		0x00
#define DW9806B_CONTROL_REG		0x02
#define DW9806B_POSITION_MSB_REG	0x03
#define DW9806B_POSITION_LSB_REG	0x04
#define DW9806B_STATUS_REG		0x05
#define DW9806B_MODE_REG		0x06
#define DW9806B_RESONANCE_REG		0x07

#define DW9806B_CONTROL_POWER_DOWN	0x01
#define DW9806B_CONTROL_RING_MODE	0x02
#define DW9806B_MODE_DEFAULT		0x61
#define DW9806B_RESONANCE_DEFAULT	0x13

struct dw9806b_device {
	struct v4l2_ctrl_handler controls;
	struct v4l2_subdev sd;
	struct regulator_bulk_data supplies[DW9806B_NUM_SUPPLIES];
	u16 current_position;
};

static const char * const dw9806b_supply_names[] = {
	"vdd",
	"vsio",
};

static inline struct dw9806b_device *
sd_to_dw9806b(struct v4l2_subdev *sd)
{
	return container_of(sd, struct dw9806b_device, sd);
}

static int dw9806b_write_reg(struct i2c_client *client, u8 reg, u8 value)
{
	u8 data[2] = { reg, value };
	int ret;

	ret = i2c_master_send(client, data, sizeof(data));
	if (ret == sizeof(data))
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int dw9806b_read_reg(struct i2c_client *client, u8 reg)
{
	int ret;

	ret = i2c_master_send(client, &reg, sizeof(reg));
	if (ret != sizeof(reg))
		return ret < 0 ? ret : -EIO;

	ret = i2c_master_recv(client, &reg, sizeof(reg));
	if (ret != sizeof(reg))
		return ret < 0 ? ret : -EIO;

	return reg;
}

static int dw9806b_hw_init(struct i2c_client *client)
{
	int info;
	int ret;

	info = dw9806b_read_reg(client, DW9806B_INFO_REG);
	if (info < 0)
		return dev_err_probe(&client->dev, info,
				     "failed to read IC information\n");

	/*
	 * Samsung's sequence is:
	 *   control=1, control=0, delay 1 ms, control=2,
	 *   mode=0x61, resonance=0x13.
	 */
	ret = dw9806b_write_reg(client, DW9806B_CONTROL_REG,
				DW9806B_CONTROL_POWER_DOWN);
	if (ret)
		return ret;

	ret = dw9806b_write_reg(client, DW9806B_CONTROL_REG, 0);
	if (ret)
		return ret;

	usleep_range(DW9806B_CTRL_DELAY_US,
		     DW9806B_CTRL_DELAY_US + 100);

	ret = dw9806b_write_reg(client, DW9806B_CONTROL_REG,
				DW9806B_CONTROL_RING_MODE);
	if (ret)
		return ret;

	ret = dw9806b_write_reg(client, DW9806B_MODE_REG,
				DW9806B_MODE_DEFAULT);
	if (ret)
		return ret;

	ret = dw9806b_write_reg(client, DW9806B_RESONANCE_REG,
				DW9806B_RESONANCE_DEFAULT);
	if (ret)
		return ret;

	dev_dbg(&client->dev, "initialized, IC information 0x%02x\n", info);

	return 0;
}

static int dw9806b_power_on(struct dw9806b_device *dw9806b)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dw9806b->sd);
	int ret;

	ret = regulator_bulk_enable(DW9806B_NUM_SUPPLIES,
				    dw9806b->supplies);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to enable VCM supplies\n");

	usleep_range(DW9806B_CTRL_DELAY_US,
		     DW9806B_CTRL_DELAY_US + 100);

	ret = dw9806b_hw_init(client);
	if (ret)
		regulator_bulk_disable(DW9806B_NUM_SUPPLIES,
				       dw9806b->supplies);

	return ret;
}

static int dw9806b_check_ready(struct i2c_client *client)
{
	int status;

	status = dw9806b_read_reg(client, DW9806B_STATUS_REG);
	if (status < 0)
		return status;

	return status & 0x01;
}

static int dw9806b_set_position(struct i2c_client *client, u16 position)
{
	u8 data[3] = {
		DW9806B_POSITION_MSB_REG,
		(position >> 8) & 0x03,
		position & 0xff,
	};
	int ready;
	int ret;

	ret = readx_poll_timeout(dw9806b_check_ready, client, ready,
				 ready <= 0, DW9806B_CTRL_DELAY_US,
				 DW9806B_MAX_RETRIES *
				 DW9806B_CTRL_DELAY_US);
	if (ret || ready < 0)
		return ret ? -EBUSY : ready;

	ret = i2c_master_send(client, data, sizeof(data));
	if (ret == sizeof(data))
		return 0;

	return ret < 0 ? ret : -EIO;
}

static int dw9806b_set_control(struct v4l2_ctrl *control)
{
	struct dw9806b_device *dw9806b =
		container_of(control->handler, struct dw9806b_device, controls);
	struct i2c_client *client = v4l2_get_subdevdata(&dw9806b->sd);
	int ret;

	if (control->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;

	ret = dw9806b_set_position(client, control->val);
	if (!ret)
		dw9806b->current_position = control->val;

	return ret;
}

static const struct v4l2_ctrl_ops dw9806b_control_ops = {
	.s_ctrl = dw9806b_set_control,
};

static int dw9806b_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int dw9806b_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops dw9806b_internal_ops = {
	.open = dw9806b_open,
	.close = dw9806b_close,
};

static const struct v4l2_subdev_core_ops dw9806b_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops dw9806b_subdev_ops = {
	.core = &dw9806b_core_ops,
};

static int dw9806b_init_controls(struct dw9806b_device *dw9806b)
{
	struct v4l2_ctrl_handler *handler = &dw9806b->controls;

	v4l2_ctrl_handler_init(handler, 1);
	v4l2_ctrl_new_std(handler, &dw9806b_control_ops,
			  V4L2_CID_FOCUS_ABSOLUTE, 0,
			  DW9806B_MAX_FOCUS_POS,
			  DW9806B_FOCUS_STEPS, 0);

	dw9806b->sd.ctrl_handler = handler;

	return handler->error;
}

static void dw9806b_cleanup(struct dw9806b_device *dw9806b)
{
	v4l2_async_unregister_subdev(&dw9806b->sd);
	v4l2_ctrl_handler_free(&dw9806b->controls);
	media_entity_cleanup(&dw9806b->sd.entity);
}

static int dw9806b_probe(struct i2c_client *client)
{
	struct dw9806b_device *dw9806b;
	unsigned int i;
	int ret;

	dw9806b = devm_kzalloc(&client->dev, sizeof(*dw9806b), GFP_KERNEL);
	if (!dw9806b)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&dw9806b->sd, client, &dw9806b_subdev_ops);
	dw9806b->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			     V4L2_SUBDEV_FL_HAS_EVENTS;
	dw9806b->sd.internal_ops = &dw9806b_internal_ops;

	for (i = 0; i < DW9806B_NUM_SUPPLIES; i++)
		dw9806b->supplies[i].supply = dw9806b_supply_names[i];

	ret = devm_regulator_bulk_get(&client->dev, DW9806B_NUM_SUPPLIES,
				      dw9806b->supplies);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to get VCM supplies\n");

	ret = dw9806b_power_on(dw9806b);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to initialize VCM\n");

	ret = dw9806b_init_controls(dw9806b);
	if (ret)
		goto err_power_off;

	ret = media_entity_pads_init(&dw9806b->sd.entity, 0, NULL);
	if (ret)
		goto err_ctrl_free;

	dw9806b->sd.entity.function = MEDIA_ENT_F_LENS;

	ret = v4l2_async_register_subdev(&dw9806b->sd);
	if (ret)
		goto err_cleanup;

	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_idle(&client->dev);

	dev_info(&client->dev, "DW9806B autofocus actuator ready\n");

	return 0;

err_cleanup:
	media_entity_cleanup(&dw9806b->sd.entity);
err_ctrl_free:
	v4l2_ctrl_handler_free(&dw9806b->controls);
err_power_off:
	regulator_bulk_disable(DW9806B_NUM_SUPPLIES, dw9806b->supplies);

	return ret;
}

static void dw9806b_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9806b_device *dw9806b = sd_to_dw9806b(sd);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		regulator_bulk_disable(DW9806B_NUM_SUPPLIES,
				       dw9806b->supplies);
	dw9806b_cleanup(dw9806b);
}

static int __maybe_unused dw9806b_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9806b_device *dw9806b = sd_to_dw9806b(sd);
	int position;
	int power_down_ret;
	int ret = 0;

	for (position = dw9806b->current_position &
			 ~(DW9806B_CTRL_STEPS - 1);
	     position >= 0; position -= DW9806B_CTRL_STEPS) {
		int step_ret = dw9806b_set_position(client, position);

		if (step_ret) {
			dev_err_once(dev, "failed to park lens: %d\n", step_ret);
			ret = step_ret;
		}
		usleep_range(DW9806B_CTRL_DELAY_US,
			     DW9806B_CTRL_DELAY_US + 100);
	}

	power_down_ret = dw9806b_write_reg(client, DW9806B_CONTROL_REG,
					   DW9806B_CONTROL_POWER_DOWN);
	if (power_down_ret) {
		dev_err(dev, "failed to enter power-down mode: %d\n",
			power_down_ret);
		if (!ret)
			ret = power_down_ret;
	}

	regulator_bulk_disable(DW9806B_NUM_SUPPLIES, dw9806b->supplies);

	return ret;
}

static int __maybe_unused dw9806b_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct dw9806b_device *dw9806b = sd_to_dw9806b(sd);
	int position;
	int ret;

	ret = dw9806b_power_on(dw9806b);
	if (ret)
		return ret;

	for (position = dw9806b->current_position %
			 DW9806B_CTRL_STEPS;
	     position < dw9806b->current_position +
			DW9806B_CTRL_STEPS - 1;
	     position += DW9806B_CTRL_STEPS) {
		ret = dw9806b_set_position(client, position);
		if (ret) {
			dev_err_ratelimited(dev,
					    "failed to restore lens: %d\n", ret);
			regulator_bulk_disable(DW9806B_NUM_SUPPLIES,
					       dw9806b->supplies);
			return ret;
		}
		usleep_range(DW9806B_CTRL_DELAY_US,
			     DW9806B_CTRL_DELAY_US + 100);
	}

	return 0;
}

static const struct dev_pm_ops dw9806b_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dw9806b_suspend, dw9806b_resume)
	SET_RUNTIME_PM_OPS(dw9806b_suspend, dw9806b_resume, NULL)
};

static const struct i2c_device_id dw9806b_id_table[] = {
	{ DW9806B_NAME },
	{ }
};
MODULE_DEVICE_TABLE(i2c, dw9806b_id_table);

static struct i2c_driver dw9806b_i2c_driver = {
	.driver = {
		.name = DW9806B_NAME,
		.pm = &dw9806b_pm_ops,
	},
	.probe = dw9806b_probe,
	.remove = dw9806b_remove,
	.id_table = dw9806b_id_table,
};
module_i2c_driver(dw9806b_i2c_driver);

MODULE_AUTHOR("Galaxy TabPro S Linux community");
MODULE_DESCRIPTION("Dongwoon DW9806B VCM driver");
MODULE_LICENSE("GPL");
