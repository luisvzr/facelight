#!/bin/bash
# Adds a Facelight toggle to the Omarchy bar's indicators (the hover strip next
# to the clock, with Reminder, Night light, Do not disturb...).
#
# Omarchy only loads indicators from the indicators plugin's own folder, so this
# clones omarchy.indicators into ~/.config/omarchy/plugins/<user>.indicators
# (once) and drops Facelight.qml into it. The shell hot-reloads the change.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
plugins="$HOME/.config/omarchy/plugins"

command -v omarchy >/dev/null || { echo "Omarchy not found; this step is only for the Omarchy bar." >&2; exit 1; }

dir=$(find "$plugins" -maxdepth 1 -type d -name '*.indicators' 2>/dev/null | head -1)
if [[ -z $dir ]]; then
  omarchy plugin clone omarchy.indicators
  dir=$(find "$plugins" -maxdepth 1 -type d -name '*.indicators' | head -1)
fi
[[ -n $dir ]] || { echo "could not find the cloned indicators plugin" >&2; exit 1; }

install -m644 "$here/Facelight.qml" "$dir/indicators/Facelight.qml"

# Show it by default (when the widget has no explicit "items" list).
if ! grep -q '"Facelight"' "$dir/Indicators.qml"; then
  sed -i 's/\(readonly property var defaultIndicatorEntries: \[.*\)\( \]\)/\1, "Facelight"\2/' "$dir/Indicators.qml"
fi

# And offer it in the widget's settings picker.
manifest="$dir/manifest.json"
if ! grep -q '"Facelight"' "$manifest"; then
  jq '(.barWidget.schema[] | select(.key == "items") | .options) +=
      [{"value": "Facelight", "label": "Facelight", "description": "Screen ring light"}]' \
    "$manifest" >"$manifest.tmp" && mv "$manifest.tmp" "$manifest"
fi

echo "Facelight added to $dir. Hover the indicators next to the clock to see it."
echo "If you set an explicit \"items\" list for the indicators widget in shell.json, add \"Facelight\" to it."
