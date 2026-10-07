"""List the pool's assets and every entity identity inside its SimGroups.

Read-only. Nothing is written and no installed archive is touched.
"""
import argparse
import sys
from collections import Counter
from pathlib import Path

from ds2_paths import MODDING_TOOLS
sys.path.insert(0, str(MODDING_TOOLS))
import ds2str  # noqa: E402

from ds2_assets import assets, simgroup  # noqa: E402


def pool_assets(path):
    stream = ds2str.StreamSet(str(path))
    for header, chunks in assets(stream):
        payload = stream.asset_payload(chunks, header["size"])
        yield header, payload


def identities(path):
    """(header, payload, records) for every asset that decodes as a SimGroup."""
    for header, payload in pool_assets(path):
        try:
            records, _ = simgroup(payload)
        except ValueError:
            continue
        yield header, payload, records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pool", type=Path)
    parser.add_argument("--list", action="store_true", help="print every identity")
    args = parser.parse_args()

    types = Counter()
    total = groups = 0
    for header, payload in pool_assets(args.pool):
        types[header["type"]] += 1
    for header, payload, records in identities(args.pool):
        groups += 1
        total += len(records)
        if args.list:
            for record in records:
                print("%s\t%s" % (header["name"], record["guid"]))
    print("assets by type:", dict(types))
    print("simgroups: %d, identities: %d" % (groups, total))


if __name__ == "__main__":
    main()
