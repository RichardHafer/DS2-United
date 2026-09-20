"""Read where each authored attribute lives inside a component, from a dump.

Guessing offsets and scanning the running game were both dead ends. The engine
carries the answer itself: a class descriptor holds an attribute table at +0x1C,
and every 16-byte row ends with a dword that packs the attribute's index in the
high half and its offset in the low half.

The offset is relative to the data area, which is what a component points at:
`[component+0x0C]` holds `header + 0x10`, and the row's offset is counted from
there. `CMD_SetOnDeathMessage` on DismemberableHealthComponent reads 0x00010004 -
index 1, offset 4 - and 0x10 + 4 = 0x14, exactly where the hash was measured in
the running game.

So the address of an attribute is simply `[component+0x0C] + offset`.
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, r"C:\Users\me\Programme\DeadSpace3-Mod\trainer")
import dmpread  # noqa: E402

BASE, IMAGE = 0x00400000, 0x03384000
ATTRIBUTE_TABLE = 0x1C
ROW_BYTES = 0x10
SLOT_NAME = 0x00


def name_hash(text):
    value = 0
    for char in text.encode("latin1"):
        if 0x41 <= char <= 0x5A:
            char += 0x20
        value = (value * 0x1003F + char) & 0xFFFFFFFF
    return value


class Image:
    def __init__(self, path):
        self.dump = dmpread.Dump(path)

    def u32(self, address):
        return self.dump.u32(address)

    def text(self, address, limit=96):
        if not address or not (BASE <= address < BASE + IMAGE):
            return ""
        data = self.dump.read(address, limit)
        if not data:
            return ""
        end = data.find(b"\0")
        if end <= 0:
            return ""
        try:
            value = data[:end].decode("latin1")
        except UnicodeDecodeError:
            return ""
        return value if value.isprintable() else ""

    def class_descriptor(self, name):
        """A descriptor holds its name at +0x04 and hash(name) at +0x08."""
        wanted = name_hash(name)
        for address in self.dump.suche(struct.pack("<I", wanted), limit=64,
                                       lo=BASE, hi=BASE + IMAGE):
            descriptor = address - 8
            if self.text(self.u32(descriptor + 4)) == name:
                return descriptor
        return None

    def attributes(self, descriptor, limit=96):
        """(name, offset, type name) for every attribute of that class."""
        table = self.u32(descriptor + ATTRIBUTE_TABLE)
        if not table:
            return []
        out = []
        # The table starts one dword into the first row: the rows that matter
        # begin at +0x0C, where the first attribute slot sits.
        for index in range(limit):
            # The table opens with the base class descriptor, then 16-byte rows of
            # (default, attribute slot, index:offset, flags).
            row = table + 4 + index * ROW_BYTES
            slot = self.u32(row + 4)
            packed = self.u32(row + 8)
            if slot is None or packed is None:
                break
            if not slot or not (BASE <= slot < BASE + IMAGE):
                break
            name = self.text(self.u32(slot + SLOT_NAME))
            if not name:
                break
            if (packed >> 16) != index:
                break
            out.append((name, packed & 0xFFFF, self.text(self.u32(slot + 0x10))))
        return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dump")
    parser.add_argument("--classes", nargs="*", default=[
        "DismemberableHealthComponent", "HealthComponent",
        "DSNPCSpawningComponent", "AwarenessComponent"])
    parser.add_argument("--wanted", nargs="*", default=[
        "CMD_SetOnDeathMessage", "CMD_SetOnHealthModifiedMsg",
        "CMD_SetSpawnDoneSendMessage", "MSG_AWARE"])
    args = parser.parse_args()

    image = Image(args.dump)
    for name in args.classes:
        descriptor = image.class_descriptor(name)
        if not descriptor:
            print("%s: descriptor not found" % name)
            continue
        attributes = image.attributes(descriptor)
        print("\n%s  descriptor %08X  %d attributes" % (name, descriptor, len(attributes)))
        for attribute, offset, kind in attributes:
            mark = "  <== wanted" if attribute in args.wanted else ""
            print("   +0x%04X  %-40s %s%s" % (offset, attribute, kind, mark))


if __name__ == "__main__":
    main()
