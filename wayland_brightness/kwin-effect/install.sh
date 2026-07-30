#!/bin/bash
# Build and install the KWin brightness effect plugin for the Samsung TabPro S.
#
# Compiles tabprobright.so against the installed kwin-devel headers and installs
# it to /usr/lib64/qt6/plugins/kwin/effects/plugins/ . KWin auto-loads it on
# next start (EnabledByDefault: true). Log out and back in (or restart kwin) to
# activate.
#
# Run on the tablet. Requires: kwin-devel, qt6-qtbase-devel, extra-cmake-modules,
# cmake, gcc-c++, libdrm-devel, libepoxy-devel, kf6-kconfig-devel,
# kf6-kcoreaddons-devel, kf6-kwindowsystem-devel, qt6-qtdeclarative-devel,
# qt6-qtwayland-devel, plasma-wayland-protocols, vulkan-headers.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

if ! rpm -q kwin-devel >/dev/null 2>&1; then
    echo "==> installing build dependencies..."
    sudo dnf install -y \
        cmake gcc-c++ make \
        kwin-devel qt6-qtbase-devel extra-cmake-modules \
        libdrm-devel libepoxy-devel \
        kf6-kconfig-devel kf6-kcoreaddons-devel kf6-kwindowsystem-devel \
        qt6-qtdeclarative-devel qt6-qtwayland-devel \
        plasma-wayland-protocols vulkan-headers
fi

echo "==> configuring build..."
mkdir -p "$HERE/build"
cd "$HERE/build"
cmake .. -DCMAKE_BUILD_TYPE=Release

echo "==> building..."
make -j"$(nproc)"

echo "==> installing (requires sudo)..."
sudo make install

echo
echo "==> done. Installed to /usr/lib64/qt6/plugins/kwin/effects/plugins/tabprobright.so"
echo "Log out of KDE and back in (or: systemctl --user restart plasma-kwin_wayland.service)"
echo "to activate. Verify: gdbus call --session --dest org.kde.KWin -o /Effects \\"
echo "  -m org.kde.kwin.Effects.isEffectLoaded tabprobright   # should print (true,)"
echo
echo "Uninstall: sudo rm /usr/lib64/qt6/plugins/kwin/effects/plugins/tabprobright.so"