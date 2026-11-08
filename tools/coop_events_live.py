"""Live co-op event viewer: tails every DS2United*.log in the game folder and
prints the events as they happen, one colour per machine, message hashes
named where the name is known.

usage: python tools/coop_events_live.py [game folder]
       (default: DS2_GAME_DIR, else the default Steam library)

Shown: gate holds/releases/passes/votes, mirror replays and world calls,
kinesis, area holds, checkpoint/restart, notices, hits, drops, guidfix.
Hidden: the per-second counter lines. Press Ctrl+C to stop.
"""
import json
import os
import re
import sys
import time

GAME = sys.argv[1] if len(sys.argv) > 1 else os.environ.get(
    "DS2_GAME_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Dead Space 2")
PLUGINS = os.path.join(GAME, "plugins")
HERE = os.path.dirname(os.path.abspath(__file__))
NAMES = {}
try:
    NAMES = json.load(open(os.path.join(HERE, "..", "analysis", "ds2_message_names_live_2026-09-17.json")))
except Exception:
    pass

SHOW = re.compile(
    r"^(coop_gate (hold|release|passed|partner|our Isaac|replay faulted|full)|"
    r"coop_mirror (replayed|direct hash|direct replayed|direct replay faulted|replay faulted|call |world hash|history|journal)|"
    r"coop_worldcalls (door|VentMarker)|coop_tk (grab|drop|launch|partner|driving)|"
    r"coop_streams (holding|released|area completed|area differs|same area)|"
    r"coop_checkpoint (applied|restart|gave up)|coop_level (joined|follow)|coop_notice shown:|"
    r"coop_hits (world hit)|coop_hit_sample|coop_drops pickup|coop_guidfix sequence|coop_npc action=allowed_scripted|"
    r"coop_netrate connection|coop_death|coop_reghost)"
)
HASH = re.compile(r"hash=([0-9A-F]{8})")
COLOURS = ["\033[96m", "\033[93m", "\033[92m", "\033[95m", "\033[94m", "\033[91m", "\033[97m", "\033[90m"]
RESET = "\033[0m"


def name_hashes(line):
    def sub(m):
        n = NAMES.get(m.group(1))
        return m.group(0) + ("(" + n + ")" if n and "name=" not in line else "")
    return HASH.sub(sub, line)


def main():
    os.system("")   # enable ANSI on Windows consoles
    files = {}
    print("watching %s  (Ctrl+C to stop)" % PLUGINS)
    while True:
        for index in range(8):
            path = os.path.join(PLUGINS, "DS2Coop" + ("." + str(index) if index else "") + ".log")
            if not os.path.exists(path):
                continue
            entry = files.get(path)
            try:
                size = os.path.getsize(path)
            except OSError:
                continue
            if entry is None or size < entry["pos"]:
                # new file or restarted instance: start at the end (or at 0 on first sight of a fresh file)
                start = 0 if size < 200000 else size
                entry = files[path] = {"pos": start, "label": "P%d" % (index + 1), "colour": COLOURS[index % len(COLOURS)], "rest": ""}
                print("%s[%s] %s%s" % (entry["colour"], entry["label"], path, RESET))
            if size == entry["pos"]:
                continue
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                f.seek(entry["pos"])
                chunk = f.read(size - entry["pos"])
                entry["pos"] = size
            text = entry["rest"] + chunk
            lines = text.split("\n")
            entry["rest"] = lines.pop()
            for line in lines:
                if SHOW.match(line):
                    stamp = time.strftime("%H:%M:%S")
                    print("%s%s [%s] %s%s" % (entry["colour"], stamp, entry["label"], name_hashes(line[:220]), RESET))
        time.sleep(0.3)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
