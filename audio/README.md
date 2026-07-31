# Galaxy TabPro S internal-speaker workaround

This directory contains an experimental userspace workaround for silent
internal speakers on the Samsung Galaxy TabPro S. ALSA and PipeWire can expose
an apparently working Realtek ALC298 analog output while the two physical
speaker amplifiers remain uninitialized.

The workaround replays only the amplifier-initialization part of the HDA
coefficient sequence used by Samsung's Windows driver, then runs it once at
every boot.

## Validated configuration

- Product: Samsung Galaxy TabPro S SM-W700
- DMI product name: `Galaxy TabPro S`
- Intel audio controller: `8086:9d70`, subsystem `144d:c135`
- Codec: Realtek ALC298 `10ec:0298`, subsystem `144d:c135`
- Tested kernel: 7.1.5 with `snd_soc_avs`
- Tested userspace: PipeWire 1.6.8

The scripts intentionally refuse to run unless the Samsung DMI and PCI
identifiers match. The init script also verifies the codec and its subsystem
before opening the HDA hardware-dependent device.

Do not bypass these checks or use the coefficient table on another machine.
Realtek coefficient writes are vendor- and board-specific.

## Result

Before initialization, direct ALSA playback completed successfully but produced
no measurable speaker response: the 997 Hz baseline was -34.3 dB and the tone
window was -35.6 dB. Replaying capture entries 33 through 312 made the same test
tone audible. The first post-initialization measurement was -2.1 dB, 31.2 dB
above its local baseline. An end-to-end retest of the packaged installer
measured -51.1 dB at baseline and -2.0 dB during the tone, a 49.1 dB increase.
Repeating the sequence and restarting the systemd service were also successful.

This sequence contains 280 coefficient writes: the matching initialization
blocks for the two external amplifiers addressed through the ALC298 at `0x31`
and `0x34`. The rest of the 1,758-entry Windows capture is deliberately not
included.

## Audio-driver prerequisite

The workaround requires an ALC298 hardware-dependent device such as
`/dev/snd/hwC0D0` and a working analog PCM. Check for the codec with:

```sh
grep -H -E '^(Vendor|Subsystem) Id:' /proc/asound/card*/codec#*
aplay -l
```

On the validated current-kernel setup, the AVS driver was selected with:

```sh
sudo install -Dm0644 99-tabpro-dsp.conf \
    /etc/modprobe.d/99-tabpro-dsp.conf
sudo reboot
```

Do this only when the analog codec/PCM is unavailable and the kernel provides
`snd_soc_avs`. The amplifier installer does not change the selected audio
driver automatically.

## Install

A C compiler and systemd are required.

```sh
sudo ./install.sh
```

The installer:

1. validates the DMI and PCI identifiers;
2. builds the small HDA replay helper from source;
3. installs the helper, the minimal coefficient table, and the init script;
4. enables and starts `tabpro-audio-amp.service`.

Check the result with:

```sh
systemctl status tabpro-audio-amp.service
journalctl -u tabpro-audio-amp.service -b
```

A successful run reports:

```text
Applied 280 coefficient writes from sequence 33 through 312.
```

## Remove

```sh
sudo ./uninstall.sh
```

The optional AVS configuration is not removed automatically because changing
the active kernel audio driver requires a separate decision. To remove it:

```sh
sudo rm /etc/modprobe.d/99-tabpro-dsp.conf
sudo reboot
```

## Scope and limitations

- This fixes the internal speakers only.
- Headset, microphones, suspend/resume, and related TabPro S variants have not
  yet been validated by this test.
- This is a userspace workaround, not the desired long-term kernel fix.
- No Samsung executable, DLL, or complete proprietary driver package is
  distributed here. The included table is the minimal hardware-initialization
  data needed for Linux interoperability.

The long-term solution should be an upstream, subsystem-specific kernel quirk.
The working sequence and measurements here are intended to make that work
reproducible. See [RESEARCH.md](RESEARCH.md) for capture, isolation, and
objective test details.
