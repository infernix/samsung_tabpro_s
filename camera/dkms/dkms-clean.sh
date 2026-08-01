#!/usr/bin/env bash
set -euo pipefail

if (($# != 1)); then
    echo "Usage: $0 KERNEL_RELEASE" >&2
    exit 2
fi

readonly kernel_build=/lib/modules/$1/build
readonly source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

if [[ ! -d $kernel_build ]]; then
    exit 0
fi

for module_dir in tps68470 ipu-bridge imx241 dw9806b; do
    make -C "$kernel_build" M="$source_dir/$module_dir" clean
done
