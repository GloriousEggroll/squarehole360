#!/usr/bin/env python3
"""Integration check against a running diagnostic game and its separate config."""
import json
import sys
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'settings'))
from app import QApplication, SettingsWindow
from model import running_states
app = QApplication([])
path = Path(sys.argv[1]).resolve()
assert running_states(path), 'Launch the game with this diagnostic config first'
window = SettingsWindow(path)
window.show()
window.widgets['resolution_scale'].setCurrentIndex(2)
window.widgets['gow3_frame_limit_mode'].setCurrentIndex(0)
window.widgets['framerate_limit'].setValue(72.0)
assert window.save_settings()
end = time.monotonic() + 15
while time.monotonic() < end:
    app.processEvents()
    if not window.awaiting_apply and window.hdr_result is not None:
        break
    time.sleep(.05)
assert not window.awaiting_apply, 'Game did not acknowledge the applied config'
assert 'Game restart needed' in window.restart_notice.text(), window.restart_notice.text()
assert 'Internal resolution' in window.restart_notice.text(), window.restart_notice.text()
print(json.dumps({'status': window.status.text(), 'restart': window.restart_notice.text(),
                  'hdr_available': window.widgets['present_hdr'].isEnabled(),
                  'hdr_probe': window.hdr_result}, indent=2))
window.grab().save(str(path.parent / 'settings-live-verified.png'))
window.saved = window.read_values()  # No prompt on the test-only editor.
window.close()
