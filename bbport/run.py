#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Windows counterpart of run.sh: prepares the game image and starts bb-probe.exe.

Same environment variables as run.sh (BB_GAME_DIR, BB_DATA_DIR, BB_FPS, BB_RENDER_RES,
BB_LIVE_RES, BB_PATCHES, BB_MODS_*, BB_USER_DIR, ...). Extra arguments go to bb-probe.
The in-game "Apply and restart" runs this script again through BB_RESTART_COMMAND;
`--after PID` waits for the previous game process to end first (its GPU device and memory).
"""
import ctypes
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

PORT = Path(__file__).resolve().parent
# Direct memory (BB_DMEM_MB, MiB) of a render size above 1080p, where patches.py also enlarges the
# graphics heap ("Increased Graphics Heap Sizes").
FULL_HD_PIXELS = 1920 * 1080
HIGH_RESOLUTION_DMEM_MB = '9152'


# Co-op: the community service that answers Bloodborne's own game-server requests
# (messages, bloodstains, sign-in); summon requests go to the shadNet server instead.
COMMUNITY_SERVER = 'https://thehuntersdream.com'
GAME_SERVER = 'https://ss4.scej-network.jp:20443'
# The server's ss.info sends the game to this port for its API (summons among them).
COMMUNITY_API_PORT = 18671


def summon_service(webapi):
    """True when a shadNet WebAPI answers Bloodborne's summon calls: the route exists there
    (an empty request is refused with 400), where other servers answer 404."""
    import urllib.error
    import urllib.request
    request = urllib.request.Request(webapi.rstrip('/') + '/summon_messenger/create', data=b'',
                                     method='POST')
    try:
        urllib.request.urlopen(request, timeout=4).close()
        return True
    except urllib.error.HTTPError as error:
        return error.code != 404
    except (OSError, ValueError):
        return False


def online_overrides(env, user_dir):
    """Writes user/host_overrides.json for this launch. A player who needs other addresses
    puts them in user/host_overrides.custom.json, which is then used as it is."""
    custom = user_dir / 'host_overrides.custom.json'
    path = custom if custom.exists() else user_dir / 'host_overrides.json'
    if path != custom:
        community = (env.get('BB_COMMUNITY_SERVER') or COMMUNITY_SERVER).rstrip('/')
        if '://' not in community:
            community = 'https://' + community
        host = community.split('://', 1)[1]
        webapi = env.get('BB_SHADNET_WEBAPI') or 'http://127.0.0.1:31315'
        # Game server -> community server (same port, 20443). Summon signs -> the shadNet
        # WebAPI when that server has the Bloodborne summon service (the shadp2p server),
        # otherwise they stay with the community server.
        overrides = {GAME_SERVER: community}
        if summon_service(webapi):
            overrides.update({f'{scheme}://{host}:{COMMUNITY_API_PORT}/summon_messenger': webapi
                              for scheme in ('https', 'http')})
            print(f'Co-op: summon signs through {webapi}')
        else:
            print(f'Co-op: {webapi} has no summon service; summon signs through {community}')
        path.write_text(json.dumps(overrides, indent=2) + '\n', encoding='utf-8')
    env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'] = str(path)


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def wait_for(pid):
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console (the launcher): console programs must not open their own."""
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def run_script(name, *args, capture=False, optional=False):
    """Runs scripts/<name>; capture: returns what it prints. optional (with capture): a failure returns None
    instead of stopping the start."""
    # Inside the packaged Bloodborne.exe (PyInstaller) the executable runs scripts itself.
    script = ['--script'] if getattr(sys, 'frozen', False) else []
    command = [sys.executable, *script, str(PORT / 'scripts' / name), *map(str, args)]
    if capture:
        # The script prints UTF-8 and this process reads UTF-8, not the Windows code page (a game
        # folder or a user name with non-ASCII letters reaches the next step intact).
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True,
                                encoding='utf-8', errors='replace', creationflags=no_console(),
                                env={**os.environ, 'PYTHONIOENCODING': 'utf-8'})
        if result.returncode:
            if optional:
                return None
            sys.exit(result.returncode)
        return result.stdout
    # stdin given: the output handles are passed explicitly (a windowed Bloodborne.exe child would
    # get none otherwise).
    result = subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console())
    if result.returncode:
        sys.exit(result.returncode)
    return None


def party_hud_args(env):
    """mods.py's party HUD option (scripts/hud_patch.py: the HUD the mods show, with party death's downed banner
    and party list, over them). Party death is always on in the port and drives the list, so the HUD is patched
    whatever an old kit's SHADPS4_BLOODBORNE_PARTY_DEATH says; BB_PARTY_HUD=0 keeps the game's HUD as it is."""
    return [] if env.get('BB_PARTY_HUD', '1') == '0' else ['--party-hud']


def game_view(env, game, out, data, icons):
    """The game folder the game starts from: mods.py's view of the player's mods, the button icons (`icons`, its
    --icons-layer arguments) and the party HUD over the game. When it cannot be made with the port's own layers,
    the game starts without them, as it did before they existed: the overlay then draws the downed banner itself
    (BB_PARTY_HUD=0) and the glyphs are the game's own. From Supermedo's bbport (bf0878b, 9461312: their own layers
    there are Quit to Desktop's texts and the HUD layout)."""
    args = [game, '--out', out, '--mods-dir', env.get('BB_MODS_DIR', data / 'mods'),
            '--config', env.get('BB_MODS_CONFIG', data / 'mods.json'), '--enabled', env.get('BB_MODS_ENABLED', '1')]
    layers = [*icons, *party_hud_args(env)]
    view = run_script('mods.py', *args, *layers, capture=True, optional=bool(layers))
    if view is None:
        print('Mods: the game view could not be made with the button icons and the party HUD; starting without them')
        env['BB_PARTY_HUD'] = '0'
        env['BB_ICONS_SET'] = 'playstation'
        view = run_script('mods.py', *args, capture=True)
    return Path(view.strip())


def frame_mode(requested, fps_limit, online=False, online_unlocked=False):
    """The frame rate mode to launch with: the unlocked one when the game menu set an FPS limit
    (bbport.ini fps_limit; the fixed 60 FPS timestep cannot go above 60), else the one asked for.
    The 30 FPS mode stays as it is. Online play runs at the fixed 60 FPS: a host above 60
    (Kaniel 2026-10-13, uncapped at 100-200 FPS) made the guests see everyone freeze and fly and
    their hits land late. The opt-in online_unlocked (BB_ONLINE_UNLOCKED_FPS) keeps the chosen
    mode online, with the game's frame cap and send pace of online_frame_env."""
    if requested == '30':
        return requested
    if online and not online_unlocked:
        return '60'
    if online and requested == '90':
        # 90 FPS++ is a fixed 90 FPS step: a PC that cannot hold 90 runs its game slower, and the
        # enemies it runs lagged for everyone (Kaniel 2026-10-22). Online: real frame time, cap 90.
        return 'uncap'
    try:
        limit = int(fps_limit or 0)
    except ValueError:
        limit = 0
    return 'uncap' if limit > 0 else requested


# The unlocked frame rate online goes no higher: the game's movement timing breaks above it.
ONLINE_FPS_CAP = 120


def online_frame_env(fps, online, online_unlocked, pace=None, asked=None):
    """The game's variables for an unlocked frame rate online: its frame cap (BB_ONLINE_FPS_CAP)
    and the send pace (SHADPS4_BLOODBORNE_SEND_PACE), which holds the game's frame-counted
    packets to the 60 FPS mode's rates. The 60 FPS++ and 90 FPS++ patches scale two of those
    counters, Uncap FPS++ none: an uncapped host sent its state several times as often.
    `pace` '0' (BB_SEND_PACE) leaves the pace out, for comparisons; `asked` '90' (the 90 mode,
    unlocked online by frame_mode) caps at 90."""
    if not (online and online_unlocked and fps in ('uncap', '90')):
        return {}
    cap = {'BB_ONLINE_FPS_CAP': '90' if asked == '90' else str(ONLINE_FPS_CAP)}
    if pace == '0':
        return cap
    return {**cap, 'SHADPS4_BLOODBORNE_SEND_PACE': '1'}


def build_check_env(out):
    """Build check on join: the game's build fingerprint names the patches that change the game and the others
    (patches.py's patches-names.json) and hashes patches.bin (bbcoop/coop.cpp)."""
    try:
        names = json.loads((Path(out) / 'patches-names.json').read_text(encoding='utf-8'))
        gameplay, other = list(names['gameplay']), list(names['other'])
    except (OSError, ValueError, KeyError, TypeError):
        gameplay, other = [], []
    return {'BB_PATCH_GAMEPLAY': ', '.join(gameplay) or 'none', 'BB_PATCH_OTHER': ', '.join(other) or 'none',
            'BB_PATCHES_FILE': str(Path(out) / 'patches.bin')}


def ini_value(path, key):
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            match = re.fullmatch(rf'{re.escape(key)}=(.*)', line.strip())
            if match:
                return match.group(1)
    except OSError:
        pass
    return None


def render_pixels(size):
    """Pixels of a "WxH" render size (BB_RENDER_RES, as patches.py reads it), 0 when absent or malformed."""
    match = re.fullmatch(r'\s*(\d+)\s*[xX]\s*(\d+)\s*', size or '')
    return int(match.group(1)) * int(match.group(2)) if match else 0


def memory_mb(value):
    """MiB of a BB_DMEM_MB value, 0 when absent or malformed (runtime_memory.c then uses 5056)."""
    value = (value or '').strip()
    return int(value) if value.isdigit() else 0


def find_executable(name):
    """bin/ (packaged), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--after':
        wait_for(int(args[1]))
        args = args[2:]
    # As run.sh: relative paths (BB_GAME_DIR, BB_DATA_DIR, fsr4_shaders/) are from the port.
    os.chdir(PORT)
    env = os.environ
    data = Path(env.get('BB_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = env['BB_CONFIG']
    if not env.get('BB_FSR411_DIR') and not (PORT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        env['BB_FSR411_DIR'] = str(data / 'fsr4_411')

    probe = Path(env['BB_PROBE']) if env.get('BB_PROBE') else find_executable('bb-probe.exe')
    if not probe or not probe.is_file():
        fail('bb-probe.exe not found: build it with build.sh (MSYS2 CLANG64) or use a packaged build.')
    # Development builds run from the MSYS2 tree: its CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'

    game = Path(env.get('BB_GAME_DIR', PORT.parent / 'CUSA03173'))
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set BB_GAME_DIR).')
    original_game = game.resolve()
    # The button icons (a generated common.tpf.dcx, scripts/button_icons.py) are a layer under the mods;
    # the note says why they are off when they cannot be made. The launcher's Play decides them for its
    # start. A start without it (the kit's PLAY.bat runs BLauncher --run) and a restart from the game's
    # menu, where the icon style may have changed, decide here: the launcher's choice is taken out of the
    # environment the game inherits, so its restart decides again. BB_ICONS=0: none (the rig).
    icons_dir, icons_note = env.pop('BB_ICONS_DIR', None), env.pop('BB_ICONS_NOTE', None)
    prompt_sets = env.pop('BB_PROMPT_SETS', None) if icons_dir is not None or icons_note is not None else None
    env.pop('BB_PROMPT_SETS', None)
    if icons_dir is None and icons_note is None and env.get('BB_ICONS', '1') != '0':
        sys.path.insert(0, str(PORT / 'scripts'))
        try:
            import button_icons
        except ImportError as error:  # nothing about the icons may stop the game from starting
            decided = {'BB_ICONS_NOTE': f'Button icons are off for this run: {error}'}
        else:
            tool = probe.parent / 'bb-gpu-capabilities.exe'
            decided = button_icons.environment(game, out, button_icons.read_ini(config),
                                               env.get('BB_MODS_DIR', data / 'mods'),
                                               env.get('BB_MODS_CONFIG', data / 'mods.json'),
                                               env.get('BB_MODS_ENABLED', '1'), env.get('BB_GAMEPAD', ''),
                                               lambda: button_icons.list_gamepads(tool))
        icons_dir, icons_note = decided.get('BB_ICONS_DIR'), decided.get('BB_ICONS_NOTE')
        prompt_sets = decided.get('BB_PROMPT_SETS')
    # button_icons=auto: the icons the port swaps in while the game runs (gpu/shim/bbport_prompts.cpp).
    if prompt_sets:
        env['BB_PROMPT_SETS'] = prompt_sets
    icons = ['--icons-layer', icons_dir] if icons_dir else []
    # Field of view (bbport.ini fov, degrees; 43 is the game's): the camera table with every camera's field of
    # view scaled, a layer under the icons and the mods. fov.py keeps it between starts while the setting and the
    # game file are the same. From Supermedo's bbport (b75e811).
    fov_layer = out / 'fov'
    fov = ini_value(config, 'fov')
    if fov and fov.isdigit() and int(fov) != 43:
        made = run_script('fov.py', game, '--out', fov_layer, '--fov', fov, capture=True, optional=True)
        if made is None:
            shutil.rmtree(fov_layer, ignore_errors=True)
            print('Field of view: its camera table could not be written; the game keeps its own')
        else:
            print(made.strip())
            icons += ['--layer', fov_layer]
    else:
        shutil.rmtree(fov_layer, ignore_errors=True)
    if icons_note:
        print(icons_note)
    # The icon set the overlay's button glyphs follow (party death's prompts): the one the game's own glyphs show.
    sys.path.insert(0, str(PORT / 'scripts'))
    try:
        import button_icons
        env['BB_ICONS_SET'] = 'playstation' if env.get('BB_ICONS', '1') == '0' else button_icons.icon_set(
            button_icons.read_ini(config), env.get('BB_GAMEPAD', ''),
            lambda: button_icons.list_gamepads(probe.parent / 'bb-gpu-capabilities.exe'))
    except Exception:  # nothing about the icons may stop the game from starting
        env['BB_ICONS_SET'] = 'playstation'
    # The view belongs to this process, which lasts for the whole game: a later launch removes it if this one is
    # killed before its own cleanup below (mods.remove_stale_views).
    env['BB_VIEW_OWNER'] = str(os.getpid())
    game = game_view(env, game, out, data, icons)
    mod_view = game if game.resolve() != original_game else None
    try:
        run_script('prepare.py', game, '--out', out)
        run_script('link_libc.py', game, '--out', out)
        run_script('link_modules.py', game, '--out', out)
        # Legacy CPU profile (scripts/cpu_compat.py, Intel 3rd generation and older): the image no
        # longer needs BMI1, LZCNT or MOVBE, and bb-probe's CPU check lets such a CPU start.
        try:
            compat = json.loads((out / 'link.json').read_text(encoding='utf-8')).get('cpu_compat', {})
        except (OSError, ValueError):
            compat = {}
        if compat.get('enabled'):
            env['BB_CPU_COMPAT'] = '1'
        else:
            env.pop('BB_CPU_COMPAT', None)
        run_script('content_profile.py', game, '--out', out, '--sku', env.get('BB_CONTENT_SKU', 'full'))

        # Patches exist for game version 01.09 only (patches.py applies none to others): other
        # versions keep the game's 30 FPS timing and change resolutions live.
        sys.path.insert(0, str(PORT / 'scripts'))
        from patches import game_app_version, game_is_patched
        version = game_app_version(game)
        patched = game_is_patched(version, env)
        if not patched:
            print(f'Game version {version}: community patches need 01.09; 30 FPS, no effect patches')
        # Sizes chosen below for the previous launch are recomputed after an in-game restart.
        if env.get('BB_AUTO_RENDER_RES') == '1':
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
                env.pop(key, None)
        fps = env.get('BB_FPS', 'uncap') if patched else '30'
        asked = fps
        online = env.get('BB_ONLINE') == '1'
        online_unlocked = env.get('BB_ONLINE_UNLOCKED_FPS') == '1'
        fps = frame_mode(fps, ini_value(config, 'fps_limit'), online=online, online_unlocked=online_unlocked)
        if fps != asked and online and not online_unlocked:
            print(f'Co-op: online play runs at 60 FPS (FPS mode {asked} is for playing offline)')
        for key in ('BB_ONLINE_FPS_CAP', 'SHADPS4_BLOODBORNE_SEND_PACE'):
            env.pop(key, None)
        unlocked_env = online_frame_env(fps, online, online_unlocked, env.get('BB_SEND_PACE'), asked)
        env.update(unlocked_env)
        if unlocked_env:
            print(f'Co-op: unlocked frame rate online (experimental), up to {unlocked_env["BB_ONLINE_FPS_CAP"]} FPS, '
                  'packets at the 60 FPS rates')
        env['BB_FPS_PRESET'] = fps  # for the co-op log's header line (bbcoop/coop.cpp)
        scaled_render = scaled_output = None
        if not env.get('BB_RENDER_RES'):
            printed = run_script('patches.py', '--print-scaled', '--settings', config, capture=True).split()
            if len(printed) == 2:
                scaled_render, scaled_output = printed
        live = '0'
        if scaled_output:
            live = env.get('BB_LIVE_RES') or ini_value(config, 'live_resolution') or '0'
            if live == 'auto':
                caps = probe.parent / 'bb-gpu-capabilities.exe'
                result = subprocess.run([str(caps), '--live-resolution'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        text=True, encoding='utf-8', errors='replace', creationflags=no_console())
                live = result.stdout.strip() if result.returncode == 0 else '0'
            live = '1' if live == '1' or not patched else '0'
        if live == '1':
            print(f'Output {scaled_output}: live resolution changes (live_resolution=0: startup patch)')
        scaled = live != '1' and bool(scaled_output)
        if scaled:
            env.update(BB_RENDER_RES=scaled_render, BB_OUTPUT_RES=scaled_output, BB_AUTO_RENDER_RES='1')
            env.setdefault('BB_DMEM_MB', HIGH_RESOLUTION_DMEM_MB)
        # Above 1080p (the scaled path above, or an explicit BB_RENDER_RES: the launcher, a test)
        # patches.py grows the graphics heap: the direct memory has to follow, also over a smaller
        # BB_DMEM_MB (the rig's 5056), or the game stops at start (exit 139, or a write to 0 at guest
        # 0x443025). A game without the patches keeps its memory.
        pixels = render_pixels(env.get('BB_RENDER_RES'))
        if patched and pixels > FULL_HD_PIXELS and memory_mb(env.get('BB_DMEM_MB')) < int(HIGH_RESOLUTION_DMEM_MB):
            env['BB_DMEM_MB'] = HIGH_RESOLUTION_DMEM_MB
            if not scaled:
                print(f'Render {env["BB_RENDER_RES"]}: direct memory {env["BB_DMEM_MB"]} MiB')
        if scaled:
            print(f'Output {scaled_output}: scene {scaled_render}, direct memory {env["BB_DMEM_MB"]} MiB '
                  '(live_resolution=1: live changes)')
        run_script('patches.py', '--out', out, '--fps', fps, '--extra', env.get('BB_PATCHES', ''),
                   '--settings', config, '--game-dir', game, '--render-res', env.get('BB_RENDER_RES', ''),
                   '--output-res', env.get('BB_OUTPUT_RES', ''),
                   '--patches-dir', env.get('BB_PATCHES_DIR', data / 'patches'),
                   '--patches-config', env.get('BB_PATCHES_CONFIG', data / 'patches.json'))
        env.update(build_check_env(out))
        # As run.sh (0.5): the GPU memory defaults the renderer is tuned for. Background upload of
        # rewritten GPU memory, command buffers decoded from a copy, small GPU outputs written
        # beside the game's writes; BB_PC_MODEL=1 is the experimental memory and translation
        # model (BB_GUEST_IN_PLACE), BB_AS_0_3=1 the 0.3 synchronisation for comparisons.
        if env.get('BB_AS_0_3') == '1':
            env.update(BB_GUEST_IN_PLACE='0', BB_HOST_COPY_WAITS='all', BB_PRODUCER_CHECK='1')
        env.setdefault('BB_PREUPLOAD', '1')
        env.setdefault('BB_GUEST_IN_PLACE', env.get('BB_PC_MODEL', '0'))
        env.setdefault('BB_COPY_GPU_BUFFERS', '1')
        env.setdefault('BB_GPU_WRITE_TWINS', '1')
        env.setdefault('BB_GPU_WRITE_TWINS_MAX', '65536')
        if not env.get('BB_VBLANK_HZ'):
            env['BB_VBLANK_HZ'] = {'uncap': '480', '90': '90'}.get(fps, '60')

        # The in-game restart starts this script again once this process is gone.
        # GPU caches (shaders, pipelines) beside the saves; read before main(), so set here.
        user_dir = Path(env.get('BB_USER_DIR', data / 'user')).resolve()
        user_dir.mkdir(parents=True, exist_ok=True)
        env.setdefault('BB_GPU_USER_DIR', str(user_dir))
        if env.get('BB_ONLINE') == '1':
            online_overrides(env, user_dir)
        this = ['--run'] if getattr(sys, 'frozen', False) else [str(Path(__file__).resolve())]
        restart = [sys.executable, *this, '--after', str(os.getpid()), *args]
        env['BB_RESTART_COMMAND'] = subprocess.list2cmdline(restart)
        command = [str(probe), str(out / 'boot-linked.bin'), '--content-profile', str(out / 'content.bin'),
                   '--patches', str(out / 'patches.bin'), '--app0', str(game),
                   '--user', str(user_dir),
                   '--timeout', env.get('BB_TIMEOUT', '0'), *args]
        # No stdin: an inherited pipe (shells such as Git Bash) cost the game its console output.
        return subprocess.run(command, stdin=subprocess.DEVNULL, creationflags=no_console()).returncode
    finally:
        if mod_view:
            # Read-only game files leave read-only links (mods.remove_overlay clears them, WinError 5).
            from mods import OWNER_SUFFIX, remove_overlay
            try:
                remove_overlay(mod_view, original_game)
                Path(str(mod_view) + OWNER_SUFFIX).unlink(missing_ok=True)
            except OSError:
                pass  # the next launch removes it (mods.remove_stale_views)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
