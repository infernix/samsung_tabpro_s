#!/usr/bin/env python3
"""Mirror intel_backlight/brightness -> mutter SetCrtcGamma for Samsung TabPro S OLED.

Wayland/GNOME equivalent of infernix/samsung_tabpro_s brightness/oledbrightness
(which used `xrandr --brightness` on X11). Watches the intel_backlight sysfs
node and applies a perceptual gamma multiplier to the eDP CRTC via mutter's
org.gnome.Mutter.DisplayConfig.SetCrtcGamma, so the hardware brightness keys
and the GNOME/GDM brightness slider actually dim the OLED panel.

WHY THIS IS NEEDED
------------------
The Samsung TabPro S (SM-W700) has a Samsung Display Corp. (SDC) OLED panel.
The kernel's `/sys/class/backlight/intel_backlight` exists and writes the
i915 PWM duty register (BLC_PWM_PCH_DUTY1 @ 0xC8254), and the brightness
keys / slider write to it — but the panel ignores it. The panel does NOT
expose backlight control over DPCD either (DPCD 0x220 caps = 0x04,
PWM-only; DPCD 0x722 writes are silently accepted but do nothing).
Windows drives the panel over I2C1 (SPNL0001 device in ACPI), but Linux
has no SPNL driver. Result: the brightness indicator moves but the panel
stays at full brightness.

The only working software dimming path on Wayland/GNOME is mutter's
SetCrtcGamma D-Bus method, which writes the i915 GAMMA_LUT (hardware LUT
on the CRTC, 1024 entries). This daemon mirrors intel_backlight -> gamma.

Note: this is software dimming (multiplies pixel values via the gamma LUT).
Below ~30% you lose shadow detail; true black is not achievable. This is
the same limitation as the original xrandr --brightness approach.

REQUIRES
--------
- GNOME on Wayland (mutter implements org.gnome.Mutter.DisplayConfig).
  KWin does NOT expose SetCrtcGamma; under KDE use a KWin effect instead.
- python3-dbus (Fedora: `dnf install python3-dbus`).
- i915 (the panel's CRTC must expose a GAMMA_LUT — Skylake i915 does,
  GAMMA_LUT_SIZE=1024).

USAGE
-----
Run directly, or install via ../install.sh as a user systemd service and a
GDM greeter autostart. Optional argv[1] = CRTC index (default: autodetect
the active CRTC via mutter GetResources).
"""
import os, sys, time, dbus, select, ctypes

BACKLIGHT      = "/sys/class/backlight/intel_backlight"
BRIGHTNESS     = BACKLIGHT + "/brightness"
MAX_BRIGHTNESS = BACKLIGHT + "/max_brightness"

SERVICE  = "org.gnome.Mutter.DisplayConfig"
OBJ_PATH = "/org/gnome/Mutter/DisplayConfig"
IFACE    = "org.gnome.Mutter.DisplayConfig"

LUT_SIZE      = 1024   # i915 GAMMA_LUT_SIZE on Skylake
MAX_VAL       = 65535
POWER         = 1.6    # perceptual curve: slider fraction f -> brightness f^POWER
POLL_SECS     = 5      # periodic resync (catches writes that miss inotify)
MIN_INTERVAL  = 0.1    # rate-limit (slider drags generate many writes)


def read_int(p):
    with open(p) as f:
        return int(f.read().strip())


def make_lut(mult, power):
    """Build a 1024-entry uint16 gamma LUT applying a brightness multiplier.

    A linear ramp raised to (1) and scaled by mult. The POWER exponent
    applies a perceptual curve to the slider fraction before multiplying,
    so the bottom half of the slider covers the dim range (OLED needs
    finer control at low brightness).
    """
    n = LUT_SIZE
    return [int((i / (n - 1)) ** power * mult * MAX_VAL) & 0xffff for i in range(n)]


def get_serial(dc):
    # GetCurrentState returns (serial, monitors, logical_monitors, props)
    return int(dc.GetCurrentState(dbus_interface=IFACE)[0])


def find_active_crtc(dc):
    # GetResources: (serial, crtcs, outputs, modes, max_w, max_h)
    # crtc tuple: (id, x, y, width, height, mode_valid, ...)
    res = dc.GetResources(dbus_interface=IFACE)
    for c in res[1]:
        if c[2] != 0 or c[3] != 0:
            return int(c[0])
    return int(res[1][0][0]) if res[1] else 0


def connect():
    bus = dbus.SessionBus()
    dc = bus.get_object(SERVICE, OBJ_PATH)
    serial = get_serial(dc)
    crtc = find_active_crtc(dc)
    return bus, dc, serial, crtc


def apply(dc, serial, crtc, mult):
    lut = make_lut(mult, POWER)
    arr = dbus.Array([dbus.UInt16(x) for x in lut], signature="q")
    dc.SetCrtcGamma(dbus.UInt32(serial), dbus.UInt32(crtc), arr, arr, arr,
                   dbus_interface=IFACE)


def inotify_init(path):
    libc = ctypes.CDLL("libc.so.6")
    IN_NONBLOCK = 0x800
    IN_MODIFY   = 0x2
    fd = libc.inotify_init1(IN_NONBLOCK)
    if fd < 0:
        raise OSError("inotify_init1")
    wd = libc.inotify_add_watch(fd, path.encode(), IN_MODIFY)
    if wd < 0:
        raise OSError("inotify_add_watch")
    return fd


def drain(fd):
    while True:
        try:
            os.read(fd, 4096)
        except BlockingIOError:
            break


def main():
    max_b = read_int(MAX_BRIGHTNESS)
    print(f"tabpro-dim: started max_b={max_b} power={POWER}", flush=True)
    bus = dc = serial = crtc = None
    fd = inotify_init(BRIGHTNESS)
    last_apply = 0
    while True:
        if dc is None:
            try:
                bus, dc, serial, crtc = connect()
                print(f"tabpro-dim: connected serial={serial} crtc={crtc}", flush=True)
            except Exception as e:
                print(f"tabpro-dim: connect failed: {e}", flush=True)
                time.sleep(2)
                continue
        r, _, _ = select.select([fd], [], [], POLL_SECS)
        now = time.time()
        if r:
            drain(fd)
            # debounce so rapid writes (slider drag) coalesce
            time.sleep(0.05)
            drain(fd)
        if not (r or (now - last_apply > POLL_SECS)):
            continue
        if now - last_apply < MIN_INTERVAL:
            time.sleep(MIN_INTERVAL - (now - last_apply))
        try:
            lvl = read_int(BRIGHTNESS)
            mult = max(0.0, min(1.0, lvl / max_b))
            apply(dc, serial, crtc, mult)
            last_apply = time.time()
            print(f"tabpro-dim: lvl={lvl}/{max_b} mult={mult:.3f}", flush=True)
        except Exception as e:
            print(f"tabpro-dim: apply failed: {e}", flush=True)
            dc = None  # force reconnect (compositor may have restarted)
            time.sleep(1)


if __name__ == "__main__":
    main()