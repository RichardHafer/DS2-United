"""Inspect DS2 stream lists and exception state without changing the dump."""
import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "trainer"))
from analyze_minidump import Dump


def inspect(path):
    dump = Dump(path)
    try:
        bases = {m[0] for m in dump.modules() if Path(m[2]).name.lower() == "deadspace2.exe"}
        if len(bases) != 1:
            raise ValueError("Expected exactly one Dead Space 2 module")
        base = bases.pop()

        def read(address, size):
            value = dump.read(address, size)
            if value is None or len(value) != size:
                raise ValueError(f"Memory unavailable at {address:08X}, size {size}")
            return value

        def u32(address):
            return struct.unpack("<I", read(address, 4))[0]

        def name(address):
            value = bytearray()
            for offset in range(160):
                byte = read(address + offset, 1)
                if byte == b"\0":
                    return value.decode("ascii", errors="replace")
                value.extend(byte)
            raise ValueError("Unterminated stream name")

        report = {"dump": str(path), "base": base, "scope": "Read-only snapshot; not a causal diagnosis"}
        exception = dump.exception()
        if exception:
            thread, code, flags, _, address, info, size, rva = exception
            registers = dump.context32_registers(rva, size)
            report["exception"] = dict(thread=thread, code=code, flags=flags,
                address=address & 0xFFFFFFFF, parameters=list(info), registers=registers)
            if registers:
                report["exception"]["stack_top"] = dump.u32(registers["esp"])
        manager = u32(base + 0x1C1C098)
        report.update(manager=manager, service=u32(base + 0x1C1C0C8), lists=[], errors=[])
        if not manager:
            return report
        for offset in (0x8C, 0x7C, 0x74, 0x84):
            listing = {"offset": offset, "entries": []}
            report["lists"].append(listing)
            try:
                pointer = u32(manager + offset)
                seen = set()
                while pointer:
                    if pointer in seen or len(seen) >= 512:
                        raise ValueError("Stream list cycle or traversal budget exhausted")
                    seen.add(pointer)
                    fields = struct.unpack("<23I", read(pointer, 0x5C))
                    descriptor = fields[6]
                    entry = dict(pointer=pointer, descriptor=descriptor, handle=fields[7],
                        status=fields[9], flags=fields[21], dependency_refs=fields[22] & 0xFFFF,
                        notification_bits=u32(pointer + 0x68), wait_handle_count=u32(pointer + 0xF4))
                    listing["entries"].append(entry)
                    if descriptor:
                        entry["name"] = name(u32(u32(descriptor) + 4))
                    if offset == 0x8C:
                        break
                    pointer = fields[0]
            except ValueError as error:
                report["errors"].append({"list_offset": offset, "message": str(error)})
        return report
    finally:
        dump.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = inspect(args.dump)
    encoded = json.dumps(report, indent=2)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)


if __name__ == "__main__":
    main()
