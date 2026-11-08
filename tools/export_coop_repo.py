"""Copies the co-op plugin out of the combined development tree into its own repository folder.

The randomizer repository ignores the co-op files (see .gitignore); this script is the other half:
it takes the co-op sources, every header they include (followed transitively), the build and test
scripts, the INI templates and the co-op tools, and writes them to the target folder. Files already in
the target that are not part of the export are left alone, so README, .gitignore and the git history
there stay as they are.

    python tools/export_coop_repo.py <target folder> [--dry-run]
"""
import glob
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SOURCES = ['src/coop_plugin.cpp', 'src/modloader.cpp'] + sorted(
    os.path.relpath(p, ROOT).replace('\\', '/') for p in
    glob.glob(os.path.join(ROOT, 'src', 'runtime_coop*.inc')) + glob.glob(os.path.join(ROOT, 'src', 'coop_*.h')))
TESTS = sorted(os.path.relpath(p, ROOT).replace('\\', '/') for p in glob.glob(os.path.join(ROOT, 'tests', 'coop_*.cpp')))
SCRIPTS = ['build_coop.cmd', 'build_modloader.cmd', 'build_ownership_probe.cmd', 'StartCoopEvents.cmd'] + sorted(
    os.path.basename(p) for pattern in ('build_bot_*.cmd', 'build_coop_*.cmd', 'build_death_ack_tests.cmd', 'build_gate*.cmd',
                                        'build_ghost_probe_tests.cmd', 'build_local_lifecycle_tests.cmd', 'build_npc_policy_tests.cmd',
                                        'build_peer*.cmd', 'build_ping_tests.cmd', 'build_save_policy_tests.cmd', 'build_template_tests.cmd',
                                        'build_uniform_tests.cmd', 'build_weapon_lookup_tests.cmd')
    for p in glob.glob(os.path.join(ROOT, pattern)))
TEMPLATES = ['DS2United.ini', 'DS2BotProfiles.ini', 'DS2ModLoader.ini', '.gitattributes']   # LICENSE lives in the target repo
FOLDERS = ['third_party']   # the network launchers stay out while network co-op is not released
TOOLS = ['tools/audit_coop_spawn_policy.py', 'tools/compare_coop_world.py', 'tools/coop_events_live.py',
         'tools/inspect_coop_runtime.py', 'tools/inspect_coop_world.py', 'tools/inspect_uniform_runtime.py',
         'tools/inspect_weapon_catalog.py', 'tools/inspect_gun_network.py', 'tools/inspect_inventory_persistence.py',
         'tools/inspect_native_respawn.py', 'tools/inspect_player_ownership.py', 'tools/verify_ownership_dump_report.py',
         'tools/ownership_dump_probe.cpp', 'tools/build_scene_kinds.py', 'tools/build_scripted_containers.py',
         'tools/embed_message_names.py', 'tools/check_ini_sections.py', 'tools/build_necroplayer_leaper.py',
         'tools/necroplayer_ubermorph.py', 'tools/coop_world_diff.py', 'tools/map_lookup.py', 'tools/vt2n.json', 'tools/dump_attribute_offsets.py'] + sorted(
    os.path.relpath(p, ROOT).replace('\\', '/') for p in glob.glob(os.path.join(ROOT, 'tools', 'live_probe_*.py')))
# Helper modules the dump and live tools import from outside this tree; the export puts them next to
# the tools, where a script's own folder is already on the import path.
OUTSIDE = {'tools/dmpread.py': '../trainer/dmpread.py', 'tools/analyze_minidump.py': '../trainer/analyze_minidump.py',
           'tools/memscan.py': '../tools/memscan.py', 'tools/ds2_assets.py': 'tools/ds2_assets.py'}
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def headers_of(files):
    """Every quoted include reachable from the given files, as repository paths."""
    seen, todo = set(), list(files)
    while todo:
        rel = todo.pop()
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            continue
        text = open(path, encoding='utf-8', errors='replace').read()
        for name in INCLUDE.findall(text):
            target = os.path.normpath(os.path.join(os.path.dirname(rel), name)).replace('\\', '/')
            if target not in seen and os.path.isfile(os.path.join(ROOT, target)):
                seen.add(target)
                todo.append(target)
    return seen


SCRIPT_REF = re.compile(r'(?:%~dp0|^|[\s"])((?:tools|src|tests)\\[\w.\\-]+\.(?:cmd|bat|py|cpp|h|c))', re.M | re.I)


def script_refs(scripts):
    """Helpers the build scripts call or compile (tools\\vcvars32.cmd and the like)."""
    refs = set()
    for rel in scripts:
        path = os.path.join(ROOT, rel)
        if os.path.isfile(path):
            for name in SCRIPT_REF.findall(open(path, encoding='utf-8', errors='replace').read()):
                refs.add(name.replace('\\', '/'))
    return refs


def plan():
    files = set(SOURCES + TESTS + SCRIPTS + TEMPLATES + TOOLS) | script_refs(SCRIPTS)
    files |= headers_of(SOURCES + TESTS + [t for t in TOOLS if t.endswith('.cpp')])
    for folder in FOLDERS:
        for path in glob.glob(os.path.join(ROOT, folder, '**', '*'), recursive=True):
            if os.path.isfile(path):
                files.add(os.path.relpath(path, ROOT).replace('\\', '/'))
    missing = sorted(f for f in files if not os.path.isfile(os.path.join(ROOT, f)))
    files = sorted(f for f in files if os.path.isfile(os.path.join(ROOT, f)) and '__pycache__' not in f)
    return [(f, f) for f in files] + sorted(OUTSIDE.items()), missing


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    target = os.path.abspath(sys.argv[1])
    dry = '--dry-run' in sys.argv
    if os.path.commonpath([target, ROOT]) == ROOT:
        print('the target must lie outside the development tree')
        return 2
    files, missing = plan()
    copied = unchanged = 0
    for rel, source in files:
        src, dst = os.path.normpath(os.path.join(ROOT, source)), os.path.join(target, rel)
        if os.path.isfile(dst) and open(src, 'rb').read() == open(dst, 'rb').read():
            unchanged += 1
            continue
        copied += 1
        if not dry:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
    for rel in missing:
        print('listed but missing:', rel)
    print('%d files, %d copied, %d unchanged%s -> %s' % (len(files), copied, unchanged, ' (dry run)' if dry else '', target))
    return 0


if __name__ == '__main__':
    sys.exit(main())
