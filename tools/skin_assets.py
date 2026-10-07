"""Build a recoloured copy of a character's model for the pool.

A DS2 character package binds its look in three places, all found offline in
exploder.str and divider.str:

    MetaModel  -> Mesh, VFX, HKX      by 16 byte resource GUID (id,id,id,w)
    Mesh       -> Material            by a 32 bit material key at +0x20C of each
                                      submesh header; the Material holds the same
                                      key at +8 of its payload
    Material   -> textures            by identity dword crc32(group + name) in the
                                      parameter block; the /assets/.../<name>_tdf.xml
                                      path strings next to it are editor text
    tg4h       header names the texture group and the texture; tg4d is the raw
                                      DXT block data with its mip chain

Resource ids are not hashes of names (only sound banks are), so a copy may take
any unused id as long as every reference is rewritten with it. Texture names
are strings, so a renamed texture keeps its length: the last character of the
stem becomes the variant's tag.

The skin is a tint applied straight to the DXT colour endpoints, mip chain
included, the way the DS1 skins were made; DXT1 blocks keep their mode by
swapping endpoints and indices when the tint reorders them.

A MeshVolatile (the skinned vertex data) has no reference anywhere; its identity
is the bitwise NOT of its mesh's, and the loader finds the mesh through that.

RCB, HKX, VFX and sound stay shared with the base enemy: the RCB references no
mesh, and renaming banks is what froze the first DS1 variant.
"""
import colorsys
import random
import re
import struct
import sys
import zlib

from ds2_paths import MODDING_TOOLS
sys.path.insert(0, str(MODDING_TOOLS))
from ds2_assets import assets  # noqa: E402

TEXTURE_PATH = re.compile(rb"/assets/[A-Za-z0-9_/]+_tdf\.xml")


def name_hash(text):
    value = 0
    for char in text.lower():
        value = (value * 0x1003F + ord(char)) & 0xFFFFFFFF
    return value


def guid16(rid, w):
    return struct.pack("<4I", rid, rid, rid, w)


def guid_text(rid, w):
    return "%08X-%08X-%08X-%08X" % (rid, rid, rid, w)


def decode565(c):
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31
    return r / 31.0, g / 63.0, b / 31.0


def encode565(r, g, b):
    r = min(31, max(0, int(round(r * 31)))); g = min(63, max(0, int(round(g * 63)))); b = min(31, max(0, int(round(b * 31))))
    return (r << 11) | (g << 5) | b


def tint565(c, tint):
    """`tint` is either (r, g, b) multipliers or a dict with any of `hue` (degrees
    to add), `sat` and `val` (multipliers); the dict form works in HSV, which
    keeps a texture's shading while it changes its colour."""
    r, g, b = decode565(c)
    if isinstance(tint, dict):
        h, sa, v = colorsys.rgb_to_hsv(r, g, b)
        h = (h + tint.get("hue", 0.0) / 360.0) % 1.0
        sa = min(1.0, sa * tint.get("sat", 1.0))
        v = min(1.0, v * tint.get("val", 1.0))
        r, g, b = colorsys.hsv_to_rgb(h, sa, v)
        return encode565(r, g, b)
    return encode565(min(1.0, r * tint[0]), min(1.0, g * tint[1]), min(1.0, b * tint[2]))


def nudge_apart(n0, n1):
    """Two equal RGB565 endpoints in a four colour block: raise one channel of
    the first by a step (never across a channel border, a packed +1 turned
    black into cyan), or lower one of the second if the first is at the top."""
    r, g, b = (n0 >> 11) & 31, (n0 >> 5) & 63, n0 & 31
    if b < 31: return (r << 11) | (g << 5) | (b + 1), n1
    if g < 63: return (r << 11) | ((g + 1) << 5) | b, n1
    if r < 31: return ((r + 1) << 11) | (g << 5) | b, n1
    r, g, b = (n1 >> 11) & 31, (n1 >> 5) & 63, n1 & 31
    if b: return n0, (r << 11) | (g << 5) | (b - 1)
    if g: return n0, (r << 11) | ((g - 1) << 5) | b
    return n0, ((r - 1) << 11) | (g << 5) | b


def swap_indices(indices, four_colour):
    """Endpoints swapped: index 0<->1, and 2<->3 in four colour mode."""
    out = 0
    for i in range(16):
        v = (indices >> (2 * i)) & 3
        if v == 0: v = 1
        elif v == 1: v = 0
        elif four_colour and v == 2: v = 3
        elif four_colour and v == 3: v = 2
        out |= v << (2 * i)
    return out


def tint_dxt(data, fmt, tint):
    out = bytearray(data)
    block = 8 if fmt == b"DXT1" else 16
    colour_at = 0 if fmt == b"DXT1" else 8
    for start in range(0, len(out) - block + 1, block):
        o = start + colour_at
        c0, c1, idx = struct.unpack_from("<HHI", out, o)
        n0, n1 = tint565(c0, tint), tint565(c1, tint)
        if fmt == b"DXT1":
            four = c0 > c1
            if four and n0 <= n1:
                if n0 < n1:
                    n0, n1 = n1, n0
                    idx = swap_indices(idx, True)
                if n0 == n1:
                    n0, n1 = nudge_apart(n0, n1)
            elif not four and n0 > n1:
                n0, n1 = n1, n0
                idx = swap_indices(idx, False)
        struct.pack_into("<HHI", out, o, n0, n1, idx)
    return bytes(out)


def load_package(stream):
    headers = {offset: body[:36] for offset, tag, body in stream.chunks() if stream.tag(body[:4]) == b"SHDR"}
    items = []
    for meta, chunks in assets(stream):
        items.append({"meta": meta, "header": headers[meta["chunk_offset"]],
                      "payload": stream.asset_payload(chunks, meta["size"])})
    return items


def load_dds(path, fmt, size):
    """Raw block data of a DDS file, checked against the texture it replaces."""
    raw = open(path, "rb").read()
    if raw[:4] != b"DDS " or struct.unpack_from("<I", raw, 4)[0] != 124:
        raise ValueError("%s is not a DDS file" % path)
    fourcc = raw[84:88]
    if fourcc != fmt:
        raise ValueError("%s is %r, the texture is %r" % (path, fourcc, fmt))
    data = raw[128:]
    if len(data) < size:
        raise ValueError("%s holds %d bytes of blocks, the texture needs %d (save with mipmaps)" % (path, len(data), size))
    return data[:size]


def rename_short(body, template, short_name):
    """Overwrite the template's short name string in place with the copy's.

    The detached limbs are assembled by that string (the walking body binds
    the crc at +12, the parts the name): with the template's name left in,
    every ghost and gold variant lost its look on dismemberment (14.09.).
    The copy's name is shorter than "skinned_packDementia", so it fits in the
    slot, NUL terminated, and every offset behind it stays put."""
    crc = struct.unpack_from("<I", body, 12)[0]
    old = [x for x in re.findall(rb"[A-Za-z0-9_]{3,64}", bytes(body)) if zlib.crc32(x.lower()) & 0xFFFFFFFF == crc]
    if len(old) != 1:
        raise ValueError("ghost template %s: short name for crc %08X not found once" % (template["meta"]["name"], crc))
    at = bytes(body).find(old[0] + b"\0")
    new = short_name.encode()
    if at < 0 or len(new) > len(old[0]):
        raise ValueError("ghost template %s: name %r does not fit the slot of %r" % (template["meta"]["name"], short_name, old[0]))
    body[at:at + len(old[0]) + 1] = new + bytes(len(old[0]) + 1 - len(new))


def ghost_material(template, body_key, short_name, colour_identity, normal_identity, keep_maps=False, base_material=None):
    """A material on the dementia pack boy's shader, bound to another model's
    colour and normal maps.

    The ghost look of the end-game pack boys is not a texture but the shader
    of `skinned_packDementia` (screen distortion with a noise map; the
    material holds its parameters). Its parameter block names the colour map
    at one identity slot and the normal map at another, the noise map at
    three more; the renderer binds textures by those identity dwords, the
    path strings are for the editor (first skin test, 12.09.). So the copy
    keeps the template byte for byte and only retargets the two slots, the
    key at +8 and the short-name crc at +12 that the walking character binds
    the material by. The template's name strings stay, different length."""
    body = bytearray(template["payload"])
    if base_material is not None:
        # words 10, 12 and 14 sit next to the shader ids (9/11, 13/15) and
        # differ per model (leaper 0x300/0x403/0x300, pack boy 0x500, the
        # template 0/0xF00/0): the detached limbs kept the base look on every
        # ghost, the walking body did not, so these are taken from the base
        # material as a vertex-format guess (14.09.)
        base = base_material["payload"]
        for at in (40, 48, 56):
            struct.pack_into("<I", body, at, struct.unpack_from("<I", base, at)[0])
        # word 2 is NOT for the limbs: with the base model's value in the
        # template body the game crashed while building the material
        # (deadspace2+0x418F9D, null pool, 14.09. 21:02). It belongs to the
        # template's own layout and stays.
    tex_ids = {}
    for m in TEXTURE_PATH.finditer(bytes(body)):
        stem = m.group().rsplit(b"/", 1)[1][:-len(b"_tdf.xml")].decode()
        group = m.group()[len(b"/assets/"):].rsplit(b"/", 1)[0].replace(b"/", b"_").decode()
        tex_ids[stem] = zlib.crc32((group + stem).encode()) & 0xFFFFFFFF
    colour = [s for s in tex_ids if s.endswith("_c")]
    normal = [s for s in tex_ids if s.endswith("_n")]
    if len(colour) != 1 or len(normal) != 1:
        raise ValueError("ghost template %s: expected one _c and one _n texture, got %s" % (template["meta"]["name"], sorted(tex_ids)))
    if keep_maps:
        # the template's own colour and normal maps stay bound (they travel
        # along like the noise map): the pure shader test, 14.09.
        rename_short(body, template, short_name)
        struct.pack_into("<I", body, 8, body_key)
        struct.pack_into("<I", body, 12, zlib.crc32(short_name.lower().encode()) & 0xFFFFFFFF)
        return bytes(body), list(tex_ids)
    replaced = {colour[0]: 0, normal[0]: 0}
    for stem, new_identity in ((colour[0], colour_identity), (normal[0], normal_identity)):
        for at in range(0, len(body) - 3, 4):
            if struct.unpack_from("<I", body, at)[0] == tex_ids[stem]:
                struct.pack_into("<I", body, at, new_identity); replaced[stem] += 1
    if replaced[colour[0]] != 1 or replaced[normal[0]] != 1:
        raise ValueError("ghost template %s: identity slots %s" % (template["meta"]["name"], replaced))
    rename_short(body, template, short_name)
    struct.pack_into("<I", body, 8, body_key)
    struct.pack_into("<I", body, 12, zlib.crc32(short_name.lower().encode()) & 0xFFFFFFFF)
    return bytes(body), [s for s in tex_ids if s not in replaced]


def build_skin(stream, model_id, variant, tint, tag, taken_ids, tints=None, files=None, character_ids=None, vfx=None, sounds=None, sound_packages=None, custom_sounds=None, ghost=None):
    """Return (new metamodel guid text, old guid text, [asset dicts], report) for the pool.

    `tint` recolours every diffuse (_c) texture; `tints` names other textures
    (or overrides) by stem, `files` replaces a texture's blocks with a DDS.
    `ghost` is (template package items, material name substring): every
    skinned material with a colour and a normal map is rebuilt on that
    template's shader (see ghost_material)."""
    tints = dict(tints or {})
    files = dict(files or {})
    items = load_package(stream)
    by_id = {it["meta"]["resource_id"]: it for it in items if it["meta"]["resource_id"]}
    if model_id not in by_id or by_id[model_id]["meta"]["type"] != "MetaModel":
        raise ValueError("model %08X is not a MetaModel in this package" % model_id)
    rng = random.Random("ds2skin/" + variant)
    used = set(taken_ids) | set(by_id)

    def fresh():
        while True:
            v = rng.getrandbits(32)
            if v > 0x1000000 and v not in used:
                used.add(v)
                return v

    def header_words(it):
        return list(struct.unpack("<9I", it["header"]))

    model = by_id[model_id]
    mw = header_words(model)
    old_model_guid = guid16(model_id, mw[7])
    payload = model["payload"]
    meshes = [it for it in items if it["meta"]["type"] == "Mesh" and
              guid16(it["meta"]["resource_id"], header_words(it)[7]) in payload]
    if not meshes:
        raise ValueError("the MetaModel references no mesh of this package")
    materials = [it for it in items if it["meta"]["type"] == "Material"]
    keys = {struct.unpack_from("<I", it["payload"], 8)[0]: it for it in materials}
    used_keys = {}
    for mesh in meshes:
        for key, mat in keys.items():
            if struct.pack("<I", key) in mesh["payload"]:
                used_keys[key] = mat
    if not used_keys:
        raise ValueError("no material key of this package appears in the referenced meshes")
    textures = {}
    for it in items:
        if it["meta"]["type"] in ("tg4h", "tg4d"):
            textures.setdefault(it["meta"]["name"], {})[it["meta"]["type"]] = it

    out = []
    # textures: every diffuse (_c) texture the used materials name. A texture's
    # identity is crc32(group + name), kept in both SHDRs and at header +0x14.
    renamed = {}
    carried = set()
    for key, mat in used_keys.items():
        for m in TEXTURE_PATH.finditer(mat["payload"]):
            stem = m.group().rsplit(b"/", 1)[1][:-len(b"_tdf.xml")].decode()
            if stem in renamed or stem in carried or stem not in textures:
                continue
            if not (stem.endswith("_c") or stem in tints or stem in files):
                # A material resolves every texture it names when it is loaded,
                # and the pool loads long before the base package: a texture
                # not resident then stays unresolved (-1) and the material draws
                # black. So the normal, occlusion, specular and glow maps travel
                # along unchanged, same identity; retail shares texture
                # identities across packages the same way (specenviroment_e).
                pair = textures[stem]
                if "tg4h" not in pair or "tg4d" not in pair:
                    continue
                for kind in ("tg4h", "tg4d"):
                    it = pair[kind]
                    out.append({"header": it["header"], "name": it["meta"]["name"], "path": it["meta"]["path"],
                                "type": kind, "payload": it["payload"], "source": it["meta"]["name"]})
                carried.add(stem)
                continue
            pair = textures[stem]
            if "tg4h" not in pair or "tg4d" not in pair:
                continue
            # the tag replaces the character before the map suffix, so
            # exploderarm_c and exploderarm_l stay two different names
            new_stem = (stem[:-3] + tag + stem[-2:]) if stem[-2] == "_" else stem[:-1] + tag
            head = bytearray(pair["tg4h"]["payload"])
            fmt = re.search(rb"DXT[135]", bytes(head))
            if not fmt:
                continue
            strings = bytes(head)[fmt.end():].split(b"\0")
            group = strings[2].decode()
            if strings[3] != stem.encode():
                raise ValueError("tg4h of %s names %r" % (stem, strings[3]))
            old_w7 = header_words(pair["tg4h"])[7]
            if zlib.crc32((group + stem).encode()) & 0xFFFFFFFF != old_w7 or struct.unpack_from("<I", head, 0x14)[0] != old_w7:
                raise ValueError("texture identity of %s is not crc32(group + name)" % stem)
            new_w7 = zlib.crc32((group + new_stem).encode()) & 0xFFFFFFFF
            i = bytes(head).rfind(stem.encode() + b"\0")
            head[i:i + len(stem)] = new_stem.encode()
            struct.pack_into("<I", head, 0x14, new_w7)
            if stem in files:
                data = load_dds(files[stem], fmt.group(), len(pair["tg4d"]["payload"]))
            else:
                chosen = tints.get(stem, tint)
                # tint null: the copy keeps the retail pixels (the DDS is edited by hand later)
                data = pair["tg4d"]["payload"] if chosen is None else tint_dxt(pair["tg4d"]["payload"], fmt.group(), chosen if isinstance(chosen, dict) else tuple(chosen))
            for kind, body in (("tg4h", bytes(head)), ("tg4d", data)):
                it = pair[kind]
                hdr = bytearray(it["header"]); struct.pack_into("<I", hdr, 28, new_w7)
                out.append({"header": bytes(hdr), "name": new_stem, "path": it["meta"]["path"].replace(stem, new_stem),
                            "type": kind, "payload": body, "source": it["meta"]["name"]})
            renamed[stem] = (new_stem, old_w7, new_w7)
    # materials: new id, new key at +8, the short name (its crc32 sits at +12)
    # gets the tag as last character, texture paths renamed
    new_keys = {}
    ghost_materials = {}
    ghost_template = None
    if ghost:
        template_items, template_name = ghost[0], ghost[1]
        ghost_keep_maps = ghost[2] if len(ghost) > 2 else False
        found = [it for it in template_items if it["meta"]["type"] == "Material" and template_name in it["meta"]["name"]]
        if len(found) != 1:
            raise ValueError("ghost template material %r not found once (%d)" % (template_name, len(found)))
        ghost_template = found[0]
        template_textures = {}
        for it in template_items:
            if it["meta"]["type"] in ("tg4h", "tg4d"):
                template_textures.setdefault(it["meta"]["name"], {})[it["meta"]["type"]] = it
    for key, mat in used_keys.items():
        rid, w7, nk = fresh(), rng.getrandbits(32), fresh()
        body = bytearray(mat["payload"])
        assert struct.unpack_from("<I", body, 8)[0] == key
        struct.pack_into("<I", body, 8, nk)
        # The short name (its crc32 at +12) gets the tag as last character too:
        # the walking character binds materials by that name, and with the
        # base name it kept drawing the base material.
        name_crc = struct.unpack_from("<I", body, 12)[0]
        short = [x for x in re.findall(rb"[A-Za-z0-9_]{3,64}", bytes(body)) if zlib.crc32(x.lower()) & 0xFFFFFFFF == name_crc]
        if len(short) != 1:
            raise ValueError("material %s: short name not found for crc %08X" % (mat["meta"]["name"], name_crc))
        new_short = short[0][:-1] + tag.encode()
        stems = [m.group().rsplit(b"/", 1)[1][:-len(b"_tdf.xml")].decode() for m in TEXTURE_PATH.finditer(mat["payload"])]
        colour_stems = [s for s in stems if s.endswith("_c") and s in renamed]
        normal_stems = [s for s in stems if s.endswith("_n") and s in textures]
        if ghost_template is not None and len(colour_stems) == 1 and len(normal_stems) == 1:
            ghost_body, extra = ghost_material(ghost_template, nk, new_short.decode(),
                                               renamed[colour_stems[0]][2], header_words(textures[normal_stems[0]]["tg4h"])[7],
                                               keep_maps=bool(ghost_keep_maps), base_material=mat)
            # the template's own helper maps (the noise map) travel along
            # unchanged, like any map a material needs when the pool loads
            for stem in extra:
                if stem in carried or stem not in template_textures:
                    continue
                for kind in ("tg4h", "tg4d"):
                    it = template_textures[stem][kind]
                    out.append({"header": it["header"], "name": it["meta"]["name"], "path": it["meta"]["path"],
                                "type": kind, "payload": it["payload"], "source": it["meta"]["name"]})
                carried.add(stem)
            hdr = bytearray(mat["header"]); struct.pack_into("<4I", hdr, 16, rid, rid, rid, w7)
            out.append({"header": bytes(hdr), "name": mat["meta"]["name"] + "_" + variant, "path": mat["meta"]["path"].replace(".mtlb", "_" + variant + ".mtlb"),
                        "type": "Material", "payload": ghost_body, "source": ghost_template["meta"]["name"]})
            new_keys[key] = nk
            if ghost_keep_maps:
                t_colour = [x for x in extra if x.endswith("_c")][0]; t_normal = [x for x in extra if x.endswith("_n")][0]
                ghost_materials[mat["meta"]["name"] + "_" + variant] = {"template": ghost_template["meta"]["name"], "colour": t_colour,
                                                                        "normal": t_normal, "extra": [x for x in extra if x not in (t_colour, t_normal)]}
            else:
                ghost_materials[mat["meta"]["name"] + "_" + variant] = {"template": ghost_template["meta"]["name"], "colour": renamed[colour_stems[0]][0],
                                                                        "normal": normal_stems[0], "extra": extra}
            continue
        body = bytearray(bytes(body).replace(short[0] + bytes([0]), new_short + bytes([0])))
        struct.pack_into("<I", body, 12, zlib.crc32(new_short.lower()) & 0xFFFFFFFF)
        for old_stem, (new_stem, old_w7, new_w7) in renamed.items():
            body = bytearray(bytes(body).replace(old_stem.encode() + b"_tdf.xml", new_stem.encode() + b"_tdf.xml"))
            # The path string is for the editor; what the renderer binds is the
            # texture identity crc32(group + name) stored as a dword in the
            # material's parameter block (first skin test: the copy still showed
            # the base texture with only the string renamed).
            bound = 0
            for at in range(0, len(body) - 3, 4):
                if struct.unpack_from("<I", body, at)[0] == old_w7:
                    struct.pack_into("<I", body, at, new_w7); bound += 1
            if old_stem.encode() + b"_tdf.xml" in mat["payload"] and not bound:
                raise ValueError("material %s names %s but holds no identity dword for it" % (mat["meta"]["name"], old_stem))
        hdr = bytearray(mat["header"]); struct.pack_into("<4I", hdr, 16, rid, rid, rid, w7)
        out.append({"header": bytes(hdr), "name": mat["meta"]["name"] + "_" + variant, "path": mat["meta"]["path"].replace(".mtlb", "_" + variant + ".mtlb"),
                    "type": "Material", "payload": bytes(body), "source": mat["meta"]["name"]})
        new_keys[key] = nk
    # meshes: new id, keys rewritten (aligned dwords only), the MeshVolatile
    # companion copied right behind its mesh, same name, same new path, and
    # the inverted identity the loader pairs it by
    volatiles = {}
    for it in items:
        if it["meta"]["type"] == "MeshVolatile":
            volatiles[it["meta"]["path"]] = it
    mesh_guids = {}
    for mesh in meshes:
        w = header_words(mesh)
        rid, w7 = fresh(), rng.getrandbits(32)
        body = bytearray(mesh["payload"])
        hits = 0
        for at in range(0, len(body) - 3, 4):
            cur = struct.unpack_from("<I", body, at)[0]
            if cur in new_keys:
                struct.pack_into("<I", body, at, new_keys[cur]); hits += 1
        if not hits:
            raise ValueError("mesh %s holds none of the material keys" % mesh["meta"]["name"])
        hdr = bytearray(mesh["header"]); struct.pack_into("<4I", hdr, 16, rid, rid, rid, w7)
        new_path = mesh["meta"]["path"].replace(".geo", "_" + variant + ".geo")
        # the mesh name, in the SHDR and inside the payload (name and nameShape),
        # gets the tag as its last character so a lookup by name cannot land on
        # the base mesh
        stem = mesh["meta"]["name"].rsplit(".", 1)[0].encode()
        new_stem = stem[:-1] + tag.encode()
        if stem not in body:
            raise ValueError("mesh %s does not carry its name" % mesh["meta"]["name"])
        body = bytearray(bytes(body).replace(stem, new_stem))
        out.append({"header": bytes(hdr), "name": mesh["meta"]["name"].replace(stem.decode(), new_stem.decode()), "path": new_path,
                    "type": "Mesh", "payload": bytes(body), "source": mesh["meta"]["name"]})
        mesh_guids[guid16(mesh["meta"]["resource_id"], w[7])] = guid16(rid, w7)
        vol = volatiles.get(mesh["meta"]["path"])
        if vol is not None:
            # A MeshVolatile's identity is the bitwise NOT of its mesh's, and the
            # loader finds the mesh by exactly that (deadspace2.exe 0x007C4DC0;
            # a volatile whose inverse names nothing writes to null, dump 15740).
            vw = header_words(vol)
            if (~vw[4] & 0xFFFFFFFF) != w[4] or (~vw[7] & 0xFFFFFFFF) != w[7]:
                raise ValueError("volatile of %s is not the inverse of its mesh" % mesh["meta"]["name"])
            vid, vw7 = ~rid & 0xFFFFFFFF, ~w7 & 0xFFFFFFFF
            used.add(vid)
            hdr = bytearray(vol["header"]); struct.pack_into("<4I", hdr, 16, vid, vid, vid, vw7)
            out.append({"header": bytes(hdr), "name": vol["meta"]["name"].replace(stem.decode(), new_stem.decode()), "path": new_path,
                        "type": "MeshVolatile", "payload": vol["payload"], "source": vol["meta"]["name"]})
    # what else the MetaModel names has to be resident when the pool loads,
    # long before the base package is: the physics (HKX, copied under a new
    # identity, the reference rewritten, name and path kept) and the effects (VFX, copied as
    # they are; retail shares the same VFX identity across packages already)
    extras = 0
    vfx = dict(vfx or {})
    recoloured = {}
    for it in items:
        kind = it["meta"]["type"]
        if kind not in ("HKX", "VFX") or not it["meta"]["resource_id"]:
            continue
        w = header_words(it)
        old = guid16(w[4], w[7])
        if old not in payload:
            continue
        hdr = bytearray(it["header"])
        name, path = it["meta"]["name"], it["meta"]["path"]
        body_vfx = it["payload"]
        stem = name.rsplit(".", 1)[0]
        if kind == "HKX" or (kind == "VFX" and stem in vfx):
            hid, hw7 = fresh(), rng.getrandbits(32)
            struct.pack_into("<4I", hdr, 16, hid, hid, hid, hw7)
            mesh_guids[old] = guid16(hid, hw7)
        if kind == "VFX" and stem in vfx:
            # An effect's light colour: three floats 80 bytes into its ".lit"
            # section (tag "til." in the file); npc_xpldr_sacklight holds
            # (1, 0.5, 0), flamer_pilot_light (0.3, 0.4, 1), nest light
            # (1, 0.6, 0). The copy gets its own identity, the MetaModel
            # reference is rewritten; the name stays, its hash is a state name.
            at = body_vfx.find(b"til.")
            if at < 0:
                raise ValueError("effect %s has no light section" % name)
            colour = tuple(float(c) for c in vfx[stem]["light"])
            body_vfx = bytearray(body_vfx)
            before = struct.unpack_from("<3f", body_vfx, at + 80)
            struct.pack_into("<3f", body_vfx, at + 80, *colour)
            body_vfx = bytes(body_vfx)
            recoloured[stem] = {"light_before": [round(c, 3) for c in before], "light": list(colour)}
        out.append({"header": bytes(hdr), "name": name, "path": path, "type": kind,
                    "payload": body_vfx, "source": it["meta"]["name"]})
        extras += 1
    # the metamodel: new id, mesh and physics references rewritten, self identity rewritten
    rid, w7 = fresh(), rng.getrandbits(32)
    body = bytes(payload)
    for old, new in mesh_guids.items():
        body = body.replace(old, new)
    body = body.replace(old_model_guid, guid16(rid, w7))
    # The model names itself a second time at +0x24/+0x28 by bare id (the
    # block (x, id, id, y) behind the GUID): that is what the walking figure's
    # own MetaModelInstance is built from (set_model/skin_build logs, 12.09.:
    # the figure held the base model while every reference said the copy).
    body = bytearray(body)
    self_refs = 0
    for at in range(0, len(body) - 3, 4):
        if struct.unpack_from("<I", body, at)[0] == model_id:
            struct.pack_into("<I", body, at, rid); self_refs += 1
    if self_refs < 2:
        raise ValueError("MetaModel carries %d bare self references, expected the pair at +0x24/+0x28" % self_refs)
    body = bytes(body)
    # The reference table (32 byte rows at [payload+0x34], count at +0x58)
    # names every mesh a second time by a part name hash (word 1, e.g.
    # sdbm("mesh_body")); the walking character is assembled through those
    # names, and with the base package's hashes it was assembled from the base
    # meshes (skin_build log, 12.09.). Fold the variant into them.
    body = bytearray(body)
    table, count = struct.unpack_from("<I", body, 0x34)[0], struct.unpack_from("<H", body, 0x58)[0]
    salt = name_hash(variant)
    renamed_parts = 0
    for i in range(count):
        at = table + i * 32
        if at + 32 > len(body):
            raise ValueError("MetaModel reference table runs past the payload")
        kind = struct.unpack_from("<I", body, at + 8)[0]
        if kind == 0x01961BED:
            part = struct.unpack_from("<I", body, at + 4)[0]
            struct.pack_into("<I", body, at + 4, (part ^ salt) & 0xFFFFFFFF); renamed_parts += 1
    if renamed_parts != len(meshes):
        raise ValueError("MetaModel names %d mesh parts, the skin copied %d meshes" % (renamed_parts, len(meshes)))
    # ghost effects: the dementia pack boy's persistent effects (buffer
    # distortion, shadow, tentacles) are listed in its MetaModel like any
    # effect; the shader alone drew a plain dark figure (14.09.). Their rows
    # replace rows of multiplayer-only effects (mp_*) in the copy, so the
    # table keeps its size and every offset behind it, and the effects travel
    # along with the template's textures and effect materials.
    ghost_effects = []
    if ghost and len(ghost) > 3 and ghost[3]:
        template_items = ghost[0]
        t_by_id = {}
        for it in template_items:
            if it["meta"]["resource_id"]: t_by_id[header_words(it)[4]] = it
        t_model = [it for it in template_items if it["meta"]["type"] == "MetaModel"]
        if len(t_model) != 1:
            raise ValueError("ghost template package holds %d MetaModels" % len(t_model))
        t_body = t_model[0]["payload"]
        t_table, t_count = struct.unpack_from("<I", t_body, 0x34)[0], struct.unpack_from("<H", t_body, 0x58)[0]
        t_rows = {}
        for i in range(t_count):
            row = t_body[t_table + i * 32: t_table + i * 32 + 32]
            ref = t_by_id.get(struct.unpack_from("<I", row, 16)[0])
            if ref is not None and ref["meta"]["type"] == "VFX": t_rows[ref["meta"]["name"]] = row
        # rows to give up: the multiplayer-only effects (mp_*) when the model
        # lists any, otherwise the effects the spec names ("replace")
        replace_names = list(ghost[4]) if len(ghost) > 4 and ghost[4] else []
        victims = []
        for i in range(count):
            at = table + i * 32
            ref = by_id.get(struct.unpack_from("<I", body, at + 16)[0])
            if ref is None or ref["meta"]["type"] != "VFX":
                continue
            if (replace_names and ref["meta"]["name"] in replace_names) or (not replace_names and ref["meta"]["name"].startswith("mp_")):
                victims.append(at)
        wanted_vfx = list(ghost[3])
        if len(victims) < len(wanted_vfx):
            raise ValueError("ghost effects: %d wanted, the model lists only %d multiplayer effect rows to replace" % (len(wanted_vfx), len(victims)))
        for vfx_name, at in zip(wanted_vfx, victims):
            if vfx_name not in t_rows:
                raise ValueError("ghost effect %s is not in the template MetaModel" % vfx_name)
            body[at:at + 32] = t_rows[vfx_name]
            ref = t_by_id[struct.unpack_from("<I", t_rows[vfx_name], 16)[0]]
            out.append({"header": ref["header"], "name": ref["meta"]["name"], "path": ref["meta"]["path"], "type": "VFX",
                        "payload": ref["payload"], "source": "ghost:" + ref["meta"]["name"]})
            extras += 1
            ghost_effects.append(vfx_name)
        already = set(carried) | set(renamed)
        for it in template_items:
            kind = it["meta"]["type"]
            if kind in ("tg4h", "tg4d") and it["meta"]["name"] not in already:
                out.append({"header": it["header"], "name": it["meta"]["name"], "path": it["meta"]["path"], "type": kind,
                            "payload": it["payload"], "source": "ghost:" + it["meta"]["name"]})
                carried.add(it["meta"]["name"])
            elif kind == "Material" and it["meta"]["name"].startswith("vfx_mat_"):
                out.append({"header": it["header"], "name": it["meta"]["name"], "path": it["meta"]["path"], "type": kind,
                            "payload": it["payload"], "source": "ghost:" + it["meta"]["name"]})
                extras += 1
    # the distortion itself: Nicole's DynamicDistortionComponent names a
    # buffer-distortion VFX that lives in the finale's level archive; the
    # effect travels along and the record gets the component (build_variants)
    ghost_distortion = None
    if ghost and len(ghost) > 5 and ghost[5]:
        spec = ghost[5]
        archive_items = ghost[5]["items"]
        vfx_items = [it for it in archive_items if it["meta"]["type"] == "VFX" and it["meta"]["name"] == spec["vfx"]]
        if len(vfx_items) != 1:
            raise ValueError("distortion effect %s not found once in %s" % (spec["vfx"], spec["archive"]))
        vit = vfx_items[0]; vw = header_words(vit)
        out.append({"header": vit["header"], "name": vit["meta"]["name"], "path": vit["meta"]["path"], "type": "VFX",
                    "payload": vit["payload"], "source": "distortion:" + vit["meta"]["name"]})
        extras += 1
        ghost_distortion = {"vfx": vit["meta"]["name"], "vfx_guid": guid_text(vw[4], vw[7]), "bone": spec["bone"], "bone_hash": "%08X" % name_hash(spec["bone"])}
    body = bytes(body)
    hdr = bytearray(model["header"]); struct.pack_into("<4I", hdr, 16, rid, rid, rid, w7)
    out.append({"header": bytes(hdr), "name": model["meta"]["name"].replace(".meta.xml", "_" + variant + ".meta.xml"),
                "path": model["meta"]["path"].replace(".mm", "_" + variant + ".mm"), "type": "MetaModel", "payload": body,
                "source": model["meta"]["name"]})
    report = {"ghost": ghost_materials, "ghost_effects": ghost_effects, "ghost_distortion": ghost_distortion, "textures": {k: v[0] for k, v in renamed.items()}, "carried": sorted(carried), "materials": len(new_keys), "meshes": len(meshes), "volatiles": len(out) - 2 * len(renamed) - 2 * len(carried) - len(new_keys) - len(meshes) - extras - 1, "extras": extras,
              "model": guid_text(rid, w7), "source_model": guid_text(model_id, mw[7]), "self_refs": self_refs}
    # The character itself: the walking figure is assembled under the
    # character name the RCB carries (as a string and as its name hash, which
    # the animation bank repeats at +12), and a copy that shares the name
    # was assembled from the base meshes whatever its model said (skin_build
    # log, 12.09.). Retail variants rename the RCB too. The name keeps its
    # length, the tag becomes its last character; the bank travels along with
    # the new hash so the animations still find their character.
    character = {}
    if character_ids:
        rcb_id, bnk_id = character_ids
        rcb = by_id.get(rcb_id)
        if rcb is None or rcb["meta"]["type"] != "RCB":
            raise ValueError("character RCB %08X not in the package" % rcb_id)
        body = bytearray(rcb["payload"])
        old_hash = struct.unpack_from("<I", body, 8)[0]
        if bnk_id is None:
            # no CMD_SetBNK on the record (slasher family): the character's own
            # bank is the one carrying its name hash at +12; the shared
            # "zombie" banks stay as they are
            own = [it for it in items if it["meta"]["type"] == "BNK" and len(it["payload"]) > 16 and struct.unpack_from("<I", it["payload"], 12)[0] == old_hash]
            # the bank with the audio table (if any) goes first: it is the one the sound swap edits
            own.sort(key=lambda it: 0 if b"___audio: play" in it["payload"] else 1)
            # a character whose banks are a shared set under another name
            # (slasherhospital: own name at +8, bank set "zombie" at +0x6C)
            # gets the RCB copy alone; the shared banks stay bound as they are
            bnk = own[0] if own else None
            if not own and struct.unpack_from("<I", body, 0x6C)[0] == old_hash:
                raise ValueError("no bank carries character hash %08X" % old_hash)
        else:
            bnk = by_id.get(bnk_id)
            own = [bnk]
        if bnk is not None and bnk["meta"]["type"] != "BNK":
            raise ValueError("character BNK not in the package")
        names = [x for x in re.findall(rb"[A-Za-z0-9_]{3,64}", bytes(body)) if name_hash(x.decode()) == old_hash]
        if len(names) == 1:
            old_name = names[0]
        else:
            # some characters (superslasher, slasherhospital) keep no name
            # string in the RCB; the hash is that of the file stem, which is
            # all the renaming needs (the string replace below is a no-op then)
            stem = rcb["meta"]["name"].rsplit(".", 1)[0].encode("latin1")
            if name_hash(stem.decode("latin1")) != old_hash:
                raise ValueError("RCB %s: character name for hash %08X not found (%d strings, stem does not hash to it)" % (rcb["meta"]["name"], old_hash, len(names)))
            old_name = stem
        new_name = old_name[:-1] + tag.encode()
        new_hash = name_hash(new_name.decode())
        body = bytearray(bytes(body).replace(old_name + b"\0", new_name + b"\0"))
        patched = 0
        for at in range(0, len(body) - 3, 4):
            if struct.unpack_from("<I", body, at)[0] == old_hash:
                struct.pack_into("<I", body, at, new_hash); patched += 1
        if patched < 1:
            raise ValueError("RCB carries no character hash dword")
        bank = bytearray(bnk["payload"]) if bnk else bytearray()
        bank_patched = 0
        for at in range(0, len(bank) - 3, 4):
            if struct.unpack_from("<I", bank, at)[0] == old_hash:
                struct.pack_into("<I", bank, at, new_hash); bank_patched += 1
        if bnk and bank_patched < 1:
            raise ValueError("BNK carries no character hash dword")
        # Sound swapping, the DS1 way: the bank's animation events name their
        # sounds as strings ("exploder_attack.emx") in a table behind
        # "___audio: play", referenced by absolute byte offset in typed
        # argument pairs (2, offset). A mapped name is appended to the bank
        # and every such pair is pointed at it; the sound itself has to be in
        # a bank that is resident (another forced character package).
        # Own sounds: private copies of the base banks with the WAVs encoded
        # as XAS1 and the events the animation bank names renamed with a
        # prefix; the swap below then points those events at the new names.
        # Spec: "custom_sounds": {"folder": ..., "prefix": "sup_",
        #        "banks": {"exploder.sbk": "sup_exploder.sbk", ...}}
        sounds = dict(sounds or {})
        private_banks = []
        if custom_sounds:
            import custom_sounds as cs
            # same length names by default ("exploder" -> "explode1"), so the
            # animation bank's block does not grow
            prefix_text = custom_sounds.get("rename", {"exploder": "explode1"})
            # Every string of the bank's table that a rename touches is an
            # event: the ".emx" names the engine plays directly, and the plain
            # names handed to the Lua handler (exploder_muttering_short), which
            # plays them as name + ".emx" (13.09.: the mutterings are the voice
            # everyone hears, and they never went through the .emx strings).
            table = bank.find(b"___audio: play")
            table_strings = []
            if table >= 0:
                main_end = struct.unpack_from("<I", bnk["payload"], 0)[0]
                pos = table
                while pos < main_end:
                    end = bnk["payload"].find(bytes([0]), pos)
                    text = bnk["payload"][pos:end].decode("latin1").lstrip("_")
                    if not text or not all(32 <= ord(c) < 127 for c in text):
                        break
                    table_strings.append(text)
                    pos = end + 1
            bank_events = sorted(set((s if s.endswith(".emx") else s + ".emx") for s in table_strings
                                     if any(k in s for k in prefix_text)))
            for s in table_strings:
                if not s.endswith(".emx") and any(k in s for k in prefix_text):
                    sounds.setdefault(s, cs.rename_event(s, prefix_text))
            for base_name, new_name_sbk in custom_sounds["banks"].items():
                source = next((it for it in items if it["meta"]["type"] == "SBK" and it["meta"]["name"] == base_name), None)
                if source is None:
                    raise ValueError("custom_sounds: bank %s is not in the package" % base_name)
                new_stem = new_name_sbk[:-4]
                new_payload, renamed_events, replaced_count = cs.build_private_bank(
                    source["payload"], new_stem, custom_sounds["folder"], base_name[:-4], bank_events, prefix_text)
                new_path = source["meta"]["path"].replace(base_name, new_name_sbk)
                if new_path == source["meta"]["path"]:
                    raise ValueError("custom_sounds: could not rename the path of " + base_name)
                identity = cs.name_hash(new_path, 64)
                hdr = bytearray(source["header"])
                struct.pack_into("<4I", hdr, 16, identity & 0xFFFFFFFF, identity & 0xFFFFFFFF, identity & 0xFFFFFFFF, identity >> 32)
                private = {"header": bytes(hdr), "name": new_name_sbk, "path": new_path, "type": "SBK", "payload": new_payload,
                           "source": "custom:" + base_name, "meta": {"name": new_name_sbk, "type": "SBK", "path": new_path}}
                private_banks.append((private, renamed_events, replaced_count))
                for old_event, new_event in renamed_events.items():
                    if old_event in table_strings:   # the plain Lua names are mapped above
                        sounds.setdefault(old_event, new_event)
        swapped = {}
        if sounds:
            table = bank.find(b"___audio: play")
            if table < 0:
                raise ValueError("BNK %s has no audio event table" % bnk["meta"]["name"])
            offsets = {}
            pos = table
            while True:
                end = bank.find(b"\0", pos)
                text = bank[pos:end].decode("latin1")
                if not text or not all(32 <= ord(c) < 127 for c in text):
                    break
                offsets[text] = pos
                pos = end + 1
            # The bank is a main block (its size is the header's first word)
            # followed by a relocation table: offsets of every pointer field
            # in the main block, the string references among them. Anything
            # behind the main block is not part of the loaded data, so the new
            # names go in front of the table and the size grows with them
            # (appended behind the table they played the wrong sounds, 13.09.).
            main_size = struct.unpack_from("<I", bank, 0)[0]
            if not (table < main_size <= len(bank)):
                raise ValueError("BNK %s: main block size %d does not cover the event table" % (bnk["meta"]["name"], main_size))
            main, relocations = bytearray(bank[:main_size]), bytes(bank[main_size:])
            for old_event, new_event in sounds.items():
                if old_event not in offsets:
                    raise ValueError("BNK %s has no event %s (it has %s)" % (bnk["meta"]["name"], old_event, ", ".join(sorted(offsets))))
                count = 0
                if len(new_event) <= len(old_event):
                    # Same length or shorter: overwrite the name in place, the
                    # block keeps its size and every pointer stays as it is
                    # (a grown block played mixed sounds twice, 13.09.).
                    at = offsets[old_event]
                    main[at:at + len(old_event) + 1] = new_event.encode("latin1") + bytes(len(old_event) + 1 - len(new_event))
                    for at2 in range(4, main_size - 3, 4):
                        if struct.unpack_from("<I", main, at2)[0] == offsets[old_event] and struct.unpack_from("<I", main, at2 - 4)[0] == 2:
                            count += 1
                    placement = "in_place"
                else:
                    new_offset = len(main)
                    main.extend(new_event.encode("latin1") + bytes([0]))
                    for at in range(4, main_size - 3, 4):
                        if struct.unpack_from("<I", main, at)[0] == offsets[old_event] and struct.unpack_from("<I", main, at - 4)[0] == 2:
                            struct.pack_into("<I", main, at, new_offset); count += 1
                    placement = "appended"
                if not count and placement == "appended":
                    raise ValueError("event %s is never referenced" % old_event)
                swapped[old_event] = {"to": new_event, "references": count, "placement": placement}
            main.extend(bytes((-len(main)) % 16))
            struct.pack_into("<I", main, 0, len(main))
            bank = bytearray(bytes(main) + relocations)
        # The sounds themselves live in SBK banks, keyed in the bank's header
        # table by the name hash of "<event>.emx" (exploder_attack.emx sits in
        # exploder_merge.sbk, vox_hunter_* in hunter_merge.sbk). A lookup only
        # finds a bank that is resident, and the model stream carried none:
        # without a retail exploder in the level even the exploder's own
        # events missed, and the figure played whatever the miss returned
        # ("mixed" sounds, 13.09.). Retail shares one SBK across many packages
        # unchanged (blade_impacts.sbk sits in 29), so the base package's
        # banks and every donor bank a swapped event needs travel along as
        # they are, same identity.
        # (the string table is the tail of the main block, padded with
        # underscores; the swapped names sit right behind that padding)
        # a plain Lua handler name is played as name + ".emx"
        events = set((v if v.endswith(".emx") else v + ".emx") for v in (sounds or {}).values())
        original = bnk["payload"] if bnk else b""
        table = original.find(b"___audio: play")
        for m in re.finditer(rb"[a-z0-9_]+\.emx", original[table:struct.unpack_from("<I", original, 0)[0]] if table >= 0 else b""):
            events.add(m.group().decode().lstrip("_"))

        def bank_holds(sbk, event):
            head_end = struct.unpack_from("<I", sbk["payload"], 8)[0]
            return sbk["payload"][:head_end].find(struct.pack("<I", name_hash(event))) >= 0

        candidates = [("package", it) for it in items if it["meta"]["type"] == "SBK"]
        for label, donor_items in (sound_packages or []):
            candidates.extend((label, it) for it in donor_items if it["meta"]["type"] == "SBK")
        chosen = {it["meta"]["name"]: ("package", it, []) for it in items if it["meta"]["type"] == "SBK"}
        for private, renamed_events, replaced_count in private_banks:
            candidates.append(("custom", private))
            chosen[private["name"]] = ("custom", private, [])
        unresolved = []
        for event in sorted(events):
            holders = [(label, it) for label, it in candidates if bank_holds(it, event)]
            if not holders:
                unresolved.append(event)
                continue
            already = [h for h in holders if h[1]["meta"]["name"] in chosen]
            label, it = already[0] if already else min(holders, key=lambda h: len(h[1]["payload"]))
            chosen.setdefault(it["meta"]["name"], (label, it, []))[2].append(event)
        if unresolved:
            raise ValueError("no sound bank of the package or of sound_packages holds: " + ", ".join(unresolved))
        sound_banks = {}
        for bank_name, (label, it, held) in sorted(chosen.items()):
            out.append({"header": it["header"], "name": it["meta"]["name"], "path": it["meta"]["path"], "type": "SBK",
                        "payload": it["payload"], "source": label + ":" + it["meta"]["name"]})
            sound_banks[bank_name] = {"source": label, "bytes": len(it["payload"]), "events": held}
            for private, renamed_events, replaced_count in private_banks:
                if private["name"] == bank_name:
                    sound_banks[bank_name]["replaced_sounds"] = replaced_count
                    sound_banks[bank_name]["renamed_events"] = renamed_events
        # every further bank of the character ("zombie" has seven) travels along
        # with the new hash, the same renaming, no sound edits
        copies = [(rcb, bytes(body), "RCB")] + ([(bnk, bytes(bank), "BNK")] if bnk else [])
        for other in own[1:]:
            other_bank = bytearray(other["payload"])
            for at in range(0, len(other_bank) - 3, 4):
                if struct.unpack_from("<I", other_bank, at)[0] == old_hash:
                    struct.pack_into("<I", other_bank, at, new_hash)
            copies.append((other, bytes(other_bank), "BNK"))
        # A bank carries its own identity at +8 (unique per retail bank, no
        # name hash that can be reproduced). A copy that keeps it registers its
        # animation entries a second time under the same keys in the engine's
        # shared table, and the erase loop that unregisters an entry follows
        # duplicates through a lookup that always returns the first one: with
        # three slasher copies resident the debug-menu checkpoint switch spun
        # forever in that loop (dump 13.09. 23:12, deadspace2+0xFB8E50). Every
        # copy gets a fresh identity; nothing else names a bank by it (the RCB
        # and the banks themselves were checked for the dword).
        bank_ids = {}
        for it, payload, kind in copies:
            w = header_words(it)
            nid, nw7 = fresh(), rng.getrandbits(32)
            hdr = bytearray(it["header"]); struct.pack_into("<4I", hdr, 16, nid, nid, nid, nw7)
            copy_name = it["meta"]["name"].replace(old_name.decode(), new_name.decode(), 1)
            if kind == "BNK":
                bank = bytearray(payload)
                old_bank_id = struct.unpack_from("<I", bank, 8)[0]
                new_bank_id = name_hash(copy_name.rsplit(".", 1)[0] + "_" + variant)
                struct.pack_into("<I", bank, 8, new_bank_id)
                payload = bytes(bank)
                bank_ids[copy_name] = {"from": "%08X" % old_bank_id, "to": "%08X" % new_bank_id}
            out.append({"header": bytes(hdr), "name": copy_name,
                        "path": it["meta"]["path"].replace(old_name.decode(), new_name.decode()), "type": kind,
                        "payload": payload, "source": it["meta"]["name"]})
            character[guid_text(w[4], w[7])] = guid_text(nid, nw7)
        report["character"] = {"name": new_name.decode(), "source": old_name.decode(), "hash": "%08X" % new_hash,
                               "rcb_dwords": patched, "bnk_dwords": bank_patched, "sounds": swapped, "sound_banks": sound_banks,
                               "bank_ids": bank_ids}
    report["character_map"] = character
    missing_vfx = sorted(set(vfx) - set(recoloured))
    if missing_vfx:
        raise ValueError("effects not referenced by the model: " + ", ".join(missing_vfx))
    report["vfx"] = recoloured
    return guid_text(rid, w7), guid_text(model_id, mw[7]), out, report
