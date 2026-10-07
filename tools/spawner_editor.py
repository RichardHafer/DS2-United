"""Edit persistent DS2 spawner rules and inspect recent spawn decisions."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import time

GUID = re.compile(r"[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{8}){3}\Z")
FAMILY = re.compile(r"\w{1,32}\Z", re.ASCII)
ZERO_GUID = "00000000-00000000-00000000-00000000"
HEADERS = ("source_guid\tmode", "source_guid\tmode\tfamilies",
           "source_guid\tmode\tfamilies\tdifficulty")
# The game writes the chosen difficulty into its own settings file, so a rule can
# name one and the plugin reads it from there instead of hunting through memory.
DIFFICULTIES = ("casual", "normal", "survivalist", "zealot", "hardcore")
ROOT = Path(__file__).resolve().parents[1]
# Shipped on its own in the release, so it reads DS2_GAME_DIR itself.
DEFAULT_PLUGINS = Path(os.environ.get("DS2_GAME_DIR")
                       or r"D:\SteamLibrary\steamapps\common\Dead Space 2") / "plugins"


def difficulty_set(text):
    """"zealot;hardcore" as a frozenset; empty text means every difficulty."""
    if not text:
        return frozenset()
    names = text.split(";")
    if len(names) != len(set(names)) or not all(n in DIFFICULTIES for n in names):
        raise ValueError("Unknown or repeated difficulty; file was not changed")
    return frozenset(names)


def difficulty_text(names):
    """The file spelling, always in the game's own order."""
    return ";".join(n for n in DIFFICULTIES if n in names)


def check_rule(mode, families, difficulty=""):
    """A rule has to say something, and it may not contradict itself."""
    if mode not in ("block", "allow", "default"):
        raise ValueError("Unknown rule mode; file was not changed")
    difficulty_set(difficulty)
    if len(families) != len(set(families)) or not all(FAMILY.fullmatch(n) for n in families):
        raise ValueError("Invalid family list; file was not changed")
    if mode == "block" and families:
        raise ValueError("A blocked spawner selects nothing, so it takes no family list")
    if mode == "default" and not families:
        raise ValueError("Rule neither sets a mode nor restricts anything")


def read_rules(path):
    if not path.exists():
        return {}
    if path.stat().st_size > 1024 * 1024:
        raise ValueError("Rules file exceeds 1 MiB")
    lines = path.read_text(encoding="ascii").splitlines()
    if not lines or lines[0] not in HEADERS:
        raise ValueError("Invalid rules header; file was not changed")
    rules = {}
    for line in lines[1:]:
        if not line:
            continue
        fields = line.split("\t")
        while len(fields) < 4:
            fields.append("")
        if len(fields) != 4 or (lines[0] == HEADERS[0] and (fields[2] or fields[3])):
            raise ValueError("Invalid rule row; file was not changed")
        if lines[0] == HEADERS[1] and fields[3]:
            raise ValueError("Difficulty needs the header that declares it")
        if not GUID.fullmatch(fields[0]) or fields[0].upper() == ZERO_GUID:
            raise ValueError("Invalid source GUID; file was not changed")
        guid, mode, difficulty = fields[0].upper(), fields[1], fields[3]
        families = fields[2].split(";") if fields[2] else []
        if sum(len(rows) for rows in rules.values()) >= 8192:
            raise ValueError("Too many rules; file was not changed")
        check_rule(mode, families, difficulty)
        rows = rules.setdefault(guid, [])
        # Several rows per spawner are fine as long as no two of them answer
        # the same difficulty: one general row at most, specific ones disjoint.
        if any(overlaps(row["difficulty"], difficulty) for row in rows):
            raise ValueError("Two rules for one spawner answer the same difficulty; file was not changed")
        rows.append({"mode": mode, "families": families, "difficulty": difficulty_text(difficulty_set(difficulty))})
    return rules


def overlaps(first, second):
    a, b = difficulty_set(first), difficulty_set(second)
    if not a and not b:
        return True
    return bool(a & b)


def row_for(rows, difficulty=""):
    """The row that answers a difficulty set: the exact match, else the general one."""
    wanted = difficulty_set(difficulty)
    general = None
    for row in rows or []:
        have = difficulty_set(row["difficulty"])
        if have == wanted:
            return row
        if not have:
            general = row
    return general


def as_rule(value):
    """A bare mode string widened into the full rule shape."""
    mode, families, difficulty = as_change(value)
    return {"mode": mode, "families": families, "difficulty": difficulty}


def as_change(value):
    """Accept a bare mode as well as a full rule, so the buttons stay simple."""
    if isinstance(value, str):
        return value, [], ""
    return (value.get("mode", "default"), list(value.get("families") or []),
            value.get("difficulty") or "")


def write_rules(path, changes):
    """Apply changes and write the file.

    A change is either one rule (a bare mode or a rule dict), which becomes
    the spawner's only row, or {"rows": [...]}, which sets all of the
    spawner's rows at once. That second form is what the scoped buttons and
    undo use, so a Zealot row can change without touching the general one.
    """
    rules = read_rules(path)
    for guid, value in changes.items():
        if not GUID.fullmatch(guid) or guid == ZERO_GUID:
            raise ValueError("Invalid requested rule")
        if isinstance(value, dict) and "rows" in value:
            rows = [dict(as_rule(row)) for row in value["rows"]]
        else:
            rows = [as_rule(value)]
        kept = []
        for row in rows:
            if row["mode"] == "default" and not row["families"]:
                continue
            check_rule(row["mode"], row["families"], row["difficulty"])
            if any(overlaps(seen["difficulty"], row["difficulty"]) for seen in kept):
                raise ValueError("Two rules for one spawner answer the same difficulty")
            row["difficulty"] = difficulty_text(difficulty_set(row["difficulty"]))
            kept.append(row)
        if kept:
            rules[guid.upper()] = kept
        else:
            rules.pop(guid.upper(), None)
    if sum(len(rows) for rows in rules.values()) > 8192:
        raise ValueError("Too many rules")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        backup = path.with_name(path.name + ".previous")
        shutil.copy2(path, backup)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="ascii", newline="\n",
                dir=path.parent, prefix=".spawner_rules_", suffix=".tmp", delete=False) as file:
            temporary = Path(file.name)
            file.write(HEADERS[2] + "\n")
            for guid, rows in sorted(rules.items()):
                for rule in rows:
                    file.write(f"{guid}\t{rule['mode']}\t{';'.join(rule['families'])}"
                               f"\t{rule.get('difficulty') or ''}\n")
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
    finally:
        if temporary and temporary.exists():
            temporary.unlink()
    return rules


def target_families(path):
    """The families the plugin can hand out, straight from [Targets] in the INI.

    The INI stays above the per-spawner list: a family switched off there can
    never be selected, so the dialog shows it as unavailable instead of letting
    a rule pretend otherwise.
    """
    if not path.exists() or path.stat().st_size > 1024 * 1024:
        return []
    families, section = [], None
    for raw in path.read_text(encoding="ascii", errors="replace").splitlines():
        line = raw.split(";")[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        elif section == "Targets" and "=" in line:
            name, _, value = line.partition("=")
            if FAMILY.fullmatch(name.strip()) and not any(n == name.strip() for n, _ in families):
                families.append((name.strip(), value.strip() != "0"))
    return families


def load_catalog(path):
    catalog = {}
    with path.open(encoding="utf-8", newline="") as file:
        for row in csv.DictReader(file, delimiter="\t"):
            guid = row["source_guid"]
            if guid not in catalog:
                catalog[guid] = dict(row)
            elif row["archive"] not in catalog[guid]["archive"].split(" | "):
                catalog[guid]["archive"] += " | " + row["archive"]
    return catalog


def recent_rows(path):
    if not path.exists():
        return []
    with path.open(encoding="ascii", newline="") as file:
        rows = list(csv.DictReader(file, delimiter="\t"))
    if len(rows) > 256:
        raise ValueError("Unexpected recent-spawn file size")
    for row in rows:
        if not GUID.fullmatch(row.get("source_guid", "")):
            raise ValueError("Invalid recent-spawn row")
    return rows


REASONS = {
    "packet_replaced": "Randomized",
    "spawner_blocked": "Original: blocked here",
    "source_disabled": "Source family disabled",
    "chance_skipped": "Chance skipped",
    "unsupported_class": "Class requires compatibility work",
    "unsupported_arrival": "Arrival requires compatibility work",
    "global_pool_unavailable": "Global resources not ready",
    "donor_unavailable": "Selected donor unavailable",
    "unknown_source": "Target missing from enemy catalog",
    "no_candidate": "No compatible candidate",
    "family_not_allowed_here": "Original: family not allowed at this spawner",
    "disabled": "Randomizer disabled",
}


def rule_label(rows):
    if not rows:
        return "default"
    parts = []
    for rule in rows:
        text = rule["mode"]
        if rule["families"]:
            text += " / only " + ", ".join(rule["families"])
        if rule.get("difficulty"):
            text += " [%s]" % rule["difficulty"].replace(";", ", ")
        parts.append(text)
    return "  |  ".join(parts)


EXTRA_HEADER = "source_guid\tkind\tcount\tchance\tx\ty\tz\tyaw\tarrival"
PLACEMENT = re.compile(r"^placement: x=(-?[\d.]+) y=(-?[\d.]+) z=(-?[\d.]+) yaw=(-?\d+)")
NEAR_SPAWNER = re.compile(r"^ placement spawner ([0-9A-F]{8}(?:-[0-9A-F]{8}){3}) distance=([\d.]+) yaw=(-?\d+)")
# Boss families come only through extra-spawn rules; the rest are the pool families.
EXTRA_KINDS = ("Guardian", "Tripod", "Zombie", "Exploder", "Leaper", "Lurker", "PackEnemy", "Pregnant",
               "Puker", "TubeBeetle", "Infector", "Stalker", "SwarmEnemy", "Brute", "DividerCore", "Hunter")


def latest_readout(log_path):
    """The last F10 readout in the plugin log: position, yaw and the spawners it listed.

    The log is appended by the plugin's own worker thread, so the readout shows
    up a few seconds after the key press. Only the tail is read; the file grows
    to several megabytes in a long session.
    """
    if not log_path.exists():
        return None
    size = log_path.stat().st_size
    with log_path.open("rb") as file:
        file.seek(max(0, size - 512 * 1024))
        lines = file.read().decode("latin1").splitlines()
    readout = None
    for line in lines:
        hit = PLACEMENT.match(line)
        if hit:
            readout = {"x": hit.group(1), "y": hit.group(2), "z": hit.group(3), "yaw": hit.group(4), "near": []}
            continue
        near = NEAR_SPAWNER.match(line)
        if near and readout is not None:
            readout["near"].append((near.group(1), float(near.group(2)), near.group(3)))
    return readout


def read_extra_rows(path):
    if not path.exists():
        return []
    rows = []
    for raw in path.read_text(encoding="latin1").splitlines()[1:]:
        if raw.strip() and not raw.startswith("#"):
            rows.append(raw.split("\t"))
    return rows


def write_extra_rows(path, rows):
    """Rewrite the extra-spawn file with tabs between every column, never spaces."""
    text = EXTRA_HEADER + "\n" + "".join("\t".join(row) + "\n" for row in rows)
    fd, temp = tempfile.mkstemp(dir=str(path.parent), prefix="DS2ExtraSpawns.", suffix=".tmp")
    with os.fdopen(fd, "w", encoding="ascii", newline="\n") as file:
        file.write(text)
    if path.exists():
        shutil.copy2(path, path.with_suffix(".tsv.previous"))
    os.replace(temp, path)


AREA_SOURCE = re.compile(r"near(?::([0-9.]+))?")


def check_extra_row(guid, kind, count, chance, position, yaw, arrival):
    """Mirror of the plugin's parser (extra_spawns.h): refuse what it would refuse."""
    area = AREA_SOURCE.fullmatch(guid)
    if area:
        if area.group(1) is not None:
            try:
                radius = float(area.group(1))
            except ValueError:
                return "Radius is not a number."
            if not 0.5 <= radius <= 100:
                return "Radius must be 0.5 to 100 metres."
        if len([p for p in position if p.strip()]) != 3:
            return "An area rule needs the position. Press the readout key in the game first."
    elif not GUID.fullmatch(guid):
        return "Source GUID is not a spawner identity."
    if not re.fullmatch(r"[A-Za-z0-9_]{1,32}", kind):
        return "Kind must be a family or pool package name (letters, digits, underscore)."
    if not count.isdigit() or not 1 <= int(count) <= 8:
        return "Count must be 1 to 8."
    if not chance.isdigit() or not 1 <= int(chance) <= 100:
        return "Chance must be 1 to 100 percent."
    filled = [p for p in position if p.strip()]
    if filled and len(filled) != 3:
        return "Give x, y and z together, or none of them."
    for value in filled + ([yaw] if yaw.strip() else []):
        try:
            float(value)
        except ValueError:
            return f"{value!r} is not a number."
    if arrival == "direct" and len(filled) != 3:
        return "Direct arrival needs a position. Press the readout key in the game first."
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plugins", type=Path, default=DEFAULT_PLUGINS)
    parser.add_argument("--catalog", type=Path, default=ROOT / "analysis/catalog/funcspawn_targets.tsv")
    parser.add_argument("--ini", type=Path, default=None,
        help="Plugin INI the family list is read from; defaults to the one next to the rules.")
    args = parser.parse_args()
    import tkinter as tk
    from tkinter import ttk, messagebox

    window = tk.Tk()
    window.title("Dead Space 2 Spawner Rules")
    window.geometry("1240x760")
    window.minsize(960, 560)
    rules_path = args.plugins / "DS2SpawnerRules.tsv"
    recent_path = args.plugins / "DS2SpawnerRecent.tsv"
    extra_path = args.plugins / "DS2ExtraSpawns.tsv"
    log_path = args.plugins / "DS2Randomizer.log"
    ini_path = args.ini or args.plugins / "DS2Randomizer.ini"
    try:
        catalog = load_catalog(args.catalog)
    except (OSError, ValueError, KeyError) as error:
        messagebox.showerror("Catalog unavailable", str(error))
        window.destroy()
        return
    rules, recent, last_signature = {}, [], None
    target_labels = {row.get("target_guid", ""): row.get("target_classes", "Unknown") for row in catalog.values()}
    bindings = ROOT / "analysis/global_target_bindings.json"
    if bindings.exists():
        try:
            for target in json.loads(bindings.read_text(encoding="utf-8"))["targets"]:
                target_labels[target["guid"]] = target["family"] + " / " + target["package"]
        except (OSError, ValueError, KeyError):
            pass
    row_guids = {}
    undo_changes = None
    status = tk.StringVar(value="Waiting for the game. Open this window with Alt+Tab while playing.")
    selection_info = tk.StringVar(value="Select one or more spawners. Rules identify the source, not every enemy of that type.")
    source_view = tk.StringVar(value="Recent spawns")
    # Which difficulties the next saved rule is written for. The plugin reads
    # the player's own choice out of the game's settings file and obeys the row
    # written for it, else the general one. Nothing ticked means every
    # difficulty; ticking Zealot and Hard Core writes one row for both and
    # leaves the general row alone.
    scope_boxes = {name: tk.BooleanVar(value=False) for name in DIFFICULTIES}
    scope_label = tk.StringVar(value="every difficulty")

    def scope_names():
        return frozenset(name for name, box in scope_boxes.items() if box.get())

    def scope_changed():
        names = scope_names()
        scope_label.set(", ".join(n for n in DIFFICULTIES if n in names) or "every difficulty")

    def scope_clear():
        for box in scope_boxes.values():
            box.set(False)
        scope_changed()
    search = tk.StringVar()
    topmost = tk.BooleanVar(value=False)
    frame = ttk.Frame(window, padding=16)
    frame.pack(fill="both", expand=True)
    ttk.Label(frame, text="Spawner Rules", font=("Segoe UI", 19, "bold")).pack(anchor="w")
    ttk.Label(frame, text="Keep scripted encounters original, or allow individual spawners. Changes affect future spawns.").pack(anchor="w", pady=(4, 12))
    toolbar = ttk.Frame(frame)
    toolbar.pack(fill="x")
    view = ttk.Combobox(toolbar, textvariable=source_view, state="readonly", width=24,
        values=("Recent spawns", "All catalog spawners", "Custom rules"))
    view.pack(side="left")
    ttk.Label(toolbar, text="  Filter:").pack(side="left")
    ttk.Entry(toolbar, textvariable=search).pack(side="left", fill="x", expand=True, padx=6)
    ttk.Checkbutton(toolbar, text="Always on top", variable=topmost,
        command=lambda: window.attributes("-topmost", topmost.get())).pack(side="right")
    table_frame = ttk.Frame(frame)
    table_frame.pack(fill="both", expand=True, pady=10)
    columns = ("name", "family", "replacement", "result", "rule", "area", "event")
    tree = ttk.Treeview(table_frame, columns=columns, show="headings", selectmode="extended")
    for column, label, width in zip(columns,
            ("Spawner", "Original family", "Replacement", "Last observed result", "Saved rule", "Area / archive", "Event"),
            (240, 110, 180, 235, 200, 320, 70)):
        tree.heading(column, text=label)
        tree.column(column, width=width, minwidth=60)
    scroll_y = ttk.Scrollbar(table_frame, orient="vertical", command=tree.yview)
    scroll_x = ttk.Scrollbar(table_frame, orient="horizontal", command=tree.xview)
    tree.configure(yscrollcommand=scroll_y.set, xscrollcommand=scroll_x.set)
    tree.grid(row=0, column=0, sticky="nsew")
    scroll_y.grid(row=0, column=1, sticky="ns")
    scroll_x.grid(row=1, column=0, sticky="ew")
    table_frame.rowconfigure(0, weight=1)
    table_frame.columnconfigure(0, weight=1)
    ttk.Label(frame, textvariable=selection_info, wraplength=1150).pack(anchor="w", pady=(0, 8))
    buttons = ttk.Frame(frame)
    buttons.pack(fill="x")

    def selected_guids():
        return {row_guids[item] for item in tree.selection() if item in row_guids}

    def describe_selection(_event=None):
        selected = selected_guids()
        if len(selected) == 1:
            guid = next(iter(selected))
            selection_info.set(f"Source GUID: {guid}  |  {catalog.get(guid, {}).get('source_name', 'Uncatalogued spawner')}")
        else:
            selection_info.set(f"{len(selected)} spawners selected. Ctrl+click or Shift+click to select several.")

    def refresh_table():
        selected = selected_guids()
        scroll = tree.yview()[0]
        tree.delete(*tree.get_children())
        row_guids.clear()
        latest = {}
        for event in recent:
            latest.setdefault(event["source_guid"], event)
        if source_view.get() == "Recent spawns":
            guids = list(latest)
        elif source_view.get() == "Custom rules":
            guids = sorted(rules)
        else:
            guids = sorted(catalog, key=lambda guid: (catalog[guid]["archive"], catalog[guid]["source_name"]))
        terms = search.get().lower().split()
        for guid in guids:
            authored, event = catalog.get(guid, {}), latest.get(guid, {})
            raw_reason = event.get("result", "")
            result = REASONS.get(raw_reason, raw_reason) if event else "Not observed this session"
            if event and event.get("actor_created") == "0":
                result += " / creation failed"
            values = (authored.get("source_name", guid), authored.get("target_classes", "Unknown"),
                target_labels.get(event.get("selected_guid"), event.get("selected_guid", "Unknown"))
                    if raw_reason == "packet_replaced" and event.get("actor_created") == "1" else "-",
                result, rule_label(rules.get(guid)), authored.get("archive", "Unknown"), event.get("sequence", ""))
            haystack = (guid + " " + " ".join(values) + " " + raw_reason).lower()
            if not all(term in haystack for term in terms):
                continue
            item = tree.insert("", "end", values=values)
            row_guids[item] = guid
            if guid in selected:
                tree.selection_add(item)
        tree.yview_moveto(scroll)
        describe_selection()

    def apply(change, description=None):
        nonlocal rules, undo_changes
        selected = selected_guids()
        if not selected:
            status.set("Select at least one spawner first.")
            return
        requested = change if isinstance(change, dict) else {guid: change for guid in selected}
        # The chooser applies to whatever is being saved right now. With nothing
        # ticked the rule becomes the spawner's only row. With difficulties
        # ticked it replaces the rows that answer any of them and leaves the
        # rest, so the intro can be blocked on Zealot and Hard Core while the
        # general row keeps its family list.
        wanted = difficulty_text(scope_names())
        try:
            before = read_rules(rules_path)
            undo = {guid: {"rows": before.get(guid, [])} for guid in requested}
            if wanted:
                scoped = {}
                for guid, value in requested.items():
                    rows = [row for row in before.get(guid, [])
                            if not difficulty_set(row["difficulty"]) or not overlaps(row["difficulty"], wanted)]
                    rows.append(dict(as_rule(value), difficulty=wanted))
                    scoped[guid] = {"rows": rows}
                requested = scoped
            rules = write_rules(rules_path, requested)
            undo_changes = undo
            status.set(f"Saved {description or change} for {len(requested)} spawner(s). "
                "The enabled game plugin reloads rules within about one second.")
            refresh_table()
        except (OSError, ValueError) as error:
            messagebox.showerror("Rules were not saved", str(error))

    def undo():
        nonlocal rules, undo_changes
        if not undo_changes:
            status.set("No editor action to undo.")
            return
        try:
            rules = write_rules(rules_path, undo_changes)
            undo_changes = None
            refresh_table()
            status.set("Last editor action undone.")
        except (OSError, ValueError) as error:
            messagebox.showerror("Undo failed", str(error))

    def limit_families():
        """Restrict one spawner to the enemies it actually copes with.

        Some spawners only work with a few families. Nothing offline says which,
        so this is where what you saw in the game gets written down.
        """
        selected = selected_guids()
        if not selected:
            status.set("Select at least one spawner first.")
            return
        families = target_families(ini_path)
        if not families:
            messagebox.showerror("No family list",
                f"No [Targets] section found in {ini_path}. Point --ini at the plugin INI.")
            return
        # Prefilled only when every selected spawner already carries the same
        # list; otherwise the dialog starts empty rather than inventing one.
        shared, first = None, True
        for guid in selected:
            row = row_for(rules.get(guid), difficulty_text(scope_names())) or {}
            current = tuple(row.get("families", ()))
            shared, first = (current if first else (shared if shared == current else ())), False
        dialog = tk.Toplevel(window)
        dialog.title("Families allowed at this spawner")
        dialog.transient(window)
        dialog.resizable(False, False)
        body = ttk.Frame(dialog, padding=16)
        body.pack(fill="both", expand=True)
        ttk.Label(body, text=f"{len(selected)} spawner(s) selected. Ticked families may replace the "
            "authored enemy here; everything else keeps the original.", wraplength=520).grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, 10))
        boxes = {}
        for index, (name, enabled) in enumerate(families):
            variable = tk.BooleanVar(value=name in (shared or ()))
            label = name if enabled else name + "  (off in the INI)"
            ttk.Checkbutton(body, text=label, variable=variable,
                state="normal" if enabled else "disabled").grid(
                row=1 + index // 3, column=index % 3, sticky="w", padx=(0, 18))
            boxes[name] = variable
        # Row 0 is the heading, then ceil(n/3) rows of checkboxes, then the note
        # and the buttons, each on its own row, or they land in the same cell.
        last = 1 + (len(families) - 1) // 3
        ttk.Label(body, text="No tick at all removes the restriction. The INI stays above this: a family "
            "switched off there is never selected, whatever is ticked.", wraplength=520).grid(
            row=last + 1, column=0, columnspan=3, sticky="w", pady=(12, 0))
        footer = ttk.Frame(body)
        footer.grid(row=last + 2, column=0, columnspan=3, sticky="ew", pady=(14, 0))

        def commit():
            chosen = [name for name, variable in boxes.items() if variable.get()]
            dialog.destroy()
            requested = {}
            for guid in selected:
                row = row_for(rules.get(guid), difficulty_text(scope_names())) or {}
                mode = row.get("mode", "default")
                # Ticking families on a blocked spawner means "randomize, but
                # only these"; ticking nothing leaves the block alone.
                if chosen and mode == "block":
                    mode = "default"
                requested[guid] = {"mode": mode, "families": chosen}
            apply(requested, f"family limit ({', '.join(chosen) or 'none'})")

        ttk.Button(footer, text="Save", command=commit).pack(side="left")
        ttk.Button(footer, text="Clear all", command=lambda: [v.set(False) for v in boxes.values()]).pack(
            side="left", padx=8)
        ttk.Button(footer, text="Cancel", command=dialog.destroy).pack(side="left")
        dialog.grab_set()

    ttk.Button(buttons, text="Keep original / Block", command=lambda: apply("block")).pack(side="left", padx=(0, 8))
    ttk.Button(buttons, text="Allow randomization", command=lambda: apply("allow")).pack(side="left", padx=(0, 8))
    ttk.Button(buttons, text="Limit families...", command=limit_families).pack(side="left", padx=(0, 8))
    ttk.Button(buttons, text="Use default rule", command=lambda: apply("default")).pack(side="left", padx=(0, 8))
    ttk.Button(buttons, text="Undo last action", command=undo).pack(side="left", padx=(0, 16))

    def copy_guid():
        selected = selected_guids()
        if len(selected) != 1:
            status.set("Select exactly one spawner to copy its GUID.")
            return
        guid = next(iter(selected))
        window.clipboard_clear()
        window.clipboard_append(guid)
        status.set(f"Copied {guid} to the clipboard.")

    def extra_spawn_dialog():
        """Write a DS2ExtraSpawns.tsv row from the selected spawner and the last F10 readout."""
        selected = selected_guids()
        readout = latest_readout(log_path)
        dialog = tk.Toplevel(window)
        dialog.title("Extra spawn at the player's position")
        dialog.transient(window)
        body = ttk.Frame(dialog, padding=12)
        body.pack(fill="both", expand=True)
        ttk.Label(body, text="Stand where the enemy should appear, look the way it should face, press the readout key "
                  "(PlacementHotkey, F10 by default), then click Read readout. The enemy spawns when the source spawner fires - "
                  "or, as an area rule, when you come within a few metres of the spot (for rooms where nothing spawns at the door).",
                  wraplength=560).grid(row=0, column=0, columnspan=4, sticky="w", pady=(0, 10))
        guid_var = tk.StringVar(value=next(iter(selected)) if len(selected) == 1 else "")
        area_var = tk.BooleanVar(value=False)
        radius_var = tk.StringVar(value="20")
        kind_var = tk.StringVar(value="Guardian")
        count_var = tk.StringVar(value="1")
        chance_var = tk.StringVar(value="100")
        pos_vars = [tk.StringVar() for _ in range(3)]
        yaw_var = tk.StringVar()
        arrival_var = tk.StringVar(value="direct")
        readout_info = tk.StringVar(value="No readout read yet.")
        ttk.Label(body, text="Source spawner GUID").grid(row=1, column=0, sticky="w")
        guid_box = ttk.Combobox(body, textvariable=guid_var, width=44)
        guid_box.grid(row=1, column=1, columnspan=3, sticky="we", pady=2)
        area_line = ttk.Frame(body)
        area_line.grid(row=13, column=0, columnspan=4, sticky="w", pady=(6, 0))
        area_check = ttk.Checkbutton(area_line, variable=area_var,
                                     text="Area rule: no spawner, fires once when I come within")
        area_check.pack(side="left")
        radius_entry = ttk.Entry(area_line, textvariable=radius_var, width=6)
        radius_entry.pack(side="left", padx=(6, 4))
        ttk.Label(area_line, text="metres of the position (the plugin borrows a live spawner itself; direct placement).").pack(side="left")

        def area_toggled(*_args):
            on = area_var.get()
            guid_box.configure(state="disabled" if on else "normal")
            radius_entry.configure(state="normal" if on else "disabled")
            if on:
                arrival_var.set("direct")
        area_var.trace_add("write", area_toggled)
        radius_entry.configure(state="disabled")
        ttk.Label(body, text="Kind (family or pool package)").grid(row=2, column=0, sticky="w")
        packages = sorted({label.split(" / ")[1] for label in target_labels.values() if " / " in label})
        ttk.Combobox(body, textvariable=kind_var, values=list(EXTRA_KINDS) + packages, width=30).grid(row=2, column=1, sticky="w", pady=2)
        ttk.Label(body, text="Count (1..8)").grid(row=3, column=0, sticky="w")
        ttk.Entry(body, textvariable=count_var, width=6).grid(row=3, column=1, sticky="w", pady=2)
        ttk.Label(body, text="Chance % (seeded)").grid(row=4, column=0, sticky="w")
        ttk.Entry(body, textvariable=chance_var, width=6).grid(row=4, column=1, sticky="w", pady=2)
        ttk.Label(body, text="Position x y z").grid(row=5, column=0, sticky="w")
        coords = ttk.Frame(body)
        coords.grid(row=5, column=1, columnspan=3, sticky="w", pady=2)
        for var in pos_vars:
            ttk.Entry(coords, textvariable=var, width=10).pack(side="left", padx=(0, 4))
        ttk.Label(body, text="Yaw (degrees)").grid(row=6, column=0, sticky="w")
        ttk.Entry(body, textvariable=yaw_var, width=8).grid(row=6, column=1, sticky="w", pady=2)
        ttk.Label(body, text="Arrival").grid(row=7, column=0, sticky="w")
        ttk.Combobox(body, textvariable=arrival_var, values=("direct", "here"), width=8, state="readonly").grid(row=7, column=1, sticky="w", pady=2)
        ttk.Label(body, text="direct = placed at the position above. here = wherever the spawner puts it (vents).",
                  wraplength=560).grid(row=8, column=0, columnspan=4, sticky="w")
        ttk.Label(body, textvariable=readout_info, wraplength=560).grid(row=9, column=0, columnspan=4, sticky="w", pady=(8, 2))
        existing = tk.Listbox(body, height=6, width=90)
        existing.grid(row=10, column=0, columnspan=4, sticky="we", pady=(6, 2))
        ttk.Label(body, text="Rows already in DS2ExtraSpawns.tsv (select one to remove it).").grid(row=11, column=0, columnspan=4, sticky="w")

        def refresh_existing():
            existing.delete(0, "end")
            for row in read_extra_rows(extra_path):
                existing.insert("end", "   ".join(row))

        def apply_readout():
            nonlocal readout
            readout = latest_readout(log_path)
            if not readout:
                readout_info.set("No readout in the log yet. Press the key in the game and wait a few seconds.")
                return
            for var, key in zip(pos_vars, ("x", "y", "z")):
                var.set(readout[key])
            yaw_var.set(readout["yaw"])
            near = [guid for guid, _distance, _yaw in readout["near"]]
            guid_box["values"] = ([guid_var.get()] if guid_var.get() and guid_var.get() not in near else []) + near
            if not guid_var.get() and near:
                guid_var.set(near[0])
            nearest = ", ".join(f"{guid[:17]}.. at {distance:.0f} m" for guid, distance, _yaw in readout["near"][:3]) or "none listed"
            readout_info.set(f"Readout: x={readout['x']} y={readout['y']} z={readout['z']} yaw={readout['yaw']}. Nearest spawners: {nearest}. "
                             "Distances are to the spawner's logic object, not to where the enemy comes out.")

        def add_row():
            guid = guid_var.get().strip().upper()
            if area_var.get():
                radius = radius_var.get().strip() or "20"
                guid = f"near:{radius}"
                arrival_var.set("direct")
            position = [var.get().strip() for var in pos_vars]
            problem = check_extra_row(guid, kind_var.get().strip(), count_var.get().strip(), chance_var.get().strip(),
                                      position, yaw_var.get().strip(), arrival_var.get())
            if problem:
                messagebox.showerror("Row refused", problem, parent=dialog)
                return
            rows = read_extra_rows(extra_path)
            rows.append([guid, kind_var.get().strip(), count_var.get().strip(), chance_var.get().strip(),
                         position[0], position[1], position[2], yaw_var.get().strip(), arrival_var.get()])
            try:
                write_extra_rows(extra_path, rows)
            except OSError as error:
                messagebox.showerror("Write failed", str(error), parent=dialog)
                return
            refresh_existing()
            status.set(f"Added {kind_var.get()} at {guid} to {extra_path.name}. The plugin reloads the file while the game runs."
                       + (" Area rules re-arm when the level's streams are torn down." if guid.startswith("near") else ""))

        def remove_row():
            picked = existing.curselection()
            if not picked:
                return
            rows = read_extra_rows(extra_path)
            if picked[0] < len(rows):
                del rows[picked[0]]
                write_extra_rows(extra_path, rows)
                refresh_existing()
                status.set(f"Removed a row from {extra_path.name}.")

        footer = ttk.Frame(body)
        footer.grid(row=12, column=0, columnspan=4, sticky="w", pady=(10, 0))
        ttk.Button(footer, text="Read readout", command=apply_readout).pack(side="left", padx=(0, 8))
        ttk.Button(footer, text="Add row", command=add_row).pack(side="left", padx=(0, 8))
        ttk.Button(footer, text="Remove selected row", command=remove_row).pack(side="left", padx=(0, 8))
        ttk.Button(footer, text="Close", command=dialog.destroy).pack(side="left")
        refresh_existing()
        if readout:
            apply_readout()

    ttk.Button(buttons, text="Copy GUID", command=copy_guid).pack(side="left", padx=(0, 8))
    ttk.Button(buttons, text="Extra spawn...", command=extra_spawn_dialog).pack(side="left", padx=(0, 16))
    ttk.Label(buttons, text="applies to:").pack(side="left")
    scope_button = ttk.Menubutton(buttons, textvariable=scope_label, width=24)
    scope_menu = tk.Menu(scope_button, tearoff=False)
    for name in DIFFICULTIES:
        scope_menu.add_checkbutton(label=name, variable=scope_boxes[name], command=scope_changed)
    scope_menu.add_separator()
    scope_menu.add_command(label="every difficulty", command=scope_clear)
    scope_button.configure(menu=scope_menu)
    scope_button.pack(side="left", padx=(4, 0))

    def select_visible():
        tree.selection_set(tree.get_children())
        describe_selection()

    ttk.Button(buttons, text="Select filtered rows", command=select_visible).pack(side="right")
    ttk.Label(frame, text="Allow overrides the source-family switch and chance only. Unsupported classes, arrivals and resource checks still apply.",
        wraplength=1150).pack(anchor="w", pady=(12, 4))
    ttk.Label(frame, text="A block leaves the authored enemy intact. It does not remove a spawn. Repeat the encounter after changing a rule to evaluate it.",
        wraplength=1150).pack(anchor="w")
    ttk.Label(frame, text="Limit families is for spawners that only cope with some enemies: tick what worked there, and everything else keeps the original. It only takes candidates away, never adds one the INI refused.",
        wraplength=1150).pack(anchor="w")
    ttk.Label(frame, text="Applies to: with nothing ticked the saved rule is the spawner's only one. With difficulties ticked it gets its own row next to the general rule, and the game uses the row for the difficulty being played, else the general one. A block on Zealot and Hard Core can sit next to a family limit for everything else.",
        wraplength=1150).pack(anchor="w")
    ttk.Label(frame, textvariable=status, wraplength=1150).pack(anchor="w", pady=(10, 0))
    health = tk.StringVar()
    ttk.Label(frame, textvariable=health, wraplength=1150).pack(anchor="w", pady=(4, 0))
    tree.bind("<<TreeviewSelect>>", describe_selection)
    search.trace_add("write", lambda *_: refresh_table())
    source_view.trace_add("write", lambda *_: refresh_table())

    def poll():
        nonlocal rules, recent, last_signature
        try:
            signature = tuple((p.stat().st_mtime_ns, p.stat().st_size) if p.exists() else None
                for p in (rules_path, recent_path))
            if signature != last_signature:
                rules = read_rules(rules_path)
                recent = recent_rows(recent_path)
                last_signature = signature
                refresh_table()
            if recent:
                age = max(0, int(time.time() - recent_path.stat().st_mtime))
                health.set(f"Captured session PID {recent[0]['pid']}, latest spawn snapshot {age}s ago. History may be from an earlier run. Rules: {rules_path}")
            else:
                health.set(f"No captured spawns. Requires SpawnerTools=1 in the plugin INI. Rules: {rules_path}")
        except (OSError, ValueError, KeyError) as error:
            health.set(f"Read failed: {error}")
        window.after(1000, poll)

    refresh_table()
    poll()
    window.mainloop()


if __name__ == "__main__":
    main()
