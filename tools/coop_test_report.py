"""Dead Space 2 United by Hafer: test report for one play session.

Reads the plugin logs of every game copy (DS2United.log, DS2United.1.log, ...), collects the
"test <scenario> <RESULT>: detail" lines the plugin writes (PASS, FAIL, FAIL healed, REFUSED,
INFO), the life state machine's transitions and its last counters, and the crash dumps written
since the session began. Prints the report and saves it under build/reports/.

    python -I tools/coop_test_report.py [--plugins DIR] [--dumps DIR] [--prev]
"""
import argparse
import datetime
import os
import re
import sys
import zlib
from collections import OrderedDict, defaultdict

DEFAULT_PLUGINS = r'D:\SteamLibrary\steamapps\common\Dead Space 2\plugins'
DEFAULT_DUMPS = r'C:\DS2CrashDumps'
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEST = re.compile(r'^test (\w+) (PASS|FAIL healed|FAIL|REFUSED|INFO): ?(.*)$')
LIFE = re.compile(r'^life: (.+?) -> (.+?) after (\d+) ms(.*)$')
ROLE = re.compile(r'^roles: (this machine|partner \d+ \(token [0-9A-F]+\)) plays (.+)$')
COUNTERS = ('scene_kind ', 'necro life ', 'necro direct_spawn ', 'necro spawn_cue ', 'necro vent_pick ', 'necro watch_suit ', 'coop_checkpoint necro_reloads ',
            'pause menu rows locked', 'coop_hits nicole target guard', 'teleport ', 'lights ')
PROBLEMS = re.compile(r'\bfaulted\b(?![=_])|FAILED|exception [0-9A-F]{8}|NOT respawned|signature mismatch')
STARTED = re.compile(r'^coop_plugin session started (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)')

# What each scenario means, for the reader.
SCENARIOS = OrderedDict([
    ('vent_spawn', 'necro comes out of a vent'),
    ('direct_spawn', 'Q spawn at the ghost (stands at the spot 1.5 s later)'),
    ('death_to_ghost', 'dead necro becomes the ghost'),
    ('rejoin', 'necro by choice is a necro again (after a restart or level change)'),
    ('watch_cue', 'scene cue played for the watching necro'),
    ('watch', 'scene watch keeps Isaac'),
    ('hud', 'necro HUD only with the ghost'),
    ('spawn_cue', 'loud roar on the other machines'),
    ('life', 'life state machine rules'),
    ('finale', 'a human plays a playable sequence (necros may spawn)'),
    ('scene_kind', 'what the game itself says each scripted moment is'),
    ('suicide', 'pause menu suicide of a living necro'),
    ('bot_ghost', "a host bot's ghost kept alive here (the host owns its life)"),
    ('nicole_target', "Nicole's camera state skipped without a target (necro machine)"),
    ('teleport', 'scripted teleport of the local Isaac (sent to the partners)'),
    ('lights', "a partner's room light state applied here"),
    ('birth_suit', 'a body we spawn is born in the wanted suit (Player::Init)'),
])


def machine_logs(plugins, prev):
    """The copies of this session: logs last written within two hours of the newest one."""
    names = sorted(n for n in os.listdir(plugins) if re.fullmatch(r'DS2Coop(\.\d+)?\.log', n))
    if prev:
        names += sorted(n for n in os.listdir(plugins) if re.fullmatch(r'DS2Coop(\.\d+)?\.prev\.log', n))
    paths = [(n, os.path.join(plugins, n)) for n in names]
    if not paths:
        return paths
    newest = max(os.path.getmtime(p) for _, p in paths)
    return [(n, p) for n, p in paths if newest - os.path.getmtime(p) <= 7200]


def session_start(path):
    """The plugin's start line; older builds lack it, then the previous log's last write."""
    with open(path, 'rb') as f:
        head = f.read(2048).decode('utf-8', 'replace').splitlines()
    for line in head[:5]:
        m = STARTED.match(line)
        if m:
            return datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S').timestamp()
    prev = path[:-4] + '.prev.log'
    return os.path.getmtime(prev) if os.path.exists(prev) else os.path.getctime(path)


def label(name):
    m = re.fullmatch(r'DS2Coop(?:\.(\d+))?(\.prev)?\.log', name)
    copy = m.group(1) or '0'
    return ('copy %s' % copy) + (' (prev)' if m.group(2) else '')


def read_lines(path):
    with open(path, 'rb') as f:
        data = f.read()
    return data.decode('utf-8', 'replace').splitlines()


def twitcher_report(plugins, started, put):
    """The Severed twitchers live in the randomizer logs: the pool line, survival's
    counters and every spawn that came as one (17.10.)."""
    names = [n for n in os.listdir(plugins) if re.fullmatch(r'DS2Randomizer(\.[^.]+)*\.log', n) and '.prev' not in n]
    rows = []
    for n in sorted(names):
        path = os.path.join(plugins, n)
        if os.path.getmtime(path) < started:
            continue
        pool = survival = refused = None
        campaign = survival_spawns = 0
        for line in read_lines(path):
            if line.startswith('twitchers enabled='):
                pool = line
            elif line.startswith('twitchers REFUSED'):
                refused = line
            elif line.startswith(('survival twitchers ', 'survival severed ')):
                survival = line
            elif line.strip().startswith(('variant=Twitcher', 'variant=SeveredPregnant')):
                campaign += 1
            elif ('skin=ds2_twitcher' in line or 'skin=ds2_severed' in line or 'skin=twitcher' in line) and ' spawn #' in ' ' + line:
                survival_spawns += 1
        if pool or survival or campaign or survival_spawns:
            rows.append((n, pool, refused, survival, campaign, survival_spawns))
    if not rows:
        return
    put('')
    put('## Twitchers')
    for n, pool, refused, survival, campaign, survival_spawns in rows:
        put('- %s: campaign replacements %d, survival spawns %d' % (n, campaign, survival_spawns))
        for line in (pool, refused, survival):
            if line:
                put('  - `%s`' % line[:260])


def crc_of(path):
    try:
        with open(path, 'rb') as f:
            return '%08X' % (zlib.crc32(f.read()) & 0xFFFFFFFF)
    except OSError:
        return '-'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--plugins', default=DEFAULT_PLUGINS)
    ap.add_argument('--dumps', default=DEFAULT_DUMPS)
    ap.add_argument('--prev', action='store_true', help='also read the .prev logs')
    args = ap.parse_args()

    logs = machine_logs(args.plugins, args.prev)
    if not logs:
        print('no DS2Coop logs in', args.plugins)
        return 1
    started = min(session_start(p) for _, p in logs)
    out = []
    put = out.append
    now = datetime.datetime.now()
    put('# Co-op test report %s' % now.strftime('%Y-%m-%d %H:%M'))
    put('')
    put('Plugin %s, session since %s' % (crc_of(os.path.join(args.plugins, 'DS2United.asi')),
                                         datetime.datetime.fromtimestamp(started).strftime('%H:%M:%S')))

    results = defaultdict(lambda: defaultdict(int))          # scenario -> result -> count
    details = defaultdict(list)                             # (scenario, result) -> lines
    per_machine = OrderedDict()
    for name, path in logs:
        lines = read_lines(path)
        who = label(name)
        info = {'tests': 0, 'life': [], 'roles': [], 'counters': {}, 'problems': [], 'lines': len(lines),
                'end': datetime.datetime.fromtimestamp(os.path.getmtime(path)).strftime('%H:%M:%S')}
        for raw in lines:
            line = raw.strip()
            m = TEST.match(line)
            if m:
                scenario, result, detail = m.groups()
                results[scenario][result] += 1
                info['tests'] += 1
                if result != 'PASS':
                    details[(scenario, result)].append('%s: %s' % (who, detail))
                continue
            m = LIFE.match(line)
            if m:
                info['life'].append('%s -> %s (%s ms)%s' % (m.group(1), m.group(2), m.group(3), m.group(4)))
                continue
            m = ROLE.match(line)
            if m and m.group(1) == 'this machine':
                info['roles'].append(m.group(2))
                continue
            for c in COUNTERS:
                if line.startswith(c):
                    info['counters'][c.strip()] = line
            if PROBLEMS.search(line) and not line.startswith(('necro respawn_trace', 'necro native_respawn')):
                info['problems'].append(line)
        per_machine[who] = info

    total_pass = sum(r.get('PASS', 0) for r in results.values())
    total_fail = sum(r.get('FAIL', 0) + r.get('FAIL healed', 0) for r in results.values())
    total_healed = sum(r.get('FAIL healed', 0) for r in results.values())
    put('')
    put('RESULT: %d PASS, %d FAIL (%d of them healed by the plugin)' % (total_pass, total_fail, total_healed))
    put('')
    put('| scenario | what | PASS | FAIL | healed | refused/info |')
    put('|---|---|---|---|---|---|')
    names = list(SCENARIOS) + sorted(s for s in results if s not in SCENARIOS)
    for s in names:
        r = results.get(s, {})
        if not r and s not in ('vent_spawn', 'direct_spawn', 'death_to_ghost', 'spawn_cue'):
            continue
        put('| %s | %s | %d | %d | %d | %d |' % (s, SCENARIOS.get(s, ''), r.get('PASS', 0), r.get('FAIL', 0), r.get('FAIL healed', 0),
                                                 r.get('REFUSED', 0) + r.get('INFO', 0)))
    untested = [s for s in ('vent_spawn', 'direct_spawn', 'death_to_ghost', 'spawn_cue') if not results.get(s)]
    if untested:
        put('')
        put('Not exercised this session: ' + ', '.join(untested))

    fails = [(k, v) for k, v in details.items() if k[1].startswith('FAIL')]
    if fails:
        put('')
        put('## Failures')
        for (scenario, result), lines in sorted(fails):
            put('- %s %s (%d):' % (scenario, result, len(lines)))
            for l in lines[:6]:
                put('  - ' + l[:300])
            if len(lines) > 6:
                put('  - ... %d more' % (len(lines) - 6))
    refused = [(k, v) for k, v in details.items() if k[1] in ('REFUSED', 'INFO')]
    if refused:
        put('')
        put('## Refused / info')
        for (scenario, result), lines in sorted(refused):
            reasons = defaultdict(int)
            for l in lines:
                detail = l.split(': ', 1)[-1]
                key = detail.split(' (')[0] if scenario == 'scene_kind' else detail.split(' at (')[0][:80]
                reasons[(l.split(': ', 1)[0] + ' ' + key) if scenario == 'scene_kind' else key] += 1
            put('- %s %s: ' % (scenario, result) + '; '.join('%s x%d' % (k, v) for k, v in sorted(reasons.items(), key=lambda x: -x[1])))

    kinds = details.get(('scene_kind', 'INFO'), [])
    if kinds:
        put('')
        put('## Scene kinds (first example of each per copy)')
        seen = set()
        for l in kinds:
            who, detail = l.split(': ', 1)
            key = (who, detail.split(' (')[0])
            if key in seen:
                continue
            seen.add(key)
            put('- %s: %s' % (who, detail[:260]))

    twitcher_report(args.plugins, started, put)

    for who, info in per_machine.items():
        put('')
        put('## %s (%d lines, last write %s, %d test lines)' % (who, info['lines'], info['end'], info['tests']))
        if info['roles']:
            put('- roles: ' + ' -> '.join(info['roles'][-12:]))
        if info['life']:
            put('- life (last %d of %d):' % (min(12, len(info['life'])), len(info['life'])))
            for l in info['life'][-12:]:
                put('  - ' + l[:200])
        for c in COUNTERS:
            line = info['counters'].get(c.strip())
            if line:
                put('- `%s`' % line[:260])
        if info['problems']:
            uniq = list(OrderedDict.fromkeys(info['problems']))
            put('- problems (%d, unique %d):' % (len(info['problems']), len(uniq)))
            for l in uniq[:8]:
                put('  - ' + l[:220])

    put('')
    dumps = []
    if os.path.isdir(args.dumps):
        for n in os.listdir(args.dumps):
            p = os.path.join(args.dumps, n)
            if os.path.isfile(p) and os.path.getmtime(p) >= started:
                dumps.append((os.path.getmtime(p), n))
    if dumps:
        put('## Crash dumps since the session began')
        for t, n in sorted(dumps):
            put('- %s %s' % (datetime.datetime.fromtimestamp(t).strftime('%H:%M:%S'), n))
    else:
        put('No crash dumps since the session began.')

    text = '\n'.join(out) + '\n'
    sys.stdout.write(text)
    folder = os.path.join(REPO, 'build', 'reports')
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, 'coop_test_%s.md' % now.strftime('%Y%m%d-%H%M'))
    with open(path, 'wb') as f:
        f.write(text.encode('utf-8'))
    print('saved', path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
