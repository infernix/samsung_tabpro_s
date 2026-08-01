#!/usr/bin/env bash
set -euo pipefail

readonly package_name=tabpro-camera
readonly package_version=0.6.0
readonly install_prefix=/opt/tabpro-camera
readonly dkms_source=/usr/src/$package_name-$package_version
readonly kernel_release=$(uname -r)

if ((EUID != 0)); then
    echo "Run this uninstaller as root, for example: sudo ./uninstall.sh" >&2
    exit 1
fi

refresh_initramfs() {
    if command -v update-initramfs >/dev/null; then
        update-initramfs -u -k "$kernel_release"
    elif command -v dracut >/dev/null; then
        dracut --force --kver "$kernel_release"
    elif command -v mkinitcpio >/dev/null; then
        mkinitcpio -P
    else
        echo "Warning: rebuild the initramfs manually before rebooting." >&2
    fi
}

dkms remove "$package_name/$package_version" --all 2>/dev/null || true

if [[ -e $install_prefix ]]; then
    if [[ ! -f $install_prefix/.tabpro-camera-managed ]]; then
        echo "Refusing to remove unmanaged path: ${install_prefix}" >&2
        exit 1
    fi
    rm -r -- "$install_prefix"
fi

if [[ -d $dkms_source ]]; then
    rm -r -- "$dkms_source"
fi
rm -f -- \
    /etc/ld.so.conf.d/00-tabpro-camera.conf \
    /usr/local/sbin/tabpro-camera-status \
    /usr/local/sbin/tabpro-camera-uninstall \
    /var/lib/tabpro-camera/install.env
rmdir -- /var/lib/tabpro-camera 2>/dev/null || true

ldconfig
refresh_initramfs

echo "Removed the Galaxy TabPro S camera workaround."
echo "Reboot to load the distribution's original camera modules and libcamera."
