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

    def test_display_probe_schedules_share_existing_test_switch(self):
        with patch('app.QTimer.singleShot') as schedule:
            window = SettingsWindow(self.path, hdr_probe=True)
            self.addCleanup(window.close)
            self.assertEqual(schedule.call_count, 1)
            self.assertTrue(window.hdr_timer.isActive())
            self.assertEqual(window.hdr_timer.interval(), 10000)


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

    def test_only_native_renderer_options_are_visible(self):
        # The renderer selection and Xenia-only options are for dev/test configs.
        window = SettingsWindow(self.path, hdr_probe=False)
        self.assertTrue(window.widgets['gow3_renderer'].isHidden())
        shown = ['present_hdr', 'fullscreen', 'mnk_mode', 'present_dither', 'present_effect',
                 'gow3_native_pipeline_prewarm', 'gow3_native_shader_prewarm_threads',
                 'controller1_gyro_mode', 'controller4_gyro_activation', 'controller2_buttons']
        xenia = ['resolution_scale', 'swap_post_effect', 'texture_cache_memory_limit_soft', 'async_shader_compilation']
        for key in shown:
            self.assertFalse(window.widgets[key].isHidden(), key)
        for key in xenia:
            self.assertTrue(window.widgets[key].isHidden(), key)
        self.assertEqual({s.key for s in visible_settings(window.read_values())},
                         {key for key, widget in window.widgets.items() if not widget.isHidden()})
        self.assertTrue(window.save_settings())
        window.close()

    def test_inactive_invalid_values_survive_native_save_but_not_xenia_save(self):
        self.path.write_text('gow3_renderer = "native"\ntexture_cache_memory_limit_soft = 8192\n'
                             'swap_post_effect = ["invalid"]\nasync_shader_compilation = "bad"\n')
        values = load(self.path)
        self.assertEqual(values['texture_cache_memory_limit_soft'], 8192)
        window = SettingsWindow(self.path, hdr_probe=False)
        self.assertEqual(window.widgets['texture_cache_memory_limit_soft'].maximum(), 4096)
        self.assertTrue(window.save_settings())
        self.assertEqual(load(self.path)['swap_post_effect'], ['invalid'])
        window.widgets['gow3_renderer'].setCurrentIndex(1)
        # The newly selected fields contain valid, editable defaults, never the
        # invalid hidden values retained in the file.
        self.assertEqual(window.widgets['texture_cache_memory_limit_soft'].value(), 384)
        self.assertTrue(window.save_settings())
        selected = load(self.path)
        self.assertEqual(selected['gow3_renderer'], 'xenia')
        self.assertEqual(selected['texture_cache_memory_limit_soft'], 384)
        self.assertEqual(selected['swap_post_effect'], 'none')
        window.close()

    def test_mode_relevant_validation(self):
        values = load(self.path)
        values.update(texture_cache_memory_limit_soft=8192, swap_post_effect=['invalid'])
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
            # Only the renderer selection lived on the Renderer tab.
            self.assertEqual(window.tabs.isTabVisible(index), section != 'Renderer', section)
        self.assertIn('bilinear', window.image_note.text())
        self.assertNotIn('FSR', window.image_note.text())
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

    def test_key_bindings_validate_and_match_the_driver_defaults(self):
        values = load(self.path)
        validate(values)
        values['keybind_x'] = 'E, Mouse5,WheelUp'
        validate(values)
        values['keybind_x'] = 'E,Hyper'
        with self.assertRaisesRegex(ValueError, 'unknown key Hyper'):
            validate(values)
        driver = (Path(__file__).resolve().parents[1] /
                  'vendor/rexglue-sdk/src/input/mnk/mnk_input_driver.cpp').read_text()
        defaults = dict(re.findall(r'REXCVAR_DEFINE_STRING\((keybind_\w+), "([^"]*)"', driver))
        self.assertEqual(defaults, {s.key: s.default for s in SETTINGS if s.key.startswith('keybind_')})

    def test_controller_mapping_round_trip_and_validation(self):
        from model import parse_mapping, format_mapping, MAPPING_DEFAULTS
        mapping = parse_mapping('a=b, rpaddle1=gyro,lt=lt')
        self.assertEqual(mapping['a'], 'b')
        self.assertEqual(mapping['rpaddle1'], 'gyro')
        self.assertEqual(format_mapping(mapping), 'a=b,rpaddle1=gyro')
        self.assertEqual(format_mapping(MAPPING_DEFAULTS), '')
        values = load(self.path)
        values['controller2_buttons'] = 'a=jump'
        with self.assertRaisesRegex(ValueError, 'Controller 2 button mapping'):
            validate(values)
        # The driver's default map and names agree with the editor's.
        driver = (Path(__file__).resolve().parents[1] /
                  'vendor/rexglue-sdk/src/input/sdl/sdl_input_driver.cpp').read_text()
        names = re.findall(r'"(\w+)"', driver.split('kSourceNames[] = {', 1)[1].split('};', 1)[0])
        self.assertEqual(set(names), set(MAPPING_DEFAULTS))

    def test_legacy_gyro_settings_move_to_player_one(self):
        self.path.write_text('gyro_mode = "mouse"\ngyro_sensitivity = 2.0\n')
        values = load(self.path)
        self.assertEqual(values['controller1_gyro_mode'], 'mouse')
        self.assertEqual(values['controller1_gyro_sensitivity'], 2.0)
        self.assertNotIn('gyro_mode', values)
        self.assertEqual(values['controller2_gyro_mode'], 'off')

    def test_button_mapping_editor(self):
        window = SettingsWindow(self.path, hdr_probe=False)
        edit = window.widgets['controller1_buttons']
        self.assertEqual(edit.summary.text(), 'Default buttons')
        edit.setText('lpaddle1=gyro')
        self.assertIn('Hold for gyro', edit.summary.text())
        self.assertTrue(window.save_settings())
        self.assertEqual(load(self.path)['controller1_buttons'], 'lpaddle1=gyro')
        from app import ButtonMappingDialog
        dialog = ButtonMappingDialog(window, 'test', 'a=b')
        self.assertEqual(dialog.text(), 'a=b')
        dialog.restore_defaults()
        self.assertEqual(dialog.text(), '')
        window.close()

    def test_key_capture_sets_and_adds_keys(self):
        from app import KeyBindEdit, KeyCaptureDialog, Qt
        edit = KeyBindEdit('Use')
        edit.setText('E')
        def press(key, modifiers=Qt.KeyboardModifier.NoModifier):
            def exec_(dialog):
                QTest.keyClick(dialog, key, modifiers)
                return dialog.result()
            return patch.object(KeyCaptureDialog, 'exec', exec_)
        with press(Qt.Key.Key_Tab):
            edit.capture(append=True)
        self.assertEqual(edit.text(), 'E,Tab')
        with press(Qt.Key.Key_Exclam, Qt.KeyboardModifier.ShiftModifier):
            edit.capture(append=False)
        self.assertEqual(edit.text(), '1')


if __name__ == '__main__': unittest.main()
