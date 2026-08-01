# Samsung TabPro S Linux compatibility tweaks
This is a collection of scripts, quirks and patches to improve the use of a Samsung TabPro S under Linux distros.

A couple of items require some attention for this device
* The NIC is an `ath10k QCA6174`. The firmware support for this device is rather messy and the wrong set of files will break it. Place the contents of the `ath10k/QCA6174/hw3.0` dir in `/lib/firmware` and be sure to delete anything else in there. This firmware works very well (30MByte/s on 5ghz without issue) on 4.7.x.
* Brightness changes do not work with `intel_backlight`, probably because the display is OLED. The way to adjust brightness is by means of `xrandr --brightness`; a script is included that uses inotify to monitor changes to `intel_backlight` so that all existing brightness management tools can function correctly. It should be started with your display manager or in your user session.
* Audio is recognized, but the internal speakers remain silent because their external amplifiers are not initialized. A tested userspace workaround for the `ALC298` codec with subsystem `144d:c135` is available in [`audio/`](audio/README.md). The original kernel Bugzilla report is https://bugzilla.kernel.org/show_bug.cgi?id=188411.
* Both Sony `IMX241` cameras require TabPro-specific power, sensor, actuator, IPU3, and libcamera support. A tested, reversible DKMS/libcamera workaround with hardware checks is available in [`camera/`](camera/README.md).
* TODO: There is an `SMO8A80` accelerometer which is visible through i2s; it should be leveraged to enable auto rotation
* TODO: When disconnecting and reconnecting the dock, the touchpad on the dock stops working. Cause as of yet unknown, but likely an X or synaptics issue.
* TODO: Buttons not tested yet, may or may not work.
