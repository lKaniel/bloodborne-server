# SPDX-License-Identifier: GPL-2.0-or-later
"""Seamless co-op in the launcher: what the release's bat files and tools\\setup.ps1 did, so a
player installs one setup .exe and plays from BLauncher.exe (Kaniel 2026-10-13: no bat files, the
launcher finds the game).

A co-op install is the release's folder: BLauncher.exe with event-reference.json and
tools\\party_boss_events.py beside it. Its answers live in user\\coop.json (the old settings.bat
is read once). Nothing here touches the player's game folders: the game the port runs is
coop-game\\CUSA03173, folder links (junctions) and hard links to the game with the 1.09 update
over it, and the co-op event files go in coop-game\\CUSA03173-mods.
"""
import collections
import ctypes
import ipaddress
import json
import os
import re
import secrets
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

# Every Bloodborne release, as the 01.09 eboot's own content-id table lists them: Japan, US, EU and Asia, each
# with its Game of the Year edition.
TITLE_IDS = ('CUSA03173', 'CUSA00207', 'CUSA00208', 'CUSA00900', 'CUSA03179', 'CUSA01363', 'CUSA03023',
             'CUSA00299', 'CUSA03014')
TESTED_TITLE = 'CUSA03173'
# Kaniel 2026-10-22: the launcher says when a game edition was not tested with co-op (#355 should make the
# others work as hosts). The Unlock Game Region patch needs no warning since #357.
UNTESTED_EDITION_TEXT = ('Co-op is tested on the EU edition (CUSA03173) only: {} should work, but it was not '
                         'tested properly. If something goes wrong, send your user\\log folder.')
GAME_VERSION = '01.09'
SHADNET_PORT, WEBAPI_PORT, MATCHING_PORT, BACKEND_PORT = 31313, 31315, 31314, 20443
GAME_SERVER = 'https://ss4.scej-network.jp:20443'
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
DETACHED = getattr(subprocess, 'DETACHED_PROCESS', 0) | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)

# The answers, as 1 - SETUP asked them (FPS is gone: co-op is always the fixed 60 FPS mode). No party
# settings: the team is made in the game's team screen (phase 6). `role` 'host' runs the co-op server.
DEFAULTS = {'game': '', 'update': '', 'role': 'guest', 'host_ip': '', 'account': '', 'password': '',
            'p2p_port': 'auto', 'harder': 'off', 'prepared': '',
            # '1' once the player pressed Done in the launcher's co-op settings (they open by themselves until then).
            'confirmed': ''}
HARDER_ON = '3,1.5,3'  # the kit's harder enemies: HP, damage and poise multipliers
HARDER_BIT, HARDER_MUCH = '1.5,1.25,1.5', '5,2,5'
# What the game takes for each multiplier (ParseHarderEnemies in the emulator); all 1 is the normal game.
HARDER_LOWEST, HARDER_HIGHEST = 1.0, 10.0


# --- answers ----------------------------------------------------------------------------------

def valid_ip(value):
    """An IPv4 address the other players can reach: never loopback (shadnet tells them the address
    it sees, and 127.0.0.1 means nothing to them)."""
    if not re.fullmatch(r'\d{1,3}(\.\d{1,3}){3}', value or ''):
        return False
    try:
        return not ipaddress.IPv4Address(value).is_loopback
    except ValueError:
        return False


def valid_host(value):
    """What Host's IP address takes: a host's IPv4 address (valid_ip) or a public server's name
    (docs/HOSTED_SERVER.md: play.example.com; no scheme, no port: the game picks the ports)."""
    if valid_ip(value):
        return True
    value = value or ''
    labels = value.split('.')
    if len(value) > 253 or len(labels) < 2 or all(label.isdigit() for label in labels):
        return False
    return all(re.fullmatch(r'[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?', label) for label in labels)


# Said when a server's name finds no address at PLAY.
UNKNOWN_SERVER_TEXT = "Can't find the server {}: check its address and your internet connection."
# Under Host's IP address.
PUBLIC_SERVER_HELP = 'Playing on a public server? Type its address here, a name like play.example.com or an IP.'


def resolve_host(value, lookup=socket.gethostbyname):
    """The IPv4 address the game uses for Host's IP address: an address as it is, a server's name looked up
    (once, at PLAY: the link and the HTTP ports then go to one address, as with a host's IP). '' when the
    name finds nothing the other players could reach."""
    value = (value or '').strip()
    if valid_ip(value):
        return value
    if not valid_host(value):
        return ''
    try:
        address = lookup(value)
    except (OSError, UnicodeError):
        return ''
    return address if valid_ip(address) else ''


def is_this_pc(value):
    """Whether the host's IP is an address of this PC: then this PC hosts (it starts the server with Start
    server). There is no Player/Host switch (Kaniel 2026-10-16); an address can be bound only where it is the
    PC's own."""
    if not valid_ip(value):
        return False
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind((value, 0))
        return True
    except OSError:
        return False


# The VPNs players use, by the address range each hands out: the host's address on one of them goes
# into Host's IP address (the launcher's connection help offers this PC's own).
VPN_RANGES = (('Radmin VPN', ipaddress.IPv4Network('26.0.0.0/8')),
              ('Tailscale', ipaddress.IPv4Network('100.64.0.0/10')))


def vpn_name(value):
    """The VPN an address belongs to ('Radmin VPN', 'Tailscale'), '' for any other address."""
    try:
        address = ipaddress.IPv4Address(value)
    except ValueError:
        return ''
    return next((name for name, network in VPN_RANGES if address in network), '')


def own_addresses():
    """This PC's IPv4 addresses (every adapter's, on Windows), [] when they cannot be read."""
    try:
        return socket.gethostbyname_ex(socket.gethostname())[2]
    except OSError:
        return []


def vpn_addresses(addresses=None):
    """This PC's VPN addresses as (VPN, address), Radmin VPN first."""
    found = [(vpn_name(ip), ip) for ip in (own_addresses() if addresses is None else addresses)]
    order = [name for name, _network in VPN_RANGES]
    return sorted({pair for pair in found if pair[0]}, key=lambda pair: (order.index(pair[0]), pair[1]))


def wants_router(host_ip):
    """Whether Start server asks the router to open the host's ports (bbport_upnp): not on a VPN address,
    where the friends come over the VPN, and not with BB_UPNP=0."""
    return os.environ.get('BB_UPNP', '1') != '0' and valid_ip(host_ip) and not vpn_name(host_ip)


def valid_name(value):
    return bool(re.fullmatch(r'[A-Za-z0-9_-]{3,16}', value or ''))


def valid_secret(value):
    return bool(re.fullmatch(r'[A-Za-z0-9_-]{1,32}', value or ''))


def valid_harder(value):
    if value == 'off':
        return True
    if not re.fullmatch(r'\d+(\.\d+)?,\d+(\.\d+)?,\d+(\.\d+)?', value or ''):
        return False
    rates = [float(part) for part in value.split(',')]
    return all(HARDER_LOWEST <= rate <= HARDER_HIGHEST for rate in rates) and rates != [1.0, 1.0, 1.0]


def harder_value(hp, damage, poise):
    """The setting for the player's own multipliers (Kaniel 2026-10-21: the presets were too specific),
    each held to what the game takes; 'off' when they make the normal game."""
    rates = []
    for rate in (hp, damage, poise):
        try:
            rate = float(rate)
        except (TypeError, ValueError):
            rate = HARDER_LOWEST
        rate = min(max(rate, HARDER_LOWEST), HARDER_HIGHEST) if rate == rate else HARDER_LOWEST
        rates.append(f'{round(rate, 2):g}')
    value = ','.join(rates)
    return 'off' if value == '1,1,1' else value


def missing(settings):
    """The answers still to give (or wrong), in the window's order."""
    checks = (('game', lambda v: bool(v)), ('host_ip', valid_host))
    return [key for key, ok in checks if not ok(str(settings.get(key, '')))]


def with_player_id(settings):
    """The settings with this player's id (phase 11, Kaniel 2026-10-15: no account name or password
    to type). `account` and `password` are the id the game signs in with: an old install's account
    stays, so its teams and worlds stay; otherwise a random one is made once. Players see only their
    characters' names."""
    if valid_name(settings.get('account', '')) and valid_secret(settings.get('password', '')):
        return settings
    settings['account'] = 'p' + secrets.token_hex(7)
    settings['password'] = secrets.token_hex(16)
    return settings


def virtual_address(online_id):
    """The game's virtual address for a player id (net_link.h VirtualAddressOf: a 24-bit FNV-1a hash in
    10.0.0.0/8, never .0 or .255). A server keeps one account at each (docs/HOSTED_SERVER.md)."""
    value = 2166136261
    for byte in online_id.encode('utf-8'):
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    host = value & 0xFFFFFF
    if host & 0xFF in (0, 0xFF):
        host ^= 0x01
    return str(ipaddress.IPv4Address((10 << 24) | host))


def with_new_player_id(settings):
    """A copy of the settings with a new id, made as with_player_id makes the first, at another
    address than the old one: the server refused the old one's address (address_taken)."""
    settings = dict(settings)
    old = virtual_address(settings.get('account', ''))
    while True:
        account = 'p' + secrets.token_hex(7)
        if virtual_address(account) != old:
            break
    settings['account'], settings['password'] = account, secrets.token_hex(16)
    return settings


def server_key(host):
    """A server's key in `server_ids`: Host's IP address as typed, in lower case. Not the address a name
    finds: a public server can move to another address and keep its accounts."""
    return (host or '').strip().lower()


def player_id_for(settings, host):
    """(id, secret) the game signs in with on `host`: the server's own id when one was made for it
    (with_server_id), else the player's one id."""
    own = (settings.get('server_ids') or {}).get(server_key(host))
    if own:
        return own[0], own[1]
    return settings.get('account', ''), settings.get('password', '')


def with_server_id(settings, host):
    """A copy of the settings with a new id for `host` only, at another address than the id it refused
    (address_taken). The player's one id stays: the friends' home hosts know it, with its teams and worlds."""
    account, secret = player_id_for(settings, host)
    fresh = with_new_player_id(dict(settings, account=account, password=secret))
    settings = dict(settings)
    settings['server_ids'] = dict(settings.get('server_ids') or {})
    settings['server_ids'][server_key(host)] = [fresh['account'], fresh['password']]
    return settings


def server_ids_of(saved):
    """The valid entries of coop.json's server_ids ({} in a file from before it)."""
    found = saved.get('server_ids') if isinstance(saved, dict) else None
    if not isinstance(found, dict):
        return {}
    return {str(host): [pair[0], pair[1]] for host, pair in found.items()
            if isinstance(pair, list) and len(pair) == 2 and all(isinstance(v, str) for v in pair)
            and valid_name(pair[0]) and valid_secret(pair[1])}


# The link's server keys (link version 4, docs/HOSTED_SERVER.md): the game encrypts its link to the
# server's public key, and a server without the key's secret cannot answer it. A public server's key
# ships here, by its name as typed (server_key): its operator reads it from shadNet's log or the admin
# `key` command. Any other server's (a friend's home host) is trusted the first time, as the game's
# log says it (p2p_sockets.cpp), and kept in coop.json's server_keys.
KNOWN_SERVER_KEYS = {}
LEARNED_KEY_LINE = "Server link: the server's key, trusted on first use: "
OTHER_KEY_LINE = 'Server link: the server says another key than the one pinned'
OTHER_KEY_TEXT = ("The server answered with another key than the one kept for it: someone else may be answering "
                  "for it, or its operator made a new key. If the operator says so, remove the server's line "
                  "from server_keys in user\\coop.json.")


def valid_link_key(value):
    return isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) is not None


def link_key_for(settings, host):
    """The key the game pins for `host`: the shipped one, else the one kept at its first link, else ''
    (the game trusts the first one)."""
    known = KNOWN_SERVER_KEYS.get(server_key(host))
    if known:
        return known
    return (settings.get('server_keys') or {}).get(server_key(host), '')


def with_link_key(settings, host, key):
    """A copy of the settings keeping `key` for `host`."""
    settings = dict(settings)
    settings['server_keys'] = dict(settings.get('server_keys') or {})
    settings['server_keys'][server_key(host)] = key
    return settings


def server_keys_of(saved):
    """The valid entries of coop.json's server_keys ({} in a file from before it)."""
    found = saved.get('server_keys') if isinstance(saved, dict) else None
    if not isinstance(found, dict):
        return {}
    return {str(host): key for host, key in found.items() if valid_link_key(key)}


def learned_key(line):
    """The key a line of the game's log says it trusted on first use, or None: only the game's own line
    (its level, thread, file:line and function, then the words; common/logging/log.h), never one carried inside other text."""
    found = re.search(r'(?:^|\] )<Info> (?:\([^()]*\) )?\S+:\d+ \w+: ' + re.escape(LEARNED_KEY_LINE) + r'([0-9a-f]{64})\s*$',
                      line)
    return found.group(1) if found else None


def learn_link_key(settings, host, key):
    """The settings keeping `key`, the game's first-use key for `host`, or None when `host` has one
    already (shipped or kept): a key from the log never replaces one."""
    if link_key_for(settings, host):
        return None
    return with_link_key(settings, host, key)


def other_key(line):
    """Whether a line of the game's log says the server answered with another key than the pinned one."""
    return OTHER_KEY_LINE in line


# How the game's log says the server refused its hello (p2p_sockets.cpp OnWelcome, an error: on stderr,
# so in console.txt, which the launcher follows), and the refusal of a new id whose virtual address
# another account has (server link_server.cpp RefusalOf).
REFUSED_LINE = 'Server link: the server refused this game: '
ADDRESS_TAKEN = 'address taken:'
# New ids made after such refusals before the launcher gives up and says so.
NEW_ID_TRIES = 3
NEW_ID_TEXT = 'The server already has a player at your id\'s address: making a new id and starting again…'
NEW_ID_FAILED_TEXT = ("The server refused {} new ids (address taken). Try again later, or tell the server's "
                      'operator.')


def address_taken(text):
    """Whether the game's log (a line, or the several lines the launcher reads at once) has the server
    refusing this id's address."""
    for line in text.splitlines():
        _before, found, reason = line.partition(REFUSED_LINE)
        if found and reason.lstrip().startswith(ADDRESS_TAKEN):
            return True
    return False


def id_retry(line, tries):
    """What the game's log asks for after `tries` new ids: 'retry' (a new id and start again),
    'give_up' (tell the player), or None (not an address refusal)."""
    if not address_taken(line):
        return None
    return 'retry' if tries < NEW_ID_TRIES else 'give_up'


def settings_path(install):
    return Path(install) / 'user' / 'coop.json'


def from_settings_bat(path):
    """The answers of the old release's settings.bat (1 - SETUP), as coop.json keeps them."""
    values = {}
    for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
        match = re.fullmatch(r'set "([A-Z0-9_]+)=(.*)"', line.strip())
        if match:
            values[match.group(1)] = match.group(2)
    settings = dict(DEFAULTS)
    settings.update({'game': values.get('GAME', ''),
                     'update': '' if values.get('UPDATE', 'none') == 'none' else values['UPDATE'],
                     'role': values.get('ROLE', 'guest') if values.get('ROLE') in ('host', 'guest') else 'guest',
                     'host_ip': values.get('HOSTIP', ''), 'account': values.get('NPID', ''),
                     'password': values.get('PASS', ''), 'p2p_port': values.get('P2PPORT', 'auto') or 'auto',
                     'harder': values.get('HARDER', 'off') or 'off'})
    return settings


def load_settings(install):
    """coop.json, else the old settings.bat beside BLauncher.exe, else the defaults; with the player's
    id, saved the first time it is made."""
    settings = read_settings(install)
    if not (valid_name(settings['account']) and valid_secret(settings['password'])):
        settings = with_player_id(settings)
        try:
            save_settings(install, settings)
        except OSError:
            pass
    return settings


def read_settings(install):
    path = settings_path(install)
    try:
        saved = json.loads(path.read_text(encoding='utf-8'))
        settings = {key: str(saved.get(key, default)) for key, default in DEFAULTS.items()}
        settings['server_ids'] = server_ids_of(saved)
        settings['server_keys'] = server_keys_of(saved)
        return settings
    except (OSError, ValueError, AttributeError):
        pass
    bat = Path(install) / 'settings.bat'
    if bat.is_file():
        try:
            return from_settings_bat(bat)
        except OSError:
            pass
    return dict(DEFAULTS)


def save_settings(install, settings):
    path = settings_path(install)
    path.parent.mkdir(parents=True, exist_ok=True)
    saved = {key: settings.get(key, default) for key, default in DEFAULTS.items()}
    if settings.get('server_ids'):  # only once a server refused the id: older files stay as they were
        saved['server_ids'] = settings['server_ids']
    if settings.get('server_keys'):
        saved['server_keys'] = settings['server_keys']
    path.write_text(json.dumps(saved, indent=2) + '\n', encoding='utf-8')


# --- the game ---------------------------------------------------------------------------------

def read_sfo(path):
    """A PS4 param.sfo's text and number values ({} when there is none)."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return {}
    if len(data) < 20 or data[:4] != b'\0PSF':
        return {}
    keys_at, data_at, count = struct.unpack_from('<III', data, 8)
    values = {}
    for index in range(min(count, 256)):
        entry = 20 + 16 * index
        if entry + 16 > len(data):
            break
        key_offset, fmt, length, _maximum, value_offset = struct.unpack_from('<HHIII', data, entry)
        key_start = keys_at + key_offset
        key_end = data.find(b'\0', key_start)
        key = data[key_start:key_end].decode('ascii', 'replace')
        at = data_at + value_offset
        if fmt == 0x0404:
            values[key] = struct.unpack_from('<I', data, at)[0] if at + 4 <= len(data) else 0
        else:
            values[key] = data[at:at + length].rstrip(b'\0').decode('utf-8', 'replace')
    return values


def game_version(folder):
    """(title id, app version) of a game folder, or None."""
    sfo = read_sfo(Path(folder) / 'sce_sys' / 'param.sfo')
    return (str(sfo.get('TITLE_ID', '')), str(sfo.get('APP_VER', ''))) if sfo else None


def untested_warning(title):
    """(text, format args) of the warning for a game co-op was not tested on, None for the tested one."""
    if title and title != TESTED_TITLE:
        return UNTESTED_EDITION_TEXT, (title,)
    return None


def is_game_folder(folder):
    return (Path(folder) / 'eboot.bin').is_file()


def is_bloodborne(folder):
    version = game_version(folder)
    return is_game_folder(folder) and (version is None or version[0] in TITLE_IDS or not version[0])


def find_game_folder(folder, depth=3):
    """The game folder (the one with eboot.bin) at or inside `folder`, not an update or mods one."""
    folder = Path(folder)
    if not folder.is_dir():
        return None
    if is_game_folder(folder):
        return folder
    if depth <= 0:
        return None
    try:
        children = sorted((entry for entry in folder.iterdir() if entry.is_dir()), key=lambda p: p.name.lower())
    except OSError:
        return None
    for child in children:
        if child.name.lower().endswith(('-patch', '-mods')):
            continue
        found = find_game_folder(child, depth - 1)
        if found:
            return found
    return None


def update_folder(game):
    """The 1.09 update dumped beside the game (CUSA03173-patch), if any."""
    game = Path(game)
    title = (game_version(game) or (game.name, ''))[0] or game.name
    for candidate in (game.parent / f'{game.name}-patch', game.parent / f'{title}-patch'):
        if is_game_folder(candidate):
            return candidate
    return None


# Folders never worth walking into while looking for the game.
SKIPPED = {'windows', '$recycle.bin', 'system volume information', 'programdata', 'recovery',
           'perflogs', 'msocache', 'node_modules', '.git', 'appdata', 'winsxs', 'config.msi',
           'coop-game', 'windowsapps', 'intel', 'amd', 'nvidia'}


# Folders around an install that hold other programs, not a game.
CROWDED = {'programs', 'local', 'appdata', 'roaming', 'program files', 'program files (x86)'}


def search_game(install, drives=None, homes=None, depth=5, budget_s=20.0, near=None):
    """A Bloodborne game folder on this PC: first next to `near` (a folder the player picked) and
    around the install, then in the usual places in the home folder, then each drive, breadth
    first to `depth` levels, for at most `budget_s` seconds. None when there is none."""
    install = Path(install)
    deadline = time.monotonic() + budget_s
    if homes is None:
        home = Path.home()
        homes = [home / name for name in ('Downloads', 'Desktop', 'Documents', 'Games')]
    if drives is None:
        drives = fixed_drives()
    first = []
    if near:
        near = Path(near)
        first += [near, near.parent]
    first.append(install / 'game')
    # Beside the install, unless that is a folder of many programs: installed per user, the parents
    # are %LOCALAPPDATA%\Programs and %LOCALAPPDATA% (thousands of folders, walked with no time
    # limit while the launcher waited).
    # A drive's root is left to the timed search below (the setup's default is D:\Games\Yharnam Together).
    for folder in (install.parent, install.parent.parent):
        if folder.name.lower() not in CROWDED and folder.parent != folder:
            first.append(folder)
    for folder in first:
        found = find_game_folder(folder, 2) if folder and Path(folder).is_dir() else None
        if found and is_bloodborne(found):
            return found
    queue = collections.deque((Path(root), 0) for root in [*homes, *drives])
    seen = set()
    while queue and time.monotonic() < deadline:
        folder, level = queue.popleft()
        key = str(folder).lower()
        if key in seen:
            continue
        seen.add(key)
        try:
            entries = list(os.scandir(folder))
        except OSError:
            continue
        names = {entry.name.lower() for entry in entries}
        if 'eboot.bin' in names and is_bloodborne(folder) and not folder.name.lower().endswith(('-patch', '-mods')):
            return folder
        if level >= depth:
            continue
        for entry in sorted(entries, key=lambda e: e.name.lower()):
            try:
                if not entry.is_dir(follow_symlinks=False) or entry.name.lower() in SKIPPED:
                    continue
            except OSError:
                continue
            if entry.name.startswith('.'):
                continue
            queue.append((Path(entry.path), level + 1))
    return None


def fixed_drives():
    """The PC's local drives (C:\\, D:\\ ...); network and removable ones are left out."""
    if sys.platform != 'win32':
        return [Path('/')]
    drives = []
    mask = ctypes.windll.kernel32.GetLogicalDrives()
    for index in range(26):
        if mask & (1 << index):
            root = f'{chr(65 + index)}:\\'
            if ctypes.windll.kernel32.GetDriveTypeW(root) == 3:  # DRIVE_FIXED
                drives.append(Path(root))
    return drives


# --- the game the port runs: links to the player's game with the update over it ---------------

def is_junction(path):
    path = Path(path)
    try:
        return path.is_junction() if hasattr(path, 'is_junction') else bool(
            os.lstat(path).st_file_attributes & 0x400)
    except (OSError, AttributeError):
        return False


def make_junction(link, target):
    if sys.platform == 'win32':
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        os.symlink(target, link, target_is_directory=True)


def remove_view(path):
    """Removes a folder made of links: a link goes as a link, never what it points at."""
    path = Path(path)
    if is_junction(path) or path.is_symlink():
        os.rmdir(path) if is_junction(path) else path.unlink()
        return
    if not path.exists():
        return
    for entry in list(path.iterdir()):
        if is_junction(entry) or entry.is_symlink():
            os.rmdir(entry) if is_junction(entry) else entry.unlink()
        elif entry.is_dir():
            remove_view(entry)
        else:
            entry.unlink()
    path.rmdir()


def link_file(link, target):
    try:
        os.link(target, link)
    except OSError:  # another drive, or a file system without hard links
        shutil.copy2(target, link)


def build_game_view(game, update, view):
    """`view`: the game with `update` over it. A folder only one of them has is a link to it, a folder
    both have is merged, a file is the update's when it has one. The player's folders stay as
    they are."""
    view = Path(view)
    if view.exists() or is_junction(view):
        remove_view(view)
    _merge(Path(game), Path(update) if update else None, view)


def _merge(game, update, view):
    view.mkdir(parents=True, exist_ok=True)
    entries = {}
    for entry in game.iterdir():
        entries[entry.name.lower()] = [entry, None]
    if update and update.is_dir():
        for entry in update.iterdir():
            entries.setdefault(entry.name.lower(), [None, None])[1] = entry
    for own, newer in entries.values():
        if own and newer and own.is_dir() and newer.is_dir():
            _merge(own, newer, view / own.name)
            continue
        entry = newer or own
        if entry.is_dir():
            make_junction(view / entry.name, entry)
        else:
            link_file(view / entry.name, entry)


def view_root(install):
    """Where coop-game goes: beside BLauncher.exe, or under %LOCALAPPDATA% when that drive cannot
    hold folder links (exFAT, FAT32)."""
    here = Path(install) / 'coop-game'
    if junctions_work(here):
        return here
    return Path(os.environ.get('LOCALAPPDATA', Path.home())) / 'BloodborneSeamlessCoop' / 'coop-game'


def junctions_work(folder):
    folder = Path(folder)
    probe = folder / '.junction-probe'
    try:
        folder.mkdir(parents=True, exist_ok=True)
        if is_junction(probe):
            os.rmdir(probe)
        make_junction(probe, folder)
        os.rmdir(probe)
        return True
    except OSError:
        return False


# --- the co-op event files ----------------------------------------------------------------------

def event_differences(made, reference):
    """The event files that differ from the ones this release was tested with (or that only one of
    the two has): every player in a party needs the same ones."""
    mine, tested = made.get('files', {}), reference.get('files', {})
    names = sorted(set(mine) | set(tested))
    return [name for name in names
            if name not in mine or name not in tested
            or mine[name].get('output_sha256') != tested[name].get('output_sha256')]


def script_command(install, script, *args):
    """BLauncher.exe --script (no Python needed), or Python from a source tree."""
    if getattr(sys, 'frozen', False):
        return [sys.executable, '--script', str(script), *map(str, args)]
    return [sys.executable, str(script), *map(str, args)]


def prepare(install, settings, say=print):
    """What 1 - SETUP did after its questions: the game view with the update over it, the co-op
    event files made from the player's own game (and compared with the tested ones), and the
    game server pointed at the host. Returns the coop-game folder; raises RuntimeError with a
    message for the player."""
    install = Path(install)
    game = find_game_folder(settings['game'])
    if not game:
        raise RuntimeError(f'No Bloodborne game folder (with eboot.bin) at {settings["game"]}.')
    title, version = game_version(game) or ('?', '?')
    update = None
    if version != GAME_VERSION:
        update = find_game_folder(settings['update'], 0) if settings.get('update') else update_folder(game)
        if not update:
            raise RuntimeError(f'Your game is version {version}; co-op needs the 1.09 update over it. '
                               'Put the dumped update folder next to the game (CUSA03173-patch) '
                               'or install the game and update from your .pkg files.')
    root = view_root(install)
    say('Preparing the game folder (links to your game, nothing is copied)…')
    view = root / 'CUSA03173'
    build_game_view(game, update, view)
    merged = game_version(view)
    if not merged or merged[1] != GAME_VERSION:
        raise RuntimeError(f'The game is version {merged[1] if merged else "?"} with the update over it; '
                           'co-op needs 01.09.')
    game_events = view / 'dvdroot_ps4' / 'event'
    if not (game_events / 'common.emevd.dcx').is_file():
        raise RuntimeError('No dvdroot_ps4\\event\\common.emevd.dcx in the game folder: is it a complete dump?')
    say('Making the co-op event files from your game…')
    events = install / 'event-patch'
    if events.exists():
        shutil.rmtree(events)
    log = install / 'user' / 'log' / 'event-patch.txt'
    log.parent.mkdir(parents=True, exist_ok=True)
    with open(log, 'w', encoding='utf-8') as out:
        code = subprocess.call(script_command(install, install / 'tools' / 'party_boss_events.py', events, game_events),
                               stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                               creationflags=NO_WINDOW, cwd=install)
    manifest = events / 'manifest.json'
    if code != 0 or not manifest.is_file():
        raise RuntimeError(f'Making the co-op event files failed (log: {log}).')
    note = ''
    reference = install / 'event-reference.json'
    if reference.is_file():
        differ = event_differences(json.loads(manifest.read_text(encoding='utf-8')),
                                   json.loads(reference.read_text(encoding='utf-8')))
        if differ:
            note = ('Your event files differ from the tested ones (' + ', '.join(differ) + '): another game '
                    'version or edition. Co-op may still work if every player has the same files.')
    mods = root / 'CUSA03173-mods'
    if mods.exists():
        remove_view(mods)
    copy_events(install, root)
    if title != TESTED_TITLE:
        note = (note + ' ' if note else '') + (f'This is {title}; co-op is tested on {TESTED_TITLE}. Every player '
                                               'in a party must use the same edition.')
    return root, note


def copy_events(install, root):
    """The co-op event files into the mods folder the port reads beside the game (every start, as
    2 - PLAY did: a newer release brings newer ones)."""
    target = Path(root) / 'CUSA03173-mods' / 'dvdroot_ps4' / 'event'
    target.mkdir(parents=True, exist_ok=True)
    for file in (Path(install) / 'event-patch').glob('*'):
        if file.is_file():
            shutil.copy2(file, target / file.name)


def prepared_key(settings):
    """What the prepared game folder was made from: preparing again when it changes."""
    return json.dumps([settings.get('game', ''), settings.get('update', '')])


def file_sha256(path):
    import hashlib
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def events_current(install):
    """Whether the event files were made by this install's event patcher (its manifest names the
    patcher's hash): a release with a new patcher makes them again (Kaniel's playtest 2026-10-17, the
    guests' night after Vicar Amelia)."""
    manifest = Path(install) / 'event-patch' / 'manifest.json'
    tool = Path(install) / 'tools' / 'party_boss_events.py'
    try:
        made = json.loads(manifest.read_text(encoding='utf-8'))
        return made.get('tool_sha256') == file_sha256(tool)
    except (OSError, ValueError):
        return False


def needs_prepare(install, settings):
    root = view_root(install)
    return (settings.get('prepared') != prepared_key(settings) or not is_game_folder(root / 'CUSA03173')
            or not events_current(install))


def host_overrides(host_ip):
    """user\\host_overrides.custom.json: the game's own server is the host's backend."""
    return json.dumps({GAME_SERVER: f'http://{host_ip}:{BACKEND_PORT}'}, indent=2) + '\n'


def import_saves(install, source):
    """Copies a character's saves into user\\savedata\\1 (a shadPS4 'user' folder, an old co-op
    folder, or a savedata folder holding CUSA... folders). Returns the folder copied from, or None."""
    source = Path(source)
    for candidate in (source / 'home' / '1000' / 'savedata', source / 'user' / 'savedata' / '1',
                      source / 'savedata' / '1', source):
        if candidate.is_dir() and any(child.is_dir() and child.name.upper().startswith('CUSA')
                                      for child in candidate.iterdir()):
            target = Path(install) / 'user' / 'savedata' / '1'
            target.mkdir(parents=True, exist_ok=True)
            shutil.copytree(candidate, target, dirs_exist_ok=True)
            return candidate
    return None


# --- playing ------------------------------------------------------------------------------------

def unlocked_fps(fps_mode):
    """Whether a launcher frame rate mode plays co-op unlocked (experimental, Kaniel 2026-10-22: the
    choice itself, with a warning when picked, not a switch): Unlocked and 90; 60 and 30 keep the
    game's fixed step."""
    return fps_mode in ('uncap', '90')


def game_environment(env, settings, install, root):
    """The game's environment for co-op, as 2 - PLAY set it, over the launcher's own."""
    install, root = Path(install), Path(root)
    env = dict(env)
    launcher_fps = env.get('BB_FPS', 'uncap')
    host = settings['host_ip']
    env.update({
        'BB_DATA_DIR': str(install), 'BB_USER_DIR': str(install / 'user'),
        'BB_GAME_DIR': str(root / 'CUSA03173'), 'BB_FPS': '60',
        'BB_LOG_FILE': str(install / 'user' / 'log' / 'shad_log.txt'), 'BB_LOG_LEVEL': 'info',
        'BB_ONLINE': '1',
        'BB_SHADNET_SERVER': f'{host}:{MATCHING_PORT}', 'BB_SHADNET_WEBAPI': f'http://{host}:{WEBAPI_PORT}',
        'BB_SHADNET_NPID': settings['account'], 'BB_SHADNET_PASSWORD': settings['password'],
        'SHADPS4_BLOODBORNE_SEAMLESS_COOP': '1',
        'SHADPS4_BLOODBORNE_RE_TRACE': '0',
    })
    # The server's key, when one is known for it (link_key_for); else the game trusts the first one.
    env.pop('BB_SHADNET_SERVER_KEY', None)
    if valid_link_key(settings.get('link_key', '')):
        env['BB_SHADNET_SERVER_KEY'] = settings['link_key']
    # The party is the team made in the game's team screen: never a setting.
    for key in ('SHADPS4_BLOODBORNE_PARTY', 'SHADPS4_BLOODBORNE_PARTY_ROLE'):
        env.pop(key, None)
    # Co-op is the fixed 60 FPS mode for everyone (a host above 60 made the guests see everyone
    # freeze and fly): no frame cap from the launcher. The opt-in unlocked frame rate keeps the
    # launcher's mode and cap (settings 'unlocked_fps' '1': the launcher picked Unlocked or 90,
    # unlocked_fps); run.py then caps it and paces the game's packets.
    env.pop('BB_ONLINE_UNLOCKED_FPS', None)
    if settings.get('unlocked_fps') == '1':
        env['BB_FPS'] = launcher_fps
        env['BB_ONLINE_UNLOCKED_FPS'] = '1'
    else:
        env.pop('BB_FPS_LIMIT', None)
    for key in ('SHADPS4_P2P_PORT', 'SHADPS4_BLOODBORNE_HARDER_ENEMIES', 'SHADPS4_BLOODBORNE_PARTY_DEATH',
                'BB_COMMUNITY_SERVER'):
        env.pop(key, None)
    port = str(settings.get('p2p_port', 'auto'))
    if port not in ('auto', '', '51610', '51611'):
        env['SHADPS4_P2P_PORT'] = port
    harder = str(settings.get('harder', 'off'))
    if harder != 'off' and valid_harder(harder):
        env['SHADPS4_BLOODBORNE_HARDER_ENEMIES'] = harder
    return env


REPORTS_SENT = 'reports-sent.json'


def reports_to_send(log):
    """Phase 11: what a crash left for the host's server: the dumps not sent yet, and with a new one
    the crashed run's logs (the .prev ones the start just made)."""
    log = Path(log)
    try:
        sent = set(json.loads((log / REPORTS_SENT).read_text(encoding='utf-8')))
    except (OSError, ValueError, TypeError):
        sent = set()
    dumps = sorted(path for path in log.glob('crash-*.dmp') if path.name not in sent)
    if not dumps:
        return []
    logs = sorted(path for path in log.glob('*.prev.txt') if path.is_file())
    return dumps + logs


def note_reports_sent(log, paths):
    log = Path(log)
    try:
        sent = json.loads((log / REPORTS_SENT).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        sent = []
    sent = sorted(set(sent) | {Path(path).name for path in paths if Path(path).suffix == '.dmp'})
    (log / REPORTS_SENT).write_text(json.dumps(sent, indent=1) + '\n', encoding='utf-8')


def report_request(host_ip, path, data, player, secret):
    return urllib.request.Request(f'http://{host_ip}:{WEBAPI_PORT}/bb/report/{Path(path).name}', data=data,
                                  method='POST', headers={'X-Player': player, 'X-Secret': secret,
                                                          'Content-Type': 'application/octet-stream'})


def send_reports(install, settings, timeout=30):
    """Sends what reports_to_send finds to the host's server; the number sent. Off the window's
    thread: a host that does not answer only costs the wait."""
    log = Path(install) / 'user' / 'log'
    sent = []
    for path in reports_to_send(log):
        try:
            request = report_request(settings['host_ip'], path, path.read_bytes(), settings['account'],
                                     settings['password'])
            with urllib.request.urlopen(request, timeout=timeout) as answer:
                if answer.status == 200:
                    sent.append(path)
        except (OSError, ValueError):
            break
    if sent:
        note_reports_sent(log, sent)
    return len(sent)


def rotate_logs(install):
    """shad_log.txt and console.txt become the .prev ones, as 2 - PLAY kept them."""
    log = Path(install) / 'user' / 'log'
    log.mkdir(parents=True, exist_ok=True)
    for name in ('shad_log', 'console'):
        if (log / f'{name}.txt').is_file():
            shutil.copy2(log / f'{name}.txt', log / f'{name}.prev.txt')
    return log / 'console.txt'


# --- the host's servers -------------------------------------------------------------------------

def port_open(port, host='127.0.0.1', timeout=0.3):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def servers_running():
    return port_open(SHADNET_PORT)


# The server line when shadnet runs but nothing answers the game's port: the host's game then says it
# failed to acquire the Bloodborne server information (players 2026-10-22, under a green "Running").
NO_BACKEND_TEXT = "Running, but the game's port 20443 doesn't answer: press Stop server, then Start server"


def server_state():
    """'running'; 'no_backend' when shadnet runs but nothing answers the game's port (BACKEND_PORT,
    where the game gets ss.info); 'stopped'."""
    if not servers_running():
        return 'stopped'
    return 'running' if port_open(BACKEND_PORT) else 'no_backend'


def server_environment(host_ip, env=None):
    """shadnet's environment: the host's address for ss.info, and its log on stderr. Without a console
    (the launcher starts it detached) Qt sends the log to the debugger, and user\\log\\shadnet.txt
    stayed empty on every PC (2026-10-22). Info and up only: the debug lines are one per request."""
    env = dict(os.environ if env is None else env)
    env.update(SHADNET_BACKEND_ADDRESS=host_ip, QT_FORCE_STDERR_LOGGING='1', QT_LOGGING_RULES='*.debug=false')
    return env


def server_command(install):
    """shadnet under its watchdog (phase 11): started again after a crash or a hang."""
    return [str(Path(install) / 'server' / 'shadnet.exe'), '--watchdog']


def start_servers(install, host_ip, wait_s=10.0):
    """HOST - Start Servers: shadnet (accounts, matchmaking, the party boards; it answers the game's
    backend port itself), and the Python backend only when nothing answers that port. Returns the
    lines to show."""
    install = Path(install)
    server = install / 'server'
    log = install / 'user' / 'log'
    log.mkdir(parents=True, exist_ok=True)
    said = []
    if not servers_running():
        env = server_environment(host_ip)
        try:
            shutil.copy2(log / 'shadnet.txt', log / 'shadnet.prev.txt')
        except OSError:
            pass  # no log yet
        with open(log / 'shadnet.txt', 'w', encoding='utf-8') as out:
            subprocess.Popen(server_command(install), cwd=server, env=env, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, creationflags=NO_WINDOW | DETACHED)
        said.append('Started the co-op server.')
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline and not (servers_running() and port_open(BACKEND_PORT)):
        time.sleep(0.25)
    if not port_open(BACKEND_PORT):
        backend = install / 'backend'
        with open(log / 'backend.txt', 'a', encoding='utf-8') as out:
            subprocess.Popen(script_command(install, backend / 'bb_dummy_server.py', '--address', host_ip, '--port',
                                            BACKEND_PORT, '--shadnet-webapi', f'http://{host_ip}:{WEBAPI_PORT}'),
                             cwd=backend, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             creationflags=NO_WINDOW | DETACHED)
        said.append('Started the backend.')
    if not servers_running():
        said.append(f'The co-op server did not start (log: {log / "shadnet.txt"}).')
    return said


def our_processes(install):
    """(pid, name, command line) of this install's shadnet (its watchdog too) and backend."""
    install = str(Path(install).resolve()).lower()
    script = ("Get-CimInstance Win32_Process -Filter \"Name='shadnet.exe' OR Name='BLauncher.exe' OR "
              "Name='python.exe' OR Name='pythonw.exe'\" | ForEach-Object { \"$($_.ProcessId)|$($_.Name)|"
              "$($_.ExecutablePath)|$($_.CommandLine)\" }")
    try:
        out = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True,
                             timeout=20, creationflags=NO_WINDOW).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    found = []
    for line in out.splitlines():
        parts = line.split('|', 3)
        if len(parts) < 4:
            continue
        pid, name, path, command = parts
        if name.lower() == 'shadnet.exe' and path.lower().startswith(install):
            found.append((int(pid), name, command))
        elif 'bb_dummy_server.py' in command and install in command.lower():
            found.append((int(pid), name, command))
    return found


def stop_order(found):
    """The watchdog first: stopped after its server, it would start that again."""
    return sorted(found, key=lambda process: '--watchdog' not in process[2])


def stop_servers(install):
    stopped = 0
    for pid, _name, _command in stop_order(our_processes(install)):
        subprocess.run(['taskkill', '/PID', str(pid), '/F'], capture_output=True, creationflags=NO_WINDOW)
        stopped += 1
    return stopped


# --- Windows Firewall ---------------------------------------------------------------------------

RULE_GAME, RULE_HOST_TCP, RULE_HOST_UDP = 'BB coop port', 'BB coop host TCP', 'BB coop host UDP'


def firewall_commands(install, host):
    """The netsh lines that let the game (and for the host the servers) through Windows Firewall."""
    probe = Path(install) / 'bin' / 'bb-probe.exe'
    lines = [f'netsh advfirewall firewall delete rule name="{RULE_GAME}" & '
             f'netsh advfirewall firewall add rule name="{RULE_GAME}" dir=in action=allow '
             f'program="{probe}" enable=yes profile=any']
    if host:
        lines.append(f'netsh advfirewall firewall delete rule name="{RULE_HOST_TCP}" & '
                     f'netsh advfirewall firewall add rule name="{RULE_HOST_TCP}" dir=in action=allow protocol=TCP '
                     f'localport={WEBAPI_PORT},{BACKEND_PORT} enable=yes profile=any')
        lines.append(f'netsh advfirewall firewall delete rule name="{RULE_HOST_UDP}" & '
                     f'netsh advfirewall firewall add rule name="{RULE_HOST_UDP}" dir=in action=allow protocol=UDP '
                     f'localport={MATCHING_PORT} enable=yes profile=any')
    return lines


def firewall_ready(install, host):
    """Whether this install's rules are there (reading rules needs no administrator)."""
    probe = str(Path(install) / 'bin' / 'bb-probe.exe').lower()
    names = [RULE_GAME] + ([RULE_HOST_TCP, RULE_HOST_UDP] if host else [])
    for name in names:
        try:
            out = subprocess.run(['netsh', 'advfirewall', 'firewall', 'show', 'rule', f'name={name}', 'verbose'],
                                 capture_output=True, text=True, timeout=15, creationflags=NO_WINDOW)
        except (OSError, subprocess.TimeoutExpired):
            return False
        if out.returncode != 0 or (name == RULE_GAME and probe not in out.stdout.lower()):
            return False
    return True


def allow_through_firewall(install, host):
    """Asks Windows for the administrator once (the UAC prompt) and adds the rules. True when the
    player said yes (the rules are then checked again by the caller)."""
    command = ' & '.join(firewall_commands(install, host))
    result = ctypes.windll.shell32.ShellExecuteW(None, 'runas', 'cmd.exe', f'/c {command}', None, 0)
    return result > 32


# --- updates (Kaniel 2026-10-16) ---------------------------------------------------------------
# The launcher looks for a newer release of this repository: every release counts, pre-releases too
# (everyone runs one build). A release carries YharnamTogether-v<version>-setup.exe (any -setup.exe counts) and its
# .sha256; the update downloads both, checks the file against the sum, and runs the setup silently
# into this install (it keeps user\), which starts the launcher again when it is done.
# By repository id, not owner/name: the id survives a rename of the owner.
RELEASES_API = 'https://api.github.com/repositories/1405692683/releases?per_page=100'  # every one: Home's notes
SETUP_SUFFIX = '-setup.exe'


def version_key(version):
    """A sort key for '0.3.4', '0.3.3-local' or 'v0.1.4': the numbers, then 1 for a plain version and 0
    for one with a suffix (0.3.3 is above 0.3.3-local), then the suffix."""
    match = re.match(r'v?(\d+(?:\.\d+)*)(?:[-+](\S+))?$', (version or '').strip())
    if not match:
        return (), 0, ''
    numbers = tuple(int(part) for part in match.group(1).split('.'))
    numbers += (0,) * (4 - len(numbers))
    return numbers, 0 if match.group(2) else 1, match.group(2) or ''


def newest_release(releases):
    """The newest release in GitHub's /releases list that carries a setup .exe and its sum, pre-releases
    included and drafts never: {'version', 'setup', 'sha256', 'page', 'notes'}, or None."""
    if not isinstance(releases, list):
        raise ValueError('GitHub did not return a list of releases')
    best = None
    for release in releases:
        if not isinstance(release, dict) or release.get('draft') or not isinstance(release.get('tag_name'), str):
            continue
        version = release['tag_name'].lstrip('v')
        if not version_key(version)[0]:
            continue
        assets = {asset.get('name'): asset.get('browser_download_url') for asset in release.get('assets') or []
                  if isinstance(asset, dict) and isinstance(asset.get('name'), str)}
        setup = next((name for name in assets if name.endswith(SETUP_SUFFIX)), None)
        if not setup or not assets.get(setup + '.sha256'):
            continue
        if best is None or version_key(version) > version_key(best['version']):
            best = {'version': version, 'setup': assets[setup], 'sha256': assets[setup + '.sha256'],
                    'page': release.get('html_url') or '', 'notes': (release.get('body') or '')[:2000],
                    'name': setup}
    return best


def fetch_releases(timeout=15):
    """GitHub's /releases list as it is now (raises OSError or ValueError when it cannot be read)."""
    request = urllib.request.Request(RELEASES_API, headers={'Accept': 'application/vnd.github+json',
                                                            'User-Agent': 'bloodborne-coop-launcher'})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode('utf-8'))


def fetch_newest_release(timeout=15):
    """newest_release() of what GitHub lists now (raises OSError or ValueError when it cannot be read)."""
    return newest_release(fetch_releases(timeout))


# Home's patch notes (Kaniel 2026-10-20: every release's, from the latest back to the first) are the
# releases' own: the top-level points of a body, without its install steps ("Getting started") and with
# its known problems ("Known gaps", "Known: ...") apart.
MONTHS = ('Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec')


def plain(text):
    """A line of a release body without its markdown marks (bold, code)."""
    return re.sub(r'\s+', ' ', text.replace('**', '').replace('`', '')).strip()


def release_notes(releases):
    """[{'version', 'date', 'notes', 'known'}] of every published release in GitHub's /releases list, newest
    first; a release without any point is left out."""
    if not isinstance(releases, list):
        return []
    found = []
    for release in releases:
        if not isinstance(release, dict) or release.get('draft') or not isinstance(release.get('tag_name'), str):
            continue
        version = release['tag_name'].lstrip('v')
        if not version_key(version)[0]:
            continue
        notes, known = [], []
        section = notes
        for line in (release.get('body') or '').replace('\r', '').split('\n') if isinstance(release.get('body'), str) else []:
            heading = line.startswith('#') or (line.startswith('**') and line.rstrip().endswith('**') and
                                               line.count('**') == 2)
            if heading:
                title = line.lower()
                section = known if 'known' in title else None if 'getting started' in title else notes
            elif line.startswith('- ') and section is not None and plain(line[2:]):
                section.append(plain(line[2:])[:400])
            elif re.match(r'\s+- ', line) and section and plain(line.strip()[2:]):
                # a sub point goes on its point's line ("Party death is always on: a killing blow ...")
                section[-1] = f'{section[-1]} {plain(line.strip()[2:])}'[:600]
            elif line.startswith('Known:') and plain(line[6:]):
                text = plain(line[6:])
                known.append((text[0].upper() + text[1:])[:400])
        if not notes and not known:
            continue
        match = re.match(r'(\d{4})-(\d\d)-(\d\d)', release.get('published_at') or '')
        date = f'{int(match.group(3))} {MONTHS[int(match.group(2)) - 1]} {match.group(1)}' if match and \
            1 <= int(match.group(2)) <= 12 else ''
        found.append({'version': version, 'date': date, 'notes': notes[:40], 'known': known[:10]})
    found.sort(key=lambda r: version_key(r['version']), reverse=True)
    return found


def update_offer(current, release):
    """The release when it is newer than the running version (an unknown running version, as in a
    source tree, is offered nothing)."""
    if not release or not version_key(current)[0]:
        return None
    return release if version_key(release['version']) > version_key(current) else None


def expected_sum(text, name):
    """The SHA-256 a .sha256 file gives for `name` ('<hex> *<name>' or '<hex>  <name>' or just '<hex>')."""
    for line in (text or '').splitlines():
        parts = line.strip().split()
        if parts and re.fullmatch(r'[0-9a-fA-F]{64}', parts[0]) and (len(parts) == 1 or parts[-1].lstrip('*') == name):
            return parts[0].lower()
    raise ValueError(f'no SHA-256 for {name} in the release')


def download_update(release, folder, progress=None, timeout=30):
    """The release's setup .exe in `folder`, checked against its .sha256 (ValueError when it does not
    match, and the file is gone). progress(fraction) is called while it downloads."""
    import hashlib
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    headers = {'User-Agent': 'bloodborne-coop-launcher'}
    with urllib.request.urlopen(urllib.request.Request(release['sha256'], headers=headers), timeout=timeout) as response:
        want = expected_sum(response.read(4096).decode('utf-8', 'replace'), release['name'])
    target = folder / release['name']
    digest, done = hashlib.sha256(), 0
    with urllib.request.urlopen(urllib.request.Request(release['setup'], headers=headers), timeout=timeout) as response:
        size = int(response.headers.get('Content-Length') or 0)
        with open(target, 'wb') as out:
            for chunk in iter(lambda: response.read(1 << 20), b''):
                out.write(chunk)
                digest.update(chunk)
                done += len(chunk)
                if progress and size:
                    progress(done / size)
    if digest.hexdigest() != want:
        target.unlink(missing_ok=True)
        raise ValueError('the downloaded update does not match its SHA-256; nothing was changed')
    return target


def setup_command(setup, install):
    """The setup run that updates this install in place and starts the launcher again: no questions, but
    its progress window stays on screen with a note not to close anything (/SILENT, and /NOCANCEL takes
    its Cancel button away), so the update does not look like the launcher vanished (Kaniel 2026-10-20)."""
    return [str(setup), '/SILENT', '/NOCANCEL', '/SUPPRESSMSGBOXES', '/NORESTART', '/CLOSEAPPLICATIONS',
            f'/DIR={Path(install)}']
