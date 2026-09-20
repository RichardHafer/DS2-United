"""Read a DS2 stream snapshot without injecting code or suspending threads."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int)
    parser.add_argument("--base", type=lambda s: int(s, 0), default=0x400000)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    kernel = c.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    kernel.ReadProcessMemory.restype = w.BOOL
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
    handle = kernel.OpenProcess(0x1010, False, args.pid)
    if not handle:
        raise c.WinError(c.get_last_error())
    try:
        name = c.create_unicode_buffer(32768)
        size = w.DWORD(len(name))
        if not kernel.QueryFullProcessImageNameW(handle, 0, name, c.byref(size)) or Path(name.value).name.lower() != 'deadspace2.exe':
            raise ValueError("PID is not Dead Space 2")
        def read(address, size):
            buffer = c.create_string_buffer(size)
            actual = c.c_size_t()
            if not kernel.ReadProcessMemory(handle, address, buffer, size, c.byref(actual)) or actual.value != size:
                raise ValueError(f"Unreadable memory: {address:08X}")
            return buffer.raw
        def integer(address):
            return struct.unpack('<I', read(address, 4))[0]
        def text(address):
            output = bytearray()
            for offset in range(160):
                byte = read(address + offset, 1)
                if byte == b'\0':
                    return output.decode('ascii', errors='replace')
                output.extend(byte)
            return '<unterminated>'
        if read(args.base, 2) != b'MZ':
            raise ValueError("Invalid module base")
        manager = integer(args.base + 0x1C1C098)
        offsets = [0x8C, 0x7C, 0x74, 0x84]
        heads = [integer(manager + off) for off in offsets]
        entries = []
        seen = set()
        for index, head in enumerate(heads):
            pointer = head
            local = set()
            while pointer and len(entries) < 512:
                if pointer in local:
                    raise ValueError("Stream list cycle")
                local.add(pointer)
                fields = struct.unpack('<23I', read(pointer, 0x5C))
                if pointer not in seen:
                    seen.add(pointer)
                    descriptor = fields[6]
                    entry = dict(pointer=pointer, descriptor=descriptor, handle=fields[7],
                                 status=fields[9], flags=fields[21], refs=fields[22] & 0xFFFF)
                    if descriptor:
                        identity = integer(descriptor)
                        entry['name'] = text(integer(identity + 4))
                    entries.append(entry)
                if index == 0:
                    break
                pointer = fields[0]
        unchanged = heads == [integer(manager + off) for off in offsets]
        report = dict(pid=args.pid, image=name.value, manager=manager, heads_unchanged=unchanged,
                      scope="Non-atomic read-only snapshot", entries=entries)
        args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
        for entry in entries:
            print(f"{entry['handle']:08X} state={entry['status']} flags={entry['flags']:08X} refs={entry['refs']} {entry.get('name', '<no descriptor>')}")
    finally:
        kernel.CloseHandle(handle)


if __name__ == '__main__':
    main()
