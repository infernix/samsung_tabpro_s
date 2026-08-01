# Samsung TabPro S (SM-W700) OLED brightness fix — Wayland

## Why this device needs a software workaround

On this tablet **none of the interfaces that normally control panel brightness
do anything**. The hardware brightness keys and the desktop slider move the
on-screen indicator, the values land in sysfs, and the Samsung Display (SDC)
OLED panel stays at full brightness:

| Interface | Status on this panel |
|---|---|
| `/sys/class/backlight/intel_backlight` (i915 PWM) | writes reach the PWM duty register (`BLC_PWM_PCH_DUTY1` @ `0xC8254`), but the panel ignores them — `actual_brightness` reads 0 |
| DPCD AUX backlight (`i915.enable_dpcd_backlight=3/2/1`) | panel advertises PWM-only in DPCD 0x220 (`=0x04`); writes to DPCD 0x722/0x723 are silently accepted and do nothing |
| ACPI `_BCM`/`_BCL` | not present in the DSDT at all (the `SAMO`/`SAM0101` device has no brightness method) |
| I2C1 `SPNL0001` (Samsung panel driver) | what Windows actually uses; Linux has no SPNL driver |
| KWin `output.brightness` / `output.dimming` (kscreen-doctor) | metadata only — kwin reports "set to N% and dimming to 100%", but it applies the dimming shader only on HDR-capable outputs, which this non-HDR OLED is not |
| `wl-gamma-control-unstable-v1` | not implemented by KWin 6.7 or mutter 49 in Fedora's builds |
| Legacy DRM `MODE_SETGAMMA` ioctl | requires DRM_MASTER, which the compositor holds — root can't take it |

So the panel's real brightness control is unreachable from Linux, and until
someone writes an `SPNL0001` I²C panel driver the only option is **dimming the
framebuffer** and wiring that to the sysfs value the brightness keys already
write.

That is exactly what the X11 `oledbrightness` script in this repo does — it
glues a completely generic tool (`xrandr --brightness`, which just loads the X
gamma ramp) to `intel_backlight` via inotify. `xrandr`'s gamma no longer exists
under Wayland, so this folder is that script's Wayland equivalent.

**This is not something you want on normal hardware.** Anywhere the i915 PWM
path works, the brightness keys already dim the panel and installing this would
dim it twice. It only makes sense on a device where real backlight control is
unreachable.

## Why there is no off-the-shelf tool for this on Wayland

- `gammastep` / `wlsunset` / `wl-gammarelay` all drive
  `wlr-gamma-control-unstable-v1`, which is wlroots-only. Neither mutter (49)
  nor KWin (6.7) implements it — verified on Fedora 43.
- **GNOME**: the only working path is mutter's own D-Bus method
  `org.gnome.Mutter.DisplayConfig.SetCrtcGamma`, which writes the i915
  `GAMMA_LUT` (1024-entry hardware LUT on the CRTC). Nothing shipped binds it to
  the backlight sysfs, hence the small Python daemon here.
- **KDE**: KWin exposes no gamma API whatsoever, `kscreen-doctor`'s
  brightness/dimming is metadata-only on this output (see the table above), and
  the legacy DRM ioctl needs DRM_MASTER. That leaves a compositor effect plugin
  as the only path — which is why there is C++ in this folder and not a
  five-line script.

## TL;DR — what's in this folder and how to use it

This folder ships **two independent fixes**, one per desktop. Pick the one
matching your session, run its installer, log out and back in. Both make the
hardware brightness keys and the desktop slider actually dim the OLED panel.

| Desktop | Fix | How it works | Install |
|---|---|---|---|
| **GNOME/Wayland** | `tabpro-dim.py` (Python daemon) | Watches `intel_backlight/brightness` via inotify and writes the i915 `GAMMA_LUT` via mutter's `SetCrtcGamma` D-Bus method. | `./install.sh` (top level) |
| **KDE/KWin** | `kwin-effect/` (C++ KWin effect plugin `tabprobright.so`) | Captures the screen to an offscreen FBO each frame and re-renders it through a `tex.rgb *= brightness` fragment shader, driven by inotify on `intel_backlight/brightness`. | `kwin-effect/install.sh` |

Both watch `brightness` rather than `actual_brightness`, because on current
kernels `actual_brightness` is stuck at 0 here — the value the X11
`oledbrightness` script reads never changes, so a straight port of it would
never dim anything. Both apply a perceptual `f^1.6` curve, so the bottom half of
the slider covers the dim range where OLED needs finer control.

### Quick start

```bash
# GNOME/Wayland
cd wayland_brightness/
./install.sh                # also offers to set AccountsService Session=gnome

# KDE/KWin
cd wayland_brightness/kwin-effect/
./install.sh                # builds tabprobright.so against kwin-devel
```

Log out and back in. The GNOME daemon auto-starts as a user systemd service
(`tabpro-dim.service`, gated to GNOME via `ConditionEnvironment`) and also at
the GDM greeter (`tabpro-dim-greeter.desktop`). The KDE effect auto-loads on
kwin start (`EnabledByDefault: true` in `metadata.json`).

### Verify

```bash
# GNOME
systemctl --user status tabpro-dim
journalctl --user -u tabpro-dim -f   # live log as you move the slider

# KDE
gdbus call --session --dest org.kde.KWin -o /Effects \
    -m org.kde.kwin.Effects.isEffectLoaded tabprobright   # (true,)
journalctl --user -b | grep -i tabprobright
```

### Caveats

- This is **software dimming, not a hardware fix**: pixel values are multiplied,
  so below ~30% you lose shadow detail and true black is not achievable. Same
  limitation as the original `xrandr --brightness` approach.
- The KWin effect plugin **must be rebuilt against the matching `kwin-devel`
  version** (the `EffectPluginFactory_iid` includes the kwin version string,
  currently `6.7.3`). After a kwin update, re-run `kwin-effect/install.sh`.
- The GNOME daemon and the KDE effect are mutually exclusive by environment —
  each only runs under its own desktop. Installing both is fine; the GNOME
  service is skipped under KDE by `ConditionEnvironment=XDG_CURRENT_DESKTOP=GNOME`.

## GNOME / mutter — Python daemon

`tabpro-dim.py` watches `/sys/class/backlight/intel_backlight/brightness` via
inotify and applies a perceptual gamma multiplier to the eDP CRTC through
mutter. So the hardware brightness keys and the GNOME/GDM slider — which already
write to `intel_backlight` — now actually dim the panel.

### Requirements

- **GNOME on Wayland** (mutter implements `SetCrtcGamma`; KWin does not)
- `python3-dbus` (`sudo dnf install python3-dbus`)
- i915 with `GAMMA_LUT` support (Skylake i915 — `GAMMA_LUT_SIZE=1024`)

### Install

```bash
sudo dnf install -y python3-dbus
cd wayland_brightness/
./install.sh
```

This installs:
- `~/.local/bin/tabpro-dim.py` — daemon for the user GNOME session
- `~/.config/systemd/user/tabpro-dim.service` — user systemd unit (enabled)
- `/usr/local/bin/tabpro-dim.py` — daemon for the GDM greeter
- `/usr/share/gdm/autostart/LoginWindow/tabpro-dim.desktop` — greeter autostart
- (optional) sets AccountsService `Session=gnome` so the next login uses GNOME

Log out and back in (or reboot). The daemon auto-starts with the graphical
session and at the GDM greeter.

### Verify

```bash
systemctl --user status tabpro-dim
journalctl --user -u tabpro-dim -f    # live log as you move the slider
# press the brightness keys — panel should dim, log shows lvl=N/937 mult=...
```

### Uninstall

```bash
systemctl --user disable --now tabpro-dim.service
rm -f ~/.local/bin/tabpro-dim.py ~/.config/systemd/user/tabpro-dim.service
systemctl --user daemon-reload
sudo rm -f /usr/local/bin/tabpro-dim.py /usr/share/gdm/autostart/LoginWindow/tabpro-dim.desktop
# restore gamma to full (in case the daemon left it dimmed):
sudo true > /sys/class/backlight/intel_backlight/brightness 2>/dev/null || \
  sudo sh -c 'echo 937 > /sys/class/backlight/intel_backlight/brightness'
```

## KDE / Plasma — KWin effect plugin

`tabprobright` captures the screen to an offscreen FBO and re-renders it through
a fragment shader multiplying RGB by a brightness factor, driven by inotify on
`intel_backlight/brightness`. This runs inside the compositor, so it has full
access to the render pipeline — the only place on KWin where dimming is possible
at all (see the diagnosis above).

### Build & install

```bash
cd wayland_brightness/kwin-effect/
./install.sh
# log out of KDE and back in (or: systemctl --user restart plasma-kwin_wayland.service)
```

Requires: `kwin-devel`, `qt6-qtbase-devel`, `extra-cmake-modules`, `cmake`,
`gcc-c++`, plus the KWin dependency chain (`libdrm-devel`, `libepoxy-devel`,
`kf6-kconfig-devel`, `kf6-kcoreaddons-devel`, `kf6-kwindowsystem-devel`,
`qt6-qtdeclarative-devel`, `qt6-qtwayland-devel`, `plasma-wayland-protocols`,
`vulkan-headers`). `install.sh` installs them if missing.

### Verify

```bash
gdbus call --session --dest org.kde.KWin -o /Effects \
    -m org.kde.kwin.Effects.isEffectLoaded tabprobright   # (true,)
# press the brightness keys — panel should dim
```

### Notes

- The effect is `EnabledByDefault: true` and auto-loads on kwin start.
- The plugin must be rebuilt against the matching `kwin-devel` version
  (the `EffectPluginFactory_iid` includes the kwin version string).
- Log: `journalctl --user -b | grep -i tabprobright`

## Files

| File | Purpose |
|---|---|
| `tabpro-dim.py` | GNOME daemon (mutter `SetCrtcGamma` over D-Bus + inotify) |
| `tabpro-dim.service` | User systemd unit (GNOME-only via `ConditionEnvironment`) |
| `tabpro-dim-greeter.desktop` | GDM greeter autostart entry (GNOME) |
| `install.sh` | One-shot installer for the GNOME path |
| `kwin-effect/` | KDE/KWin C++ effect plugin (`tabprobright.so`) |
| `kwin-effect/install.sh` | Build & install the KWin effect |

## Hardware reference

- Panel: SDC "Color LCD", product code 0xa029 (eDP, 2160x1440@60)
- i915: Intel Sunrise Point-LP, device ID `191e`
- DPCD rev: 0x12; DPCD 0x220 (backlight caps) = 0x04 (PWM-only)
- `/sys/class/backlight/intel_backlight`: type=`raw`, max=937, actual=0
