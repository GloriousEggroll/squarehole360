#!/usr/bin/env python3
"""Standalone graphics settings editor. No game process is needed."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from PySide6.QtCore import Qt, QProcess, QTimer, Signal
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDialog, QDialogButtonBox, QDoubleSpinBox, QLineEdit,
    QFormLayout, QHBoxLayout, QLabel, QMainWindow, QMessageBox, QPushButton,
    QScrollArea, QSpinBox, QTabWidget, QVBoxLayout, QWidget)
from model import CONFIG, ROOT, WINDOWS, binary, bind_keys, game_running, SETTINGS, LIVE_SETTINGS, BY_KEY, running_states, fingerprint, load, save, applies, listed, widget_value, MAPPING_SOURCES, MAPPING_TARGETS, MAPPING_DEFAULTS, parse_mapping, format_mapping


def _qt_key_names():
    names = {getattr(Qt.Key, f'Key_{c}'): c for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'}
    names.update({getattr(Qt.Key, f'Key_F{n}'): f'F{n}' for n in range(1, 25)})
    for qt, name in (('Escape', 'Escape'), ('Return', 'Return'), ('Enter', 'Return'), ('Space', 'Space'),
                     ('Tab', 'Tab'), ('Backtab', 'Tab'), ('Backspace', 'Backspace'), ('Delete', 'Delete'),
                     ('Insert', 'Insert'), ('Home', 'Home'), ('End', 'End'), ('PageUp', 'PageUp'),
                     ('PageDown', 'PageDown'), ('Left', 'Left'), ('Right', 'Right'), ('Up', 'Up'),
                     ('Down', 'Down'), ('Shift', 'Shift'), ('Control', 'Control'), ('Alt', 'Alt'),
                     ('Meta', 'Control') if sys.platform == 'darwin' else ('Meta', ''),
                     ('Print', 'PrintScreen'), ('Pause', 'Pause'), ('CapsLock', 'CapsLock'),
                     ('NumLock', 'NumLock'), ('ScrollLock', 'ScrollLock'), ('QuoteLeft', 'Backtick'),
                     ('AsciiTilde', 'Backtick'), ('Minus', 'Minus'), ('Underscore', 'Minus'),
                     ('Equal', 'Plus'), ('Plus', 'Plus'), ('Comma', 'Comma'), ('Less', 'Comma'),
                     ('Period', 'Period'), ('Greater', 'Period'), ('Semicolon', 'Semicolon'),
                     ('Colon', 'Semicolon'), ('Slash', 'Slash'), ('Question', 'Slash'),
                     ('Backslash', 'Backslash'), ('Bar', 'Backslash'), ('BracketLeft', 'LBracket'),
                     ('BraceLeft', 'LBracket'), ('BracketRight', 'RBracket'), ('BraceRight', 'RBracket'),
                     ('Apostrophe', 'Quote'), ('QuoteDbl', 'Quote'), ('Exclam', '1'), ('At', '2'),
                     ('NumberSign', '3'), ('Dollar', '4'), ('Percent', '5'), ('AsciiCircum', '6'),
                     ('Ampersand', '7'), ('Asterisk', '8'), ('ParenLeft', '9'), ('ParenRight', '0')):
        if name:
            names[getattr(Qt.Key, f'Key_{qt}')] = name
    return names


QT_KEY_NAMES = _qt_key_names()
QT_KEYPAD_NAMES = {Qt.Key.Key_Enter: 'NumpadEnter', Qt.Key.Key_Plus: 'NumpadPlus',
                   Qt.Key.Key_Minus: 'NumpadMinus', Qt.Key.Key_Asterisk: 'NumpadStar',
                   Qt.Key.Key_Slash: 'NumpadSlash',
                   **{getattr(Qt.Key, f'Key_{n}'): f'Numpad{n}' for n in range(10)}}
QT_BUTTON_NAMES = {Qt.MouseButton.LeftButton: 'LMB', Qt.MouseButton.RightButton: 'RMB',
                   Qt.MouseButton.MiddleButton: 'MMB', Qt.MouseButton.BackButton: 'Mouse4',
                   Qt.MouseButton.ForwardButton: 'Mouse5'}


class KeyCaptureDialog(QDialog):
    """Waits for one key, mouse button or wheel step over its capture area."""
    def __init__(self, parent, title):
        super().__init__(parent)
        self.setWindowTitle(title)
        self.key = None
        layout = QVBoxLayout(self)
        self.area = QLabel('Press a key, click a mouse button here or turn the wheel.')
        self.area.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.area.setMinimumSize(360, 140)
        self.area.setStyleSheet('border: 1px solid palette(mid); padding: 12px;')
        layout.addWidget(self.area)
        cancel = QPushButton('Cancel')
        cancel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        cancel.setAutoDefault(False)
        cancel.clicked.connect(self.reject)
        layout.addWidget(cancel, 0, Qt.AlignmentFlag.AlignRight)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def finish(self, key):
        if key:
            self.key = key
            self.accept()

    def event(self, event):
        # Tab, Enter and Escape would otherwise move focus or close the dialog.
        if event.type() == event.Type.KeyPress:
            self.keyPressEvent(event)
            return True
        return super().event(event)

    def keyPressEvent(self, event):
        key = Qt.Key(event.key())
        keypad = event.modifiers() & Qt.KeyboardModifier.KeypadModifier
        self.finish(QT_KEYPAD_NAMES.get(key) if keypad and key in QT_KEYPAD_NAMES else QT_KEY_NAMES.get(key))

    def mousePressEvent(self, event):
        if self.area.geometry().contains(event.position().toPoint()):
            self.finish(QT_BUTTON_NAMES.get(event.button()))

    def wheelEvent(self, event):
        if event.angleDelta().y():
            self.finish('WheelUp' if event.angleDelta().y() > 0 else 'WheelDown')


class ButtonMappingDialog(QDialog):
    """One dropdown per physical controller input: the game button it presses."""

    def __init__(self, parent, title, text):
        super().__init__(parent)
        self.setWindowTitle(title)
        mapping = parse_mapping(text)
        form = QFormLayout()
        self.boxes = {}
        for source, label in MAPPING_SOURCES:
            box = QComboBox()
            for target, target_label in MAPPING_TARGETS:
                box.addItem(target_label + (' (default)' if target == MAPPING_DEFAULTS[source] else ''), target)
            box.setCurrentIndex(box.findData(mapping[source]))
            form.addRow(label, box)
            self.boxes[source] = box
        note = QLabel('Face buttons are by position: A is the bottom button (Cross on PlayStation). '
                      '“Hold for gyro” works with Gyro on set to “While a gyro button is held”.')
        note.setWordWrap(True)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel |
                                   QDialogButtonBox.StandardButton.RestoreDefaults)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        buttons.button(QDialogButtonBox.StandardButton.RestoreDefaults).clicked.connect(self.restore_defaults)
        inner = QWidget()
        inner.setLayout(form)
        scroll = QScrollArea()
        scroll.setWidget(inner)
        scroll.setWidgetResizable(True)
        layout = QVBoxLayout(self)
        layout.addWidget(note)
        layout.addWidget(scroll, 1)
        layout.addWidget(buttons)
        self.resize(520, 640)

    def restore_defaults(self):
        for source, box in self.boxes.items():
            box.setCurrentIndex(box.findData(MAPPING_DEFAULTS[source]))

    def text(self):
        return format_mapping({source: box.currentData() for source, box in self.boxes.items()})


class ButtonMappingEdit(QWidget):
    """A player's button mapping: a summary of the changes and an editor."""
    textChanged = Signal(str)

    def __init__(self, label):
        super().__init__()
        self.label = label
        self.value = ''
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.summary = QLabel()
        self.summary.setWordWrap(True)
        layout.addWidget(self.summary, 1)
        edit = QPushButton('Edit…')
        edit.clicked.connect(self.edit)
        layout.addWidget(edit)
        reset = QPushButton('Reset')
        reset.clicked.connect(lambda: self.setText(''))
        layout.addWidget(reset)
        self.setText('')

    def edit(self):
        dialog = ButtonMappingDialog(self, self.label, self.value)
        if dialog.exec():
            self.setText(dialog.text())

    def text(self):
        return self.value

    def setText(self, value):
        try:
            value = format_mapping(parse_mapping(value))
        except ValueError:
            pass  # Shown as written; saving reports the invalid entry.
        sources, targets = dict(MAPPING_SOURCES), dict(MAPPING_TARGETS)
        changes = [f'{sources.get(s, s)} → {targets.get(t, t)}' for s, _, t in
                   (entry.partition('=') for entry in value.split(',') if entry)]
        self.summary.setText('; '.join(changes) if changes else 'Default buttons')
        if value != self.value:
            self.value = value
            if not self.signalsBlocked():
                self.textChanged.emit(value)


class KeyBindEdit(QWidget):
    """A binding's keys as text, with buttons that capture a key to set or add."""
    textChanged = Signal(str)

    def __init__(self, label):
        super().__init__()
        self.label = label
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.edit = QLineEdit()
        self.edit.setPlaceholderText('Not bound')
        self.edit.textChanged.connect(self.textChanged)
        layout.addWidget(self.edit, 1)
        for text, tip, append in (('Set…', 'Replace with the next key you press', False),
                                  ('Add…', 'Add another key for this action', True)):
            button = QPushButton(text)
            button.setToolTip(tip)
            button.clicked.connect(lambda _=False, append=append: self.capture(append))
            layout.addWidget(button)
        clear = QPushButton('Clear')
        clear.clicked.connect(lambda: self.edit.setText(''))
        layout.addWidget(clear)

    def capture(self, append):
        dialog = KeyCaptureDialog(self, f'{self.label}: press a key')
        if dialog.exec() and dialog.key:
            keys = bind_keys(self.edit.text()) if append else []
            if dialog.key not in keys:
                keys.append(dialog.key)
            self.edit.setText(','.join(keys))

    def text(self):
        return self.edit.text()

    def setText(self, value):
        self.edit.setText(value)

    def blockSignals(self, block):
        self.edit.blockSignals(block)
        return super().blockSignals(block)


class SettingsWindow(QMainWindow):
    def __init__(self, path=CONFIG, hdr_probe=True):
        super().__init__()
        self.path = path
        self.values = load(path)
        self.digest = fingerprint(path)
        self.widgets = {}
        self.rows = {}
        self.section_pages = {}
        self.loaded_widget_values = {}
        self.hdr_result = None
        self.hdr_process = None
        self.hdr_status = QLabel('Checking HDR support…')
        self.hdr_status.setWordWrap(True)
        self.setWindowTitle('Gears of War 3 — Settings')
        self.resize(820, 880)
        root = QWidget()
        layout = QVBoxLayout(root)
        title = QLabel('Gears of War 3')
        title.setStyleSheet('font-size: 24px; font-weight: bold; padding: 6px 0;')
        layout.addWidget(title)
        layout.addWidget(QLabel('Apply changes while playing • Some options require a game restart'))
        shortcut_note = QLabel('Settings are also available in-game by pressing both stick buttons (L3+R3), or F4 on keyboard.')
        shortcut_note.setWordWrap(True)
        layout.addWidget(shortcut_note)
        tabs = self.tabs = QTabWidget()
        layout.addWidget(tabs, 1)
        for section in dict.fromkeys(s.section for s in SETTINGS):
            pane = QWidget()
            form = QFormLayout(pane)
            form.setContentsMargins(20, 20, 20, 20)
            form.setVerticalSpacing(16)
            form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
            for setting in (s for s in SETTINGS if s.section == section):
                if setting.choices:
                    widget = QComboBox()
                    for value, label in setting.choices:
                        widget.addItem(label, value)
                    widget.currentIndexChanged.connect(self.changed)
                elif isinstance(setting.default, bool):
                    widget = QCheckBox()
                    widget.toggled.connect(self.changed)
                elif setting.key.startswith('keybind_'):
                    widget = KeyBindEdit(setting.label)
                    widget.textChanged.connect(self.changed)
                elif setting.key.endswith('_buttons') and setting.key.startswith('controller'):
                    widget = ButtonMappingEdit(f'{setting.section}: button mapping')
                    widget.textChanged.connect(self.changed)
                elif isinstance(setting.default, str):
                    widget = QLineEdit()
                    widget.textChanged.connect(self.changed)
                else:
                    widget = QDoubleSpinBox() if isinstance(setting.default, float) else QSpinBox()
                    widget.setRange(setting.minimum, setting.maximum)
                    if isinstance(widget, QDoubleSpinBox):
                        widget.setDecimals(2)
                        widget.setSingleStep(0.1 if setting.maximum <= 2 else 1.0)
                    widget.valueChanged.connect(self.changed)
                widget.setObjectName(setting.key)
                widget.setToolTip(setting.help + ('\nApplies while the game is running.' if setting.key in LIVE_SETTINGS else '\nRequires a game restart.'))
                self.widgets[setting.key] = widget
                label = QLabel(setting.label)
                label.setToolTip(setting.help)
                form.addRow(label, widget)
                self.rows[setting.key] = (form, label, widget)
            if section == 'Display':
                note = QLabel('60 FPS is the default maximum. Unlimited removes the host limit; the game\n'
                              'may still have its own limits. VSync can also constrain presentation.\n'
                              'Fullscreen uses your desktop resolution. Monitor 0 uses the default.')
                note.setWordWrap(True)
                form.addRow(note)
                displays = '\n'.join(f'{i}: {s.name()} — {s.refreshRate():.2f} Hz'
                                      for i, s in enumerate(QApplication.screens(), 1))
                form.addRow(QLabel('Connected displays (desktop order):\n' + displays))
            if section == 'Image':
                self.image_note = QLabel()
                self.image_note.setWordWrap(True)
                form.addRow(self.image_note)
            if section == 'HDR':
                form.addRow(self.hdr_status)
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setWidget(pane)
            tabs.addTab(scroll, section)
            self.section_pages[section] = scroll
        self.restart_notice = QLabel()
        self.restart_notice.setWordWrap(True)
        self.restart_notice.setStyleSheet('color: #efb366; font-weight: bold;')
        layout.addWidget(self.restart_notice)
        self.status = QLabel(str(path))
        self.status.setWordWrap(True)
        self.status.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        layout.addWidget(self.status)
        buttons = QHBoxLayout()
        for label, callback in [('Restore defaults', self.defaults), ('Reload', self.reload)]:
            button = QPushButton(label)
            button.clicked.connect(callback)
            buttons.addWidget(button)
        buttons.addStretch()
        for label, callback in [('Apply', self.save_settings), ('Apply and launch', self.launch)]:
            button = QPushButton(label)
            button.clicked.connect(callback)
            buttons.addWidget(button)
        layout.addLayout(buttons)
        self.setCentralWidget(root)
        self.populate()
        self.live_timer = QTimer(self)
        self.live_timer.timeout.connect(self.refresh_runtime_status)
        self.live_timer.start(500)
        self.awaiting_apply = False
        self.hdr_timer = QTimer(self)
        self.hdr_timer.timeout.connect(self.check_hdr)
        if hdr_probe:
            QTimer.singleShot(0, self.check_hdr)
            self.hdr_timer.start(10000)

    def check_hdr(self):
        if self.hdr_process is not None:
            return
        probe = binary('gow3-hdr-probe', ROOT)
        if not probe.is_file():
            self.update_hdr_status({'available': False, 'reason': 'HDR detection tool is unavailable.'})
            return
        process = QProcess(self)
        self.hdr_process = process
        timer = QTimer(process)
        timer.setSingleShot(True)
        timer.timeout.connect(process.kill)
        timer.start(4000)
        def finished(*_):
            if self.hdr_process is not process:
                return
            timer.stop()
            try:
                result = json.loads(bytes(process.readAllStandardOutput()).decode())
                if process.exitCode() != 0 or not isinstance(result, dict):
                    raise ValueError()
            except (ValueError, UnicodeDecodeError):
                result = {'available': False, 'reason': 'HDR detection failed; using SDR.'}
            self.hdr_process = None
            process.deleteLater()
            self.update_hdr_status(result)
        process.finished.connect(finished)
        process.errorOccurred.connect(lambda error: finished() if error == QProcess.ProcessError.FailedToStart else None)
        process.start(str(probe))

    def update_hdr_status(self, result):
        self.hdr_result = result
        self.dependencies()

    def hdr_available(self):
        if not self.hdr_result or not self.hdr_result.get('available'):
            return False
        monitor = self.widgets['monitor'].value()
        if monitor == 0:
            return any(d.get('hdr') for d in self.hdr_result.get('displays', []))
        screens = QApplication.screens()
        if monitor > len(screens):
            return False
        name = screens[monitor - 1].name()
        return any(d.get('name') == name and d.get('hdr') for d in self.hdr_result.get('displays', []))

    def populate(self):
        for setting in SETTINGS:
            w = self.widgets[setting.key]
            w.blockSignals(True)
            value = widget_value(setting, self.values)
            self.loaded_widget_values[setting.key] = value
            if setting.choices:
                w.setCurrentIndex(w.findData(value))
            elif isinstance(setting.default, bool):
                w.setChecked(value)
            elif isinstance(setting.default, str):
                w.setText(value)
            else:
                w.setValue(value)
            w.blockSignals(False)
        self.saved = self.read_values()
        self.dependencies()

    def read_values(self):
        values = dict(self.values)
        mode = self.widgets['gow3_renderer'].currentData()
        for s in SETTINGS:
            w = self.widgets[s.key]
            value = w.currentData() if s.choices else (
                w.isChecked() if isinstance(s.default, bool) else
                w.text() if isinstance(s.default, str) else w.value())
            if applies(s, mode) or value != self.loaded_widget_values.get(s.key):
                values[s.key] = value
        return values

    def dependencies(self):
        mode = self.widgets['gow3_renderer'].currentData()
        if mode is None:
            return  # Initial population has not selected the renderer yet.
        self.image_note.setText('The game image is scaled to the window using bilinear filtering.'
                                if mode == 'native' else
                                'FSR 1 spatial upscaling and CAS sharpening are available.')
        active_sections = set()
        hidden_focus = False
        focus = QApplication.focusWidget()
        for setting in SETTINGS:
            visible = applies(setting, mode) and listed(setting)
            form, label, widget = self.rows[setting.key]
            hidden_focus |= not visible and (focus is widget or widget.isAncestorOf(focus) if focus else False)
            form.setRowVisible(widget, visible)
            if visible:
                active_sections.add(setting.section)
        for section, pane in self.section_pages.items():
            self.tabs.setTabVisible(self.tabs.indexOf(pane), section in active_sections)
        if not self.tabs.isTabVisible(self.tabs.currentIndex()):
            self.tabs.setCurrentIndex(next(i for i in range(self.tabs.count()) if self.tabs.isTabVisible(i)))
        if hidden_focus:
            self.tabs.currentWidget().setFocus()
        self.widgets['framerate_limit'].setEnabled(
            self.widgets['gow3_frame_limit_mode'].currentData() == 'custom')
        effect = self.widgets['present_effect'].currentData()
        temporal = effect in ('fsr3', 'dlss', 'xess')
        self.widgets['present_fsr_sharpness_reduction'].setEnabled(effect == 'fsr' or temporal)
        self.widgets['present_fsr_max_upsampling_passes'].setEnabled(effect == 'fsr')
        self.widgets['present_frame_generation'].setEnabled(temporal)
        self.widgets['present_fsr3_ghosting_reduction'].setEnabled(effect == 'fsr3')
        online = self.widgets['network_enabled'].isChecked()
        lobby = online and bool(self.widgets['network_lobby_server'].text().strip())
        for key in ('network_lobby_server', 'network_frame_rate_lock', 'network_address',
                    'network_broadcast_peers'):
            self.widgets[key].setEnabled(online)
        for key in ('network_lobby_room', 'network_lobby_password'):
            self.widgets[key].setEnabled(lobby)
        self.widgets['present_cas_additional_sharpness'].setEnabled(effect == 'cas')
        for key in ('window_width', 'window_height'):
            self.widgets[key].setEnabled(not self.widgets['fullscreen'].currentData())
        self.widgets['vulkan_batch_occlusion_queries'].setEnabled(
            self.widgets['occlusion_query_enable'].isChecked() and
            self.widgets['vulkan_async_occlusion_queries'].isChecked())
        self.widgets['readback_memexport_fast'].setEnabled(
            self.widgets['readback_memexport'].isChecked())
        self.widgets['vulkan_async_shader_translation'].setEnabled(
            self.widgets['async_shader_compilation'].isChecked())
        self.widgets['gow3_native_shader_prewarm_threads'].setEnabled(
            self.widgets['gow3_native_shader_prewarm'].isChecked() or
            self.widgets['gow3_native_pipeline_prewarm'].isChecked())
        hdr = self.hdr_available()
        self.widgets['present_hdr'].setEnabled(hdr)
        if self.hdr_result is not None and not hdr:
            self.widgets['present_hdr'].blockSignals(True)
            self.widgets['present_hdr'].setChecked(False)
            self.widgets['present_hdr'].blockSignals(False)
        for key in ('present_hdr_paper_white', 'present_hdr_peak_nits', 'present_hdr_highlights'):
            self.widgets[key].setEnabled(hdr and self.widgets['present_hdr'].isChecked())
        if self.hdr_result is not None:
            names = ', '.join(d['name'] for d in self.hdr_result.get('displays', []) if d.get('hdr'))
            self.hdr_status.setText('HDR available: ' + names if hdr else
                'HDR unavailable on the selected display. ' + self.hdr_result.get('reason', '') +
                ' Enable HDR in desktop settings, or choose an HDR display.')

    def changed(self, *_):
        self.dependencies()
        self.status.setText('Unsaved changes • Click Apply')
        self.refresh_runtime_status()

    def defaults(self):
        # Retain unknown expert settings, and leave changes unsaved for review.
        saved = self.saved
        self.values.update({s.key: s.default for s in SETTINGS})
        self.populate()
        self.saved = saved
        self.changed()

    def confirm_discard(self):
        return self.read_values() == self.saved or QMessageBox.question(
            self, 'Unsaved settings', 'Discard unsaved changes?',
            QMessageBox.StandardButton.Discard | QMessageBox.StandardButton.Cancel,
            QMessageBox.StandardButton.Cancel) == QMessageBox.StandardButton.Discard

    def reload(self):
        if not self.confirm_discard():
            return
        try:
            self.values = load(self.path)
            self.digest = fingerprint(self.path)
            self.populate()
            self.status.setText('Reloaded ' + str(self.path))
        except (ValueError, OSError) as error:
            QMessageBox.warning(self, 'Unable to reload', str(error))

    def save_settings(self):
        try:
            values = self.read_values()
            self.digest = save(values, self.path, self.digest)
            self.values = values
            self.saved = dict(values)
            self.awaiting_apply = True
            self.status.setText('Saved • Waiting for the game to apply changes…')
            self.refresh_runtime_status()
            return True
        except (ValueError, OSError) as error:
            QMessageBox.warning(self, 'Unable to save', str(error))
            return False

    def refresh_runtime_status(self):
        states = running_states(self.path)
        restart = {key for state in states for key in state.get('restart', [])}
        dirty_restart = {key for key, value in self.read_values().items()
                         if key in BY_KEY and applies(BY_KEY[key], self.widgets['gow3_renderer'].currentData())
                         and key not in LIVE_SETTINGS and self.saved.get(key) != value}
        restart = {key for key in restart if key not in BY_KEY or applies(BY_KEY[key], self.widgets['gow3_renderer'].currentData())}
        if restart:
            labels = ', '.join(BY_KEY[key].label if key in BY_KEY else key for key in sorted(restart))
            self.restart_notice.setText('Game restart needed: ' + labels)
        elif dirty_restart and states:
            labels = ', '.join(BY_KEY[key].label for key in sorted(dirty_restart))
            self.restart_notice.setText('After applying, a game restart is needed for: ' + labels)
        else:
            self.restart_notice.clear()
        if not getattr(self, 'awaiting_apply', False):
            return
        if not states:
            self.status.setText('Saved • Settings will be used when you launch the game')
            self.awaiting_apply = False
        elif all(state.get('digest') == self.digest for state in states):
            if any(state.get('pending') for state in states):
                self.status.setText('Saved • Waiting for the game to finish applying changes…')
                return
            errors = [state['error'] for state in states if state.get('error')]
            ignored = {key for state in states for key in state.get('ignored', [])}
            if errors:
                self.status.setText('Saved • Some changes could not be applied: ' + '; '.join(errors))
            elif ignored:
                self.status.setText('Applied supported changes • Command-line overrides remain active: ' + ', '.join(sorted(ignored)))
            else:
                self.status.setText('Applied to the running game' + (' • Remaining changes need a restart' if restart else ''))
            self.awaiting_apply = False
        if not self.awaiting_apply and self.read_values() != self.saved:
            self.status.setText(self.status.text() + ' • Unsaved changes remain')

    def launch(self):
        if not self.save_settings():
            return
        try:
            if game_running():
                QMessageBox.information(self, 'Game already running',
                    'Settings saved. Close the running game before launching again.')
                return
            (ROOT / 'logs').mkdir(exist_ok=True)
            with (ROOT / 'logs/settings-launch.log').open('a') as output:
                if WINDOWS:
                    # Packages have the launcher exe; a checkout has the batch file.
                    launcher = ROOT / 'squarehole360.exe'
                    command = [str(launcher)] if launcher.exists() else \
                        ['cmd', '/c', str(ROOT / 'tools/run-windows.bat')]
                    subprocess.Popen(command + ['--gow3_config=' + str(self.path.resolve())],
                                     cwd=ROOT, stdout=output, stderr=output,
                                     creationflags=subprocess.DETACHED_PROCESS |
                                     subprocess.CREATE_NEW_PROCESS_GROUP)
                else:
                    launcher = 'tools/run-macos.sh' if sys.platform == 'darwin' else 'tools/run-linux.sh'
                    subprocess.Popen([str(ROOT / launcher),
                                      '--gow3_config=' + str(self.path.resolve())],
                                     cwd=ROOT, stdout=output, stderr=output, start_new_session=True)
            self.status.setText('Game launched • You can close settings')
        except OSError as error:
            QMessageBox.warning(self, 'Unable to launch', str(error))

    def closeEvent(self, event):
        if self.confirm_discard():
            self.live_timer.stop()
            self.hdr_timer.stop()
            if self.hdr_process is not None:
                self.hdr_process.kill()
                self.hdr_process.waitForFinished(1000)
            event.accept()
        else:
            event.ignore()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, default=CONFIG)
    args = parser.parse_args()
    app = QApplication(sys.argv[:1])
    try:
        window = SettingsWindow(args.config)
    except (ValueError, OSError) as error:
        QMessageBox.critical(None, 'Unable to load settings', str(error))
        return 1
    window.show()
    return app.exec()


if __name__ == '__main__':
    sys.exit(main())
