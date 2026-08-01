#!/usr/bin/env bash
set -uo pipefail

capture_test=false
case ${1:-} in
    "") ;;
    --capture-test) capture_test=true ;;
    *)
        echo "Usage: $0 [--capture-test]" >&2
        exit 2
        ;;
esac

failures=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*" >&2; failures=$((failures + 1)); }
info() { printf 'INFO  %s\n' "$*"; }

vendor=
product=
board=
[[ -r /sys/class/dmi/id/sys_vendor ]] &&
    vendor=$(< /sys/class/dmi/id/sys_vendor)
[[ -r /sys/class/dmi/id/product_name ]] &&
    product=$(< /sys/class/dmi/id/product_name)
[[ -r /sys/class/dmi/id/board_name ]] &&
    board=$(< /sys/class/dmi/id/board_name)
info "System: ${vendor:-unknown} / ${product:-unknown} / ${board:-unknown}"

if [[ $vendor == "SAMSUNG ELECTRONICS CO., LTD." &&
      $product == "Galaxy TabPro S"* ]]; then
    pass "Galaxy TabPro S DMI"
else
    fail "Unexpected DMI identity"
fi

if command -v dkms >/dev/null &&
   dkms status -m tabpro-camera -v 0.6.0 2>/dev/null | grep -q installed; then
    pass "DKMS package tabpro-camera/0.6.0"
else
    fail "DKMS package tabpro-camera/0.6.0 is not installed"
fi

for module_name in intel_skl_int3472_tps68470 ipu_bridge imx241 dw9806b; do
    module_file=$(modinfo -F filename "$module_name" 2>/dev/null || true)
    if [[ $module_file == *updates* ]]; then
        pass "${module_name}: ${module_file}"
    else
        fail "${module_name} is not the DKMS module (${module_file:-missing})"
    fi
done

if [[ -r /opt/tabpro-camera/.tabpro-camera-managed ]]; then
    pass "Managed libcamera installation"
else
    fail "Managed libcamera installation is missing"
fi

loader_library=$(ldconfig -p 2>/dev/null |
    awk '/libcamera[.]so[.]0[.]7 / && !found { print $NF; found = 1 }')
if [[ $loader_library == /opt/tabpro-camera/lib/libcamera.so.0.7 ]]; then
    pass "Dynamic loader selects the TabPro libcamera"
else
    fail "Dynamic loader selects ${loader_library:-no libcamera 0.7 library}"
fi

cam_command=/opt/tabpro-camera/bin/cam
if [[ ! -x $cam_command ]]; then
    cam_command=$(command -v cam 2>/dev/null || true)
fi

camera_list=
if [[ -n $cam_command ]]; then
    camera_list=$($cam_command -l 2>&1)
    camera_result=$?
    printf '%s\n' "$camera_list"
    if ((camera_result == 0)) &&
       grep -Fq '\_SB_.PCI0.LNK0' <<<"$camera_list" &&
       grep -Fq '\_SB_.PCI0.LNK1' <<<"$camera_list"; then
        pass "Both IMX241 cameras enumerate"
    else
        fail "Both expected camera IDs did not enumerate"
    fi
else
    fail "cam utility was not found"
fi

if command -v wpctl >/dev/null; then
    video_status=$(wpctl status 2>/dev/null | sed -n '/Video/,/Settings/p')
    video_count=$(grep -c 'imx241' <<<"$video_status" || true)
    if ((video_count >= 2)); then
        pass "PipeWire exposes two IMX241 video sources"
    else
        info "PipeWire video sources are not active yet; log out/in after reboot if needed"
    fi
fi

if [[ $capture_test == true && -n $cam_command ]]; then
    info "Close Snapshot, browsers, and video-call apps before this test."
    if "$cam_command" -c 1 --capture=140 \
        -s role=viewfinder,width=1280,height=720,pixelformat=NV12; then
        pass "Rear camera captured 140 frames and completed autofocus scan"
    else
        fail "Rear-camera capture failed (it may be busy)"
    fi
    if "$cam_command" -c 2 --capture=40 \
        -s role=viewfinder,width=1280,height=720,pixelformat=NV12; then
        pass "Front camera captured 40 frames"
    else
        fail "Front-camera capture failed (it may be busy)"
    fi
fi

if ((failures)); then
    printf '\n%d check(s) failed. Include this full output in a bug report.\n' \
        "$failures" >&2
    exit 1
fi

printf '\nAll requested camera checks passed.\n'
