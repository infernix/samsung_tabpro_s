#!/usr/bin/env bash
set -euo pipefail

if (($# < 1)); then
    echo "Usage: $0 KERNEL_RELEASE [MAKE_ARGUMENT ...]" >&2
    exit 2
fi

readonly kernel_release=$1
shift
make_arguments=("$@")
readonly kernel_build=/lib/modules/$kernel_release/build
readonly source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

if [[ ! -d $kernel_build ]]; then
    echo "Kernel headers not found at ${kernel_build}." >&2
    exit 1
fi

build_module() {
    local module_dir=$1
    local argument
    local has_llvm_argument=false

    for argument in "${make_arguments[@]}"; do
        [[ $argument == LLVM=* ]] && has_llvm_argument=true
    done
    if [[ $has_llvm_argument == false ]] &&
       grep -qx 'CONFIG_CC_IS_CLANG=y' "$kernel_build/include/config/auto.conf"; then
        make_arguments+=(LLVM=1)
    fi

    make "${make_arguments[@]}" -C "$kernel_build" \
        M="$source_dir/$module_dir" modules
}

build_module tps68470
build_module ipu-bridge
build_module imx241
build_module dw9806b
