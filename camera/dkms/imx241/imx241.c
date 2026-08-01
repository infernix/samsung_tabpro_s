// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX241 image sensor driver
 *
 * Initial support is for the Samsung Galaxy TabPro S (SM-W700), whose ACPI
 * firmware describes two INT347F camera modules behind a TPS68470 PMIC.
 *
 * The 2592x1944 register sequence below was recovered from Samsung's official
 * 2016 Windows driver package.  Unlike the older public Rockchip sequence, it
 * is configured for the tablet's actual 19.2 MHz input clock.
 */

#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/version.h>

#include <media/media-entity.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define IMX241_REG_CHIP_ID		CCI_REG8(0x3032)
#define IMX241_CHIP_ID			0x40

#define IMX241_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX241_MODE_STANDBY		0x00
#define IMX241_MODE_STREAMING		0x01

#define IMX241_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX241_EXPOSURE_MIN		1
#define IMX241_EXPOSURE_OFFSET		5

#define IMX241_REG_ANALOG_GAIN		CCI_REG8(0x0205)
#define IMX241_ANALOG_GAIN_MIN		0x100
#define IMX241_ANALOG_GAIN_MAX		0x1000
#define IMX241_ANALOG_GAIN_DEFAULT	0x100

#define IMX241_REG_FRAME_LENGTH		CCI_REG16(0x0340)
#define IMX241_FRAME_LENGTH_MAX		0xffff

#define IMX241_XCLK_FREQ		19200000
#define IMX241_LINK_FREQ		400000000LL
#define IMX241_PIXEL_RATE		160000000LL

#define IMX241_WIDTH			2592U
#define IMX241_HEIGHT			1944U
#define IMX241_LINE_LENGTH		2912U
#define IMX241_FRAME_LENGTH		2280U

static const char * const imx241_supply_names[] = {
	"avdd",
	"dvdd",
	"dovdd",
};

#define IMX241_NUM_SUPPLIES ARRAY_SIZE(imx241_supply_names)

static const s64 imx241_link_freq_menu[] = {
	IMX241_LINK_FREQ,
};

/*
 * Samsung GalaxyTabProS_DriverPackage_160513.zip, imx241.sys:
 * native 2592x1944 mode for 19.2 MHz MCLK, 2 CSI-2 lanes.
 */
static const struct cci_reg_sequence imx241_2592x1944_regs[] = {
	{ CCI_REG8(0x0101), 0x00 },
	{ CCI_REG8(0x303c), 0x3b },
	{ CCI_REG8(0x303d), 0x00 },
	{ CCI_REG8(0x3041), 0xd7 },
	{ CCI_REG8(0x30e0), 0x00 },
	{ CCI_REG8(0x30e1), 0x00 },
	{ CCI_REG8(0x30f6), 0x20 },
	{ CCI_REG8(0x34ce), 0xff },
	{ CCI_REG8(0x0340), 0x08 },
	{ CCI_REG8(0x0341), 0xe8 },
	{ CCI_REG8(0x0342), 0x05 },
	{ CCI_REG8(0x0343), 0xb0 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x0a },
	{ CCI_REG8(0x0349), 0x1f },
	{ CCI_REG8(0x034a), 0x07 },
	{ CCI_REG8(0x034b), 0x97 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x3048), 0x20 },
	{ CCI_REG8(0x30d5), 0x00 },
	{ CCI_REG8(0x3165), 0x20 },
	{ CCI_REG8(0x30d1), 0x00 },
	{ CCI_REG8(0x30d0), 0x2a },
	{ CCI_REG8(0x3102), 0x13 },
	{ CCI_REG8(0x3103), 0x47 },
	{ CCI_REG8(0x3049), 0x01 },
	{ CCI_REG8(0x304d), 0x02 },
	{ CCI_REG8(0x304c), 0xd7 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x034c), 0x0a },
	{ CCI_REG8(0x034d), 0x20 },
	{ CCI_REG8(0x034e), 0x07 },
	{ CCI_REG8(0x034f), 0x98 },
	{ CCI_REG8(0x0305), 0x02 },
	{ CCI_REG8(0x0307), 0x68 },
	{ CCI_REG8(0x3037), 0x0a },
	{ CCI_REG8(0x3038), 0x01 },
	{ CCI_REG8(0x303e), 0x01 },
	{ CCI_REG8(0x30a2), 0x0e },
	{ CCI_REG8(0x30a5), 0x61 },
	{ CCI_REG8(0x30a7), 0x40 },
	{ CCI_REG8(0x31aa), 0x02 },
	{ CCI_REG8(0x3301), 0x00 },
	{ CCI_REG8(0x3318), 0x60 },
	{ CCI_REG8(0x0202), 0x08 },
	{ CCI_REG8(0x0203), 0xe3 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0210), 0x01 },
	{ CCI_REG8(0x0211), 0x00 },
	{ CCI_REG8(0x0212), 0x01 },
	{ CCI_REG8(0x0213), 0x00 },
	{ CCI_REG8(0x0214), 0x01 },
	{ CCI_REG8(0x0215), 0x00 },
};

struct imx241 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *xclk;
	struct gpio_desc *avdd_enable_gpio;
	struct gpio_descs *enable_gpios;
	struct regulator *io_active_supply;
	struct regulator_bulk_data supplies[IMX241_NUM_SUPPLIES];
	unsigned int num_supplies;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
};

static inline struct imx241 *to_imx241(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx241, sd);
}

static void imx241_set_enable_gpios(struct imx241 *imx241, int value)
{
	unsigned int i;

	if (!imx241->enable_gpios)
		return;

	for (i = 0; i < imx241->enable_gpios->ndescs; i++)
		gpiod_set_value_cansleep(imx241->enable_gpios->desc[i], value);
}

static int imx241_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx241 *imx241 = to_imx241(sd);
	int ret;

	ret = clk_prepare_enable(imx241->xclk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable xclk\n");

	if (imx241->io_active_supply) {
		ret = regulator_enable(imx241->io_active_supply);
		if (ret) {
			clk_disable_unprepare(imx241->xclk);
			return dev_err_probe(dev, ret,
					     "failed to enable shared VSIO\n");
		}
	}

	if (imx241->avdd_enable_gpio)
		gpiod_set_value_cansleep(imx241->avdd_enable_gpio, 1);

	ret = regulator_bulk_enable(imx241->num_supplies, imx241->supplies);
	if (ret) {
		if (imx241->avdd_enable_gpio)
			gpiod_set_value_cansleep(imx241->avdd_enable_gpio, 0);
		if (imx241->io_active_supply)
			regulator_disable(imx241->io_active_supply);
		clk_disable_unprepare(imx241->xclk);
		return dev_err_probe(dev, ret, "failed to enable supplies\n");
	}

	/* Reproduce Samsung's documented low-to-high camera-enable pulse. */
	imx241_set_enable_gpios(imx241, 0);
	usleep_range(1000, 2000);
	imx241_set_enable_gpios(imx241, 1);

	/* More than the required 8192 external-clock cycles before I2C. */
	usleep_range(1000, 2000);

	return 0;
}

static int imx241_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx241 *imx241 = to_imx241(sd);

	imx241_set_enable_gpios(imx241, 0);
	usleep_range(1000, 2000);
	regulator_bulk_disable(imx241->num_supplies, imx241->supplies);
	if (imx241->avdd_enable_gpio)
		gpiod_set_value_cansleep(imx241->avdd_enable_gpio, 0);
	if (imx241->io_active_supply)
		regulator_disable(imx241->io_active_supply);
	clk_disable_unprepare(imx241->xclk);

	return 0;
}

static int imx241_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx241 *imx241 =
		container_of(ctrl->handler, struct imx241, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&imx241->sd);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		u32 exposure_max = IMX241_HEIGHT + ctrl->val -
				   IMX241_EXPOSURE_OFFSET;

		ret = __v4l2_ctrl_modify_range(imx241->exposure,
					       IMX241_EXPOSURE_MIN,
					       exposure_max, 1,
					       min_t(u32, exposure_max, 0x08e3));
		if (ret)
			return ret;
	}

	if (!pm_runtime_get_if_in_use(&client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(imx241->regmap, IMX241_REG_EXPOSURE,
			  ctrl->val, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN: {
		u32 gain = 256 - (256 * 256 / ctrl->val);

		cci_write(imx241->regmap, IMX241_REG_ANALOG_GAIN,
			  min(gain, 240U), &ret);
		break;
	}
	case V4L2_CID_VBLANK:
		cci_write(imx241->regmap, IMX241_REG_FRAME_LENGTH,
			  IMX241_HEIGHT + ctrl->val, &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx241_ctrl_ops = {
	.s_ctrl = imx241_set_ctrl,
};

static int imx241_init_controls(struct imx241 *imx241)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx241->sd);
	struct v4l2_ctrl_handler *handler = &imx241->ctrl_handler;
	struct v4l2_ctrl *ctrl;
	u32 hblank = IMX241_LINE_LENGTH - IMX241_WIDTH;
	u32 vblank = IMX241_FRAME_LENGTH - IMX241_HEIGHT;
	u32 exposure_max = IMX241_FRAME_LENGTH - IMX241_EXPOSURE_OFFSET;
	int ret;

	ret = v4l2_ctrl_handler_init(handler, 7);
	if (ret)
		return ret;

	ctrl = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(imx241_link_freq_menu) - 1,
				      0, imx241_link_freq_menu);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			  IMX241_PIXEL_RATE, IMX241_PIXEL_RATE, 1,
			  IMX241_PIXEL_RATE);

	ctrl = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
				 hblank, hblank, 1, hblank);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx241->vblank =
		v4l2_ctrl_new_std(handler, &imx241_ctrl_ops,
				  V4L2_CID_VBLANK, vblank,
				  IMX241_FRAME_LENGTH_MAX - IMX241_HEIGHT,
				  1, vblank);

	imx241->exposure =
		v4l2_ctrl_new_std(handler, &imx241_ctrl_ops,
				  V4L2_CID_EXPOSURE, IMX241_EXPOSURE_MIN,
				  exposure_max, 1, 0x08e3);

	v4l2_ctrl_new_std(handler, &imx241_ctrl_ops,
			  V4L2_CID_ANALOGUE_GAIN,
			  IMX241_ANALOG_GAIN_MIN, IMX241_ANALOG_GAIN_MAX,
			  1, IMX241_ANALOG_GAIN_DEFAULT);

	if (handler->error) {
		ret = handler->error;
		dev_err_probe(&client->dev, ret, "control init failed\n");
		v4l2_ctrl_handler_free(handler);
		return ret;
	}

	imx241->sd.ctrl_handler = handler;

	return 0;
}

static int imx241_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct imx241 *imx241 = to_imx241(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret < 0)
		return ret;

	cci_write(imx241->regmap, IMX241_REG_MODE_SELECT,
		  IMX241_MODE_STANDBY, &ret);
	cci_multi_reg_write(imx241->regmap, imx241_2592x1944_regs,
			    ARRAY_SIZE(imx241_2592x1944_regs), &ret);
	if (ret)
		goto error_pm_put;

	ret = __v4l2_ctrl_handler_setup(imx241->sd.ctrl_handler);
	if (ret)
		goto error_pm_put;

	ret = cci_write(imx241->regmap, IMX241_REG_MODE_SELECT,
			IMX241_MODE_STREAMING, NULL);
	if (ret)
		goto error_pm_put;

	return 0;

error_pm_put:
	pm_runtime_put_autosuspend(&client->dev);
	return ret;
}

static int imx241_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct imx241 *imx241 = to_imx241(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	ret = cci_write(imx241->regmap, IMX241_REG_MODE_SELECT,
			IMX241_MODE_STANDBY, NULL);
	pm_runtime_put_autosuspend(&client->dev);

	return ret;
}

static int imx241_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	return 0;
}

static int imx241_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SRGGB10_1X10)
		return -EINVAL;

	fse->min_width = IMX241_WIDTH;
	fse->max_width = IMX241_WIDTH;
	fse->min_height = IMX241_HEIGHT;
	fse->max_height = IMX241_HEIGHT;

	return 0;
}

static int imx241_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *crop;

	fmt->format.code = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->format.width = IMX241_WIDTH;
	fmt->format.height = IMX241_HEIGHT;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->format.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;

	format = v4l2_subdev_state_get_format(state, fmt->pad);
	*format = fmt->format;

	crop = v4l2_subdev_state_get_crop(state, fmt->pad);
	crop->left = 0;
	crop->top = 0;
	crop->width = IMX241_WIDTH;
	crop->height = IMX241_HEIGHT;

	return 0;
}

static int imx241_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, sel->pad);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX241_WIDTH;
		sel->r.height = IMX241_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx241_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.code = MEDIA_BUS_FMT_SRGGB10_1X10,
			.width = IMX241_WIDTH,
			.height = IMX241_HEIGHT,
		},
	};

	return imx241_set_pad_format(sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops imx241_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx241_pad_ops = {
	.enum_mbus_code = imx241_enum_mbus_code,
	.enum_frame_size = imx241_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx241_set_pad_format,
	.get_selection = imx241_get_selection,
	.enable_streams = imx241_enable_streams,
	.disable_streams = imx241_disable_streams,
};

static const struct v4l2_subdev_ops imx241_subdev_ops = {
	.video = &imx241_video_ops,
	.pad = &imx241_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx241_internal_ops = {
	.init_state = imx241_init_state,
};

static int imx241_check_hwcfg(struct device *dev)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *endpoint;
	unsigned int i;
	int ret;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "waiting for fwnode graph endpoint\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &bus_cfg);
	fwnode_handle_put(endpoint);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != 2) {
		ret = dev_err_probe(dev, -EINVAL,
				    "only two CSI-2 lanes are supported\n");
		goto out_free;
	}

	for (i = 0; i < bus_cfg.nr_of_link_frequencies; i++)
		if (bus_cfg.link_frequencies[i] == IMX241_LINK_FREQ)
			break;

	if (i == bus_cfg.nr_of_link_frequencies)
		ret = dev_err_probe(dev, -EINVAL,
				    "400 MHz link frequency is required\n");

out_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int imx241_get_regulators(struct imx241 *imx241)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx241->sd);
	unsigned int i;

	if (imx241->avdd_enable_gpio) {
		imx241->io_active_supply =
			devm_regulator_get(&client->dev, "io-active");
		if (IS_ERR(imx241->io_active_supply))
			return PTR_ERR(imx241->io_active_supply);

		imx241->supplies[0].supply = "dvdd";
		imx241->supplies[1].supply = "dovdd";
		imx241->num_supplies = 2;
	} else {
		for (i = 0; i < IMX241_NUM_SUPPLIES; i++)
			imx241->supplies[i].supply = imx241_supply_names[i];
		imx241->num_supplies = IMX241_NUM_SUPPLIES;
	}

	return devm_regulator_bulk_get(&client->dev, imx241->num_supplies,
				       imx241->supplies);
}

static int imx241_identify(struct imx241 *imx241)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx241->sd);
	u64 value;
	int ret;

	ret = cci_read(imx241->regmap, IMX241_REG_CHIP_ID, &value, NULL);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to read IMX241 chip ID\n");

	if (value != IMX241_CHIP_ID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "unexpected chip ID 0x%02llx\n", value);

	dev_info(&client->dev, "detected Sony IMX241 (ID 0x%02llx)\n",
		 value);
	return 0;
}

static int imx241_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx241 *imx241;
	int ret;

	imx241 = devm_kzalloc(dev, sizeof(*imx241), GFP_KERNEL);
	if (!imx241)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&imx241->sd, client, &imx241_subdev_ops);
	imx241->sd.internal_ops = &imx241_internal_ops;

	ret = imx241_check_hwcfg(dev);
	if (ret)
		return ret;

	imx241->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx241->regmap))
		return dev_err_probe(dev, PTR_ERR(imx241->regmap),
				     "failed to initialize CCI\n");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
	imx241->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
#else
	imx241->xclk = devm_clk_get(dev, NULL);
#endif
	if (IS_ERR(imx241->xclk))
		return dev_err_probe(dev, PTR_ERR(imx241->xclk),
				     "failed to get xclk\n");

	if (clk_get_rate(imx241->xclk) != IMX241_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "19.2 MHz xclk is required\n");

	imx241->avdd_enable_gpio =
		devm_gpiod_get_optional(dev, "avdd-enable", GPIOD_OUT_LOW);
	if (IS_ERR(imx241->avdd_enable_gpio))
		return dev_err_probe(dev, PTR_ERR(imx241->avdd_enable_gpio),
				     "failed to get analog-supply enable GPIO\n");

	imx241->enable_gpios =
		devm_gpiod_get_array_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(imx241->enable_gpios))
		return dev_err_probe(dev, PTR_ERR(imx241->enable_gpios),
				     "failed to get camera-enable GPIOs\n");

	if (imx241->enable_gpios && imx241->enable_gpios->ndescs != 2)
		return dev_err_probe(dev, -EINVAL,
				     "expected two camera-enable GPIOs\n");

	ret = imx241_get_regulators(imx241);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get supplies\n");

	ret = imx241_power_on(dev);
	if (ret)
		return ret;

	ret = imx241_identify(imx241);
	if (ret)
		goto error_power_off;

	ret = imx241_init_controls(imx241);
	if (ret)
		goto error_power_off;

	imx241->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx241->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx241->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx241->sd.entity, 1, &imx241->pad);
	if (ret)
		goto error_ctrl_free;

	imx241->sd.state_lock = imx241->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&imx241->sd);
	if (ret)
		goto error_entity_cleanup;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&imx241->sd);
	if (ret)
		goto error_subdev_cleanup;

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(&imx241->sd);
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
error_entity_cleanup:
	media_entity_cleanup(&imx241->sd.entity);
error_ctrl_free:
	v4l2_ctrl_handler_free(&imx241->ctrl_handler);
error_power_off:
	imx241_power_off(dev);
	return ret;
}

static void imx241_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx241 *imx241 = to_imx241(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&imx241->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev)) {
		imx241_power_off(&client->dev);
		pm_runtime_set_suspended(&client->dev);
	}
}

static const struct acpi_device_id imx241_acpi_ids[] = {
	{ "INT347F" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, imx241_acpi_ids);

static const struct of_device_id imx241_of_ids[] = {
	{ .compatible = "sony,imx241" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx241_of_ids);

static const struct dev_pm_ops imx241_pm_ops = {
	SET_RUNTIME_PM_OPS(imx241_power_off, imx241_power_on, NULL)
};

static struct i2c_driver imx241_i2c_driver = {
	.driver = {
		.name = "imx241",
		.acpi_match_table = imx241_acpi_ids,
		.of_match_table = imx241_of_ids,
		.pm = &imx241_pm_ops,
	},
	.probe = imx241_probe,
	.remove = imx241_remove,
};
module_i2c_driver(imx241_i2c_driver);

MODULE_AUTHOR("Galaxy TabPro S Linux community");
MODULE_DESCRIPTION("Sony IMX241 image sensor driver");
MODULE_LICENSE("GPL");
