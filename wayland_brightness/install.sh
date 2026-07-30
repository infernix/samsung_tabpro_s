#!/bin/bash
# Install the Samsung TabPro S OLED brightness fix on the local tablet.
#
# - Copies tabpro-dim.py to ~/.local/bin/ (user session) and /usr/local/bin/ (greeter)
# - Installs the user systemd service tabpro-dim.service (enabled for graphical-session.target)
# - Installs a GDM greeter autostart so brightness works at the login screen
# - (Optional) sets the AccountsService default session to GNOME, since mutter's
#   SetCrtcGamma is the only working path on this OLED panel
#
# Run on the tablet itself (or via SSH to the tablet). Requires sudo.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

if [ ! -d /sys/class/backlight/intel_backlight ]; then
    echo "error: /sys/class/backlight/intel_backlight not found — not a TabPro S?" >&2
    exit 1
fi

if ! command -v python3 >/dev/null; then
    echo "error: python3 not found" >&2
    exit 1
fi

# python3-dbus is required
if ! python3 -c "import dbus" 2>/dev/null; then
    echo "installing python3-dbus..."
    sudo dnf install -y python3-dbus
fi

echo "==> installing user daemon to ~/.local/bin/tabpro-dim.py"
mkdir -p "$HOME/.local/bin"
install -m 0755 "$HERE/tabpro-dim.py" "$HOME/.local/bin/tabpro-dim.py"

echo "==> installing greeter daemon to /usr/local/bin/tabpro-dim.py"
sudo install -m 0755 "$HERE/tabpro-dim.py" /usr/local/bin/tabpro-dim.py

echo "==> installing user systemd service"
mkdir -p "$HOME/.config/systemd/user"
install -m 0644 "$HERE/tabpro-dim.service" "$HOME/.config/systemd/user/tabpro-dim.service"
systemctl --user daemon-reload
systemctl --user enable --now tabpro-dim.service

echo "==> installing GDM greeter autostart"
sudo mkdir -p /usr/share/gdm/autostart/LoginWindow
sudo install -m 0644 "$HERE/tabpro-dim-greeter.desktop" \
    /usr/share/gdm/autostart/LoginWindow/tabpro-dim.desktop

echo "==> done."
echo
echo "The daemon is now active in your GNOME session and at the GDM greeter."
echo "Test: press the brightness keys (or move the GNOME slider) — the OLED panel"
echo "should dim. The daemon logs to the user journal: journalctl --user -u tabpro-dim -f"
echo
echo "NOTE: this only works under GNOME/Wayland (mutter). KWin does not expose"
echo "SetCrtcGamma; for KDE see README.md."
echo
read -r -p "Set AccountsService default session to GNOME? [y/N] " ans
if [[ "$ans" =~ ^[Yy]$ ]]; then
    sudo sed -i 's/^Session=.*/Session=gnome/' /var/lib/AccountsService/users/"$USER" 2>/dev/null \
        || echo "warning: could not edit AccountsService (set Session=gnome manually)" >&2
    echo "Default session set to GNOME. Log out and back in to take effect."
fi