#!/usr/bin/env bash
set -euo pipefail

readonly script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

if ((EUID != 0)); then
    echo "Run this installer as root, for example: sudo ./install.sh" >&2
    exit 1
fi

for command_name in cc install mktemp systemctl; do
    if ! command -v "$command_name" >/dev/null; then
        echo "Required command not found: ${command_name}" >&2
        exit 1
    fi
done

"$script_dir/tabpro-audio-amp-init" --check-hardware

build_dir=$(mktemp -d)
cleanup() {
    rm -r -- "$build_dir"
}
trap cleanup EXIT

cc -O2 -Wall -Wextra -Werror -std=c11 \
    "$script_dir/tabpro-hda-coef-replay.c" \
    -o "$build_dir/tabpro-hda-coef-replay"

install -Dm0755 "$build_dir/tabpro-hda-coef-replay" \
    /usr/local/libexec/tabpro-hda-coef-replay
install -Dm0755 "$script_dir/tabpro-audio-amp-init" \
    /usr/local/libexec/tabpro-audio-amp-init
install -Dm0644 "$script_dir/alc298-smw700-amp.tsv" \
    /usr/local/share/tabpro-audio/alc298-smw700-amp.tsv
install -Dm0644 "$script_dir/tabpro-audio-amp.service" \
    /etc/systemd/system/tabpro-audio-amp.service

systemctl daemon-reload
systemctl enable tabpro-audio-amp.service
systemctl restart tabpro-audio-amp.service
systemctl --no-pager --full status tabpro-audio-amp.service
