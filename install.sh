#!/bin/bash
# facelight installer for Omarchy (works on any Arch + Hyprland box too).
#
#   curl -fsSL https://raw.githubusercontent.com/luisvzr/facelight/main/install.sh | bash
#
# Builds facelight, installs it to ~/.local/bin, enables the systemd user
# service and, on Omarchy, adds the toggle to the bar's indicators.
# Run it again any time to update. `install.sh --uninstall` removes everything
# except the cloned indicators plugin.
set -euo pipefail

repo="https://github.com/luisvzr/facelight"
src="${XDG_DATA_HOME:-$HOME/.local/share}/facelight"

say() { printf '\033[1;33m󰛨\033[0m %s\n' "$*"; }

if [[ ${1:-} == "--uninstall" ]]; then
  systemctl --user disable --now facelight.service 2>/dev/null || true
  rm -f "$HOME/.local/bin/facelight" "$HOME/.config/systemd/user/facelight.service"
  find "$HOME/.config/omarchy/plugins" -path '*.indicators/indicators/Facelight.qml' -delete 2>/dev/null || true
  systemctl --user daemon-reload
  say "facelight removed."
  exit 0
fi

# Use the checkout this script lives in, or clone/update one.
here=""
[[ -f ${BASH_SOURCE[0]:-} ]] && here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [[ -n $here && -f $here/facelight.c ]]; then
  src=$here
elif [[ -d $src/.git ]]; then
  say "Updating $src"
  git -C "$src" pull --ff-only --quiet
else
  say "Downloading facelight"
  git clone --quiet --depth 1 "$repo" "$src"
fi

missing=()
command -v cc >/dev/null || missing+=(gcc)
command -v make >/dev/null || missing+=(make)
command -v wayland-scanner >/dev/null || missing+=(wayland)
pkg-config --exists wayland-protocols 2>/dev/null || missing+=(wayland-protocols)
command -v jq >/dev/null || missing+=(jq)
if ((${#missing[@]})); then
  say "Installing build dependencies: ${missing[*]}"
  if command -v omarchy >/dev/null; then omarchy pkg add "${missing[@]}"; else sudo pacman -S --needed --noconfirm "${missing[@]}"; fi
fi

say "Building"
make -C "$src" --quiet install >/dev/null
make -C "$src" --quiet clean >/dev/null

say "Starting the service"
systemctl --user daemon-reload
systemctl --user enable --now facelight.service >/dev/null 2>&1
systemctl --user restart facelight.service

if command -v omarchy >/dev/null; then
  say "Adding the toggle to the Omarchy bar"
  "$src/omarchy/indicator.sh" >/dev/null
  omarchy restart shell >/dev/null 2>&1 || true
fi

cat <<'EOF'

  facelight is on. Turn your camera on and your screen lights your face.

  Toggle by hand:  hover the indicators next to the clock and click the bulb
                   or run  facelight toggle
  Update:          run this installer again
  Remove:          bash ~/.local/share/facelight/install.sh --uninstall

EOF
