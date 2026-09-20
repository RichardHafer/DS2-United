"""Check offline PC lifecycle sites. This does not validate a running process."""
import argparse
import hashlib
import json
from pathlib import Path
import pefile

SITES = {
    "stream_update_entry": (0x74BBC0, "83EC08538BD9E845CF0C00", "native stream update; budget and boolean arguments, ret 0C"),
    "stream_update_event_call": (0x74C570, "E84BF6FFFF5EC20400", "event receiver invokes stream update after deriving its time budget"),
    "stream_manager_destructor": (0x74BC90, "83EC1C568BF133C9B8D0747400", "manager teardown entry; invalidates private runtime ownership before native cleanup"),
    "stream_manager_clear_global": (0x74BDB6, "C70598C0010200000000", "teardown clears the global stream manager pointer"),
    "stream_load_toc_flag": (0x74BE23, "F6842440010000027418", "flag 2 enters the native TOC load path"),
    "stream_load_toc_call": (0x74BE3E, "E8DDD4FFFF8BD8", "filename loader retains the returned TOC for its paired release"),
    "actor_packet_stream_handle": (0x7764A0, "8B4E08894C2410", "actor creation saves SpawnInfo+08 as its stream handle"),
    "actor_stream_dispatch_test": (0x776535, "E8A4C7C8FF83F803", "status queried after construction; compare against dispatched state 3"),
    "stream_identity_handle_lookup": (0x7487C0, "8B818C0000008B5424045633F685C0", "identity comparison at stream+2C across active, pending, dispatched and release lists"),
    "stream_lookup_handle_field": (0x74881A, "8B401C5EC20400", "matched stream returns handle at +1C"),
    "toc_descriptor_binary_search": (0x848B02, "8D0C16D1E98D2C898B2CA83B7D00", "descriptor table ordered by unsigned identity value"),
    "toc_cross_registry_lookup": (0x748BC0, "8B4424248B083B4C2414", "dependency resolver searches other registered TOCs after local miss"),
    "toc_filename_load": (0x749320, "83EC248B442428535556", "manager thiscall, filename and optional prefix, ret 8"),
    "toc_relocation": (0x848CB0, "32C08139287BBA9C0F859D0000008379040A", "in-place TOC relocation, boolean AL result"),
    "toc_registry_capacity": (0x7494FE, "8B873001000083F820730D", "32 TOC entries; capacity failure skips insertion without clearing return pointer"),
    "toc_registry_insert": (0x749509, "89B487B0000000FF8730010000", "TOC pointer stored at manager+B0 and count at +130 incremented"),
    "toc_remove": (0x748300, "578B7C240885FF745C", "removes pointer by identity and frees it; caller must establish lifetime"),
    "toc_release_reference": (0x74A925, "B9FFFF00006601480E0FB7480E", "decrements TOC +0E before masked zero check and removal"),
    "metamodel_format": (0x840419, "81396C644D4D0F85D70200008379040E", "MetaModel magic and version 14 checked by PC relocation routine"),
    "metamodel_asset_table": (0x84042F, "8B413485C0740503C1894134", "serialized asset table offset +34 relocated against payload base"),
    "spawning_constructor": (0xF64B20, "8B442408568BF183C01056", "thiscall, owner and attribute header, ret 8"),
    "spawning_destructor": (0xF84870, "568BF18B4E10C7060C40B601", "payload at +0C; releases payload minus 10"),
    "attribute_retain": (0x7CEA40, "56578B7C240C8BF166FF460C", "16-bit reference count at header +0C"),
    "attribute_release": (0x7CEB60, "8B442404568BF185C0", "removes owner and frees final attribute reference"),
    "component_layout_lookup": (0x8198B0, "538B5C24088B03568D7114", "cache keyed by first component-list dword"),
    "component_layout_constructor": (0x819330, "5556578B7C24108B078BF18906", "copies component class descriptors"),
    "component_construction": (0x818F60, "51FF410453568B742414", "list slots at +8 reference attribute-header cells"),
    "actor_creation": (0x7CF3B0, "558B6C24088B451056578B38", "constructs actor, components, SpawnInit and post-init"),
    "manager_creation": (0x776480, "53568B74240C33C08BD9", "creation listeners can inspect the packet before factory"),
    "stream_status": (0x40DAA0, "8B44240450E89168FFFF85C0", "handle resolved through 40433B; status at stream +24"),
    "stream_filename_load": (0x74BDD0, "81EC2401000053558BE9", "filename loader; ownership and readiness remain incomplete"),
    "stream_unknown_name_branch": (0x74BF9B, "E8A0F9FFFF8BF085DB", "missing descriptor path calls new-record loader 74B940"),
    "stream_new_record_load": (0x74B940, "83EC38538B5C245055", "accepts null descriptor; allocates stream record and queues filename"),
    "stream_new_record_queue": (0x74BB28, "E8D3000E008946288B761C", "82BC00 queues filename; request stored at stream +28"),
    "stream_request_filename_copy": (0x859F63, "8B44241468A0000000508D4E0851", "request filename copied into a 160-byte field at +8"),
    "stream_pending_consumer": (0x858600, "803D1073080200578BF9", "selects queued request, opens through 56F3C0"),
    "stream_open_callback": (0x859490, "8B4424048B48148B91B0000000", "routes request state 1 to open completion, state 3 to read completion"),
    "stream_open_failed": (0x8593E7, "C786B00000009AFFFFFF", "request state -102 when file operation result is not 1"),
    "file_search_path_open": (0x56EA6A, "8B8FC00000008B018B400C", "filesystem backend open virtual slot +0C, search paths tried in order"),
    "win32_file_open": (0x56878D, "FF15308C150233C983F8FF", "CreateFileA import call and INVALID_HANDLE_VALUE check; backend linkage pending"),
    "drive_backend_open_slot": (0x1AF8E74, "B0BC5600", "vtable 1AF8E68 slot +0C is 56BCB0"),
    "drive_backend_create": (0x56D8C4, "C700688EAF018BF0", "logical-drive initialization constructs this filesystem backend"),
    "drive_device_backend": (0x56D98D, "89BEC0000000", "device +C0 receives the backend used by file-wrapper virtual open"),
    "drive_unicode_open": (0x56BDB7, "FF156C8C15028BF083FEFF", "CreateFileW call, failure compared with INVALID_HANDLE_VALUE"),
    "stream_existing_load": (0x74BFF0, "81ECB4000000538B9C24C8000000", "existing record loader, four arguments, ret 10"),
    "stream_load_hold_flag": (0x74C4A4, "F6C3107504834F5420", "load flag 10 suppresses setting stream flag 20; not a refcount increment"),
    "stream_release": (0x74B060, "8A44240883EC18240153", "release selects dependency count or explicit hold flag"),
    "stream_release_gates": (0x74B0AA, "F64354200F856902000066837B5800", "unload blocked by hold bit 20 or nonzero 16-bit dependency count"),
    "stream_dependency_increment": (0x74C414, "66FF47588B44242485C0", "dependency loaded by 74B940, counter at stream +58"),
    "stream_dependency_count": (0x74C9C0, "668B4158C3", "returns 16-bit stream dependency count"),
    "stream_lock_handle": (0x74AC80, "8B442404535032DBE8AE96CBFF85C0", "sets flag 80 after handle resolution, bool result, ret 4"),
    "stream_unlock_handle": (0x74ACB0, "8B442404535032DBE87E96CBFF85C0", "clears flag 80 after handle resolution, bool result, ret 4"),
    "stream_bulk_release": (0x74B8D0, "56578BF98B477C85C07422", "skips locked entries in pending and dispatched lists"),
    "simgroup_format_check": (0x84A89B, "817E242818B5CC756D817E2803000100", "requires magic CCB51828 and version 00010003"),
    "parameter_image_dispatch": (0x85D090, "8B5424048B0A32C081F9D9951701", "dispatches loose image magic 91C8773C to 85D020"),
    "parameter_existing_skip": (0x85D059, "E8D21AF7FF83C40485C07508", "7CEB30 lookup; existing image skips 85CF20 construction"),
    "simgroup_reference_resolve": (0x85BC29, "50E8012FF7FF83C404890685C0", "resolves each image ID to header and retains through 7CEA40"),
    "parameter_relocation": (0x85C0C0, "8B410885C0740503C1894108", "relocates serialized table offsets in place"),
    "parameter_type_resolution": (0x85BF90, "51535556578BE9C644241300", "resolves class IDs in place; caller can disable strict failure reporting"),
    "parameter_construction": (0x85CF20, "558BEC83EC185356578B7D08", "descriptor validation, field decode and registration at 7CEBC0"),
}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("exe", type=Path)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    pe = pefile.PE(str(args.exe), fast_load=True)
    preferred_base = pe.OPTIONAL_HEADER.ImageBase
    with args.exe.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    report = {"file": str(args.exe), "sha256": digest,
              "preferred_base": hex(preferred_base), "scope": "Offline file only", "sites": []}
    for name, (va, signature, observation) in SITES.items():
        expected = bytes.fromhex(signature)
        actual = pe.get_data(va - 0x400000, len(expected))
        report["sites"].append({"name": name, "rva": hex(va - 0x400000),
                                "matches": actual == expected, "expected": signature,
                                "actual": actual.hex().upper(), "observation": observation})
    report["all_match"] = all(s["matches"] for s in report["sites"])
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    for s in report["sites"]:
        print(s["name"], "PASS" if s["matches"] else "FAIL")
    raise SystemExit(0 if report["all_match"] else 1)

if __name__ == "__main__":
    main()
