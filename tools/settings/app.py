#!/usr/bin/env python3
"""Standalone graphics settings editor. No game process is needed."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from PySide6.QtCore import Qt, QProcess, QTimer
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QDoubleSpinBox,
    QFormLayout, QHBoxLayout, QLabel, QMainWindow, QMessageBox, QPushButton,
    QScrollArea, QSpinBox, QTabWidget, QVBoxLayout, QWidget)
from model import CONFIG, ROOT, WINDOWS, binary, game_running, SETTINGS, LIVE_SETTINGS, BY_KEY, running_states, fingerprint, load, save, applies, widget_value


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
        self.vrr_result = None
        self.vrr_process = None
        self.vrr_status = QLabel('Checking display VRR support…')
        self.vrr_status.setWordWrap(True)
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
                form.addRow(self.vrr_status)
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
        self.vrr_timer = QTimer(self)
        self.vrr_timer.timeout.connect(self.check_vrr)
        if hdr_probe:
            QTimer.singleShot(0, self.check_hdr)
            self.hdr_timer.start(10000)
            QTimer.singleShot(0, self.check_vrr)
            self.vrr_timer.start(10000)

    def check_vrr(self):
        if self.vrr_process is not None:
            return
        probe = binary('gow3-vrr-probe', ROOT)
        if not probe.is_file():
            self.update_vrr_status({'available': False, 'reason': 'VRR detection tool is unavailable.'})
            return
        process = QProcess(self)
        self.vrr_process = process
        timer = QTimer(process)
        timer.setSingleShot(True)
        timer.timeout.connect(process.kill)
        timer.start(4000)
        def finished(*_):
            if self.vrr_process is not process:
                return
            timer.stop()
            try:
                result = json.loads(bytes(process.readAllStandardOutput()).decode())
                if process.exitCode() != 0:
                    raise ValueError()
            except (ValueError, UnicodeDecodeError):
                result = {'available': False, 'reason': 'VRR detection failed.'}
            self.vrr_process = None
            process.deleteLater()
            self.update_vrr_status(result)
        process.finished.connect(finished)
        process.errorOccurred.connect(lambda error: finished() if error == QProcess.ProcessError.FailedToStart else None)
        process.start(str(probe))

    def update_vrr_status(self, result):
        if (not isinstance(result, dict) or type(result.get('available')) is not bool or
                not isinstance(result.get('reason', ''), str) or
                not isinstance(result.get('displays', []), list) or
                any(not isinstance(d, dict) or not isinstance(d.get('name'), str) or not d['name'] or
                    not isinstance(d.get('description', ''), str) or not isinstance(d.get('reason', ''), str) or
                    type(d.get('enabled')) is not bool or type(d.get('available')) is not bool
                    for d in result.get('displays', []))):
            result = {'available': False, 'reason': 'VRR detection returned invalid display data.'}
        self.vrr_result = result
        self.refresh_vrr_status()

    def refresh_vrr_status(self, states=None):
        # Report compositor state only; this status never changes settings or
        # desktop policy and never participates in Apply acknowledgment.
        monitor = self.widgets['monitor'].value()
        preview = False
        reason = ''
        name = ''
        if monitor == 0:
            states = running_states(self.path) if states is None else states
            outputs = {s['vrr_output'] for s in states if isinstance(s.get('vrr_output'), str) and s['vrr_output']}
            if len(outputs) == 1:
                name = next(iter(outputs))
            elif outputs:
                reason = 'Running games are using different outputs.'
            else:
                screen = QApplication.primaryScreen()
                name = screen.name() if screen else ''
                preview = True
        else:
            screens = QApplication.screens()
            if monitor <= len(screens):
                name = screens[monitor - 1].name()
            else:
                reason = 'The selected monitor is not connected.'
        result = self.vrr_result or {}
        displays = [d for d in result.get('displays', []) if d['name'] == name]
        display = displays[0] if len(displays) == 1 else None
        available = result.get('available') is True and display is not None and display['available']
        if available:
            label = display.get('description') or name
            text = 'Display VRR: ' + ('Enabled' if display['enabled'] else 'Disabled') + ' on ' + label + '.'
        elif self.vrr_result is None:
            text = 'Checking display VRR support…'
        else:
            reason = reason or (display.get('reason') if display else '') or result.get('reason') or 'VRR status is unavailable for the selected output.'
            text = 'Display VRR: Unavailable. ' + reason
        if preview:
            text += ' Default-display preview; the game checks its actual output at launch.'
        text += ' The compositor controls VRR automatically; the game leaves desktop policy unchanged. Reported Enabled does not confirm variable scanout on every frame.'
        self.vrr_status.setText(text)

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
                w.isChecked() if isinstance(s.default, bool) else w.value())
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
            visible = applies(setting, mode)
            form, label, widget = self.rows[setting.key]
            hidden_focus |= not visible and (focus is widget or widget.isAncestorOf(focus) if focus else False)
            form.setRowVisible(widget, visible)
            if visible:
                active_sections.add(setting.section)
        for section, pane in self.section_pages.items():
            self.tabs.setTabVisible(self.tabs.indexOf(pane), section in active_sections)
        if not self.tabs.isTabVisible(self.tabs.currentIndex()):
            self.tabs.setCurrentWidget(self.section_pages['Renderer'])
        if hidden_focus:
            self.widgets['gow3_renderer'].setFocus()
        self.widgets['framerate_limit'].setEnabled(
            self.widgets['gow3_frame_limit_mode'].currentData() == 'custom')
        self.refresh_vrr_status()
        effect = self.widgets['present_effect'].currentData()
        self.widgets['present_fsr_sharpness_reduction'].setEnabled(effect in ('fsr', 'fsr3'))
        self.widgets['present_fsr_max_upsampling_passes'].setEnabled(effect == 'fsr')
        self.widgets['present_frame_generation'].setEnabled(effect == 'fsr3')
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
        self.refresh_vrr_status(states)
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
                    subprocess.Popen(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                      '-File', str(ROOT / 'tools/run-windows.ps1'),
                                      '--gow3_config=' + str(self.path.resolve())],
                                     cwd=ROOT, stdout=output, stderr=output,
                                     creationflags=subprocess.DETACHED_PROCESS |
                                     subprocess.CREATE_NEW_PROCESS_GROUP)
                else:
                    subprocess.Popen([str(ROOT / 'tools/run-linux.sh'),
                                      '--gow3_config=' + str(self.path.resolve())],
                                     cwd=ROOT, stdout=output, stderr=output, start_new_session=True)
            self.status.setText('Game launched • You can close settings')
        except OSError as error:
            QMessageBox.warning(self, 'Unable to launch', str(error))

    def closeEvent(self, event):
        if self.confirm_discard():
            self.live_timer.stop()
            self.hdr_timer.stop()
            self.vrr_timer.stop()
            if self.hdr_process is not None:
                self.hdr_process.kill()
                self.hdr_process.waitForFinished(1000)
            if self.vrr_process is not None:
                process = self.vrr_process
                process.kill()
                process.waitForFinished(1000)
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
