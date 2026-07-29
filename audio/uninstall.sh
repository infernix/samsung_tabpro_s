#!/usr/bin/env bash
set -euo pipefail

if ((EUID != 0)); then
    echo "Run this uninstaller as root, for example: sudo ./uninstall.sh" >&2
    exit 1
fi

systemctl disable --now tabpro-audio-amp.service 2>/dev/null || true

rm -f -- \
    /etc/systemd/system/tabpro-audio-amp.service \
    /usr/local/libexec/tabpro-audio-amp-init \
    /usr/local/libexec/tabpro-hda-coef-replay \
    /usr/local/share/tabpro-audio/alc298-smw700-amp.tsv

rmdir -- /usr/local/share/tabpro-audio 2>/dev/null || true
systemctl daemon-reload
systemctl reset-failed tabpro-audio-amp.service 2>/dev/null || true

echo "Removed the Galaxy TabPro S speaker-amplifier workaround."
