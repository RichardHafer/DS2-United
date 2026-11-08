"""Read retail uniform code and state from a running game or a full crash dump."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "trainer"))
sys.path.insert(0, str(ROOT / "tools"))

# PC VAs recovered from the retail iMsgMPUniformStreamsPreloaded references.
FUNCTIONS = {
    "LoadMPUniformStreams": (0x42DD50, 0x42DE16),
    "PreloadUniformStreams": (0xED5E90, 0xED5F6C),
    "ChangeToUniform": (0xEE0610, 0xEE079F),
    "AttachUniformToPlayer": (0xEB4E10, 0xEB5040),
    "LoadUniformStream": (0xEC6310, 0xEC63CE),
    "PlayerAttachModel": (0x892490, 0x8925E6),
    "AnimatedAttachModel": (0x753EE0, 0x753F59),
    "BuildBoneBuffer": (0x753CE0, 0x753ED5),
    "UniformManagerCtor": (0xEC6140, 0xEC61A6),
    "UniformManagerEvents": (0xED5BF0, 0xED5DCB),
}


def inspect(read):
    def u32(a):
        raw = read(a, 4)
        return struct.unpack("<I", raw)[0] if raw and len(raw) == 4 else 0

    def body(p):
        view = u32(p + 0x194)
        proxy = u32(p + 0x68)
        return dict(player=p, vtable=u32(p), view=view,
                    bones=u32(view + 0xB8) if view else 0,
                    bone_count=u32(view + 0xB0) if view else 0,
                    bone_buffer=u32(p + 0x1E0), buffer_count=u32(p + 0x1E4),
                    animation_source=u32(p + 0x188), mmi=u32(p + 0x140),
                    uniform=u32(p + 0x1688), proxy=proxy,
                    proxy_flags=u32(proxy + 0x30) if proxy else 0)

    manager = u32(0x201C0DC)
    result = dict(manager=manager, vtable=u32(manager) if manager else 0,
                  uniforms=[], players=[], functions={})
    for name, (start, end) in FUNCTIONS.items():
        code = read(start, end - start)
        result["functions"][name] = dict(va=start, size=end-start,
            head=code[:16].hex() if code else None,
            sha256=hashlib.sha256(code).hexdigest() if code else None)
    if not manager or result["vtable"] != 0x1B5CA78:
        result["state"] = "manager_unavailable"
        return result
    for field, offset, stride in [("uniforms", 0x10, 0x18), ("players", 0x1C, 12)]:
        ptr, count, capacity = (u32(manager + offset + i * 4) for i in range(3))
        if not 0 <= count <= capacity <= 128:
            result["state"] = "invalid_array"
            return result
        for i in range(count):
            raw = read(ptr + i * stride, stride)
            if not raw or len(raw) != stride:
                result["state"] = "unreadable_entry"
                return result
            entry = dict(index=i)
            if field == "uniforms":
                entry.update(loaded=raw[0], dlc=raw[1], activations=raw[2],
                             guid=list(struct.unpack_from("<4I", raw, 4)),
                             stream=struct.unpack_from("<I", raw, 20)[0])
            else:
                safe = struct.unpack_from("<I", raw, 4)[0]
                entry.update(waiting=raw[0], uniform_index=struct.unpack_from("b", raw, 1)[0],
                             body=body(safe - 0x10) if safe else None)
            result[field].append(entry)
    result["state"] = "ok"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--disassembly", type=Path)
    args = parser.parse_args()
    if args.dump:
        from analyze_minidump import Dump
        source = Dump(args.dump)
        read, close = source.read, source.close
        origin = str(args.dump)
    else:
        from memscan import find_pid, read_mem, k32
        pid = find_pid("deadspace2")
        if not pid:
            parser.error("deadspace2 is not running; no process was started")
        handle = k32.OpenProcess(0x410, False, pid)
        if not handle:
            raise OSError("OpenProcess failed")
        read = lambda a, n: read_mem(handle, a, n)
        close = lambda: k32.CloseHandle(handle)
        origin = f"live PID {pid}"
    try:
        result = inspect(read)
        result["source"] = origin
        if args.dump:
            exception = source.exception()
            if exception:
                regs = source.context32_registers(exception[-1], exception[-2])
                result["exception"] = dict(address=exception[4], registers=regs)
                if exception[4] == 0x106B1D0 and regs and source.u32(regs["esp"] + 8) == 0x1076AA9:
                    skin = regs["edi"]
                    count = source.u32(skin + 0xBC)
                    indices = source.u32(skin + 0x11C)
                    raw = read(indices, count * 4) if indices and count is not None and count <= 132 else None
                    result["fault_skin"] = dict(address=skin, count=count, pose=source.u32(skin + 0x120),
                        indices=list(struct.unpack(f"<{count}I", raw)) if raw else None)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        if args.disassembly:
            import capstone
            decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
            lines = [f"Source: {origin}; PC x86; VAs at base 00400000"]
            for name, (start, end) in FUNCTIONS.items():
                lines.append(f"\n{name}:")
                for ins in decoder.disasm(read(start, end - start) or b"", start):
                    lines.append(f"{ins.address:08X} {ins.bytes.hex():24} {ins.mnemonic:8} {ins.op_str}")
            args.disassembly.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"{origin}: {result['state']}; {len(result['players'])} registered players")
    finally:
        close()


if __name__ == "__main__":
    main()
