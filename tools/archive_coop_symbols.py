"""Keep the optimized plugin and its private symbols together for dump analysis."""
import hashlib
from pathlib import Path
from shutil import copy2


if __name__ == "__main__":
    build = Path(__file__).resolve().parents[1] / "build"
    plugin = build / "DS2United.asi"
    symbols = plugin.with_suffix(".pdb")
    digest = hashlib.sha256(plugin.read_bytes()).hexdigest()
    archive = build / "symbols" / digest
    archive.mkdir(parents=True, exist_ok=True)
    for source in (plugin, symbols):
        copy2(source, archive)
    print(f"Archived plugin and symbols in {archive}")
