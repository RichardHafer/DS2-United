"""Check offline which vent spawns can be replaced, by whom, and whether the
donor can actually get out of the vent.

This reproduces the plugin's own rule - `compatibleVent` in `src/selection.h` -
against the data the plugin actually carries, `src/generated_enemy_catalog.h`,
so a mismatch between rule and data shows up here instead of in the game.

What makes a vent arrival work:

* The climb-out belongs to the species. A wall vent carries no animation; the
  entity does, through its own `<Species>VentComponent`. So a donor can only
  come out of a vent if it is itself an authored vent arrival.
* The spawning component holds the rest of the contract, and on a vent spawn it
  is three fields: the mode, the activation flags, and a GUID naming the vent.
  That GUID is the row's own `vent_guid` in all 427 extracted vent spawns, so two
  spawns of different species differ in which vent they sit on and in nothing
  else. `ventFreeKey` is that contract with the vent pointer removed.

With `VentRebinding=1` the runtime points the donor at the source's vent, so
`ventFreeKey` equality is the honest test. With it off the donor arrives through
its own vent and the vent has to be the same one, which is what `arrivalKey` and
`crossArrivalKey` carry.

A donor is *reachable* when it is authored in the same archive as the source,
because that archive is loaded exactly when the source spawns.
"""
import argparse
import collections
import csv
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

ROW = re.compile(
    r'\{\{(0x[0-9A-Fa-f]+u),(0x[0-9A-Fa-f]+u),(0x[0-9A-Fa-f]+u),(0x[0-9A-Fa-f]+u)\},'
    r'\{(0x[0-9A-Fa-f]+u),(0x[0-9A-Fa-f]+u),(0x[0-9A-Fa-f]+u),(0x[0-9A-Fa-f]+u)\},'
    r'"([^"]*)","([^"]*)",(\w+),(\w+),(\w+),"([^"]*)","([^"]*)","([^"]*)","([^"]*)"')


def guid(words):
    return "-".join("%08X" % int(w[2:-1], 16) for w in words)


def load_catalog():
    text = (ROOT / "src/generated_enemy_catalog.h").read_text(encoding="utf-8")
    out = {}
    for m in ROW.finditer(text):
        out[guid(m.group(1, 2, 3, 4))] = dict(
            model=guid(m.group(5, 6, 7, 8)), family=m.group(9), context=m.group(10),
            ordinary=m.group(11) == "true", ambiguous=m.group(12) == "true",
            extended=m.group(13) == "true", arrival=m.group(14), cross=m.group(15),
            scripted=m.group(16), vent_free=m.group(17))
    return out


def load_targets():
    with (ROOT / "analysis/catalog/enemy_targets.tsv").open(newline="", encoding="utf-8") as f:
        return {r["guid"]: r for r in csv.DictReader(f, delimiter="\t")}


def vent_context(context):
    return context.startswith("SPAWNMODE_VENT")


def compatible(source, target, cross_family, rebinding, has_vent):
    """compatibleVent, plus the filters buildPlan applies around it."""
    if not target["ordinary"] or target["ambiguous"]:
        return False
    if not target["model"] or target["model"] == source["model"]:
        return False
    if source["context"] != target["context"]:
        return False
    family_allowed = target["family"] == source["family"] or cross_family
    if rebinding and family_allowed and has_vent(source) and has_vent(target) \
            and source["vent_free"] and source["vent_free"] == target["vent_free"]:
        return True
    if target["family"] == source["family"]:
        return bool(source["arrival"]) and source["arrival"] == target["arrival"]
    return cross_family and bool(source["cross"]) and source["cross"] == target["cross"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path, default=ROOT / "analysis/vent_coverage.json")
    parser.add_argument("--no-cross-family", action="store_true")
    parser.add_argument("--lookups", type=int, default=8)
    args = parser.parse_args()
    cross_family = not args.no_cross_family

    catalog = load_catalog()
    targets = load_targets()

    def archive(g):
        return targets.get(g, {}).get("archive", "")

    def components(g):
        return targets.get(g, {}).get("vent_components", "")

    def has_vent(record):
        return bool(targets.get(record["guid"], {}).get("vent_guid", ""))

    for g, record in catalog.items():
        record["guid"] = g

    sources = {g: c for g, c in catalog.items()
               if vent_context(c["context"]) and c["ordinary"] and not c["ambiguous"]}
    print("vent spawns eligible as a source (ordinary, unambiguous): %d\n" % len(sources))

    report = {"cross_family": cross_family, "sources": len(sources), "modes": {}}
    for label, rebinding in (("rebinding off", False), ("rebinding on", True)):
        counts = collections.Counter()
        by_context = collections.Counter()
        total_context = collections.Counter()
        donor_species = collections.Counter()
        unsafe_total = 0
        entries = []
        for g, source in sorted(sources.items()):
            donors = [d for d, t in catalog.items()
                      if d != g and compatible(source, t, cross_family, rebinding, has_vent)]
            unsafe = [d for d in donors
                      if not vent_context(catalog[d]["context"]) or not components(d)]
            unsafe_total += len(unsafe)
            resident = [d for d in donors if archive(d) == archive(g)]
            total_context[source["context"]] += 1
            if not donors:
                counts["no donor anywhere"] += 1
            elif resident:
                counts["donor in the same archive"] += 1
                by_context[source["context"]] += 1
            else:
                counts["donor only in another archive"] += 1
            for d in donors:
                donor_species[catalog[d]["family"]] += 1
            entries.append({"guid": g, "family": source["family"], "context": source["context"],
                            "donors": len(donors), "resident_donors": len(resident),
                            "donor_families": sorted({catalog[d]["family"] for d in donors})})
        print("%s:" % label)
        for key in ("donor in the same archive", "donor only in another archive", "no donor anywhere"):
            print("   %-32s %3d" % (key, counts[key]))
        print("   %-32s %3d   <- must stay 0" % ("donors that cannot climb out", unsafe_total))
        for context, n in total_context.most_common():
            print("      %-42s %3d of %3d reachable" % (context, by_context[context], n))
        print("   donor species offered: %s" % ", ".join(
            "%s %d" % (f, n) for f, n in donor_species.most_common(8)))
        print()
        report["modes"][label] = {"counts": dict(counts), "unsafe": unsafe_total,
                                 "entries": entries}

    # Does the lookup budget actually reach a donor that can be loaded? The plan
    # groups donors by model, orders the models, and then takes one donor per
    # model per round until maxDonorLookups is full. A donor authored in another
    # archive is not there when the spawn fires, so the question is whether a
    # reachable one lands inside the budget.
    print("what the lookup budget reaches (rebinding on, %d lookups):" % args.lookups)
    for label, locality in (("without the locality rule", False), ("with it", True)):
        reached = starved = nodonor = 0
        for g, source in sorted(sources.items()):
            donors = [d for d, t in catalog.items()
                      if d != g and compatible(source, t, cross_family, True, has_vent)]
            if not donors:
                nodonor += 1
                continue
            by_model = collections.defaultdict(list)
            for d in donors:
                by_model[catalog[d]["model"]].append(d)
            def chapter(x):
                parts = archive(x).split("\\")
                return parts[1] if len(parts) > 1 else ""

            def rank(d):
                if archive(d) == archive(g):
                    return 0
                if chapter(d) and chapter(d) == chapter(g):
                    return 1
                return 2

            def ok(d):
                return rank(d) < 2
            for model in by_model:
                by_model[model].sort(key=lambda d: (rank(d), d))
            models = sorted(by_model)
            if locality:
                models.sort(key=lambda m: rank(by_model[m][0]))
            plan = []
            depth = 0
            while len(plan) < args.lookups:
                progressed = False
                for model in models:
                    if len(plan) >= args.lookups:
                        break
                    if depth < len(by_model[model]):
                        plan.append(by_model[model][depth])
                        progressed = True
                if not progressed:
                    break
                depth += 1
            if any(ok(d) for d in plan):
                reached += 1
            else:
                starved += 1
        print("   %-28s donor that can be there, inside the budget: %3d, only unreachable ones: %3d, no donor: %3d"
              % (label, reached, starved, nodonor))

    args.report.write_text(json.dumps(report, indent=1))
    print("\nfull report: %s" % args.report)


if __name__ == "__main__":
    main()
