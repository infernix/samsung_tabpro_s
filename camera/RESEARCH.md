# Camera reverse-engineering and validation notes

## Hardware topology

The CIO2 media graph exposes two Sony IMX241 sensors:

```text
imx241 4-0010 -> ipu3-csi2 0
imx241 5-0037 -> ipu3-csi2 1
```

Only the sensor on I2C adapter 4 has a voice-coil focus actuator:

```text
dw9806b 4-000c
focus_absolute: min=0 max=1023 step=1 default=0
```

Kernel boot messages confirmed that both sensors identify as IMX241 ID `0x40`
and that the DW9806B is instantiated for `INT347F:00`.

The IPU3 controller is PCI `8086:9d32` with Samsung subsystem `144d:c135`.
Both camera ACPI nodes depend on the TPS68470 at `INT3472:04`. The rear
`INT347F:00` module uses the PMIC's CORE/ANA/VCM/VSIO rails and GPIOs 7 and 9.
The front `INT347F:01` module uses AUX1/AUX2 plus the shared VSIO rail and
GPIOs 4, 5, and 6.

Samsung's `SkcController.sys` programs the front GPIO electrical controls as
`GPCTL4A=0x8a` and `GPCTL5A/GPCTL6A=0xc2`. Programming only the generic GPIO
mode bits left the front analog rail unusable. The board-specific TPS68470
initialization in this package preserves those complete values.

## Processing pipeline

Both cameras pass RAW10 through Intel CIO2 and the IPU3 ImgU. The custom
libcamera installation selects:

```text
/opt/tabpro-camera/share/libcamera/ipa/ipu3/imx241.yaml
```

The processed viewfinder is NV12 at up to 1280x720. The processed still role
accepts NV12 at 2560x1920. The sensor reports a 2592x1944 native array.

The IMX241 tuning does not yet include measured colour-correction matrices or
lens-shading tables. IPU3 AWB therefore uses its generic behaviour and an
identity CCM. This explains why colour is usable but not comparable with the
original Windows factory pipeline.

## Focus isolation

An early focus sweep was invalid because every new libcamera capture session
started the IPU3 autofocus algorithm and overwrote the manual V4L2 position.
The valid controlled sweep forced one position on every frame while leaving
exposure and white balance active.

Results for the static validation scene:

| VCM position | Laplacian deviation |
|---:|---:|
| 0 | 0.01555 |
| 64 | 0.01563 |
| 128 | 0.01543 |
| 192 | 0.01577 |
| 256 | 0.01574 |
| 384 | 0.01616 |
| 512 | 0.01847 |
| 768 | 0.01504 |
| 1023 | 0.01547 |

The actuator therefore moves and affects optical focus. The poor automatic
result was not a dead VCM or an immutable physical blur.

## Autofocus root cause

The stock IPU3 `Af::afScan()` implementation treated the first sufficiently
negative variance derivative as the end of the hill climb. In the validation
scene it first stabilized near step `208`, even though the global contrast
maximum was around `512`.

The updated implementation records the best variance over the complete
configured range. It then returns to that position and performs the existing
fine scan around it. Three post-install runs ended at focus positions `517`,
`548`, and `560`; two pre-install confirmation runs ended at `548` and `552`.

## Objective validation

Before the change, a settled 1280x720 frame measured:

```text
Laplacian standard deviation: 0.0159681
```

After the change, a settled frame measured:

```text
Laplacian standard deviation: 0.0178906
```

That is approximately a 12% increase in this scene. The controlled position
512 sample reached `0.0184654`.

The improvement is also visible in the width and definition of high-contrast
edges. It does not correct the remaining magenta colour bias because focus and
colour calibration are separate ISP problems.

## End-to-end result

The packaged `tabpro-camera/0.6.0` installation was validated end to end on
August 1, 2026. The preflight accepted the hardware and kernel, all four source
modules built before installation, DKMS built and signed the installed copies,
the isolated libcamera became the first ABI 0.7 library in the loader cache,
and `mkinitcpio` completed for both installed kernels.

After loading the packaged modules and libcamera IPA:

- `cam -l` enumerated both IMX241 cameras.
- rear camera captured 140/140 requested 1280x720 NV12 frames.
- front camera captured 40/40 requested 1280x720 NV12 frames.
- rear autofocus settled in the expected global-maximum region.
- Snapshot opened a live PipeWire stream from the front `LNK1` module and
  exposed the second camera through its switch-camera control.
- PipeWire still exposed the expected internal audio sinks and source after
  WirePlumber was restarted to load the updated libcamera IPA.
- the `tabpro-audio-amp` service remained active.
- the distribution libcamera remained untouched and is restored as the default
  when the loader entry is removed by the uninstaller.

The public `tabpro-camera-status --capture-test` command produced a final
`All requested camera checks passed` result. The same installation also
recovered correctly from a DKMS `added` state left by an interrupted attempt;
this exercises the installer's repeat-run path rather than only a clean first
installation.

The camera application should be restarted after updating libcamera so that
it does not keep the old shared library and IPA module loaded.

## Packaged kernel changes

The DKMS package replaces four modular drivers for the selected kernel:

1. `intel_skl_int3472_tps68470` adds the regulator consumers, GPIO lookups,
   front GPIO electrical configuration, and a safe clock-consumer fallback for
   module reloads.
2. `ipu-bridge` adds `INT347F` at a 400 MHz link frequency and recognizes the
   `dw9806b` VCM type.
3. `imx241` implements the 2592x1944 RAW10 mode, exposure, analogue gain,
   blanking, power sequencing, and both ACPI and Device Tree matches.
4. `dw9806b` implements the 0..1023 absolute-focus control, Samsung actuator
   initialization, runtime power management, and a controlled park sequence.

The IMX241 mode register list was recovered from Samsung's public 2016 driver
package and consists of hardware configuration values required for Linux
interoperability. No Samsung executable, DLL, or complete proprietary driver
is redistributed. The remaining driver bases carry their original Linux SPDX
identifiers and author notices; new TabPro-specific code is GPL-2.0.

## Compatibility validation

All four packaged module sources were compiled with warnings treated as errors
against the running CachyOS `7.1.5-1-cachyos` headers. API compatibility was
also compiled against an official, prepared upstream Linux 6.12 tree. The
compatibility layer covers:

- the 6.12 identifier versus 6.13+ string form of exported namespaces;
- the pre-6.17 clock lookup versus `devm_v4l2_sensor_clk_get()`;
- the INT3472 private-header move into exported platform data;
- allocation helpers introduced after 6.12.

This is compile validation, not a claim that an SM-W700 was runtime-tested on
every intermediate kernel. The installer therefore restricts the accepted
range to 6.12 through 7.1 and verifies that IPU3, the bridge, and INT3472 are
modules rather than built into the kernel.

The libcamera patch applies cleanly to exact commit
`191e202178f02430b5942397c70d215cdd2056fa` (`v0.7.2`). The installer checks
that commit before building and stages the installation under `/opt` before
changing the system. This avoids depending on a moving branch or overwriting
distribution-owned libcamera files.

## Installer safety boundaries

The hardware gate combines DMI, PCI, and ACPI checks instead of relying on a
single model string. This permits an LTE model with the same camera topology
to be tested while refusing unrelated Samsung systems.

Before installation, the script checks all dependencies, builds libcamera,
stages its installed image, and compiles every DKMS module. The uninstaller
will remove `/opt/tabpro-camera` only if the package ownership marker exists.
The distribution libraries are selected again after the loader entry is
removed and `ldconfig` is run.

The audio amplifier workaround is independent. Camera validation included a
post-WirePlumber check that the internal audio sinks and source remained
present; the published camera installer does not restart or reconfigure audio.

## Future calibration work

Meaningful colour improvement requires:

1. RAW10 capture and reliable IPU3 unpacking.
2. A neutral grey target for black level and AWB validation.
3. A calibrated colour chart under at least daylight and tungsten lighting.
4. Generation and validation of CCM and lens-shading data.
5. Separate measurements for the front and rear modules, even though both
   sensors identify as `imx241`.

Without those inputs, ad-hoc saturation, gamma, or sharpening changes risk
making one room look better while degrading other lighting conditions.
