#!/usr/bin/env bash
set -euo pipefail

readonly package_name=tabpro-camera
readonly package_version=0.6.0
readonly libcamera_version=0.7.2
readonly libcamera_commit=191e202178f02430b5942397c70d215cdd2056fa
readonly libcamera_url=https://git.libcamera.org/libcamera/libcamera.git
readonly install_prefix=/opt/tabpro-camera
readonly state_dir=/var/lib/tabpro-camera
readonly script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
readonly kernel_release=$(uname -r)
readonly kernel_build=/lib/modules/$kernel_release/build

mode=install
case ${1:-} in
    "") ;;
    --check) mode=check ;;
    --build-only) mode=build ;;
    *)
        echo "Usage: $0 [--check|--build-only]" >&2
        exit 2
        ;;
esac

fail() {
    echo "Error: $*" >&2
    exit 1
}

note() {
    echo "==> $*"
}

validate_hardware() {
    local pci_device
    local ipu_found=false
    local sensor_count=0

    [[ -r /sys/class/dmi/id/sys_vendor ]] ||
        fail "DMI system vendor is unavailable."
    [[ -r /sys/class/dmi/id/product_name ]] ||
        fail "DMI product name is unavailable."

    [[ $(< /sys/class/dmi/id/sys_vendor) == "SAMSUNG ELECTRONICS CO., LTD." ]] ||
        fail "This is not a Samsung system."
    [[ $(< /sys/class/dmi/id/product_name) == "Galaxy TabPro S"* ]] ||
        fail "DMI does not identify a Galaxy TabPro S."

    for pci_device in /sys/bus/pci/devices/*; do
        if [[ -r $pci_device/vendor && -r $pci_device/device &&
              -r $pci_device/subsystem_vendor &&
              -r $pci_device/subsystem_device ]] &&
           [[ $(< "$pci_device/vendor") == 0x8086 ]] &&
           [[ $(< "$pci_device/device") == 0x9d32 ]] &&
           [[ $(< "$pci_device/subsystem_vendor") == 0x144d ]] &&
           [[ $(< "$pci_device/subsystem_device") == 0xc135 ]]; then
            ipu_found=true
            break
        fi
    done
    [[ $ipu_found == true ]] ||
        fail "Expected IPU3 CIO2 controller 8086:9d32/144d:c135 was not found."

    for sensor in /sys/bus/acpi/devices/INT347F:*; do
        [[ -e $sensor ]] && ((sensor_count += 1))
    done
    ((sensor_count == 2)) ||
        fail "Expected two ACPI INT347F camera devices, found ${sensor_count}."
    [[ -e /sys/bus/acpi/devices/INT3472:04 ]] ||
        fail "Expected TPS68470 ACPI device INT3472:04 was not found."

    note "Validated Galaxy TabPro S camera hardware (${sensor_count} IMX241 modules)."
}

validate_kernel() {
    local config=$kernel_build/.config
    local major minor

    [[ -d $kernel_build ]] ||
        fail "Headers for ${kernel_release} are missing at ${kernel_build}."

    IFS=. read -r major minor _ <<<"${kernel_release%%-*}"
    [[ $major =~ ^[0-9]+$ && $minor =~ ^[0-9]+$ ]] ||
        fail "Cannot parse kernel version ${kernel_release}."
    if ((major < 6 || (major == 6 && minor < 12) ||
         major > 7 || (major == 7 && minor > 1))); then
        fail "Supported kernel range is 6.12 through 7.1; found ${kernel_release}."
    fi

    [[ -r $config ]] || fail "Kernel configuration is unavailable at ${config}."
    for setting in \
        CONFIG_VIDEO_IPU3_CIO2=m \
        CONFIG_VIDEO_IPU3_IMGU=m \
        CONFIG_IPU_BRIDGE=m \
        CONFIG_INTEL_SKL_INT3472=m; do
        grep -qx "$setting" "$config" ||
            fail "Kernel must provide ${setting}; built-in variants cannot be overridden safely."
    done
    for setting in CONFIG_MEDIA_CONTROLLER=y CONFIG_VIDEO_V4L2_SUBDEV_API=y; do
        grep -qx "$setting" "$config" || fail "Kernel must provide ${setting}."
    done

    note "Validated kernel ${kernel_release} and required modular IPU3 drivers."
}

validate_commands() {
    local command_name
    local missing=()

    for command_name in awk cc c++ dkms git grep install ldconfig make mktemp \
                        ninja pkg-config python3 readelf sed sort tar; do
        command -v "$command_name" >/dev/null || missing+=("$command_name")
    done
    ((${#missing[@]} == 0)) ||
        fail "Missing required commands: ${missing[*]}"

    if [[ -n ${MESON:-} ]]; then
        [[ -x $MESON ]] || fail "MESON does not point to an executable: ${MESON}"
    else
        command -v meson >/dev/null || fail "Meson is required (command: meson)."
    fi

    for dependency in libudev yaml-0.1 gnutls libevent_pthreads; do
        pkg-config --exists "$dependency" ||
            fail "Missing libcamera build dependency: ${dependency}"
    done
    python3 -c 'import jinja2, ply, yaml' 2>/dev/null ||
        fail "Python modules Jinja2, PLY, and PyYAML are required."
    if ! ldconfig -p | awk '
        /libcamera[.]so[.]0[.]7 / { found = 1 }
        END { exit !found }
    '; then
        fail "This release requires the distribution's libcamera ABI 0.7."
    fi
    note "Validated build commands and libcamera dependencies."
}

refresh_initramfs() {
    if command -v update-initramfs >/dev/null; then
        update-initramfs -u -k "$kernel_release"
    elif command -v dracut >/dev/null; then
        dracut --force --kver "$kernel_release"
    elif command -v mkinitcpio >/dev/null; then
        mkinitcpio -P
    else
        echo "Warning: no supported initramfs tool found; rebuild it manually." >&2
    fi
}

if [[ $mode != build ]]; then
    validate_hardware
fi
validate_kernel
validate_commands

if [[ $mode == check ]]; then
    note "Preflight passed. No files were changed."
    exit 0
fi

if [[ $mode == install && $EUID -ne 0 ]]; then
    fail "Run the installer as root, for example: sudo ./install.sh"
fi

build_dir=$(mktemp -d -t tabpro-camera-build.XXXXXXXX)
cleanup() {
    rm -r -- "$build_dir"
}
trap cleanup EXIT

readonly source_tree=$build_dir/libcamera
readonly libcamera_build=$build_dir/libcamera-build
readonly stage_dir=$build_dir/stage
readonly meson_command=${MESON:-meson}

note "Preparing libcamera ${libcamera_version}."
if [[ -n ${LIBCAMERA_SOURCE_DIR:-} ]]; then
    git clone --no-hardlinks "$LIBCAMERA_SOURCE_DIR" "$source_tree"
    git -C "$source_tree" checkout --detach "$libcamera_commit"
else
    git clone --depth 1 --branch "v${libcamera_version}" \
        "$libcamera_url" "$source_tree"
fi
[[ $(git -C "$source_tree" rev-parse HEAD) == "$libcamera_commit" ]] ||
    fail "Downloaded libcamera commit does not match the reviewed source."
git -C "$source_tree" apply --check \
    "$script_dir/libcamera/0001-tabpro-s-imx241-ipu3-support.patch"
git -C "$source_tree" apply \
    "$script_dir/libcamera/0001-tabpro-s-imx241-ipu3-support.patch"

"$meson_command" setup "$libcamera_build" "$source_tree" \
    --prefix="$install_prefix" \
    --libdir=lib \
    --buildtype=release \
    -Dwerror=false \
    -Dpipelines=ipu3 \
    -Dipas=ipu3 \
    -Dcam=enabled \
    -Dqcam=disabled \
    -Dgstreamer=disabled \
    -Dpycamera=disabled \
    -Ddocumentation=disabled \
    -Dlc-compliance=disabled \
    -Dtracing=disabled \
    -Dlibunwind=disabled \
    -Dlibdw=disabled \
    -Dcam-output-kms=disabled \
    -Dcam-output-sdl2=disabled \
    -Dcam-jpeg=disabled \
    -Dapps-output-dng=disabled \
    -Dsoftisp-gpu=disabled \
    -Dtest=false
ninja -C "$libcamera_build"
DESTDIR="$stage_dir" ninja -C "$libcamera_build" install

readonly staged_prefix=$stage_dir$install_prefix
[[ -f $staged_prefix/lib/libcamera.so.0.7.2 ]] ||
    fail "Staged libcamera library was not produced."
[[ -f $staged_prefix/lib/libcamera/ipa/ipa_ipu3.so ]] ||
    fail "Staged IPU3 IPA module was not produced."
[[ -f $staged_prefix/share/libcamera/ipa/ipu3/imx241.yaml ]] ||
    fail "Staged IMX241 tuning file was not produced."
printf 'package=%s\nversion=%s\nlibcamera_commit=%s\n' \
    "$package_name" "$package_version" "$libcamera_commit" \
    >"$staged_prefix/.tabpro-camera-managed"

note "Compiling the four camera modules before changing the system."
cp -a -- "$script_dir/dkms" "$build_dir/dkms-test"
"$build_dir/dkms-test/dkms-build.sh" "$kernel_release"

if [[ $mode == build ]]; then
    note "Build-only validation passed. No system files were changed."
    exit 0
fi

if [[ -e $install_prefix &&
      ! -f $install_prefix/.tabpro-camera-managed ]]; then
    fail "${install_prefix} exists but is not managed by this installer."
fi

note "Installing DKMS package ${package_name}/${package_version}."
while IFS= read -r old_version; do
    [[ -n $old_version ]] || continue
    dkms remove "$package_name/$old_version" --all
done < <(dkms status -m "$package_name" 2>/dev/null |
         sed -n 's|^tabpro-camera/\([^,:]*\)[:,].*|\1|p' | sort -u)

readonly dkms_source=/usr/src/$package_name-$package_version
if [[ -e $dkms_source ]]; then
    rm -r -- "$dkms_source"
fi
install -d -m 0755 "$dkms_source"
cp -a -- "$script_dir/dkms/." "$dkms_source/"
dkms add "$package_name/$package_version"
dkms install "$package_name/$package_version" -k "$kernel_release" --force

note "Installing the isolated libcamera build under ${install_prefix}."
if [[ -d $install_prefix ]]; then
    rm -r -- "$install_prefix"
fi
install -d -m 0755 "$(dirname -- "$install_prefix")"
cp -a -- "$staged_prefix" "$install_prefix"
install -Dm0644 "$script_dir/tabpro-camera.ld.so.conf" \
    /etc/ld.so.conf.d/00-tabpro-camera.conf
install -Dm0755 "$script_dir/status.sh" /usr/local/sbin/tabpro-camera-status
install -Dm0755 "$script_dir/uninstall.sh" /usr/local/sbin/tabpro-camera-uninstall
install -d -m 0755 "$state_dir"
printf 'PACKAGE_VERSION=%q\nKERNEL_RELEASE=%q\n' \
    "$package_version" "$kernel_release" >"$state_dir/install.env"

ldconfig
refresh_initramfs

first_library=$(ldconfig -p | awk '
    /libcamera[.]so[.]0[.]7 / && !found { print $NF; found = 1 }
')
[[ $first_library == "$install_prefix/lib/libcamera.so.0.7" ]] ||
    fail "The custom libcamera library is not first in the dynamic-loader cache."

note "Installation completed. Reboot, then run: tabpro-camera-status --capture-test"
