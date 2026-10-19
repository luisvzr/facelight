import QtQuick
import Quickshell
import Quickshell.Io
import qs.Ui

// facelight indicator for the Omarchy bar: lit while the ring light is on,
// click to toggle it by hand (starts the daemon if it is not running).
BarIndicator {
  id: root

  property string state: "stopped"
  readonly property string bin: "f=$(command -v facelight || echo \"$HOME/.local/bin/facelight\"); "

  active: state === "on"
  activeText: "󰛨"
  inactiveText: "󰛨"
  activeTooltipText: "Facelight on"
  inactiveTooltipText: state === "stopped" ? "Facelight (daemon stopped)" : "Facelight"

  function update(raw) {
    var data = extractData(raw)
    state = String(data.alt || "stopped")
  }

  Process {
    id: follow
    command: ["sh", "-c", root.bin + "exec \"$f\" status --follow"]
    running: true
    stdout: SplitParser {
      onRead: function(data) { root.update(data) }
    }
    onExited: restart.start()
  }

  Timer {
    id: restart
    interval: 5000
    onTriggered: follow.running = true
  }

  onPressed: function() {
    Quickshell.execDetached(["sh", "-c", root.bin +
      "\"$f\" toggle 2>/dev/null || { systemctl --user start facelight.service || (\"$f\" &); sleep 0.5; \"$f\" toggle; }"])
  }
}
