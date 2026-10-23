"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
"""
import argparse
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

EBOOT_BASE=0x400000
# BB_FPS presets: patch names from patches/Bloodborne.xml (app version 01.09). Their patch lists
# follow shadps4-emu/ps4_cheats PATCHES/Bloodborne.xml of 2026-10-02 (older lists missed timesteps:
# messengers and loading screen pictures replayed their animations).
# Above 60 FPS the sprint fix always goes with the frame rate patch: without it sprinting drops to
# half speed (the game's wall detector measured distance per frame, a 30 FPS rule).
FPS_PRESETS={'30':[],'60':['60 FPS++'],'90':['90 FPS++','Sprint Fix (High FPS)'],
             'uncap':['Uncap FPS++','Sprint Fix (High FPS)']}
# Upscaler presets (bbport.ini "preset", the in-game menu): output / render size ratio. The game
# then renders at 1920x1080 / ratio and the port's temporal upscaler restores the output size.
OUTPUT_SIZE=(1920,1080)
PRESET_SCALES=[1.0,1.5,1.7,2.0,3.0]
# The community patch changes two independent consumers: the game render/window setup
# and the UI movie viewport. Keep the latter at native size so glyph rasterisation and
# vector tessellation do not inherit the scene's FSR resolution.
RESOLUTION_TEMPLATE='Resolution Patch 1280x720 (16:9)'
# Effect switches (bbport.ini, in-game menu, launcher): key -> (patch when the key is 0, patch
# when it is 1). The game reads these at start; a change applies after a restart.
EFFECTS={
    'effect_chromatic_aberration':('Disable Chromatic Aberration',None),
    'effect_dof':('Disable DoF',None),
    'effect_motion_blur':('Disable Motion Blur (perf increase)',None),
    'effect_ssao':('Disable SSAO',None),
    'effect_game_aa':('Disable AA',None),
    'effect_dynamic_shadows':('Disable Dynamic Light Shadows (perf increase)',None),
    'effect_ssr':(None,'Enable Screen Space Reflections (READ NOTE)'),
    'skip_intro':(None,'Skip Intro'),
    'debug_camera':(None,'Restore Debug Camera'),
    'debug_menu':(None,'Restore Debug Menu (READ NOTES)'),
    # Cheats and gameplay tweaks (launcher "Cheats" page).
    'cheat_no_death':(None,'Player No Dead (Read note)'),
    'cheat_stealth':(None,'Player Stealth (Read note)'),
    'cheat_silent':(None,'Player Silent (Read note)'),
    'cheat_rally_no_decay':(None,'No Rally Decay'),
    'cheat_enemy_control':(None,'Enemy Control'),
    'tweak_no_rally':(None,'Disable Rally (HP Regain)'),
    'tweak_camera_distance':(None,'Increased camera distance'),
    'tweak_no_camera_rotation':(None,'Disable Camera Auto Rotation via Movement'),
    'tweak_easy_run':(None,'Sensitive Analog Input (easier to run)'),
    'tweak_ragdoll':(None,'DS1-like physics'),
}
# Build check on join: the patches that change the game for everyone in a party (cheats and gameplay
# tweaks, the launcher's Cheats page) and their short names in the game's build fingerprint; a
# player whose differ is refused by the co-op server. The rest (frame rate, effects, resolution)
# change only how the game looks and are only logged.
GAMEPLAY_PATCHES={
    'Player No Dead (Read note)':'no death',
    'Player Stealth (Read note)':'stealth',
    'Player Silent (Read note)':'silent',
    'No Rally Decay':'rally no decay',
    'Enemy Control':'enemy control',
    'Disable Rally (HP Regain)':'no rally',
}


def build_check_names(names):
    """(the gameplay patches' short names, the other patches' names), each in the order given."""
    gameplay=[GAMEPLAY_PATCHES[n] for n in names if n in GAMEPLAY_PATCHES]
    other=[n for n in names if n not in GAMEPLAY_PATCHES]
    return gameplay,other


def write_build_check_names(out, names):
    """out/patches-names.json: run.py hands the names to the game (BB_PATCH_GAMEPLAY, BB_PATCH_OTHER)."""
    gameplay,other=build_check_names(list(dict.fromkeys(names)))
    (out/'patches-names.json').write_text(json.dumps({'gameplay':gameplay,'other':other})+'\n',encoding='utf-8')


# Intel CPUs: the game's tone mapping turns black (DLC areas most; reported as darker than on the
# PS4): the game code runs natively, and Intel's approximate float instructions differ from the
# PS4's AMD CPU, so a block the game gates on a computed flag never runs. The community fix runs
# it always; on by default on Intel. BB_INTEL_TONEMAP_FIX=0/1 forces it off or on.
INTEL_TONEMAP='Intel Black Tonemap Fix'


def intel_cpu(cpuinfo='/proc/cpuinfo'):
    try:
        with open(cpuinfo, encoding='utf-8', errors='replace') as f:
            return any(line.startswith('vendor_id') and 'GenuineIntel' in line for line in f)
    except OSError:
        return False


# The title's PLAY ONLINE / PLAY OFFLINE dialog: the port has no PSN, the game goes straight to the
# main menu offline. On by default; BB_SKIP_NETWORK_CHOICE=0 (or false, no, off) shows it.
# Seamless co-op plays online (SHADPS4_BLOODBORNE_SEAMLESS_COOP=1): the dialog stays.
SKIP_NETWORK_CHOICE='Skip Online/Offline Choice'


# Two per-frame GX command heaps of 1 MiB instead of 256 KiB: the game writes through NULL when
# one fills (deadinside28 #162, PR #224). On by default; BB_GX_COMMAND_HEAP=0 leaves them.
GX_COMMAND_HEAP='Larger GX Per-Frame Command Heap'


def gx_command_heap(env=os.environ):
    return env.get('BB_GX_COMMAND_HEAP','1')!='0'


def skip_network_choice(env=os.environ):
    if env.get('SHADPS4_BLOODBORNE_SEAMLESS_COOP')=='1':
        return False
    return env.get('BB_SKIP_NETWORK_CHOICE','1').strip().lower() not in ('0','false','no','off')


def intel_tonemap_fix(env=os.environ, cpuinfo='/proc/cpuinfo'):
    forced=env.get('BB_INTEL_TONEMAP_FIX')
    return forced=='1' if forced in ('0','1') else intel_cpu(cpuinfo)

# model_lod: -2 highest, 0 the game's, 1 lower, 2 lowest.
MODEL_LOD={'-2':'Model LOD -2 (Highest)','1':'Model LOD 1 (Lower)','2':'Model LOD 2 (Lowest)'}


# The game version the patches are written for: byte writes at that version's addresses.
APP_VERSION='01.09'


def game_is_patched(version, env=os.environ, app_version=APP_VERSION):
    """Whether the patches apply to a game of this APP_VER: its own, or an unreadable one (None)
    that the game check reports on its own. BB_FORCE_PATCHES applies them to any version. Shared
    by main() and run.sh (--print-patched)."""
    return bool(env.get('BB_FORCE_PATCHES')) or not version or version==app_version


def game_app_version(game):
    """APP_VER of the game folder's param.sfo ("01.09"), or None when it cannot be read."""
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('APP_VER')
    except (OSError, ValueError, ImportError):
        return None


DEBUG_MENU='Restore Debug Menu (READ NOTES)'
# What the patched game reads for the debug menu (BB_FILE_TRACE=1), under dvdroot_ps4. The game asks
# for lower-case names; the PS4's file system and the port's runtime ignore case.
DEBUG_MENU_FONTS=('adhoc/font/DbgFont14h.ccm','adhoc/font/DbgFont14h.tpf')
DEBUG_MENU_SHADERS=('adhoc/FontShader/debugFont_vs.vpo','adhoc/FontShader/debugFont_ps.ppo')


def find_ignoring_case(root, relative):
    """`root`/`relative`, each component matched ignoring case; None when absent."""
    path=Path(root)
    for part in relative.split('/'):
        exact=path/part
        if exact.exists():
            path=exact
            continue
        try:
            matches=[e for e in path.iterdir() if e.name.casefold()==part.casefold()]
        except OSError:
            return None
        if not matches:
            return None
        path=matches[0]
    return path


def debug_menu_problem(game):
    """Why the debug menu patch cannot work with this game folder, or None."""
    dvdroot=Path(game)/'dvdroot_ps4'
    missing=[]
    for relative in DEBUG_MENU_FONTS:
        found=find_ignoring_case(dvdroot,relative)
        if not found or not found.is_file() or found.stat().st_size==0:
            missing.append(relative)
    if not missing:
        return None
    hint=''
    if all(find_ignoring_case(dvdroot,'font/'+r.rsplit('/',1)[1]) for r in missing):
        hint=' (they are in dvdroot_ps4/font: the game reads them from dvdroot_ps4/adhoc/font)'
    return ('the debug menu needs the font files from https://www.nexusmods.com/bloodborne/mods/253 '
            f'in dvdroot_ps4 of the game folder or of a mod: missing {", ".join(missing)}{hint}')


# Game patches that cannot go together: (kept, dropped, why). The effect switches keep the first
# and drop the second with a message; a patch list naming both is refused. (Patches over the
# co-op sites are the other kind of conflict: see coop_conflicts.)
EXCLUSIVE_PATCHES=[
    ('Restore Debug Camera','Enemy Control','it conflicts with the free camera: same buttons'),
]


def validate_patch_requirements(names, game):
    """The patches to apply: raises for conflicts; drops the debug menu (with a message) when its
    font files are missing, as the game crashes opening the menu without them."""
    for kept,dropped,_ in EXCLUSIVE_PATCHES:
        if kept in names and dropped in names:
            raise ValueError(f'{kept} conflicts with {dropped}; enable only one')
    if DEBUG_MENU in names:
        problem=debug_menu_problem(game)
        if problem:
            print(f'Patches: debug menu off: {problem}',file=sys.stderr)
            return [n for n in names if n!=DEBUG_MENU]
        absent=[r for r in DEBUG_MENU_SHADERS if not find_ignoring_case(Path(game)/'dvdroot_ps4',r)]
        if absent:
            print(f'Patches: debug menu: {", ".join(absent)} not found (the mod\'s font shaders); '
                  'the menu may not draw',file=sys.stderr)
    return names


def effect_patches(settings):
    names=[]
    for key,(off,on) in EFFECTS.items():
        if key not in settings: continue
        name=on if settings[key]=='1' else off
        if name: names.append(name)
    for kept,dropped,why in EXCLUSIVE_PATCHES:
        if kept in names and dropped in names:
            names.remove(dropped)
            print(f'Patches: {dropped} skipped ({why})')
    # Rally that never decays means nothing without Rally.
    if 'No Rally Decay' in names and 'Disable Rally (HP Regain)' in names:
        names.remove('No Rally Decay')
    lod=MODEL_LOD.get(settings.get('model_lod','0'))
    if lod: names.append(lod)
    return names
SCENE_WIDTH=0x02196A6B-EBOOT_BASE
SCENE_HEIGHT=0x02196A7A-EBOOT_BASE
UI_WIDTH=0x02358554-EBOOT_BASE
UI_HEIGHT=0x0235855D-EBOOT_BASE


def read_settings(path):
    settings={}
    if path.exists():
        # UTF-8 whatever the Windows code page is (a BOM from Notepad is dropped).
        for line in path.read_text(encoding='utf-8-sig', errors='replace').splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def render_size(settings,override=''):
    """Render resolution for the upscaler preset, or None for native."""
    if override:
        w,h=(int(v) for v in override.lower().split('x'))
        return (w,h)
    if settings.get('upscaler','fsr3')=='off': return None
    preset=int(settings.get('preset','0') or 0)
    scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    if scale==1.0: return None
    # Even sizes (the game has half-resolution buffers).
    return tuple(max(2,round(v/scale/2)*2) for v in OUTPUT_SIZE)


def output_size(settings):
    """Output (UI) size from bbport.ini output_res, e.g. 3840x2160; 1920x1080 by default."""
    try:
        w,h=(int(v) for v in settings.get('output_res','').lower().split('x'))
        if w>0 and h>0: return (w,h)
    except ValueError:
        pass
    return OUTPUT_SIZE


def scaled_sizes(settings):
    """(render, output) for an output other than 1080p (above it, or 720p for the Steam Deck):
    the game renders at output / preset scale (or at the output size without upscaler) and the
    upscaler fills the output. None at 1080p and for TAA (native, live host targets only)."""
    out=output_size(settings)
    if out==OUTPUT_SIZE or settings.get('upscaler')=='taa': return None
    scale=1.0
    if settings.get('upscaler','fsr3')!='off':
        preset=int(settings.get('preset','0') or 0)
        scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    render=tuple(max(2,round(v/scale/2)*2) for v in out)
    # A scene of exactly 1920x1080 (4K Performance) is indistinguishable from the game's UI
    # coordinate space, which the port's UI composition recognizes by that size.
    if render==OUTPUT_SIZE: render=(1916,1078)
    return render,out


def resolution_writes(xml,size,app_version,segments,ui=OUTPUT_SIZE):
    writes=compile_patches(xml,[RESOLUTION_TEMPLATE],app_version,segments)
    replacements={SCENE_WIDTH:(0xB8,0x500,size[0]),
                  SCENE_HEIGHT:(0xB8,0x2D0,size[1]),
                  UI_WIDTH:(0xB8,0x500,ui[0]),
                  UI_HEIGHT:(0xB9,0x2D0,ui[1])}
    out=[]
    seen=set()
    for offset,data in writes:
        if offset in replacements:
            opcode,old,value=replacements[offset]
            if data!=bytes([opcode])+old.to_bytes(3,'little') or offset in seen:
                raise ValueError(f'unexpected resolution patch at {offset+EBOOT_BASE:#x}')
            data=bytes([opcode])+value.to_bytes(3,'little')
            seen.add(offset)
        out.append((offset,data))
    if seen!=replacements.keys():
        raise ValueError('resolution patch is missing scene/UI viewport instructions')
    return out


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def compile_patches(xml, names, app_version, segments):
    return [(offset,data) for _,offset,data in compile_named(xml,names,app_version,segments)]


def compile_named(xml, names, app_version, segments):
    """compile_patches with each write's patch name: [(name, offset, data)]."""
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    missing=[n for n in names if n not in found]
    if missing: raise ValueError(f'patches not found for app version {app_version}: {missing}')
    writes=[]
    for name in names:
        for line in found[name].iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            data=encode(line)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                raise ValueError(f'{name}: address {line.get("Address")} is outside the eboot')
            writes.append((name,offset,data))
    return writes


# Third-party patch files (shadPS4/GoldHEN XML) in the data directory's patches/ folder.
# The retail releases (scripts/game_check.py SUPPORTED_TITLES): one 1.09 executable.
BLOODBORNE_IDS={'CUSA00900','CUSA00207','CUSA00208','CUSA00299','CUSA01363',
                'CUSA03179','CUSA03173','CUSA03014','CUSA03023'}


def external_patches(directory, app_version=APP_VERSION, exclude=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml'):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve(): continue  # the built-in file (patches/ in a checkout)
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        ids={e.text.strip() for e in root.iter('ID') if e.text}
        if ids and not ids&BLOODBORNE_IDS: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text(encoding='utf-8-sig'))
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def compile_external(selected, segments):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole."""
    return [(offset,data) for _,offset,data in compile_external_named(selected,segments)]


def compile_external_named(selected, segments):
    """compile_external with each write's patch key: [(key, offset, data)]."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=[(key,offset,data) for offset,data in ours]
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def drop_replaced_fps(named, fps_names, external_named):
    """An external FPS patch of its own (e.g. "60FPS (no deltatime)", at 60 FPS++'s addresses
    with other values) replaces the built-in preset whole: applied over it, the two mixed
    (Supermedo 8042500). Returns (named without the preset if replaced, the replacing keys)."""
    fps_ranges=[(offset,offset+len(data)) for name,offset,data in named if name in fps_names]
    replacing=[]
    for key,offset,data in external_named:
        if key not in replacing and any(start<offset+len(data) and offset<end for start,end in fps_ranges):
            replacing.append(key)
    if not replacing:
        return named,[]
    return [w for w in named if w[0] not in fps_names],replacing


# The co-op site table: every place in the eboot seamless co-op patches, hooks or calls
# (emulator/src/core/bloodborne_patch_sites.h; its unit test writes this copy). The game patches
# here are applied before co-op installs: one that writes over a co-op site makes co-op skip that
# feature ("Uncap FPS++" once rewrote the sign timers and joins were slow for two days unnoticed).
COOP_SITES=Path(__file__).resolve().parent.parent/'patches/coop-sites.json'


def load_coop_sites(path=COOP_SITES):
    """{'features': {name: required}, 'sites': [{name, feature, kind, offset, original, accepted}]}
    with offsets as ints and the original bytes as bytes."""
    table=json.loads(Path(path).read_text(encoding='utf-8'))
    sites=[dict(site,offset=int(site['offset'],16),original=bytes.fromhex(site['original']))
           for site in table['sites']]
    return {'features':{f['name']:f['required'] for f in table['features']},'sites':sites}


def matches_pattern(data, pattern):
    """Whether DATA is the hex PATTERN ("C7 43 28 ?? ...", ?? for any byte)."""
    pairs=pattern.split()
    return len(data)==len(pairs) and all(p=='??' or int(p,16)==b for p,b in zip(pairs,data))


def matches_coop_site(data, site):
    return bytes(data)==site['original'] or any(matches_pattern(data,v) for v in site['accepted'])


def coop_conflicts(named_writes, coop=None):
    """[(patch, site name, offset, feature, required)] for every co-op site that the writes
    [(patch, offset, data)] leave holding neither its original bytes nor a variant the table
    accepts, once per patch that wrote over it."""
    coop=coop or load_coop_sites()
    conflicts=[]
    for site in coop['sites']:
        start,end=site['offset'],site['offset']+len(site['original'])
        data=bytearray(site['original'])
        writers=[]
        for name,offset,value in named_writes:
            if offset<end and start<offset+len(value):
                first,last=max(offset,start),min(offset+len(value),end)
                data[first-start:last-start]=value[first-offset:last-offset]
                if name not in writers: writers.append(name)
        if writers and not matches_coop_site(data,site):
            required=coop['features'][site['feature']]
            conflicts+=[(name,site['name'],start,site['feature'],required) for name in writers]
    return conflicts


def report_coop_conflicts(conflicts, out=sys.stderr):
    """Says, loudly, which game patch breaks which co-op feature (the launcher shows this output
    and keeps it in last_run.log)."""
    for name,site,offset,feature,required in conflicts:
        kind='required' if required else 'optional'
        print(f'Patches: CO-OP CONFLICT: "{name}" writes over {site} at {offset+EBOOT_BASE:#x}, '
              f'a site of co-op feature "{feature}" ({kind}); seamless co-op skips that feature. '
              'Turn the patch off to play co-op with it.',file=out)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--xml',type=Path,default=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml')
    p.add_argument('--fps',choices=sorted(FPS_PRESETS),default='uncap')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--app-version',default=APP_VERSION)
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--settings',type=Path,default=Path(__file__).resolve().parent.parent/'bbport.ini')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('BB_GAME_DIR','../CUSA03173')))
    p.add_argument('--render-res',default='',help='render resolution WxH (overrides the preset)')
    p.add_argument('--print-preset-size',action='store_true',help='print the selected preset size, if reduced')
    p.add_argument('--output-res',default='',help='output resolution WxH (the upscaler\'s; the UI stays 1920x1080)')
    p.add_argument('--print-scaled',action='store_true',
                   help='print "RENDER OUTPUT" (WxH) when bbport.ini selects an output other than 1080p')
    p.add_argument('--print-patched',action='store_true',
                   help='print "patched" or "unpatched" for the version of --game-dir (run.sh asks this)')
    a=p.parse_args()
    if a.print_patched:
        print('patched' if game_is_patched(game_app_version(a.game_dir),os.environ,a.app_version) else 'unpatched')
        return
    if a.print_scaled:
        sizes=scaled_sizes(read_settings(a.settings))
        if sizes: print(f'{sizes[0][0]}x{sizes[0][1]} {sizes[1][0]}x{sizes[1][1]}')
        return
    if a.print_preset_size:
        settings=read_settings(a.settings)
        if 'BB_UPSCALER' in os.environ:
            settings['upscaler']='fsr3' if os.environ['BB_UPSCALER']=='fsr3' else 'off'
        if 'BB_UPSCALE_PRESET' in os.environ:
            settings['preset']=os.environ['BB_UPSCALE_PRESET']
        size=render_size(settings)
        if size: print(f'{size[0]}x{size[1]}')
        return
    # The patches are byte writes at the addresses of one game version: on another version they
    # would corrupt code. Such a game runs unpatched (run.sh/run.py then choose 30 FPS).
    version=game_app_version(a.game_dir)
    if not game_is_patched(version,os.environ,a.app_version):
        blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,0)
        (a.out/'patches.bin').write_bytes(blob)
        write_build_check_names(a.out,[])
        print(f'Patches: game version {version}, patches are for {a.app_version}: none applied '
              '(30 FPS, no effect or resolution patches)')
        return
    names=FPS_PRESETS[a.fps]+[n.strip() for n in a.extra.split(';') if n.strip()]
    names+=[n for n in effect_patches(read_settings(a.settings)) if n not in names]
    if intel_tonemap_fix() and INTEL_TONEMAP not in names:
        names.append(INTEL_TONEMAP)
    if skip_network_choice() and SKIP_NETWORK_CHOICE not in names:
        names.append(SKIP_NETWORK_CHOICE)
    if gx_command_heap() and GX_COMMAND_HEAP not in names:
        names.append(GX_COMMAND_HEAP)
    names=validate_patch_requirements(names,a.game_dir)
    segments=eboot_segments((a.out/'eboot.elf').read_bytes())
    named=compile_named(a.xml,names,a.app_version,segments)
    size=render_size(read_settings(a.settings),a.render_res) if a.render_res else None
    # The UI keeps the game's 1920x1080 coordinates even for a larger output: the port draws
    # it into the output-size image with a viewport scaled by output / 1920
    # (UiComposition::NativeViewport), so it is rasterized at the output resolution.
    ui=OUTPUT_SIZE
    if size:
        named+=[(RESOLUTION_TEMPLATE,offset,data)
                for offset,data in resolution_writes(a.xml,size,a.app_version,segments,ui)]
        if size[0]*size[1]>OUTPUT_SIZE[0]*OUTPUT_SIZE[1]:
            heap='Increased Graphics Heap Sizes'
            named+=compile_named(a.xml,[heap],a.app_version,segments)
            names.append(heap)
        print(f'Patches: scene {size[0]}x{size[1]}; UI {ui[0]}x{ui[1]}')
    if a.patches_dir:
        # After the built-in ones: an external patch of the same bytes wins.
        external=compile_external_named(external_selection(external_patches(a.patches_dir,a.app_version,a.xml),
                                                           a.patches_config),segments)
        named,replacing=drop_replaced_fps(named,FPS_PRESETS[a.fps],external)
        if replacing:
            print(f'Patches: external {", ".join(replacing)} replaces the built-in '
                  f'{", ".join(FPS_PRESETS[a.fps])}')
            names=[n for n in names if n not in FPS_PRESETS[a.fps]]
        named+=external
    try:
        report_coop_conflicts(coop_conflicts(named))
    except (OSError, ValueError, KeyError) as error:
        # A missing or damaged table must not keep the game from starting.
        print(f'Patches: co-op sites not checked ({error})',file=sys.stderr)
    writes=[(offset,data) for _,offset,data in named]
    # BBPATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots (60/90 FPS++ replace function pointers).
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (a.out/'patches.bin').write_bytes(blob)
    write_build_check_names(a.out,[name for name,_,_ in named])
    print(f'Patches: FPS preset {a.fps}; {len(writes)} writes from {names or "none"}')


if __name__=='__main__':
    main()
