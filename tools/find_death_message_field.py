"""Find where a death message actually lives, by looking at the running game.

The value is authored as a string, so at runtime it is either the engine's hash
of that string or a pointer to it. Searching the plugin's own guesses failed
(every `sites=` entry stayed zero), so this asks the process instead: with the
hospital lobby loaded, the four slashers must carry
`hospital_lobby_slasher_dead` somewhere, and wherever that lands is the field.

Read-only. Nothing is written to the game.
"""
import argparse
import ctypes
import struct
import subprocess
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
BASE, IMAGE = 0x00400000, 0x03384000


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", ctypes.c_ulong), ("RegionSize", ctypes.c_size_t),
                ("State", ctypes.c_ulong), ("Protect", ctypes.c_ulong), ("Type", ctypes.c_ulong)]


def engine_hash(text):
    value = 0
    for char in text.lower():
        value = (value * 0x1003F + ord(char)) & 0xFFFFFFFF
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", default="hospital_lobby_slasher_dead")
    parser.add_argument("--limit", type=int, default=40)
    args = parser.parse_args()

    pid = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "(Get-Process -Name deadspace2 -ErrorAction SilentlyContinue).Id"],
                         capture_output=True, text=True).stdout.split()
    if not pid:
        print("game not running")
        return
    handle = k32.OpenProcess(0x0410, False, int(pid[0]))
    hashed = engine_hash(args.name)
    print("pid %s, looking for %r: hash %08X and any pointer to the string"
          % (pid[0], args.name, hashed))

    def read(address, size):
        buffer = (ctypes.c_char * size)()
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(handle, ctypes.c_void_p(address), buffer, size,
                                     ctypes.byref(got)):
            return None
        return bytes(buffer[:got.value])

    # Where does the string itself live? Those addresses are what a pointer field
    # would hold.
    needle = args.name.encode() + b"\0"
    strings, hashes = [], []
    address = 0x10000
    info = MEMORY_BASIC_INFORMATION()
    scanned = 0
    while address < 0x7FFF0000:
        if not k32.VirtualQueryEx(handle, ctypes.c_void_p(address), ctypes.byref(info),
                                  ctypes.sizeof(info)):
            break
        size = info.RegionSize
        if (info.State == MEM_COMMIT and not (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))
                and size <= 64 * 1024 * 1024):
            data = read(address, size)
            if data:
                scanned += size
                start = data.find(needle)
                while start != -1 and len(strings) < args.limit:
                    strings.append(address + start)
                    start = data.find(needle, start + 1)
                target = struct.pack("<I", hashed)
                start = data.find(target)
                while start != -1 and len(hashes) < 4096:
                    if start % 4 == 0:
                        hashes.append(address + start)
                    start = data.find(target, start + 1)
        address += size
    print("scanned %.0f MB" % (scanned / 1048576))
    print("the string itself: %d place(s): %s" % (len(strings), ["%08X" % s for s in strings[:8]]))
    outside = [h for h in hashes if not (BASE <= h < BASE + IMAGE)]
    print("the hash as a dword: %d total, %d outside the executable" % (len(hashes), len(outside)))
    for hit in outside[:args.limit]:
        # What object is this in? The owner field of a component points at its
        # actor, and an object starts with a vtable inside the executable.
        note = ""
        for back in range(0, 0x400, 4):
            head = read(hit - back, 4)
            if not head:
                continue
            vtable, = struct.unpack("<I", head)
            if BASE <= vtable < BASE + IMAGE:
                owner = read(hit - back + 0x10, 4)
                owner_value = struct.unpack("<I", owner)[0] if owner else 0
                note = "object %08X vtable %08X offset +0x%X owner %08X" % (
                    hit - back, vtable, back, owner_value)
                break
        print("   %08X  %s" % (hit, note or "no object head found"))

    if strings:
        pointers = []
        for value in strings:
            target = struct.pack("<I", value)
            address = 0x10000
            while address < 0x7FFF0000 and len(pointers) < 32:
                if not k32.VirtualQueryEx(handle, ctypes.c_void_p(address), ctypes.byref(info),
                                          ctypes.sizeof(info)):
                    break
                size = info.RegionSize
                if (info.State == MEM_COMMIT and not (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))
                        and size <= 64 * 1024 * 1024):
                    data = read(address, size)
                    if data:
                        start = data.find(target)
                        while start != -1 and len(pointers) < 32:
                            if start % 4 == 0:
                                pointers.append(address + start)
                            start = data.find(target, start + 1)
                address += size
        print("pointers to the string: %d: %s" % (len(pointers), ["%08X" % p for p in pointers[:12]]))


if __name__ == "__main__":
    main()
