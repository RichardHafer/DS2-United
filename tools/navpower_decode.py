"""Decode Dead Space 2 PC NavPower nav collections (asset type NAV).

Format notes: analysis/NAVPOWER_FORMAT_2026-10-20.md. Everything here is
read-only; the archive, streams and dumps are never written.

Sources, combinable:
  --map mp_bombrun [--dat DS2DAT9.DAT]  NAV assets of one level folder, read straight
                                        from the archives (stream names from the
                                        catalog; without --dat every DS2DATn.DAT)
  FILE.str                              every NAV asset in an extracted stream set
  FILE.navcollection.xml.win            one raw payload

Actions, combinable in one run:
  --summary            one block per collection, plus validation
  --space              how the graphs join into one NavSpace (portal joins, islands)
  --obj DIR            one OBJ per collection and a combined OBJ for Blender
  --point X Y Z        on-mesh test and nearest mesh point (repeatable)
  --reach X Y Z        mesh connectivity from here to every --point
  --dump FILE.dmp      compare nav graphs resident in a PC minidump with the files

Coordinates are game world coordinates, Y up. The space emulates load time:
level space 0, layer 0, resources flagged dynamic (lifts) left out unless
--deferred.

  python -I tools/navpower_decode.py --map mp_bombrun --dat DS2DAT9.DAT --summary
  python -I tools/navpower_decode.py --map ch09_mng --point -140.2 -7.8 -30.4 --reach -138.1 -7.8 -30.3
"""
import argparse
import json
import math
import struct
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
for extra in (ROOT / "dist" / "NecroPlayable_Kit" / "modding_tools", ROOT.parent / "trainer"):
    if extra.is_dir():
        sys.path.append(str(extra))

from ds2_paths import GAME_DIR  # noqa: E402

CATALOG = ROOT / "analysis" / "catalog" / "assets.jsonl"

GRAPH_HEADER_SIZE = 0x13C
AREA_SIZE = 0x34
EDGE_SIZE = 0x18
SECTION_NAVGRAPHS = 0x10000
PAD_SENTINEL = b"hv"
# Runtime rules, verified in the PC image (see the format notes).
PORTAL_DIST_SCALE = 2.0
PORTAL_DIST_VOX = 5.0
OVERLAP_MIN = 0.01
EDGE_MIN = 0.0001

EDGE_TYPES = {0: "normal", 1: "portal", 2: "type2", 3: "type3"}


# ---------------------------------------------------------------- containers

def sdbm_path(path):
    h = 0
    for c in path.lower().replace("/", "\\").encode("utf-8"):
        h = (c + (h << 6) + (h << 16) - h) & 0xFFFFFFFF
    return h


def read_dat_entries(dat_paths, names):
    """Return {name: (dat, bytes)} for archive entries, matched by sdbm path hash.

    The first archive that holds a name wins; DS2DAT7/8 carry duplicates.
    """
    out = {}
    for dat_path in dat_paths:
        with open(dat_path, "rb") as f:
            head = f.read(16)
            if head[:4] != b"BIGH":
                raise ValueError("%s is not a BIGH archive" % dat_path)
            count = struct.unpack(">I", head[8:12])[0]
            table = f.read(count * 12)
            entries = {}
            for i in range(count):
                off, size, h = struct.unpack_from(">III", table, i * 12)
                entries.setdefault(h, (off, size))
            for name in names:
                hit = entries.get(sdbm_path(name))
                if hit is None or name in out:
                    continue
                f.seek(hit[0])
                out[name] = (Path(dat_path).name, f.read(hit[1]))
    for name in names:
        if name not in out:
            print("warning: %s is in no archive" % name)
    return out


def default_dats():
    return sorted(p for p in GAME_DIR.glob("DS2DAT*.DAT") if p.stem[6:].isdigit())


def stream_nav_assets(blob, label):
    """Yield (name, payload) for every NAV asset of a PC .str stream set."""
    import ds2str
    from ds2_assets import assets
    s = ds2str.StreamSet.__new__(ds2str.StreamSet)
    s.path, s.data = label, blob
    magic = blob[:4]
    if magic == b"3slo":
        s.headerless, s.hdrlen = False, struct.unpack_from("<I", blob, 4)[0]
    elif magic == b"COHS":
        s.headerless, s.hdrlen = True, 0
    else:
        raise ValueError("%s: not a PC stream set" % label)
    s.pc, s.end = True, "<"
    for meta, chunks in assets(s):
        if meta["type"] == "NAV":
            yield meta["name"], s.asset_payload(chunks, meta["size"])


def catalog_streams(map_name, catalog):
    """Stream names inside the DAT that carry NAV assets of one map."""
    key = "\\%s\\" % map_name.lower()
    tail = "\\%s.str" % map_name.lower()
    names = []
    with open(catalog, encoding="utf-8") as f:
        for line in f:
            if '"NAV"' not in line:
                continue
            row = json.loads(line)
            arch = row["archive"].lower()
            if key not in arch and not arch.endswith(tail):
                continue
            rel = row["archive"].split("\\", 1)[1]
            if rel not in names:
                names.append(rel)
    return names


# ---------------------------------------------------------------- decoding

class Edge:
    __slots__ = ("index", "adj", "pos", "flags1", "flags2")

    def __init__(self, index, adj, pos, flags1, flags2):
        self.index, self.adj, self.pos, self.flags1, self.flags2 = index, adj, pos, flags1, flags2

    @property
    def type(self):
        return (self.flags1 >> 13) & 3

    @property
    def obid(self):
        return self.flags1 & 0xFFF


class Area:
    __slots__ = ("offset", "pos", "radius", "word20", "word24", "flags", "edges", "graph", "normal", "centroid")

    def __init__(self, offset, pos, radius, word20, word24, flags, edges):
        self.offset, self.pos, self.radius = offset, pos, radius
        self.word20, self.word24, self.flags, self.edges = word20, word24, flags, edges
        self.graph = None
        n = len(edges)
        self.normal = newell_normal([e.pos for e in edges]) if n else (0.0, 1.0, 0.0)
        self.centroid = tuple(sum(e.pos[k] for e in edges) / n for k in range(3)) if n else pos

    @property
    def verts(self):
        return [e.pos for e in self.edges]

    @property
    def num_edges(self):
        return self.flags[0] & 0x7F

    @property
    def island(self):
        return (self.flags[0] >> 7) & 0xFFFF

    @property
    def plan_layer(self):
        return (self.flags[0] >> 23) & 0x1F

    @property
    def ob_cost(self):
        return (self.flags[1] >> 8) & 0xF

    @property
    def static_cost(self):
        return (self.flags[1] >> 12) & 0xF

    @property
    def basis_vert(self):
        return (self.flags[2] >> 24) & 0x7F


class Graph:
    def __init__(self, collection, resource, base, data):
        self.collection, self.resource, self.base = collection, resource, base
        self.index = len(collection.graphs)
        (self.version, self.layer, self.area_bytes, self.total_bytes) = struct.unpack_from("<4I", data, base)
        (self.build_scale, self.vox_size, self.radius, self.step_height, self.height) = struct.unpack_from("<5f", data, base + 0x10)
        self.bbox = struct.unpack_from("<6f", data, base + 0x24)
        self.pad = data[base + 0x3C:base + GRAPH_HEADER_SIZE]
        self.areas = {}
        self.kd_bbox = None
        self.kd_leaves = []
        self.kd_nodes = 0
        self.problems = []

    @property
    def name(self):
        return self.collection.name if len(self.collection.graphs) < 2 else "%s#%d" % (self.collection.name, self.index)


class Collection:
    def __init__(self, name, data, source):
        self.name, self.data, self.source = name, data, source
        self.resources = []
        self.graphs = []
        self.problems = []
        self.images = []


def short_name(asset_name):
    stem = asset_name.split(".navcollection")[0]
    head, sep, tail = stem.rpartition("_0x")
    return head if sep else stem


def decode_collection(name, data, source=""):
    c = Collection(short_name(name), data, source)
    if len(data) < 12:
        c.problems.append("too short")
        return c
    version, count, array_off = struct.unpack_from("<3I", data, 0)
    if version != 2:
        c.problems.append("NavResourceHeader version %d, the game wants 2" % version)
        return c
    for i in range(count):
        ro = array_off + i * 0x1C
        _next, space, subkey, _handle, nav_rel, _space_ptr, flags = struct.unpack_from("<7I", data, ro)
        c.resources.append({"space": space, "subkey": subkey, "flags": flags, "image": ro + nav_rel if nav_rel else 0})
    ends = []
    for res in c.resources:
        if res["image"]:
            ends.append(decode_image(c, data, res))
    if ends and max(ends) != len(data):
        c.problems.append("last image ends at 0x%X, payload is 0x%X bytes" % (max(ends), len(data)))
    return c


def decode_image(c, data, res):
    img = res["image"]
    endian, version, size, checksum, runtime, constant = struct.unpack_from("<6I", data, img)
    body = data[img + 0x18:img + 0x18 + size]
    words = len(body) // 4
    calc = sum(struct.unpack_from(">%dI" % words, body)) & 0xFFFFFFFF if words else 0
    image = {"offset": img, "endian": endian, "version": version, "size": size, "checksum": checksum,
             "checksum_calc": calc, "runtime_flags": runtime, "constant_flags": constant, "sections": []}
    res["header"] = image
    c.images.append(image)
    if version != 2:
        c.problems.append("ResourceHeader version %d, the game wants 2" % version)
    if endian != 0:
        c.problems.append("big-endian image, not decoded")
        return img + 0x18 + size
    if size and calc != checksum:
        c.problems.append("checksum 0x%08X, computed 0x%08X" % (checksum, calc))
    pos, end = img + 0x18, img + 0x18 + size
    while pos + 12 <= end:
        sid, ssize, ptr = struct.unpack_from("<3I", data, pos)
        image["sections"].append((sid, ssize, ptr))
        body_at = pos + 12
        if ptr != 0:
            c.problems.append("section 0x%X built with 64-bit pointers" % sid)
        if sid == SECTION_NAVGRAPHS:
            decode_navset(c, res, data, body_at, body_at + ssize)
        else:
            c.problems.append("unknown section id 0x%X (%d bytes) skipped" % (sid, ssize))
        pos = body_at + ssize
    if pos != end:
        c.problems.append("sections end at 0x%X, image at 0x%X" % (pos, end))
    return end


def decode_navset(c, res, data, at, end):
    big = data[at]
    version, count = struct.unpack_from("<2I", data, at + 4)
    if big:
        c.problems.append("NavSet flagged big-endian")
    if version < 0x1A:
        c.problems.append("NavSet version 0x%X < 0x1A" % version)
    g_at = at + 12
    for _ in range(count):
        g = Graph(c, res, g_at, data)
        c.graphs.append(g)
        decode_graph(g, data)
        if g.total_bytes <= 0:
            break
        g_at += g.total_bytes
    if g_at != end:
        c.problems.append("graphs end at 0x%X, section at 0x%X" % (g_at, end))


def read_area(data, base, off):
    a = base + off
    pos = struct.unpack_from("<3f", data, a + 0x10)
    radius, w20, w24 = struct.unpack_from("<fII", data, a + 0x1C)
    flags = struct.unpack_from("<3I", data, a + 0x28)
    edges = []
    for i in range(flags[0] & 0x7F):
        e = a + AREA_SIZE + i * EDGE_SIZE
        adj = struct.unpack_from("<I", data, e)[0]
        epos = struct.unpack_from("<3f", data, e + 4)
        f1, f2 = struct.unpack_from("<2I", data, e + 0x10)
        edges.append(Edge(i, adj, epos, f1, f2))
    return Area(off, pos, radius, w20, w24, flags, edges)


def decode_graph(g, data):
    base = g.base
    if g.version != 0x1B:
        g.problems.append("NavGraph version 0x%X (files seen so far are 0x1B)" % g.version)
    if g.pad[-2:] != PAD_SENTINEL:
        g.problems.append("header pad does not end in 'hv'")
    off = GRAPH_HEADER_SIZE
    stop = GRAPH_HEADER_SIZE + g.area_bytes
    while off < stop:
        area = read_area(data, base, off)
        area.graph = g
        if area.num_edges < 3:
            g.problems.append("area 0x%X has %d edges" % (off, area.num_edges))
        g.areas[off] = area
        off += AREA_SIZE + area.num_edges * EDGE_SIZE
    if off != stop:
        g.problems.append("areas end at 0x%X, areaBytes says 0x%X" % (off, stop))
    for a in g.areas.values():
        for e in a.edges:
            if e.adj and e.adj not in g.areas:
                g.problems.append("area 0x%X edge %d points at 0x%X, not an area" % (a.offset, e.index, e.adj))
            elif e.adj:
                back = [x for x in g.areas[e.adj].edges if x.adj == a.offset]
                if not back:
                    g.problems.append("area 0x%X edge %d adjacency is one-way" % (a.offset, e.index))
    decode_kdtree(g, data, base + stop)


def decode_kdtree(g, data, at):
    g.kd_bbox = struct.unpack_from("<6f", data, at)
    size = struct.unpack_from("<I", data, at + 0x18)[0]
    nodes_at = at + 0x1C
    end = nodes_at + size
    if end != g.base + g.total_bytes:
        g.problems.append("KD tree ends at 0x%X, graph at 0x%X" % (end - g.base, g.total_bytes))
    stack = [nodes_at] if size else []
    seen = []
    while stack:
        n = stack.pop()
        if n >= end:
            g.problems.append("KD node outside tree")
            break
        word = struct.unpack_from("<I", data, n)[0]
        if word & 0x80000000:
            seen.append(word & 0x7FFFFFFF)
            continue
        g.kd_nodes += 1
        right = word & 0x0FFFFFFF
        stack.append(n + right)
        stack.append(n + 12)
    g.kd_leaves = seen
    missing = set(g.areas) - set(seen)
    extra = [x for x in seen if x not in g.areas]
    if missing or extra or len(seen) != len(set(seen)):
        g.problems.append("KD leaves do not match areas (%d missing, %d unknown, %d leaves)" % (len(missing), len(extra), len(seen)))


# ---------------------------------------------------------------- geometry

def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def length(a):
    return math.sqrt(dot(a, a))


def newell_normal(verts):
    nx = ny = nz = 0.0
    for i, p in enumerate(verts):
        q = verts[(i + 1) % len(verts)]
        nx += (p[1] - q[1]) * (p[2] + q[2])
        ny += (p[2] - q[2]) * (p[0] + q[0])
        nz += (p[0] - q[0]) * (p[1] + q[1])
    n = length((nx, ny, nz)) or 1.0
    return (nx / n, ny / n, nz / n)


def closest_on_segment(p, a, b):
    ab = sub(b, a)
    den = dot(ab, ab)
    t = 0.0 if den <= 0 else max(0.0, min(1.0, dot(sub(p, a), ab) / den))
    return (a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t)


def inside_xz(verts, x, z):
    inside = False
    n = len(verts)
    for i in range(n):
        x1, _, z1 = verts[i]
        x2, _, z2 = verts[(i + 1) % n]
        if (z1 > z) != (z2 > z):
            xi = x1 + (z - z1) * (x2 - x1) / (z2 - z1)
            if xi > x:
                inside = not inside
    return inside


def surface_y(area, x, z):
    n, c = area.normal, area.centroid
    if abs(n[1]) < 1e-6:
        return None
    return c[1] - (n[0] * (x - c[0]) + n[2] * (z - c[2])) / n[1]


def closest_on_area(area, p):
    """Nearest point of the polygon to p, in 3D."""
    n, c = area.normal, area.centroid
    d = dot(sub(p, c), n)
    proj = (p[0] - n[0] * d, p[1] - n[1] * d, p[2] - n[2] * d)
    if abs(n[1]) > 0.3 and inside_xz(area.verts, proj[0], proj[2]):
        return proj
    best, best_d = None, None
    vs = area.verts
    for i in range(len(vs)):
        q = closest_on_segment(p, vs[i], vs[(i + 1) % len(vs)])
        dd = dot(sub(q, p), sub(q, p))
        if best_d is None or dd < best_d:
            best, best_d = q, dd
    return best


# ---------------------------------------------------------------- space

class Space:
    """All graphs of one map in one layer, joined the way the runtime joins them."""

    def __init__(self, collections, layer=0, space_name=0, deferred=False):
        # Load-time state: resources with flags bit 0 are added later by game code.
        self.graphs = [g for c in collections for g in c.graphs
                       if g.layer == layer and g.areas and g.resource["space"] == space_name
                       and (deferred or not g.resource["flags"] & 1)]
        self.areas = [a for g in self.graphs for a in g.areas.values()]
        self.links = defaultdict(set)
        self.portal_pairs = []
        self.portal_rejects = []
        for g in self.graphs:
            for a in g.areas.values():
                for e in a.edges:
                    if e.adj:
                        self.links[id(a)].add(id(g.areas[e.adj]))
        self.by_id = {id(a): a for a in self.areas}
        self.join_portals()
        self.components()

    def portal_edges(self):
        out = []
        for g in self.graphs:
            for a in g.areas.values():
                for e in a.edges:
                    if e.type == 1 and not e.adj:
                        nxt = a.edges[(e.index + 1) % len(a.edges)].pos
                        out.append((g, a, e, e.pos, nxt))
        return out

    def join_portals(self):
        edges = self.portal_edges()
        for i, (ga, aa, ea, a0, a1) in enumerate(edges):
            dist = max(PORTAL_DIST_SCALE, PORTAL_DIST_VOX * ga.vox_size)
            for gb, ab, eb, b0, b1 in edges[i + 1:]:
                if gb is ga:
                    continue
                if (min(a0[0], a1[0]) - dist > max(b0[0], b1[0]) or min(b0[0], b1[0]) - dist > max(a0[0], a1[0]) or
                        min(a0[2], a1[2]) - dist > max(b0[2], b1[2]) or min(b0[2], b1[2]) - dist > max(a0[2], a1[2]) or
                        min(a0[1], a1[1]) - dist > max(b0[1], b1[1]) or min(b0[1], b1[1]) - dist > max(a0[1], a1[1])):
                    continue
                verdict = overlap_rule(a0, a1, aa.normal, b0, b1, ab.normal, dist)
                if verdict[0]:
                    self.portal_pairs.append((aa, ea, ab, eb, verdict[1], verdict[2]))
                    self.links[id(aa)].add(id(ab))
                    self.links[id(ab)].add(id(aa))
                elif verdict[2] is not None and verdict[2] <= dist:
                    self.portal_rejects.append((aa, ea, ab, eb, verdict[1], verdict[2], verdict[3]))

    def components(self):
        self.comp = {}
        n = 0
        for a in self.areas:
            if id(a) in self.comp:
                continue
            stack = [id(a)]
            self.comp[id(a)] = n
            while stack:
                cur = stack.pop()
                for nb in self.links[cur]:
                    if nb not in self.comp:
                        self.comp[nb] = n
                        stack.append(nb)
            n += 1
        self.num_components = n

    def component_areas(self, cid):
        return [a for a in self.areas if self.comp[id(a)] == cid]

    def locate(self, p, tol_below=1.0, tol_above=1.0):
        """Areas whose footprint holds p, and the nearest mesh point overall."""
        hits = []
        best = None
        for a in self.areas:
            if abs(a.normal[1]) > 0.3 and inside_xz(a.verts, p[0], p[2]):
                y = surface_y(a, p[0], p[2])
                if y is not None:
                    hits.append((p[1] - y, a, y))
            if length(sub(a.centroid, p)) - a.radius - 1.0 > (best[0] if best else 1e9):
                continue
            q = closest_on_area(a, p)
            d = length(sub(q, p))
            if best is None or d < best[0]:
                best = (d, a, q)
        hits.sort(key=lambda h: abs(h[0]))
        on = [h for h in hits if -tol_below <= h[0] <= tol_above]
        return on, hits, best


def overlap_rule(a0, a1, na, b0, b1, nb, dist):
    """GenerateOverlapsForEdge as read from the PC and Xbox code; returns (ok, overlap, gap, reason)."""
    ca_b0 = closest_on_segment(b0, a0, a1)
    ca_b1 = closest_on_segment(b1, a0, a1)
    ov_a = length(sub(ca_b0, ca_b1))
    cb_a0 = closest_on_segment(a0, b0, b1)
    cb_a1 = closest_on_segment(a1, b0, b1)
    ov_b = length(sub(cb_a0, cb_a1))
    d1 = length(sub(ca_b1, cb_a0))
    d2 = length(sub(ca_b0, cb_a1))
    gap = min(d1, d2)
    overlap = min(ov_a, ov_b)
    if ov_a < OVERLAP_MIN or ov_b < OVERLAP_MIN:
        return False, overlap, gap, "overlap below 1 cm"
    if d1 > dist and d2 > dist:
        return False, overlap, gap, "farther than %.2f m" % dist
    if d1 <= EDGE_MIN or d2 <= EDGE_MIN:
        return False, overlap, gap, "coincident"
    seg_a = (ca_b1, ca_b0)
    seg_b = (cb_a1, cb_a0)
    if not right_side(seg_a, seg_b, na) or not right_side(seg_b, seg_a, nb):
        return False, overlap, gap, "not facing"
    return True, overlap, gap, ""


def right_side(seg_a, seg_b, normal):
    d = sub(seg_a[1], seg_a[0])
    mid = tuple((seg_b[0][k] + seg_b[1][k]) * 0.5 for k in range(3))
    return dot(cross(d, sub(mid, seg_a[0])), normal) <= 1e-6


# ---------------------------------------------------------------- output

def print_summary(collections):
    for c in collections:
        print("== %s  (%d bytes, %s)" % (c.name, len(c.data), c.source))
        for r in c.resources:
            print("   resource space=%d subkey=%d flags=0x%X image@0x%X" % (r["space"], r["subkey"], r["flags"], r["image"]))
        for im in c.images:
            ok = "ok" if im["checksum"] == im["checksum_calc"] else "BAD"
            print("   image v%d size=%d checksum=0x%08X (%s) runtime=0x%X sections=%s" % (
                im["version"], im["size"], im["checksum"], ok, im["runtime_flags"],
                ",".join("0x%X/%d" % (s, n) for s, n, _p in im["sections"]) or "none"))
        for g in c.graphs:
            edges = sum(len(a.edges) for a in g.areas.values())
            types = defaultdict(int)
            walls = links = 0
            for a in g.areas.values():
                for e in a.edges:
                    types[EDGE_TYPES[e.type]] += 1
                    if e.adj:
                        links += 1
                    elif e.type == 0:
                        walls += 1
            sizes = defaultdict(int)
            for a in g.areas.values():
                sizes[a.num_edges] += 1
            r = g.resource
            print("   graph %d (resource space=%d subkey=%d flags=0x%X) v0x%X layer=%d areas=%d edges=%d (internal %d, wall %d, %s) kd nodes=%d leaves=%d" % (
                g.index, r["space"], r["subkey"], r["flags"], g.version, g.layer, len(g.areas), edges, links, walls,
                ", ".join("%s %d" % kv for kv in sorted(types.items())), g.kd_nodes, len(g.kd_leaves)))
            print("     build scale=%.2f vox=%.2f radius=%.2f step=%.2f height=%.2f" % (
                g.build_scale, g.vox_size, g.radius, g.step_height, g.height))
            print("     bbox min (%.2f %.2f %.2f) max (%.2f %.2f %.2f)" % g.bbox)
            print("     polygon sizes %s" % " ".join("%d:%d" % kv for kv in sorted(sizes.items())))
            for p in g.problems[:8]:
                print("     ! %s" % p)
        for p in c.problems:
            print("   ! %s" % p)


def write_obj(collections, out_dir, space=None, points=()):
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    combined = ["# DS2 NavPower meshes, game coordinates (Y up)"]
    base_index = 0
    for c in collections:
        if not any(g.areas for g in c.graphs):
            continue
        lines = ["# %s" % c.name, "o %s" % c.name]
        verts, index = [], {}
        faces = []
        for g in c.graphs:
            for a in g.areas.values():
                face = []
                for v in a.verts:
                    key = tuple(round(x, 5) for x in v)
                    if key not in index:
                        index[key] = len(verts) + 1
                        verts.append(key)
                    face.append(index[key])
                faces.append(face)
        lines += ["v %.5f %.5f %.5f" % v for v in verts]
        lines += ["f " + " ".join(str(i) for i in f) for f in faces]
        path = out_dir / ("%s.obj" % c.name)
        path.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
        written.append(path)
        combined.append("o %s" % c.name)
        combined += ["v %.5f %.5f %.5f" % v for v in verts]
        combined += ["f " + " ".join(str(i + base_index) for i in f) for f in faces]
        base_index += len(verts)
    if space is not None:
        portal_lines = []
        for g in space.graphs:
            for a in g.areas.values():
                for e in a.edges:
                    if e.type == 1:
                        portal_lines.append((e.pos, a.edges[(e.index + 1) % len(a.edges)].pos))
        if portal_lines:
            combined.append("o portal_edges")
            for p, q in portal_lines:
                combined.append("v %.5f %.5f %.5f" % p)
                combined.append("v %.5f %.5f %.5f" % q)
                base_index += 2
                combined.append("l %d %d" % (base_index - 1, base_index))
        if space.portal_pairs:
            combined.append("o portal_joins")
            for aa, _ea, ab, _eb, _ov, _gap in space.portal_pairs:
                combined.append("v %.5f %.5f %.5f" % aa.centroid)
                combined.append("v %.5f %.5f %.5f" % ab.centroid)
                base_index += 2
                combined.append("l %d %d" % (base_index - 1, base_index))
        if points:
            # a 2 m vertical tick per query point and a line to its nearest mesh point
            combined.append("o query_points")
            for p in points:
                _on, _hits, best = space.locate(p)
                ends = [(p[0], p[1] - 1.0, p[2]), (p[0], p[1] + 1.0, p[2])]
                if best:
                    ends += [tuple(p), best[2]]
                for i in range(0, len(ends), 2):
                    combined.append("v %.5f %.5f %.5f" % ends[i])
                    combined.append("v %.5f %.5f %.5f" % ends[i + 1])
                    base_index += 2
                    combined.append("l %d %d" % (base_index - 1, base_index))
    path = out_dir / "all_collections.obj"
    path.write_text("\n".join(combined) + "\n", encoding="ascii", newline="\n")
    written.append(path)
    return written


def fmt(p):
    return "(%.2f %.2f %.2f)" % tuple(p)


def report_points(space, points, tol):
    for p in points:
        on, hits, best = space.locate(p, tol, tol)
        print("point %s" % fmt(p))
        if on:
            dy, a, y = on[0]
            print("   ON mesh: %s area 0x%X, surface y=%.2f (point is %.2f m %s it), component %d" % (
                a.graph.name, a.offset, y, abs(dy), "above" if dy >= 0 else "below", space.comp[id(a)]))
        else:
            print("   OFF mesh (no polygon footprint within %.1f m vertically)" % tol)
        for dy, a, y in hits[:4]:
            if on and a is on[0][1]:
                continue
            print("   footprint also in %s area 0x%X, surface y=%.2f, dy=%+.2f, component %d" % (
                a.graph.name, a.offset, y, dy, space.comp[id(a)]))
        if best:
            d, a, q = best
            horiz = math.hypot(q[0] - p[0], q[2] - p[2])
            print("   nearest mesh point %s in %s area 0x%X: %.2f m (horizontal %.2f, vertical %+.2f), component %d" % (
                fmt(q), a.graph.name, a.offset, d, horiz, q[1] - p[1], space.comp[id(a)]))


def area_for(space, p, tol):
    on, _hits, best = space.locate(p, tol, tol)
    if on:
        return on[0][1], "on"
    return (best[1], "nearest") if best else (None, "none")


def segment_distance(p0, p1, q0, q1):
    best = None
    for a, b, c in ((p0, q0, q1), (p1, q0, q1)):
        x = closest_on_segment(a, b, c)
        d = length(sub(a, x))
        best = d if best is None or d < best else best
    for a, b, c in ((q0, p0, p1), (q1, p0, p1)):
        x = closest_on_segment(a, b, c)
        d = length(sub(a, x))
        best = d if d < best else best
    return best


def boundary_edges(area):
    out = []
    for e in area.edges:
        if not e.adj:
            out.append((e, e.pos, area.edges[(e.index + 1) % len(area.edges)].pos))
    return out


def closest_gap(space, ca, cb):
    """Closest pair of boundary edges between two components."""
    best = None
    a_areas = space.component_areas(ca)
    b_areas = space.component_areas(cb)
    for a in a_areas:
        for b in b_areas:
            if length(sub(a.centroid, b.centroid)) - a.radius - b.radius > (best[0] if best else 1e9):
                continue
            for ea, p0, p1 in boundary_edges(a):
                for eb, q0, q1 in boundary_edges(b):
                    d = segment_distance(p0, p1, q0, q1)
                    if best is None or d < best[0]:
                        best = (d, a, ea, p0, p1, b, eb, q0, q1)
    return best


def report_reach(space, start, targets, tol):
    a0, how = area_for(space, start, tol)
    if a0 is None:
        print("reach: no mesh")
        return
    c0 = space.comp[id(a0)]
    size0 = len(space.component_areas(c0))
    print("reach from %s: %s area 0x%X (%s), component %d with %d areas in %s" % (
        fmt(start), a0.graph.name, a0.offset, how, c0, size0,
        ", ".join(sorted({a.graph.name for a in space.component_areas(c0)}))))
    for p in targets:
        a1, how1 = area_for(space, p, tol)
        if a1 is None:
            continue
        c1 = space.comp[id(a1)]
        if c1 == c0:
            print("   to %s: %s area 0x%X (%s) is CONNECTED" % (fmt(p), a1.graph.name, a1.offset, how1))
            continue
        print("   to %s: %s area 0x%X (%s) is NOT connected (component %d, %d areas in %s)" % (
            fmt(p), a1.graph.name, a1.offset, how1, c1, len(space.component_areas(c1)),
            ", ".join(sorted({a.graph.name for a in space.component_areas(c1)}))))
        gap = closest_gap(space, c0, c1)
        if gap:
            d, aa, ea, p0, p1, ab, eb, q0, q1 = gap
            print("      closest boundary pair %.2f m apart:" % d)
            print("        %s area 0x%X edge %d (%s) %s-%s" % (aa.graph.name, aa.offset, ea.index, EDGE_TYPES[ea.type], fmt(p0), fmt(p1)))
            print("        %s area 0x%X edge %d (%s) %s-%s" % (ab.graph.name, ab.offset, eb.index, EDGE_TYPES[eb.type], fmt(q0), fmt(q1)))


def report_space(space):
    sizes = sorted(((len(space.component_areas(i)), i) for i in range(space.num_components)), reverse=True)
    print("space: %d graphs, %d areas, %d portal joins, %d components" % (
        len(space.graphs), len(space.areas), len(space.portal_pairs), space.num_components))
    for n, cid in sizes[:12]:
        names = sorted({a.graph.name for a in space.component_areas(cid)})
        print("   component %d: %d areas in %s" % (cid, n, ", ".join(names)))
    joined = defaultdict(int)
    for aa, _ea, ab, _eb, _ov, _gap in space.portal_pairs:
        joined[tuple(sorted((aa.graph.name, ab.graph.name)))] += 1
    for (x, y), n in sorted(joined.items()):
        print("   join %s <-> %s: %d edge pairs" % (x, y, n))
    unmatched = 0
    matched = {id(e) for aa, ea, ab, eb, _o, _g in space.portal_pairs for e in (ea, eb)}
    for g, a, e, p0, p1 in space.portal_edges():
        if id(e) not in matched:
            unmatched += 1
    print("   portal edges without a partner: %d" % unmatched)
    reasons = defaultdict(int)
    for r in space.portal_rejects:
        reasons[r[6]] += 1
    for k, v in sorted(reasons.items()):
        print("   near portal pairs rejected (%s): %d" % (k, v))


# ---------------------------------------------------------------- dump

def compare_dump(collections, dump_path):
    """Find nav graph headers resident in a PC minidump and diff them against the files."""
    from analyze_minidump import Dump
    from dump_scan import find_bytes, regions
    d = Dump(Path(dump_path))
    files = [g for c in collections for g in c.graphs if g.areas]
    if not files:
        print("dump: nothing to compare")
        return
    pad_probe = files[0].pad[0x80:0x90]
    mapped = sum(e - s for s, e in regions(d))
    hits = list(find_bytes(d, pad_probe))
    print("dump %s: %.0f MB mapped, %d graph headers by pad signature" % (dump_path, mapped / 1e6, len(hits)))
    matched = set()
    resident = []
    for h in hits:
        hdr = h - 0x3C - 0x80
        raw = d.read(hdr, 0x40)
        if not raw:
            continue
        ver, layer, area_bytes, total = struct.unpack_from("<4I", raw)
        if ver != 0x1B or total > 1 << 26:
            continue
        bbox = struct.unpack_from("<6f", raw, 0x24)
        blob = d.read(hdr, total)
        if not blob:
            print("   %08X unreadable" % hdr)
            continue
        cand = [g for g in files if g.area_bytes == area_bytes and g.total_bytes == total and
                max(abs(x - y) for x, y in zip(g.bbox, bbox)) < 1e-3]
        if not cand:
            print("   %08X v0x%X areas %d bytes bbox %s: no file graph matches" % (hdr, ver, area_bytes, fmt(bbox[:3])))
            continue
        g = cand[0]
        matched.add(id(g))
        resident.append((hdr, g, blob))
        diff_report(hdr, g, blob, d)
    for g in files:
        if id(g) not in matched:
            print("   %s: not resident in the dump" % g.name)
    compare_joins(collections, resident, d)


def static_owner(resident, p):
    for h2, g2, _b2 in resident:
        if h2 + GRAPH_HEADER_SIZE <= p < h2 + GRAPH_HEADER_SIZE + g2.area_bytes:
            return g2
    return None


def compare_joins(collections, resident, d, depth=6):
    """Cross-graph links the runtime made, against the joins this tool predicts.

    Portal joins run through runtime areas allocated in the NavPower pool, so the
    walk follows those (same Area layout) until it reaches another static graph.
    """
    live = defaultdict(int)
    pool = defaultdict(int)
    for hdr, g, blob in resident:
        for off in g.areas:
            la = read_area(blob, 0, off)
            for e in la.edges:
                p = e.adj
                if not p or hdr <= p < hdr + len(blob):
                    continue
                pool[g.name] += 1
                reached = set()
                frontier, seen = [p], {p}
                for _ in range(depth):
                    nxt = []
                    for q in frontier:
                        owner = static_owner(resident, q)
                        if owner is not None:
                            if owner is not g:
                                reached.add(owner.name)
                            continue
                        raw = d.read(q, AREA_SIZE + 0x7F * EDGE_SIZE) or d.read(q, AREA_SIZE + 16 * EDGE_SIZE)
                        if not raw:
                            continue
                        dyn = read_area(raw, 0, 0) if (struct.unpack_from("<I", raw, 0x28)[0] & 0x7F) * EDGE_SIZE + AREA_SIZE <= len(raw) else None
                        if dyn is None:
                            continue
                        for de in dyn.edges:
                            if de.adj and de.adj not in seen:
                                seen.add(de.adj)
                                nxt.append(de.adj)
                    frontier = nxt
                for name in reached:
                    live[tuple(sorted((g.name, name)))] += 1
    names = {g.name for _h, g, _b in resident}
    space = Space(collections)
    predicted = defaultdict(int)
    for aa, _ea, ab, _eb, _ov, _gap in space.portal_pairs:
        if aa.graph.name in names and ab.graph.name in names:
            predicted[tuple(sorted((aa.graph.name, ab.graph.name)))] += 1
    print("   graph joins: %d live pairs, %d predicted pairs" % (len(live), len(predicted)))
    for pair in sorted(set(live) | set(predicted)):
        state = "both" if pair in live and pair in predicted else ("live only" if pair in live else "predicted only")
        print("      %-55s %s (live edges %d)" % ("%s <-> %s" % pair, state, live.get(pair, 0)))
    for name, n in sorted(pool.items()):
        print("      %s: %d edges lead into runtime areas" % (name, n))


def cyclic_shift(a, b):
    """Shift k with b == a rotated by k, else None."""
    n = len(a)
    if n != len(b):
        return None
    for k in range(n):
        if all(a[(i + k) % n] == b[i] for i in range(n)):
            return k
    return None


def diff_report(hdr, g, live, d):
    """Compare every area of a resident graph with the file: geometry, order, adjacency."""
    total = len(live)
    stats = defaultdict(int)
    samples = []
    for off, fa in sorted(g.areas.items()):
        la = read_area(live, 0, off)
        if la.num_edges != fa.num_edges:
            stats["edge count changed"] += 1
            samples.append("area 0x%X edges %d -> %d" % (off, fa.num_edges, la.num_edges))
            continue
        same_pos = la.pos == fa.pos and la.radius == fa.radius
        stats["centre and radius identical" if same_pos else "centre or radius moved"] += 1
        fv = [e.pos for e in fa.edges]
        lv = [e.pos for e in la.edges]
        k = cyclic_shift(fv, lv)
        if k is None:
            stats["vertex cycle changed"] += 1
            if len(samples) < 6:
                samples.append("area 0x%X vertices %s -> %s" % (off, " ".join(fmt(v) for v in fv), " ".join(fmt(v) for v in lv)))
            continue
        stats["vertex cycle identical" if k == 0 else "vertex cycle rotated"] += 1
        for i, le in enumerate(la.edges):
            fe = fa.edges[(i + k) % fa.num_edges]
            if le.type != fe.type or le.obid != fe.obid:
                stats["edge type changed"] += 1
            p = le.adj
            if not fe.adj and not p:
                stats["open edge, still open"] += 1
            elif fe.adj and p == (hdr + fe.adj) & 0xFFFFFFFF:
                stats["adjacency relocated to the same area"] += 1
            elif fe.adj and not p:
                stats["adjacency cleared at runtime"] += 1
            elif not fe.adj and p and not hdr <= p < hdr + total:
                stats["open edge joined to another graph"] += 1
            elif fe.adj and p and not hdr <= p < hdr + total:
                stats["adjacency redirected outside the graph"] += 1
            else:
                stats["adjacency differs"] += 1
                if len(samples) < 6:
                    samples.append("area 0x%X edge %d file 0x%X live 0x%08X" % (off, i, fe.adj, p))
    kd_same = live[GRAPH_HEADER_SIZE + g.area_bytes:] == g.collection.data[g.base + GRAPH_HEADER_SIZE + g.area_bytes:g.base + g.total_bytes]
    # the image is used in place, so its header sits at the same distance as in the file
    res = d.read(hdr - (g.base - g.resource["image"]), 0x18)
    rt = struct.unpack_from("<6I", res) if res else None
    filed = g.resource.get("header", {})
    if rt:
        head = "resource header v%d size %d checksum %s, runtime flags 0x%X" % (
            rt[1], rt[2], "= file" if rt[3] == filed.get("checksum") else "0x%08X" % rt[3], rt[4])
    else:
        head = "resource header unreadable"
    print("   %08X = %s (%d areas): KD tree %s; %s" % (hdr, g.name, len(g.areas), "identical" if kd_same else "differs", head))
    for key in sorted(stats):
        print("      %-40s %d" % (key, stats[key]))
    for s in samples:
        print("      ! %s" % s)


# ---------------------------------------------------------------- main

def load_sources(args):
    collections = []
    if args.map:
        dats = []
        for dat in args.dat or default_dats():
            p = Path(dat)
            dats.append(p if p.exists() else GAME_DIR / dat)
        names = catalog_streams(args.map, args.catalog)
        if not names:
            raise SystemExit("catalog has no NAV streams for %s" % args.map)
        for name, (dat, blob) in read_dat_entries(dats, names).items():
            for asset, payload in stream_nav_assets(blob, name):
                collections.append(decode_collection(asset, payload, "%s:%s" % (dat, name)))
    elif args.dat:
        raise SystemExit("--dat needs --map")
    for path in args.files:
        p = Path(path)
        blob = p.read_bytes()
        if blob[:4] in (b"3slo", b"COHS"):
            for asset, payload in stream_nav_assets(blob, str(p)):
                collections.append(decode_collection(asset, payload, p.name))
        else:
            collections.append(decode_collection(p.name, blob, p.name))
    return collections


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help=".str stream sets or raw .navcollection.xml.win payloads")
    ap.add_argument("--dat", action="append", help="DS2 BIGH archive, e.g. DS2DAT9.DAT (default: every DS2DATn.DAT)")
    ap.add_argument("--map", help="level folder inside the archives, e.g. mp_bombrun or ch09_mng")
    ap.add_argument("--catalog", type=Path, default=CATALOG)
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--space", action="store_true", help="report how the graphs join into one space")
    ap.add_argument("--obj", type=Path, help="write OBJ files into this folder")
    ap.add_argument("--point", nargs=3, type=float, action="append", default=[], metavar=("X", "Y", "Z"))
    ap.add_argument("--reach", nargs=3, type=float, metavar=("X", "Y", "Z"))
    ap.add_argument("--tol", type=float, default=1.0, help="vertical tolerance for on-mesh, metres")
    ap.add_argument("--layer", type=int, default=0)
    ap.add_argument("--space-name", type=lambda v: int(v, 0), default=0, help="NavSpace name hash, 0 = level space")
    ap.add_argument("--deferred", action="store_true", help="also join resources the game adds later (flags bit 0)")
    ap.add_argument("--dump", help="PC minidump to compare against")
    args = ap.parse_args()
    collections = load_sources(args)
    if not collections:
        raise SystemExit("no NAV resources found")
    print("%d collections, %d with geometry" % (len(collections), sum(1 for c in collections if any(g.areas for g in c.graphs))))
    space = None
    if args.space or args.point or args.reach or args.obj:
        space = Space(collections, args.layer, args.space_name, args.deferred)
    if args.summary:
        print_summary(collections)
    if args.space:
        report_space(space)
    if args.obj:
        for path in write_obj(collections, args.obj, space, args.point):
            print("wrote %s" % path)
    if args.point:
        report_points(space, args.point, args.tol)
    if args.reach:
        report_reach(space, args.reach, args.point, args.tol)
    if args.dump:
        compare_dump(collections, args.dump)


if __name__ == "__main__":
    main()
