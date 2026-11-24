#!/usr/bin/env bash
# Mangowm-Dotfiles installer
# Works two ways:
#   1) curl -fsSL https://raw.githubusercontent.com/code-smith404/Mangowm-Dotfiles/main/install.sh | bash
#      (downloads the repo itself, then installs everything)
#   2) ./install.sh   (from inside a cloned copy of this repo)
set -euo pipefail

REPO_URL="https://github.com/code-smith404/Mangowm-Dotfiles.git"
CFG="${XDG_CONFIG_HOME:-$HOME/.config}"

# If the script is not running from a checkout, download the repo to a temp dir.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
if [ ! -f "$HERE/mango/config.conf" ]; then
    TMP="$(mktemp -d)"
    trap 'rm -rf "$TMP"' EXIT
    echo "==> Downloading dotfiles..."
    git clone --depth 1 "$REPO_URL" "$TMP/Mangowm-Dotfiles"
    HERE="$TMP/Mangowm-Dotfiles"
fi

echo "==> Installing configs to $CFG"
mkdir -p "$CFG/mango" "$CFG/mangobar" "$CFG/foot" "$CFG/rofi" "$CFG/mango-settings"
install -m 644 "$HERE/mango/config.conf"  "$CFG/mango/"
install -m 644 "$HERE/mango/mangowall.c"  "$CFG/mango/"
install -m 644 "$HERE/mango/swaylock.pam" "$CFG/mango/"
install -m 644 "$HERE/mangobar/config.jsonc" "$HERE/mangobar/style.css" "$CFG/mangobar/"
install -m 644 "$HERE/foot/foot.ini"      "$CFG/foot/"
install -m 644 "$HERE/rofi/config.rasi"   "$CFG/rofi/"
install -m 644 "$HERE/mango-settings/settings.ini" "$CFG/mango-settings/"

echo "==> Installing wallpapers to ~/Pictures/Wallpapers"
mkdir -p "$HOME/Pictures/Wallpapers"
install -m 644 "$HERE/wallpapers/dusk-aurora.png"      "$HOME/Pictures/Wallpapers/"
install -m 644 "$HERE/wallpapers/dusk-aurora-lock.png" "$HOME/Pictures/Wallpapers/"

# mangowall (minimal swaybg replacement). Build it if toolchain is present.
if command -v cc >/dev/null 2>&1; then
    mkdir -p "$HOME/.local/bin"
    if cc "$HERE/mango/mangowall.c" \
          "$HERE/protocols/wlr-layer-shell-unstable-v1-protocol.c" \
          "$HERE/protocols/xdg-shell-protocol.c" \
          -I "$HERE/protocols" -o "$HOME/.local/bin/mangowall" \
          $(pkg-config --cflags --libs libpng wayland-client) 2>/dev/null; then
        echo "==> Built mangowall -> ~/.local/bin/mangowall"
    else
        echo "==> Skipped building mangowall (needs: pkg-config, libpng, wayland-client)."
        echo "    Build manually with:"
        echo "    cc mango/mangowall.c protocols/wlr-layer-shell-unstable-v1-protocol.c protocols/xdg-shell-protocol.c -I protocols \\"
        echo "       -o ~/.local/bin/mangowall \$(pkg-config --cflags --libs libpng wayland-client)"
    fi
fi

# swaylock PAM profile (root required)
if [ "$(id -u)" -eq 0 ]; then
    install -Dm 644 "$CFG/mango/swaylock.pam" /etc/pam.d/swaylock
    echo "==> Installed /etc/pam.d/swaylock"
else
    echo "==> Last step (needs root): make the lock screen work"
    echo "    sudo install -Dm644 $CFG/mango/swaylock.pam /etc/pam.d/swaylock"
fi

echo
echo "Done. Restart mangowm (or reboot into it) to apply."
