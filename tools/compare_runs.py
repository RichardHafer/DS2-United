"""Diff two randomizer logs: where did two runs decide differently, and why.

Two players on the same seed should meet the same enemies. When they do not, the
question is always the same - is it the seed, the configuration, or simply that
they played differently? The logs already carry every decision, so this answers
it without anyone replaying anything.

The join is on the authored target GUID, which is the enemy the level wanted at
that spot. Every difference is then put into one of a few buckets, because the
bucket is the answer:

    rule        one side has a spawner rule the other has not, or a rule that
                only applies on one difficulty
    too early   the pool was not resident yet on one side, so nothing was
                replaced there
    donor       both replaced, but a different donor won - that would be a real
                seed or configuration difference
    reached     only one side ever got to that spawn
"""
import argparse
import collections
import re
from pathlib import Path

SPAWN = re.compile(r"spawn=\d+ source=\w+ target=([0-9A-F-]+) ")
DECISION = re.compile(r"selection=(\w+) attempts=(\d+) tick=(\d+) "
                      r"source_family=(\w+) target_family=(\w+) selected=([0-9A-F-]+)")
HEADER = re.compile(r"seed=(\d+) chance=(\d+) cross_family=(\d+) pool=(\d+)")
CATALOG = re.compile(r"embedded_catalog=([0-9a-f]+)")
RULES = re.compile(r"spawner_rules tick=\d+ result=accepted entries=(\d+) restricted=(\d+)"
                   r"(?: difficulty_scoped=(\d+) playing=(\w+))?")


class Run:
    def __init__(self, path):
        self.path = Path(path)
        text = self.path.read_text(encoding="latin1")
        self.decisions, pending = {}, None
        for line in text.splitlines():
            found = SPAWN.match(line)
            if found:
                pending = found.group(1)
                continue
            found = DECISION.match(line)
            if found and pending:
                self.decisions.setdefault(pending, {
                    "result": found.group(1), "attempts": int(found.group(2)),
                    "family": found.group(5), "selected": found.group(6)})
                pending = None
        self.header = HEADER.search(text)
        self.catalog = CATALOG.search(text)
        self.rules = RULES.search(text)

    def setup(self):
        seed, chance, cross, pool = self.header.groups() if self.header else ("?",) * 4
        catalog = self.catalog.group(1)[:16] if self.catalog else "?"
        rules = self.rules.groups() if self.rules else (None, None, None, None)
        return {"seed": seed, "chance": chance, "cross_family": cross, "pool": pool,
                "catalog": catalog, "rules": rules[0], "difficulty": rules[3] or "?"}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("left", type=Path)
    parser.add_argument("right", type=Path)
    parser.add_argument("--all", action="store_true", help="list every difference, not just a sample")
    args = parser.parse_args()

    left, right = Run(args.left), Run(args.right)
    a, b = left.setup(), right.setup()
    print("%-14s %-24s %s" % ("", args.left.name, args.right.name))
    for key in ("seed", "chance", "cross_family", "pool", "catalog", "rules", "difficulty"):
        mark = "" if a[key] == b[key] else "   <-- differs"
        print("%-14s %-24s %s%s" % (key, a[key], b[key], mark))
    if a["catalog"] != b["catalog"] or a["pool"] != b["pool"]:
        print("\nDifferent build or pool. That explains everything below on its own.")

    common = sorted(set(left.decisions) & set(right.decisions))
    print("\nDecisions: left %d, right %d, shared %d"
          % (len(left.decisions), len(right.decisions), len(common)))
    buckets = collections.defaultdict(list)
    for guid in common:
        x, y = left.decisions[guid], right.decisions[guid]
        if x["result"] == y["result"] and x["selected"] == y["selected"]:
            buckets["same"].append(guid)
        elif "spawner_blocked" in (x["result"], y["result"]):
            buckets["rule"].append((guid, x, y))
        elif "global_pool_unavailable" in (x["result"], y["result"]):
            buckets["too early"].append((guid, x, y))
        elif x["result"] == y["result"] == "packet_replaced":
            buckets["donor"].append((guid, x, y))
        else:
            buckets["other"].append((guid, x, y))
    for name in ("same", "rule", "too early", "donor", "other"):
        rows = buckets.get(name, [])
        if not rows:
            continue
        print("   %-10s %d" % (name, len(rows)))
    for name in ("donor", "other"):
        rows = buckets.get(name, [])
        if not rows:
            continue
        print("\n%s, this would be a real difference:" % name)
        for guid, x, y in (rows if args.all else rows[:15]):
            print("   %s  left=%-11s %-10s a=%d   right=%-11s %-10s a=%d"
                  % (guid[:17], x["result"][:11], x["family"], x["attempts"],
                     y["result"][:11], y["family"], y["attempts"]))
    only_left = len(set(left.decisions) - set(right.decisions))
    only_right = len(set(right.decisions) - set(left.decisions))
    print("\nreached only on the left: %d, only on the right: %d" % (only_left, only_right))


if __name__ == "__main__":
    main()
