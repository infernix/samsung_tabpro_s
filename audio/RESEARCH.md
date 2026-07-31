# Reverse-engineering and validation notes

These notes document the provenance of the speaker-amplifier workaround so the
result can be reviewed and, ideally, replaced by an upstream kernel quirk.

## Device identity

The tested Windows codec identity is:

```text
INTELAUDIO\FUNC_01&VEN_10EC&DEV_0298&SUBSYS_144DC135
```

Linux reports the same board as:

```text
Intel audio controller: 8086:9d70, subsystem 144d:c135
Realtek codec:          10ec:0298, subsystem 144d:c135
```

## Capture

The HDA command stream was captured while the official 2016 Samsung Windows
audio driver initialized the codec in an instrumented QEMU environment. The
resulting ordered trace contained 1,758 writes to Realtek processing
coefficients through node `0x20`.

The complete driver package, Samsung binaries, and the full trace are not part
of this contribution.

## Isolation

Replaying the complete coefficient trace is not a safe workaround: unrelated
codec state can be overwritten, and early experiments required a reboot to
restore the codec.

The trace was therefore divided into functional blocks and tested from a clean
boot. Entries 33 through 312 are a self-contained pair of matching external
amplifier initialization blocks:

- entries 33-172 target indirect address `0x31`;
- entries 173-312 target indirect address `0x34`.

Each amplifier block programs the same register/value set through ALC298
coefficient indices `0x22`, `0x23`, `0x25`, and `0x26`. No ordinary pin-routing
or mixer coefficient writes are included in the distributed table.

## Objective test

The test used direct ALSA playback and capture at 48 kHz with a 997 Hz stereo
tone. The mixer was temporarily set to full output and restored afterward.

Before replay:

```text
Playback status:          0
997 Hz baseline mean:   -34.3 dB
997 Hz tone mean:       -35.6 dB
Result: no speaker response
```

First test after replay:

```text
Playback status:          0
997 Hz baseline mean:   -33.3 dB
997 Hz tone mean:        -2.1 dB
Increase over baseline:  31.2 dB
```

End-to-end package retest:

```text
Playback status:          0
997 Hz baseline mean:   -51.1 dB
997 Hz tone mean:        -2.0 dB
Increase over baseline:  49.1 dB
```

The sequence was also applied repeatedly by restarting the oneshot systemd
service, with successful HDA ioctl completion each time.

## Upstream direction

The userspace helper proves that the missing operation is board-specific
external-amplifier initialization. A permanent fix should match subsystem
`144d:c135` and perform the equivalent initialization in the appropriate Linux
HDA/Realtek or AVS layer. The exact placement and power-management behavior
should be reviewed by ALSA maintainers.

Suspend/resume has not yet been characterized. An upstream implementation may
need to repeat the sequence after codec or amplifier power loss.
