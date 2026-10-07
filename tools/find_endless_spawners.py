"""Find FuncSpawn entities that level logic can trigger without end.

A spawner is endless when a cycle in its chapter's message graph reaches it:
a MultiManager that re-triggers itself, or an enemy whose death message fires
its own spawner again. Counters (VariableOperator -> VariableWatcher) are not
edges, so waves that end after N kills stay finite. A MultiManager with a
response limit cannot loop forever and is left out of every cycle.

    python tools/find_endless_spawners.py            report
    python tools/find_endless_spawners.py --write    also writes src/generated_endless_spawners.h
"""
import collections
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "analysis" / "catalog" / "entities.jsonl"
HEADER = ROOT / "src" / "generated_endless_spawners.h"
OUT_FIELD = re.compile(r"^CMD_Set(Target\d*|.*Message\d*|Broadcast\dMessage)$")
IN_FIELD = "CMD_SetTargetName"


def chapter(archive):
    parts = archive.replace("\\", "/").split("/")
    return parts[1] if len(parts) > 1 else parts[0]


def fields(row):
    out = {}
    for image in row.get("images", []):
        for field in image.get("fields", []):
            name = field.get("name")
            if not name:
                continue
            value = field.get("string_candidate", field.get("guid_candidate", field.get("value")))
            out.setdefault(name, value)
    return out


def load():
    rows = []
    for line in CATALOG.open(encoding="utf-8"):
        row = json.loads(line)
        cls = row["images"][0].get("class") if row.get("images") else ""
        rows.append((row["guid"], cls, chapter(row["archive"]), row["archive"], fields(row)))
    return rows


def analyse(rows):
    by_guid = {guid: f for guid, _cls, _ch, _ar, f in rows}
    per_chapter = collections.defaultdict(list)
    for row in rows:
        per_chapter[row[2]].append(row)
    endless = []
    for ch, items in per_chapter.items():
        receivers = collections.defaultdict(list)     # message -> entity guids
        sends = {}                                     # entity guid -> messages it can send
        spawners = {}
        for guid, cls, _ch, archive, f in items:
            name = f.get(IN_FIELD)
            if isinstance(name, str) and name:
                receivers[name.lower()].append(guid)
            out = set()
            limited = cls == "MultiManager" and isinstance(f.get("CMD_SetResponseLimit"), int) and f["CMD_SetResponseLimit"] > 0
            for key, value in f.items():
                if key != IN_FIELD and OUT_FIELD.match(key) and isinstance(value, str) and value:
                    out.add(value.lower())
            if cls == "FuncSpawn":
                spawners[guid] = (f.get(IN_FIELD) or "", f.get("CMD_SetSpawnTarget"), archive)
                template = by_guid.get(f.get("CMD_SetSpawnTarget") or "", {})
                for key in ("CMD_SetOnDeathMessage", "CMD_SetHealthModifiedMessage"):
                    value = template.get(key)
                    if isinstance(value, str) and value:
                        out.add(value.lower())
            sends[guid] = (out, limited)
        # entity graph: A -> B when A sends a message B receives
        graph = {g: [r for m in sends[g][0] for r in receivers.get(m, [])] for g in sends if not sends[g][1]}
        # strongly connected components (iterative Tarjan)
        index, low, on, stack, comp = {}, {}, set(), [], {}
        counter = [0]
        for start in graph:
            if start in index:
                continue
            work = [(start, iter(graph.get(start, [])))]
            index[start] = low[start] = counter[0]; counter[0] += 1; stack.append(start); on.add(start)
            while work:
                node, it = work[-1]
                advanced = False
                for nxt in it:
                    if nxt not in graph:
                        continue
                    if nxt not in index:
                        index[nxt] = low[nxt] = counter[0]; counter[0] += 1; stack.append(nxt); on.add(nxt)
                        work.append((nxt, iter(graph.get(nxt, []))))
                        advanced = True
                        break
                    if nxt in on:
                        low[node] = min(low[node], index[nxt])
                if advanced:
                    continue
                work.pop()
                if work:
                    parent = work[-1][0]
                    low[parent] = min(low[parent], low[node])
                if low[node] == index[node]:
                    members = []
                    while True:
                        w = stack.pop(); on.discard(w); members.append(w)
                        if w == node:
                            break
                    looping = len(members) > 1 or node in graph.get(node, [])
                    for w in members:
                        comp[w] = looping
        # every entity reachable from a looping component
        seeds = [g for g, looping in comp.items() if looping]
        reached, todo = set(seeds), list(seeds)
        while todo:
            g = todo.pop()
            for nxt in graph.get(g, []):
                if nxt not in reached:
                    reached.add(nxt); todo.append(nxt)
        for guid, (name, target, archive) in spawners.items():
            if guid in reached:
                endless.append((ch, guid, name, target, archive))
    return sorted(endless)


def write_header(endless):
    lines = [
        "// Generated by tools/find_endless_spawners.py - do not edit.",
        "// Spawners that level logic can trigger without end. They keep their authored enemy.",
        "#pragma once",
        "#include <cstdint>",
        "namespace ds2 {",
        "struct EndlessSpawner { uint32_t guid[4]; const char* name; };",
        "inline constexpr EndlessSpawner kEndlessSpawners[] = {",
    ]
    for ch, guid, name, _target, _archive in endless:
        words = ",".join("0x%su" % w for w in guid.split("-"))
        lines.append('    {{%s}, "%s/%s"},' % (words, ch, name))
    lines += ["};", "}  // namespace ds2", ""]
    HEADER.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main():
    endless = analyse(load())
    for ch, guid, name, target, archive in endless:
        print("%-16s %s %-44s -> %s" % (ch, guid, name, target))
    print("%d endless spawners" % len(endless))
    if "--write" in sys.argv:
        write_header(endless)
        print("wrote", HEADER)


if __name__ == "__main__":
    main()
