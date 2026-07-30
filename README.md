# Samsung TabPro S Linux compatibility tweaks
This is a collection of scripts, quirks and patches to improve the use of a Samsung TabPro S under Linux distros.

A couple of items require some attention for this device
* The NIC is an `ath10k QCA6174`. The firmware support for this device is rather messy and the wrong set of files will break it. Place the contents of the `ath10k/QCA6174/hw3.0` dir in `/lib/firmware` and be sure to delete anything else in there. This firmware works very well (30MByte/s on 5ghz without issue) on 4.7.x.
* Brightness changes do not work with `intel_backlight`, probably because the display is OLED. In X11 the way to adjust brightness is by means of `xrandr --brightness`; a script is included that uses inotify to monitor changes to `intel_backlight` so that all existing brightness management tools can function correctly. It should be started with your display manager or in your user session. For Wayland GNOME and KDE dekstop environments look at [wayland_brightness](wayland_brightness/README.md).
* Audio works. See https://github.com/infernix/samsung_tabpro_s/issues/2 for details
* TODO: There is an `SMO8A80` accelerometer which is visible through i2s; it should be leveraged to enable auto rotation
* TODO: When disconnecting and reconnecting the dock, the touchpad on the dock stops working. Cause as of yet unknown, but likely an X or synaptics issue. See https://github.com/infernix/samsung_tabpro_s/issues/8 for details
* TODO: camera doesn't work - see https://github.com/infernix/samsung_tabpro_s/issues/3
* Buttons (both hardware - volume, power) and hotkeys (FN+F1..F12) do work in modern distros
