"""Shared settings schema and validated, atomic TOML persistence."""
from dataclasses import dataclass
from pathlib import Path
import hashlib
import math
import re
import os
import shutil
import tempfile
import tomllib
import tomli_w

ROOT = Path(__file__).resolve().parents[2]
WINDOWS = os.name == 'nt'
BIN_DIR = 'build-windows/bin' if WINDOWS else 'build/bin'


def binary(name, root=None):
    return (root or ROOT) / BIN_DIR / (name + '.exe' if WINDOWS else name)


def process_identity(pid):
    """Return (executable, start_time) for a live pid; start_time matches the
    game's Gow3ProcessStartTime(). Raises OSError when the pid is not running."""
    if not WINDOWS:
        proc = Path('/proc') / str(pid)
        stat = (proc / 'stat').read_text().rsplit(')', 1)[1].split()
        return (proc / 'exe').resolve(strict=True), stat[19]
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        raise OSError(ctypes.get_last_error(), 'process not running')
    try:
        size = wintypes.DWORD(32768)
        name = ctypes.create_unicode_buffer(size.value)
        if not kernel32.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(size)):
            raise OSError(ctypes.get_last_error(), 'cannot query process image')
        times = [wintypes.FILETIME() for _ in range(4)]
        if not kernel32.GetProcessTimes(handle, *[ctypes.byref(t) for t in times]):
            raise OSError(ctypes.get_last_error(), 'cannot query process times')
        exit_code = wintypes.DWORD()
        if kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)) and exit_code.value != 259:
            raise OSError('process exited')  # 259 = STILL_ACTIVE
        created = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        return Path(name.value).resolve(), str(created)
    finally:
        kernel32.CloseHandle(handle)
CONFIG = ROOT / 'config/gow3.toml'

@dataclass(frozen=True)
class Setting:
    key: str
    label: str
    default: object
    section: str
    help: str = ''
    choices: tuple = ()
    minimum: float = 0
    maximum: float = 1000
    renderer: str = 'shared'

S = Setting
SETTINGS = [
 S('gow3_renderer', 'Renderer', 'native', 'Renderer', 'Changing the renderer requires a game restart.', (('native','Native'),('xenia','Emulated (Xenia)'))),
 S('fullscreen', 'Window mode', False, 'Display', 'Borderless fullscreen fills the monitor at its desktop resolution. Windowed restores the configured window size.', ((False,'Windowed'),(True,'Borderless fullscreen'))),
 S('monitor', 'Monitor', 0, 'Display', '0 uses the window manager’s default; other values select a monitor.', minimum=0, maximum=16),
 S('window_width', 'Window width', 1280, 'Display', 'Logical pixels; used only in windowed mode.', minimum=640, maximum=8192),
 S('window_height', 'Window height', 720, 'Display', 'Logical pixels; used only in windowed mode.', minimum=360, maximum=8192),
 S('present_letterbox', 'Preserve aspect ratio', True, 'Display', 'Add bars as needed instead of stretching the image.'),
 S('vsync', 'Vertical sync', False, 'Display', 'Synchronize presentation to the display. Off allows tearing if the desktop permits it.'),
 S('gow3_frame_limit_mode', 'Frame-rate limit', 'custom', 'Display', 'A maximum only. Slow frames are never forced to catch up.', (('custom','Custom maximum'),('display','Monitor refresh rate'),('unlimited','Unlimited'))),
 S('framerate_limit', 'Maximum FPS', 60.0, 'Display', 'Default: 60. Applies when Custom maximum is selected; enter at least 1 FPS.', minimum=0, maximum=1000),
 S('gow3_low_latency', 'Low latency', False, 'Display', 'Keeps the game at most one frame ahead of the renderer and applies the frame-rate limit before input is read. Lowers input lag; may cost a little throughput.'),
 S('present_anti_lag', 'AMD Anti-Lag', False, 'Display', 'Uses the driver\u2019s anti-lag (VK_AMD_anti_lag) when available: AMD on Windows, or Mesa on Linux (turning it on there takes effect after a restart). No effect elsewhere.'),
 S('present_effect', 'Upscaling', 'fsr', 'Image', 'FSR 1 scales the final image to the window. FSR 3.1, DLSS and XeSS (native renderer) also use depth and motion from earlier frames for a sharper, steadier image; menus use FSR 1. DLSS or XeSS fall back to FSR 3.1 where unavailable.', (('fsr','AMD FSR 1 (spatial)'),('fsr3','AMD FSR 3.1 (temporal)'),('dlss','NVIDIA DLSS (temporal, NVIDIA RTX)'),('xess','Intel XeSS (temporal, Windows)'),('cas','AMD CAS sharpening'),('bilinear','Bilinear')), renderer='shared'),
 S('present_frame_generation', 'Frame generation', False, 'Image', 'FSR 3.1 shows a generated frame between game frames: smoother motion at the cost of some input latency. Needs FSR 3.1 upscaling; best above 60 FPS.', renderer='shared'),
 S('present_fsr_sharpness_reduction', 'FSR softness', 0.2, 'Image', '0 is the sharpest; larger values reduce sharpening.', minimum=0, maximum=2, renderer='shared'),
 S('present_fsr3_ghosting_reduction', 'FSR 3.1 ghosting reduction', 1.0, 'Image', 'Shortens FSR 3.1\u2019s history. 0 keeps the sharpest still image; higher values reduce smearing on grass, foliage and animated characters but add shimmer.', minimum=0, maximum=1, renderer='shared'),
 S('present_fsr_max_upsampling_passes', 'FSR scaling passes', 2, 'Image', 'Two passes cover 720p to 4K. Each pass scales by at most 2×. FSR 3.1 always uses one.', minimum=1, maximum=4, renderer='shared'),
 S('present_cas_additional_sharpness', 'CAS sharpness', 0.0, 'Image', 'Additional sharpening when CAS is selected.', minimum=0, maximum=1, renderer='shared'),
 S('resolution_scale', 'Internal resolution', 1, 'Image', '1× renders at the game’s original resolution; 2× is about 1440p and 3× about 4K.', ((1,'1× (original resolution)'),(2,'2× (about 1440p)'),(3,'3× (about 4K)')), renderer='xenia'),
 S('swap_post_effect', 'Post-process anti-aliasing', 'none', 'Image', 'FXAA smooths edges before scaling; it may soften the picture.', (('none','None'),('fxaa','FXAA'),('fxaa_extreme','FXAA (strong)')), renderer='xenia'),
 S('present_dither', 'Output dithering', False, 'Image', 'Reduce visible color banding in gradients.'),
 S('native_2x_msaa', 'Native 2× MSAA support', True, 'Image', 'Honor game-requested 2× multisampling when supported.', renderer='xenia'),
 S('present_hdr', 'HDR output', False, 'HDR', 'Use native HDR10 output when the selected display has HDR enabled. The game checks again at launch and uses SDR if unavailable.'),
 S('present_hdr_paper_white', 'Paper white (nits)', 203.0, 'HDR', 'Brightness of ordinary scene content before highlight expansion.', minimum=80, maximum=500),
 S('present_hdr_peak_nits', 'Peak brightness (nits; 0 = automatic)', 0.0, 'HDR', 'Automatic uses the display’s reported peak brightness. Set a custom value to calibrate for your display.', minimum=0, maximum=10000),
 S('present_hdr_highlights', 'Highlight expansion', 1.0, 'HDR', 'Expand bright highlights toward the peak brightness. 0 keeps the original SDR brightness range inside HDR output.', minimum=0, maximum=1),
 S('gow3_native_pipeline_prewarm', 'Prepare previously used pipelines', True, 'Shaders', 'Prepare pipeline combinations observed on earlier runs before the game starts.', renderer='native'),
 S('gow3_native_shader_prewarm', 'Prepare shaders in the background', True, 'Shaders', 'Compile discovered game shaders before their first draw when possible.', renderer='native'),
 S('gow3_native_shader_prewarm_threads', 'Shader preparation workers', 0, 'Shaders', '0 selects available CPU threads, up to 32.', minimum=0, maximum=32, renderer='native'),
 S('async_shader_compilation', 'Asynchronous shader compilation', True, 'Shaders', 'Compile new pipelines in the background; effects may briefly appear late.', renderer='xenia'),
 S('vulkan_async_shader_translation', 'Background shader translation', True, 'Shaders', 'Translate new shaders on workers. Requires asynchronous compilation.', renderer='xenia'),
 S('vulkan_pipeline_creation_threads', 'Shader worker threads', -1, 'Shaders', '-1 uses every detected logical CPU thread; 0 disables workers.', minimum=-1, maximum=256, renderer='xenia'),
 S('store_shaders', 'Keep shader cache', True, 'Shaders', 'Keep discovered Xenia shaders for later launches.', renderer='xenia'),
 S('vulkan_async_skip_incomplete_frames', 'Hold frames with pending shaders', False, 'Shaders', 'Hold the previous frame while shaders compile; this may cause stutter.', renderer='xenia'),
 S('texture_cache_memory_limit_soft', 'Texture cache target (MB)', 384, 'Advanced', minimum=128, maximum=4096, renderer='xenia'),
 S('texture_cache_memory_limit_hard', 'Texture cache maximum (MB)', 768, 'Advanced', minimum=128, maximum=8192, renderer='xenia'),
 S('texture_cache_memory_limit_soft_lifetime', 'Unused texture retention (seconds)', 30, 'Advanced', minimum=1, maximum=3600, renderer='xenia'),
 S('render_target_path_vulkan', 'Render-target method', 'fsi', 'Advanced', 'Choose fragment interlock or framebuffer objects.', (('fsi','Fragment interlock (validated)'),('fbo','Framebuffer objects')), renderer='xenia'),
 S('gamma_render_target_as_unorm16', '16-bit gamma render targets', False, 'Advanced', 'Keep disabled for the validated Gears rendering path.', renderer='xenia'),
 S('vulkan_unclamped_vertex_indices', 'Unclamped vertex indices', True, 'Advanced', 'Required by the validated rendering fixes for this game.', renderer='xenia'),
 S('depth_float24_round', 'Round 24-bit depth', False, 'Advanced', 'Depth-precision compatibility option.', renderer='xenia'),
 S('depth_float24_convert_in_pixel_shader', 'Pixel-shader depth conversion', False, 'Advanced', 'Can improve depth matching on some paths at a substantial GPU cost.', renderer='xenia'),
 S('occlusion_query_enable', 'Occlusion queries', True, 'Advanced', 'Allow the game to query whether geometry is visible.', renderer='xenia'),
 S('vulkan_async_occlusion_queries', 'Asynchronous visibility queries', True, 'Advanced', 'Collect completed visibility results in batches.', renderer='xenia'),
 S('vulkan_batch_occlusion_queries', 'Group visibility queries', True, 'Advanced', 'Send visibility queries together. Requires asynchronous visibility queries.', renderer='xenia'),
 S('readback_memexport', 'Copy shader output to CPU memory', False, 'Advanced', 'Copy shader-written data to CPU memory when needed by the game.', renderer='xenia'),
 S('readback_memexport_fast', 'Buffered shader readbacks', True, 'Advanced', 'When CPU copies are enabled, reuse a completed previous result when available. Otherwise waits for the GPU; bursts can cause long freezes.', renderer='xenia'),
 S('gow3_60fps', 'Unlock original 30 FPS game cap', True, 'Advanced', 'Required for limits above 30 FPS. The frame-rate limit setting supplies the actual ceiling.'),
 S('gow3_movie_pacing', 'Smooth prerecorded movie timing', True, 'Advanced', 'Poll movie timing every millisecond instead of every 25 ms. Keeps the video’s original frame rate.'),
 S('user_gamertag', 'Gamertag', 'User', 'Online', 'Your name in multiplayer: 1–15 letters, digits and single spaces, starting with a letter. Each installation also has its own player ID in user/identity.toml.'),
 S('network_enabled', 'Networking', True, 'Online', 'Off disables System Link and internet play; the game then reports no network cable.'),
 S('network_lobby_server', 'Lobby server', '', 'Online', 'Internet play: address of a squarehole-lobby server (host or host:port, default port 36360). Leave empty to play System Link on your local network only.'),
 S('network_lobby_room', 'Room', 'public', 'Online', 'Only players in the same room on the lobby server see each other’s System Link parties.'),
 S('network_lobby_password', 'Room password', '', 'Online', 'Optional. The first player to enter a room sets its password; others must use the same one.'),
 S('network_frame_rate_lock', 'Multiplayer frame-rate lock', 60, 'Online', 'Frame-rate limit while other players are connected, so every machine runs the game at the same speed. 0 keeps your normal limit.', minimum=0, maximum=240),
 S('network_address', 'Network address', '', 'Online', 'Advanced. The IPv4 address other System Link players reach you at; empty detects it.'),
 S('network_broadcast_peers', 'System Link peers', '', 'Online', 'Advanced. Comma-separated IPv4 addresses that receive System Link broadcasts directly, for networks that block broadcasts or several copies on one PC.'),
 S('mnk_mode', 'Mouse and keyboard controls', True, 'Input', 'Play with the mouse and keyboard; a controller keeps working alongside them. Keys act as controller buttons.'),
 S('mnk_mouse_mode', 'Mouse aiming', 'look', 'Input', 'Mouse look turns the camera directly with the mouse. Right stick moves the stick instead, with the game’s stick aiming feel, for example for a gyro mapped to the mouse.', (('look','Mouse look'),('stick','As right stick'))),
 S('mnk_sensitivity', 'Mouse sensitivity', 1.0, 'Input', 'Multiplies the game’s mouse sensitivity, or the stick speed when the mouse acts as the right stick.', minimum=0.01, maximum=10),
 S('gyro_mode', 'Gyro aiming', 'off', 'Input', 'For controllers with a gyro, such as DualSense, DualShock 4, Switch Pro and Steam controllers. As a mouse turns the camera directly, like Steam Input’s gyro to mouse. As right stick moves the stick instead, with the game’s stick aiming feel.', (('off','Off'),('mouse','As a mouse'),('joystick','As right stick'))),
 S('gyro_activation', 'Gyro on', 'always', 'Input', 'Always, or only while aiming with the left trigger.', (('always','Always'),('aiming','While aiming'))),
 S('gyro_turn_axis', 'Gyro turning', 'yaw', 'Input', 'Yaw turns when you turn the controller left and right. Roll turns when you tilt it. Player space follows either, relative to how you hold the controller.', (('yaw','Yaw'),('roll','Roll'),('player','Player space'))),
 S('gyro_sensitivity', 'Gyro sensitivity', 1.0, 'Input', 'How far the camera turns per degree the controller turns: 1 matches the real world.', minimum=0.1, maximum=10),
 S('gyro_invert_y', 'Invert gyro up and down', False, 'Input'),
 S('keybind_lstick_up', 'Move forward', 'W', 'Controls', 'Left stick up.'),
 S('keybind_lstick_down', 'Move back', 'S', 'Controls', 'Left stick down.'),
 S('keybind_lstick_left', 'Move left', 'A', 'Controls', 'Left stick left.'),
 S('keybind_lstick_right', 'Move right', 'D', 'Controls', 'Left stick right.'),
 S('keybind_right_trigger', 'Fire', 'LMB', 'Controls', 'Right trigger.'),
 S('keybind_left_trigger', 'Aim', 'RMB', 'Controls', 'Left trigger.'),
 S('keybind_a', 'Cover, roadie run, roll', 'Space,Return', 'Controls', 'A button; also accepts in menus.'),
 S('keybind_x', 'Use, pick up, revive', 'E', 'Controls', 'X button.'),
 S('keybind_b', 'Melee', 'F,Backspace', 'Controls', 'B button; also goes back in menus.'),
 S('keybind_right_shoulder', 'Reload', 'R', 'Controls', 'Right bumper.'),
 S('keybind_y', 'Point of interest', 'Q', 'Controls', 'Y button.'),
 S('keybind_left_shoulder', 'Tactical view', 'Shift', 'Controls', 'Left bumper.'),
 S('keybind_rstick_press', 'Zoom', 'MMB,V', 'Controls', 'Right stick click.'),
 S('keybind_lstick_press', 'Left stick click', 'C', 'Controls', 'Left stick click.'),
 S('keybind_dpad_left', 'Weapon: left', '1,Left,WheelDown', 'Controls', 'D-pad left.'),
 S('keybind_dpad_right', 'Weapon: right', '2,Right,WheelUp', 'Controls', 'D-pad right.'),
 S('keybind_dpad_down', 'Weapon: pistol', '3,Down', 'Controls', 'D-pad down.'),
 S('keybind_dpad_up', 'Weapon: grenades', '4,Up', 'Controls', 'D-pad up.'),
 S('keybind_back', 'Scoreboard', 'Tab', 'Controls', 'Back button.'),
 S('keybind_start', 'Pause', 'Escape', 'Controls', 'Start button.'),
 S('keybind_rstick_up', 'Look up (keys)', '', 'Controls', 'Right stick up, in addition to the mouse.'),
 S('keybind_rstick_down', 'Look down (keys)', '', 'Controls', 'Right stick down.'),
 S('keybind_rstick_left', 'Look left (keys)', '', 'Controls', 'Right stick left.'),
 S('keybind_rstick_right', 'Look right (keys)', '', 'Controls', 'Right stick right.'),
 S('keybind_guide', 'Guide button', '', 'Controls', 'Xbox Guide button.'),
]
BY_KEY = {s.key:s for s in SETTINGS}
# The C++ watcher applies only these reviewed settings, on their owning threads.
LIVE_SETTINGS = {
    'fullscreen', 'window_width', 'window_height', 'gow3_frame_limit_mode', 'framerate_limit',
    'present_effect', 'present_fsr_sharpness_reduction', 'present_fsr3_ghosting_reduction',
    'present_fsr_max_upsampling_passes', 'present_cas_additional_sharpness',
    'present_dither', 'swap_post_effect', 'present_frame_generation',
    'gow3_low_latency', 'present_anti_lag',
} | {s.key for s in SETTINGS if s.section in ('Input', 'Controls')}

def game_running():
    executable = binary('gow3').resolve()
    if WINDOWS:
        import ctypes
        from ctypes import wintypes
        pids = (wintypes.DWORD * 8192)()
        used = wintypes.DWORD()
        if not ctypes.WinDLL('psapi').EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(used)):
            return False
        candidates = pids[:used.value // ctypes.sizeof(wintypes.DWORD)]
    else:
        candidates = [int(p.name) for p in Path('/proc').iterdir() if p.name.isdigit()]
    for pid in candidates:
        try:
            if process_identity(pid)[0] == executable:
                return True
        except (OSError, IndexError):
            continue
    return False


def running_states(path):
    states = []
    executable = binary('gow3').resolve()
    for status in path.parent.glob(path.name + '.runtime.*.toml'):
        try:
            data = tomllib.loads(status.read_text())
            running, start_time = process_identity(int(data['pid']))
            if (running != executable or
                    start_time != data['start_time'] or
                    Path(data['config']).resolve() != path.resolve()):
                continue
            states.append(data)
        except (OSError, ValueError, KeyError, IndexError, TypeError):
            continue
    return states


def replace_with_retry(source, target, attempts=20):
    # Windows refuses to replace a file another process has open; the game
    # only holds gow3.toml for the moment it takes to read it.
    for attempt in range(attempts):
        try:
            os.replace(source, target)
            return
        except PermissionError:
            if attempt + 1 == attempts:
                raise
            import time
            time.sleep(0.05)


def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None

def renderer_mode(value):
    if value == 'reference':
        return 'xenia'
    if value not in ('native', 'xenia'):
        raise ValueError('Renderer: unsupported option')
    return value


def applies(setting, mode):
    return setting.renderer == 'shared' or setting.renderer == renderer_mode(mode)


def visible_settings(values):
    mode = renderer_mode(values.get('gow3_renderer', 'native'))
    return [s for s in SETTINGS if applies(s, mode)]


# Key names the keyboard/mouse driver accepts (vendor/rexglue-sdk/src/ui/keybinds.cpp),
# plus the wheel, which presses a bind briefly per step.
KEY_NAMES = frozenset(
    [f'F{n}' for n in range(1, 25)] + [chr(c) for c in range(ord('A'), ord('Z') + 1)] +
    [str(n) for n in range(10)] + [f'Numpad{n}' for n in range(10)] +
    ['Backtick', 'Minus', 'Plus', 'Comma', 'Period', 'Semicolon', 'Slash', 'Backslash',
     'LBracket', 'RBracket', 'Quote', 'Escape', 'Return', 'Space', 'Tab', 'Backspace', 'Delete',
     'Insert', 'Home', 'End', 'PageUp', 'PageDown', 'Left', 'Right', 'Up', 'Down', 'Shift',
     'Control', 'Alt', 'NumpadEnter', 'NumpadPlus', 'NumpadMinus', 'NumpadStar', 'NumpadSlash',
     'PrintScreen', 'Pause', 'CapsLock', 'NumLock', 'ScrollLock', 'LMB', 'RMB', 'MMB', 'Mouse4',
     'Mouse5', 'WheelUp', 'WheelDown'])


def bind_keys(value):
    """The keys of a binding: a comma-separated list, empty for none."""
    return [key.strip() for key in value.split(',') if key.strip()]


GAMERTAG = re.compile(r'[A-Za-z](?:[A-Za-z0-9]| (?! ))*')
IPV4 = re.compile(r'(?:(?:25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(?:25[0-5]|2[0-4]\d|1?\d?\d)')
HOST_PORT = re.compile(r'[A-Za-z0-9](?:[A-Za-z0-9.-]{0,252})(?::(?:6553[0-5]|655[0-2]\d|65[0-4]\d\d|6[0-4]\d{3}|[1-5]?\d{1,4}))?')


def validate_setting(s, value):
    if type(value) is not type(s.default):
        if not (type(s.default) is float and type(value) is int):
            raise ValueError(f'{s.label}: incorrect value type')
    if s.key == 'user_gamertag' and (len(value) > 15 or not GAMERTAG.fullmatch(value) or value.endswith(' ')):
        raise ValueError(f'{s.label}: use 1–15 letters, digits and single spaces, starting with a letter')
    if s.key == 'network_lobby_server' and value and not HOST_PORT.fullmatch(value):
        raise ValueError(f'{s.label}: use a host name or IPv4 address, optionally followed by :port')
    if s.key in ('network_lobby_room', 'network_lobby_password') and (len(value) > 31 or not value.isprintable()):
        raise ValueError(f'{s.label}: use up to 31 printable characters')
    if s.key.startswith('keybind_'):
        unknown = [key for key in bind_keys(value) if key not in KEY_NAMES]
        if unknown:
            raise ValueError(f'{s.label}: unknown key {unknown[0]}')
    if s.key == 'network_lobby_room' and not value.strip():
        raise ValueError(f'{s.label}: enter a room name')
    if s.key == 'network_address' and value and not IPV4.fullmatch(value):
        raise ValueError(f'{s.label}: use an IPv4 address such as 192.168.1.20')
    if s.key == 'network_broadcast_peers' and value and not all(IPV4.fullmatch(p.strip()) for p in value.split(',')):
        raise ValueError(f'{s.label}: use comma-separated IPv4 addresses')
    if s.choices:
        if value not in [v for v, _ in s.choices]:
            raise ValueError(f'{s.label}: unsupported option')
    elif type(value) in (float, int):
        if not math.isfinite(value) or not s.minimum <= value <= s.maximum:
            raise ValueError(f'{s.label}: use {s.minimum}–{s.maximum}')


def widget_value(setting, values):
    value = values.get(setting.key, setting.default)
    if setting.key == 'gow3_renderer':
        return renderer_mode(value)
    try:
        validate_setting(setting, value)
    except ValueError:
        # Inactive legacy options remain on disk, but never enter a widget with
        # an invalid type or range. Selecting that renderer edits valid values.
        return setting.default
    return value


def validate(values):
    mode = renderer_mode(values.get('gow3_renderer', 'native'))
    for key, value in values.items():
        s = BY_KEY.get(key)
        if s is None or not applies(s, mode):
            continue
        validate_setting(s, mode if key == 'gow3_renderer' else value)
    if mode == 'xenia' and values['texture_cache_memory_limit_soft'] > values['texture_cache_memory_limit_hard']:
        raise ValueError('Texture cache target must not exceed its maximum.')
    if values['gow3_frame_limit_mode'] == 'custom' and values['framerate_limit'] < 1:
        raise ValueError('Custom maximum must be at least 1 FPS. Select Unlimited to remove the cap.')

def load(path=CONFIG):
    values = {s.key:s.default for s in SETTINGS}
    if path.exists(): values.update(tomllib.loads(path.read_text()))
    # Filtering is owned by the game. Legacy overrides are not editable and
    # must not survive a settings round-trip as an expert/unknown option.
    values['anisotropic_override'] = -1
    values['gow3_renderer'] = renderer_mode(values['gow3_renderer'])
    validate(values)
    return values

def save(values, path=CONFIG, expected=None):
    values = dict(values)
    values['anisotropic_override'] = -1
    values['gow3_renderer'] = renderer_mode(values['gow3_renderer'])
    validate(values)
    if fingerprint(path) != expected:
        raise ValueError('The config changed on disk. Reload it before saving.')
    # Other known and unknown keys survive a GUI round-trip. Saves are atomic.
    path.parent.mkdir(parents=True, exist_ok=True)
    body = '# Gears of War 3 settings. Edit here or run ./tools/settings.sh.\n'
    body += '# Apply supported changes live; other changes need a game restart. CLI options override this file.\n\n'
    body += tomli_w.dumps(values)
    if path.exists(): shutil.copy2(path, path.with_suffix('.toml.bak'))
    fd, name = tempfile.mkstemp(prefix=path.name+'.', dir=path.parent)
    try:
        # LF on every platform: the game hashes the raw bytes it reads.
        with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as f:
            f.write(body);f.flush();os.fsync(f.fileno())
        replace_with_retry(name, path)
    finally:
        if os.path.exists(name): os.unlink(name)
    return fingerprint(path)
