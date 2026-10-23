import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import re
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/settings'))
from model import SETTINGS, LIVE_SETTINGS, running_states, load, save, fingerprint, BY_KEY, visible_settings, widget_value, validate
from app import QApplication, SettingsWindow
from PySide6.QtTest import QTest


class SettingsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'settings.toml'

    def test_defaults_unknown_keys_and_backup(self):
        values = load(self.path)
        self.assertEqual(values['gow3_renderer'], 'native')
        self.assertEqual(values['framerate_limit'], 60)
        self.assertFalse(values['vsync'])
        self.assertFalse(values['readback_memexport'])
        self.assertEqual(values['anisotropic_override'], -1)
        values['future_option'] = {'enabled': True}
        first = save(values, self.path)
        before = self.path.read_bytes()
        values['framerate_limit'] = 72.5
        save(values, self.path, first)
        self.assertEqual(load(self.path), values)
        self.assertEqual(self.path.with_suffix('.toml.bak').read_bytes(), before)

    def test_external_edit_is_not_overwritten(self):
        values = load(self.path)
        digest = save(values, self.path)
        self.path.write_text(self.path.read_text() + '\nexternal = true\n')
        with self.assertRaisesRegex(ValueError, 'changed on disk'):
            save(values, self.path, digest)
        self.assertTrue(load(self.path)['external'])

    def test_reject_invalid_settings(self):
        for key, value in [('framerate_limit', 0.0), ('framerate_limit', float('nan')),
                           ('framerate_limit', True), ('present_effect', 'fsr4'),
                           ('swap_post_effect', ''), ('gow3_renderer', 'unknown'),
                           ('texture_cache_memory_limit_soft', 900)]:
            values = load(self.path)
            values['gow3_renderer'] = 'xenia'
            values[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                save(values, self.path)

    def test_renderer_choice_round_trip_requires_restart(self):
        values = load(self.path)
        values['gow3_renderer'] = 'xenia'
        save(values, self.path)
        window = SettingsWindow(self.path, hdr_probe=False)
        self.assertNotIn('gow3_renderer', LIVE_SETTINGS)
        self.assertTrue(window.save_settings())
        state = {'digest': window.digest, 'restart': [], 'error': ''}
        with patch('app.running_states', return_value=[state]):
            choice = window.widgets['gow3_renderer']
            choice.setCurrentIndex(choice.findData('native'))
            self.assertIn('restart is needed', window.restart_notice.text())
            self.assertTrue(window.save_settings())
            self.assertEqual(load(self.path)['gow3_renderer'], 'native')
            state.update(digest=window.digest, restart=['gow3_renderer'])
            window.refresh_runtime_status()
            self.assertIn('Renderer', window.restart_notice.text())
        window.close()

    def test_gui_round_trip_and_modes(self):
        values = load(self.path); values['gow3_renderer'] = 'xenia'; save(values, self.path)
        window = SettingsWindow(self.path, hdr_probe=False)
        self.assertEqual(set(window.widgets), {s.key for s in SETTINGS})
        self.assertFalse(window.widgets['readback_memexport'].isChecked())
        self.assertFalse(window.widgets['readback_memexport_fast'].isEnabled())
        window.widgets['readback_memexport'].setChecked(True)
        self.assertTrue(window.widgets['readback_memexport_fast'].isEnabled())
        self.assertTrue(window.save_settings())
        self.assertTrue(load(self.path)['readback_memexport'])
        window.widgets['readback_memexport'].setChecked(False)
        self.assertTrue(window.save_settings())
        self.assertFalse(load(self.path)['readback_memexport'])
        mode = window.widgets['gow3_frame_limit_mode']
        for value in ('unlimited', 'display', 'custom'):
            mode.setCurrentIndex(mode.findData(value))
            self.assertEqual(window.widgets['framerate_limit'].isEnabled(), value == 'custom')
            self.assertTrue(window.save_settings())
            self.assertEqual(load(self.path)['gow3_frame_limit_mode'], value)
        window.widgets['framerate_limit'].setValue(83.5)
        window.widgets['fullscreen'].setCurrentIndex(window.widgets['fullscreen'].findData(True))
        self.assertFalse(window.widgets['window_width'].isEnabled())
        self.assertEqual(window.widgets['fullscreen'].currentText(), 'Borderless fullscreen')
        self.assertTrue(window.save_settings())
        self.assertEqual(load(self.path)['framerate_limit'], 83.5)
        window.defaults()
        self.assertNotEqual(window.read_values(), window.saved)
        self.assertTrue(window.save_settings())
        self.assertEqual(load(self.path)['framerate_limit'], 60.0)
        window.close()

    def test_hdr_controls_require_live_detection(self):
        window = SettingsWindow(self.path, hdr_probe=False)
        self.assertFalse(window.widgets['present_hdr'].isEnabled())
        window.update_hdr_status({'available': True, 'displays': [{'name': 'HDR display', 'hdr': True}]})
        self.assertTrue(window.widgets['present_hdr'].isEnabled())
        window.widgets['present_hdr'].setChecked(True)
        self.assertTrue(window.widgets['present_hdr_peak_nits'].isEnabled())
        window.update_hdr_status({'available': False})
        self.assertFalse(window.widgets['present_hdr'].isEnabled())
        self.assertFalse(window.widgets['present_hdr'].isChecked())
        self.assertFalse(window.widgets['present_hdr_peak_nits'].isEnabled())
        self.assertTrue(window.save_settings())
        self.assertFalse(load(self.path)['present_hdr'])
        window.close()

    def test_vrr_is_shared_read_only_status(self):
        screen = SimpleNamespace(name=lambda: 'DP-1')
        with patch('app.QApplication.primaryScreen', return_value=screen):
            window = SettingsWindow(self.path, hdr_probe=False)
            self.addCleanup(window.close)
            self.assertNotIn('gow3_vrr', load(self.path))
            self.assertNotIn('gow3_vrr', LIVE_SETTINGS)
            self.assertNotIn('gow3_vrr', BY_KEY)
            self.assertNotIn('gow3_vrr', window.widgets)
            self.assertFalse(window.hdr_timer.isActive())
            self.assertFalse(window.vrr_timer.isActive())
            before = window.read_values()
            window.update_vrr_status({'available': True, 'displays': [
                {'name': 'DP-1', 'description': 'Game display', 'enabled': False, 'available': True, 'reason': ''}]})
            self.assertIn('Display VRR: Disabled on Game display', window.vrr_status.text())
            self.assertIn('compositor controls VRR automatically', window.vrr_status.text())
            self.assertIn('does not confirm variable scanout on every frame', window.vrr_status.text())
            self.assertEqual(window.read_values(), before)
            for mode in ('native', 'xenia', 'native'):
                picker = window.widgets['gow3_renderer']
                picker.setCurrentIndex(picker.findData(mode))
                self.assertFalse(window.vrr_status.isHidden())
                self.assertTrue(window.tabs.isTabVisible(window.tabs.indexOf(window.section_pages['Display'])))
                self.assertNotIn('gow3_vrr', {s.key for s in visible_settings(window.read_values())})
            self.assertTrue(window.save_settings())
            self.assertNotIn('gow3_vrr', load(self.path))

    def test_vrr_status_updates_do_not_modify_settings(self):
        screen = SimpleNamespace(name=lambda: 'DP-1')
        with patch('app.QApplication.primaryScreen', return_value=screen):
            window = SettingsWindow(self.path, hdr_probe=False)
            self.addCleanup(window.close)
            self.assertTrue(window.save_settings())
            before = self.path.read_bytes()
            values = window.read_values()
            for available, enabled, label in ((True, True, 'Enabled'), (True, False, 'Disabled'), (False, False, 'Unavailable')):
                window.update_vrr_status({'available': True, 'displays': [
                    {'name': 'DP-1', 'enabled': enabled, 'available': available, 'reason': 'Unsupported monitor.' if not available else ''}]})
                self.assertIn('Display VRR: ' + label, window.vrr_status.text())
                self.assertEqual(window.read_values(), values)
                self.assertEqual(self.path.read_bytes(), before)
            window.update_vrr_status({'available': False, 'reason': 'Desktop status unavailable.'})
            self.assertIn('Desktop status unavailable', window.vrr_status.text())
            self.assertEqual(window.read_values(), values)
            self.assertEqual(self.path.read_bytes(), before)

    def test_vrr_tracks_selected_monitor_and_actual_running_output(self):
        screens = [SimpleNamespace(name=lambda: 'DP-1', refreshRate=lambda: 60.0),
                   SimpleNamespace(name=lambda: 'HDMI-A-1', refreshRate=lambda: 60.0)]
        with patch('app.QApplication.screens', return_value=screens), \
                patch('app.QApplication.primaryScreen', return_value=screens[0]):
            window = SettingsWindow(self.path, hdr_probe=False)
            self.addCleanup(window.close)
            window.update_vrr_status({'available': True, 'displays': [
                {'name': 'DP-1', 'enabled': True, 'available': True},
                {'name': 'HDMI-A-1', 'enabled': False, 'available': True}]})
            self.assertIn('Enabled on DP-1', window.vrr_status.text())
            self.assertIn('preview', window.vrr_status.text())
            window.widgets['monitor'].setValue(2)
            self.assertIn('Disabled on HDMI-A-1', window.vrr_status.text())
            window.widgets['monitor'].setValue(1)
            self.assertIn('Enabled on DP-1', window.vrr_status.text())
            window.widgets['monitor'].setValue(3)
            self.assertIn('Unavailable', window.vrr_status.text())
            self.assertIn('not connected', window.vrr_status.text())
            window.widgets['monitor'].setValue(0)
            with patch('app.running_states', return_value=[{'vrr_output': 'HDMI-A-1'}]):
                window.refresh_runtime_status()
                self.assertIn('Disabled on HDMI-A-1', window.vrr_status.text())
                self.assertNotIn('preview', window.vrr_status.text())
            with patch('app.running_states', return_value=[{'vrr_output': 'HDMI-A-1'}, {'vrr_output': 'DP-1'}]):
                window.refresh_runtime_status()
                self.assertIn('Unavailable', window.vrr_status.text())
                self.assertIn('different outputs', window.vrr_status.text())
            window.refresh_runtime_status()
            self.assertIn('Enabled on DP-1', window.vrr_status.text())

    def test_vrr_rejects_malformed_probe_without_editing_settings(self):
        window = SettingsWindow(self.path, hdr_probe=False)
        self.addCleanup(window.close)
        before = window.read_values()
        for malformed in ([], {'available': 'yes'}, {'available': True, 'displays': [
                {'name': 'DP-1', 'enabled': 'yes', 'available': True}]},
                {'available': True, 'displays': [{'name': 'DP-1', 'enabled': True, 'controllable': True}]}):
            window.update_vrr_status(malformed)
            self.assertIn('invalid display data', window.vrr_status.text())
            self.assertEqual(window.read_values(), before)

    def test_read_only_vrr_probe_does_not_delay_apply(self):
        window = SettingsWindow(self.path, hdr_probe=False)
        self.addCleanup(window.close)
        self.assertTrue(window.save_settings())
        state = {'digest': window.digest, 'pending': True, 'vrr_pending': True, 'restart': [], 'error': ''}
        with patch('app.running_states', return_value=[state]):
            window.awaiting_apply = True
            window.refresh_runtime_status()
            self.assertTrue(window.awaiting_apply)
            self.assertIn('finish applying', window.status.text())
            state['pending'] = False
            window.refresh_runtime_status()
            self.assertFalse(window.awaiting_apply)  # Read-only VRR detection remains pending.
            self.assertIn('Applied to the running game', window.status.text())

    def test_vrr_probe_runs_asynchronously_and_updates_status(self):
        # Private read-only stub exercises QProcess delivery, never the actual
        # desktop helper or game. No display policy is changed by this test.
        root = Path(self.directory.name) / 'probe-root'
        probe = root / 'build/bin/gow3-vrr-probe'
        probe.parent.mkdir(parents=True)
        probe.write_text('#!' + sys.executable + '\nprint(\'{"available": true, "displays": '
                         '[{"name": "DP-1", "enabled": true, "available": true}]}\')\n')
        probe.chmod(0o700)
        screen = SimpleNamespace(name=lambda: 'DP-1')
        with patch('app.ROOT', root), patch('app.QApplication.primaryScreen', return_value=screen):
            window = SettingsWindow(self.path, hdr_probe=False)
            self.addCleanup(window.close)
            before = window.read_values()
            window.check_vrr()
            self.assertIsNotNone(window.vrr_process)
            deadline = time.monotonic() + 3
            while window.vrr_process is not None and time.monotonic() < deadline:
                QTest.qWait(10)
            self.assertIsNone(window.vrr_process)
            self.assertIn('Display VRR: Enabled', window.vrr_status.text())
            self.assertEqual(window.read_values(), before)

    def test_display_probe_schedules_share_existing_test_switch(self):
        with patch('app.QTimer.singleShot') as schedule:
            window = SettingsWindow(self.path, hdr_probe=True)
            self.addCleanup(window.close)
            self.assertEqual(schedule.call_count, 2)
            self.assertTrue(window.hdr_timer.isActive())
            self.assertTrue(window.vrr_timer.isActive())
            self.assertEqual(window.hdr_timer.interval(), 10000)
            self.assertEqual(window.vrr_timer.interval(), 10000)


    def test_apply_waits_for_game_and_restart_notice_clears(self):
        values = load(self.path); values['gow3_renderer'] = 'xenia'; save(values, self.path)
        window = SettingsWindow(self.path, hdr_probe=False)
        self.assertTrue(window.save_settings())
        state = {'digest': window.digest, 'restart': [], 'error': ''}
        with patch('app.running_states', return_value=[state]):
            window.widgets['present_fsr_sharpness_reduction'].setValue(0.8)
            window.widgets['resolution_scale'].setCurrentIndex(1)
            self.assertIn('restart is needed', window.restart_notice.text())
            self.assertTrue(window.save_settings())
            self.assertIn('Waiting', window.status.text())
            # A late detection result or another edit must not block the ACK.
            window.widgets['present_fsr_sharpness_reduction'].setValue(0.7)
            state.update(digest=window.digest, restart=['resolution_scale'])
            window.refresh_runtime_status()
            self.assertIn('Applied', window.status.text())
            self.assertFalse(window.awaiting_apply)
            self.assertIn('Unsaved changes remain', window.status.text())
            self.assertIn('Game restart needed', window.restart_notice.text())
            window.widgets['resolution_scale'].setCurrentIndex(0)
            self.assertTrue(window.save_settings())
            state.update(digest=window.digest, restart=[])
            window.refresh_runtime_status()
            self.assertEqual(window.restart_notice.text(), '')
        window.close()

    def test_legacy_renderer_is_canonicalized_without_switching_mode(self):
        self.path.write_text('gow3_renderer = "reference"\n')
        values = load(self.path)
        self.assertEqual(values['gow3_renderer'], 'xenia')
        save(values, self.path, fingerprint(self.path))
        self.assertIn('gow3_renderer = "xenia"', self.path.read_text())
        self.assertNotIn('reference', self.path.read_text())

    def test_legacy_filtering_is_normalized_on_load_and_save(self):
        self.assertNotIn('anisotropic_override', BY_KEY)
        for mode in ('native', 'xenia', 'reference'):
            for legacy in ('5', '0', '-1', 'true', '"forced"'):
                with self.subTest(mode=mode, legacy=legacy):
                    text = f'gow3_renderer = "{mode}"\nanisotropic_override = {legacy}\nfuture_option = 7\n'
                    self.path.write_text(text)
                    values = load(self.path)
                    self.assertEqual(values['anisotropic_override'], -1)
                    self.assertEqual(self.path.read_text(), text)  # Loading does not rewrite the user's file.
                    self.assertEqual(values['gow3_renderer'], 'xenia' if mode == 'reference' else mode)
                    values['anisotropic_override'] = 5  # An old caller cannot save a forced override either.
                    save(values, self.path, fingerprint(self.path))
                    self.assertEqual(load(self.path)['anisotropic_override'], -1)
                    self.assertIn('anisotropic_override = -1', self.path.read_text())
                    self.assertEqual(load(self.path)['future_option'], 7)
                    self.assertEqual(self.path.with_suffix('.toml.bak').read_text(), text)
                    self.assertEqual(values['anisotropic_override'], 5)  # Save owns its normalized copy.

    def test_gui_has_no_filtering_override_and_preserves_game_default(self):
        self.path.write_text('gow3_renderer = "native"\nanisotropic_override = 5\n')
        window = SettingsWindow(self.path, hdr_probe=False)
        self.addCleanup(window.close)
        self.assertNotIn('anisotropic_override', window.widgets)
        picker = window.widgets['gow3_renderer']
        for mode in ('native', 'xenia', 'native'):
            picker.setCurrentIndex(picker.findData(mode))
            self.assertNotIn('anisotropic_override', {s.key for s in visible_settings(window.read_values())})
            self.assertEqual(window.read_values()['anisotropic_override'], -1)
            self.assertTrue(window.save_settings())
            self.assertEqual(load(self.path)['anisotropic_override'], -1)
        window.defaults()
        self.assertTrue(window.save_settings())
        self.assertEqual(load(self.path)['anisotropic_override'], -1)

    def test_only_selected_renderer_options_are_visible(self):
        window = SettingsWindow(self.path, hdr_probe=False)
        picker = window.widgets['gow3_renderer']
        self.assertEqual([picker.itemText(i) for i in range(picker.count())], ['Native', 'Xenia'])
        shared = ['gow3_renderer', 'present_hdr', 'fullscreen', 'mnk_mode', 'present_dither']
        native = ['gow3_native_pipeline_prewarm', 'gow3_native_shader_prewarm_threads']
        xenia = ['resolution_scale', 'present_effect', 'texture_cache_memory_limit_soft', 'async_shader_compilation']
        for mode in ('native', 'xenia', 'native'):
            picker.setCurrentIndex(picker.findData(mode))
            for key in shared:
                self.assertFalse(window.widgets[key].isHidden(), (mode, key))
            for key in native:
                self.assertEqual(window.widgets[key].isHidden(), mode != 'native', key)
            for key in xenia:
                self.assertEqual(window.widgets[key].isHidden(), mode != 'xenia', key)
            self.assertEqual({s.key for s in visible_settings(window.read_values())},
                             {key for key, widget in window.widgets.items() if not widget.isHidden()})
        self.assertTrue(window.save_settings())
        window.close()

    def test_inactive_invalid_values_survive_native_save_but_not_xenia_save(self):
        self.path.write_text('gow3_renderer = "native"\ntexture_cache_memory_limit_soft = 8192\n'
                             'present_effect = ["invalid"]\nasync_shader_compilation = "bad"\n')
        values = load(self.path)
        self.assertEqual(values['texture_cache_memory_limit_soft'], 8192)
        window = SettingsWindow(self.path, hdr_probe=False)
        self.assertEqual(window.widgets['texture_cache_memory_limit_soft'].maximum(), 4096)
        self.assertTrue(window.save_settings())
        self.assertEqual(load(self.path)['present_effect'], ['invalid'])
        window.widgets['gow3_renderer'].setCurrentIndex(1)
        # The newly selected fields contain valid, editable defaults, never the
        # invalid hidden values retained in the file.
        self.assertEqual(window.widgets['texture_cache_memory_limit_soft'].value(), 384)
        self.assertTrue(window.save_settings())
        selected = load(self.path)
        self.assertEqual(selected['gow3_renderer'], 'xenia')
        self.assertEqual(selected['texture_cache_memory_limit_soft'], 384)
        self.assertEqual(selected['present_effect'], 'fsr')
        window.close()

    def test_mode_relevant_validation(self):
        values = load(self.path)
        values.update(texture_cache_memory_limit_soft=8192, present_effect=['invalid'])
        validate(values)
        values['gow3_renderer'] = 'xenia'
        with self.assertRaises(ValueError): validate(values)
        values = load(self.path); values['gow3_native_shader_prewarm_threads'] = 33
        with self.assertRaises(ValueError): validate(values)
        values['gow3_renderer'] = 'xenia'
        validate(values)
        values['present_hdr_paper_white'] = -1
        with self.assertRaises(ValueError): validate(values)

    def test_visible_tabs_and_renderer_notes(self):
        window = SettingsWindow(self.path, hdr_probe=False)
        for section, tab in window.section_pages.items():
            index = window.tabs.indexOf(tab)
            self.assertGreaterEqual(index, 0, section)
            self.assertTrue(window.tabs.isTabVisible(index), section)
        self.assertIn('bilinear', window.image_note.text())
        self.assertNotIn('FSR', window.image_note.text())
        window.widgets['gow3_renderer'].setCurrentIndex(1)
        self.assertIn('FSR 1', window.image_note.text())
        self.assertTrue(window.save_settings())
        window.close()

    def test_shader_worker_limits_and_dependencies(self):
        window = SettingsWindow(self.path, hdr_probe=False)
        workers = window.widgets['gow3_native_shader_prewarm_threads']
        self.assertEqual((workers.minimum(), workers.maximum()), (0, 32))
        self.assertEqual(BY_KEY['texture_cache_memory_limit_soft'].maximum, 4096)
        self.assertNotIn('gow3_native_shader_prewarm', LIVE_SETTINGS)
        self.assertNotIn('gow3_native_pipeline_prewarm', LIVE_SETTINGS)
        window.widgets['gow3_native_shader_prewarm'].setChecked(False)
        window.widgets['gow3_native_pipeline_prewarm'].setChecked(False)
        self.assertFalse(workers.isEnabled())
        window.widgets['gow3_native_pipeline_prewarm'].setChecked(True)
        self.assertTrue(workers.isEnabled())
        window.widgets['gow3_renderer'].setCurrentIndex(1)
        window.widgets['async_shader_compilation'].setChecked(False)
        self.assertFalse(window.widgets['vulkan_async_shader_translation'].isEnabled())
        self.assertTrue(window.save_settings())
        window.close()

    def test_stale_runtime_status_is_ignored(self):
        status = self.path.with_name(self.path.name + '.runtime.999999999.toml')
        status.write_text('pid = 999999999\nstart_time = "0"\n')
        self.assertEqual(running_states(self.path), [])

    def test_live_setting_list_matches_renderer_handlers(self):
        source = (Path(__file__).resolve().parents[1] / 'src/gow3_settings_live.cpp').read_text()
        declaration = source.split('live_keys = {', 1)[1].split('};', 1)[0]
        self.assertEqual(set(re.findall(r'"([a-z0-9_]+)"', declaration)), LIVE_SETTINGS)


if __name__ == '__main__': unittest.main()
