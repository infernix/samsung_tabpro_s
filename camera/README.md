# Galaxy TabPro S front and rear camera workaround

This directory contains an experimental, reversible Linux workaround for both
Sony IMX241 cameras in the Samsung Galaxy TabPro S. It packages the kernel and
libcamera changes that were validated together; installing only one half does
not produce a reliable camera pipeline.

The runtime test was performed on an SM-W700 Wi-Fi tablet. LTE variants are not
claimed as tested yet. The installer deliberately accepts a `Galaxy TabPro S*`
DMI name only when the same Samsung IPU3 PCI identity, two `INT347F` sensors,
and the expected `INT3472:04` camera PMIC are also present. Please report the
complete `tabpro-camera-status` output if an LTE variant passes or fails.

## What it fixes

The tablet firmware describes the camera modules, but mainline Linux does not
yet have the complete board support:

- the IPU bridge lacks the `INT347F`/IMX241 link-frequency entry;
- the TPS68470 needs the TabPro S regulator consumers and GPIO mappings;
- the front module needs its Samsung GPIO4/5/6 electrical configuration;
- Linux needs IMX241 sensor and DW9806B focus-actuator drivers;
- libcamera needs the IMX241 gain helper and IPU3 tuning data;
- the stock IPU3 autofocus can stop at a false local maximum.

This package supplies four DKMS modules and a pinned, isolated libcamera build
under `/opt/tabpro-camera`. It does not overwrite the distribution's libcamera
files. A loader configuration makes compatible applications use the isolated
build, and uninstalling removes that configuration.

## Validated scope

| Component | Validated result |
|---|---|
| Hardware | Samsung Galaxy TabPro S SM-W700, board `SM-W700NZWAMXO` |
| Runtime kernel | `7.1.5-1-cachyos` |
| Compile compatibility | upstream Linux `6.12` and CachyOS Linux `7.1.5` headers |
| Supported installer range | Linux `6.12` through `7.1`, modular IPU3/INT3472 drivers |
| libcamera | ABI 0.7, source pinned to `v0.7.2` commit `191e202178f0` |
| User application | GNOME Snapshot 50 through PipeWire 1.6.8 |
| Rear camera | 140/140 processed 1280x720 frames; autofocus actuator working |
| Front camera | 40/40 processed 1280x720 frames; fixed focus |

The installer refuses unsupported hardware, kernels outside the reviewed
range, built-in IPU bridge/INT3472 drivers that DKMS cannot replace, missing
headers, and distributions without libcamera ABI 0.7.

## Dependencies

Install the headers for the **currently running kernel**, DKMS, a C/C++ build
toolchain, Git, Meson, Ninja, pkg-config, and the development packages for
libudev, libyaml, GnuTLS, and libevent. Python Jinja2, YAML, and PLY modules are
also required by the libcamera build.

On Arch Linux and derivatives, a typical command is:

```sh
sudo pacman -S --needed base-devel dkms git meson ninja pkgconf \
  libcamera pipewire-libcamera libyaml gnutls libevent \
  python-jinja python-yaml python-ply linux-headers
```

Replace `linux-headers` with the header package matching a custom kernel, such
as `linux-cachyos-headers`.

On Debian-family systems with a sufficiently new libcamera ABI, the equivalent
packages are generally:

```sh
sudo apt install build-essential dkms git meson ninja-build pkg-config \
  linux-headers-$(uname -r) libcamera-dev libpipewire-0.3-modules \
  libudev-dev libyaml-dev libgnutls28-dev libevent-dev \
  python3-jinja2 python3-yaml python3-ply
```

Package names vary. `./install.sh --check` reports the exact missing command,
library, kernel option, or hardware identity without changing the system.

## Install

From the repository root:

```sh
cd camera
./install.sh --check
sudo ./install.sh
sudo reboot
```

The build downloads only the reviewed libcamera `v0.7.2` source, verifies its
commit ID, applies the bundled patch, stages the result, and compiles all four
kernel modules before making system changes.

After reboot, close Snapshot, browsers, and video-call applications, then run:

```sh
tabpro-camera-status --capture-test
```

The non-destructive capture test discards the frames after verifying 140 rear
and 40 front frames. Running `tabpro-camera-status` without the option only
checks hardware, modules, libcamera, enumeration, and PipeWire.

`libcamera` currently prints warnings about missing static IMX241 properties,
location, rotation, and unverified sensor delays. These are known limitations
of the new sensor support; a final `All requested camera checks passed` result
means that they did not prevent enumeration or capture.

GNOME Snapshot is a convenient user-facing test application. It may open the
front camera first; use the lower-right switch-camera button to select the rear
module. Allow roughly three seconds for the rear autofocus scan to settle.

## What is installed

- DKMS package `tabpro-camera/0.6.0` in `/usr/src` and the DKMS tree;
- `intel_skl_int3472_tps68470`, `ipu-bridge`, `imx241`, and `dw9806b` modules;
- the custom libcamera build in `/opt/tabpro-camera`;
- `/etc/ld.so.conf.d/00-tabpro-camera.conf`;
- `tabpro-camera-status` and `tabpro-camera-uninstall` in `/usr/local/sbin`.

The installer refreshes the initramfs with `update-initramfs`, `dracut`, or
`mkinitcpio` when one is available. It does not alter the audio workaround or
restart PipeWire/WirePlumber during installation; the reboot loads everything
in a clean order.

## Uninstall

From this directory, or using the installed copy:

```sh
sudo ./uninstall.sh
# or
sudo tabpro-camera-uninstall
sudo reboot
```

The uninstaller removes only a `/opt/tabpro-camera` directory carrying this
package's ownership marker. It removes the DKMS package and loader entry,
refreshes the initramfs, and leaves distribution-owned files untouched.

## Image quality and autofocus

The sensor array is 2592x1944. The IPU3 still role accepted processed NV12 at
2560x1920, while Snapshot used a 1280x720 viewfinder/JPEG path in the test.
Snapshot therefore does not expose all five megapixels.

The rear autofocus previously stopped around focus step `208`, although a
controlled sweep found the sharpest region around `512..560`. Scanning the
complete range and returning to the global maximum raised the measured
Laplacian deviation from `0.01597` to `0.01789`, about 12% in the same scene.

The front camera is physically fixed-focus. The current IPU3 tuning is also
uncalibrated: it has automatic exposure/white balance and tone mapping, but no
measured colour-correction matrix or lens-shading table. A substantial colour
improvement requires RAW captures and a calibrated chart; arbitrary saturation
or sharpening is not a safe general fix.

## Reporting results

For the existing camera issue, include:

```sh
tabpro-camera-status
uname -a
journalctl -b -k | grep -E 'imx241|dw9806b|ipu3|tps68470'
```

State the exact model (Wi-Fi or LTE), distribution, kernel package, whether
Secure Boot is enabled, and which camera failed. Secure Boot systems may need
the normal distribution-specific DKMS/MOK signing procedure.

Implementation measurements and source provenance are in
[`RESEARCH.md`](RESEARCH.md). The libcamera change is kept as the standalone
[`0001-tabpro-s-imx241-ipu3-support.patch`](libcamera/0001-tabpro-s-imx241-ipu3-support.patch).
