"""Exercise editor actions using temporary files without touching the game."""
import sys
import tempfile
from pathlib import Path
import tkinter as tk
from tkinter import ttk

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import spawner_editor


def main():
    original_tk = tk.Tk
    errors = []
    guid = "00000001-00000002-00000003-00000004"
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        catalog = path / "catalog.tsv"
        catalog.write_text("source_guid\tsource_name\tarchive\ttarget_classes\n" +
            f"{guid}\tspawn_test\tds2dat0/ch01/test.str\tZombie\n", encoding="utf-8")
        (path / "DS2SpawnerRecent.tsv").write_text(
            "pid\tsequence\ttick\tsource_guid\ttarget_guid\tselected_guid\tresult\tactor_created\n" +
            f"1\t1\t1\t{guid}\t{guid}\t{guid}\tpacket_replaced\t1\n", encoding="ascii")

        def create_window():
            root = original_tk()
            root.withdraw()

            def descendants(widget):
                for child in widget.winfo_children():
                    yield child
                    yield from descendants(child)

            def exercise():
                try:
                    widgets = list(descendants(root))
                    tree = next(w for w in widgets if isinstance(w, ttk.Treeview))
                    buttons = {str(w.cget("text")): w for w in widgets if isinstance(w, ttk.Button)}
                    assert len(tree.get_children()) == 1
                    tree.selection_set(tree.get_children())
                    buttons["Keep original / Block"].invoke()
                    assert spawner_editor.read_rules(path / "DS2SpawnerRules.tsv") == {guid: "block"}
                    buttons["Allow randomization"].invoke()
                    assert spawner_editor.read_rules(path / "DS2SpawnerRules.tsv") == {guid: "allow"}
                    buttons["Undo last action"].invoke()
                    assert spawner_editor.read_rules(path / "DS2SpawnerRules.tsv") == {guid: "block"}
                    buttons["Use default rule"].invoke()
                    assert spawner_editor.read_rules(path / "DS2SpawnerRules.tsv") == {}
                except BaseException as error:
                    errors.append(error)
                finally:
                    root.destroy()

            root.after(150, exercise)
            return root

        tk.Tk = create_window
        sys.argv = ["spawner_editor", "--plugins", str(path), "--catalog", str(catalog)]
        try:
            spawner_editor.main()
        finally:
            tk.Tk = original_tk
    if errors:
        raise errors[0]
    print("Editor UI passed: recent selection, block, allow, undo and reset using temporary files.")


if __name__ == "__main__":
    main()
