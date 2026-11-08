"""Find how an importer mapped glTF axes, from a rest pose, and map poses back.

An importer (UE5's glTF Interchange, for one) converts every bone's local
transform with one axis map: v_out = s * P * v, P a signed permutation (a turn
or a mirror), s a unit scale. Comparing the importer's reference pose with the
known glTF rest pose (the scene sidecar) gives P and s, so any edited pose maps
back exactly, whichever convention the importer used.
"""
import itertools
import math


def signed_permutations():
    for order in itertools.permutations(range(3)):
        for signs in itertools.product((1, -1), repeat=3):
            m = [[0.0] * 3 for _ in range(3)]
            for row, column in enumerate(order):
                m[row][column] = float(signs[row])
            yield m


def det3(m):
    return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))


def apply(m, v):
    return [sum(m[i][j] * v[j] for j in range(3)) for i in range(3)]


def transpose(m):
    return [[m[j][i] for j in range(3)] for i in range(3)]


def calibrate(translations, rotations=None):
    """translations: [(glTF local t, imported local t)], rotations: [(glTF q xyzw, imported q xyzw)].
    Returns (P, s, flip, error): imported = s * P * glTF for vectors, and for the vector
    part of a quaternion imported.xyz = flip * P * glTF.xyz (flip -1 for a mirror)."""
    best = None
    for p in signed_permutations():
        num = den = 0.0
        for g, u in translations:
            pg = apply(p, g)
            num += sum(a * b for a, b in zip(pg, u))
            den += sum(a * a for a in pg)
        if den <= 0.0:
            continue
        s = num / den
        if s <= 0.0:
            continue
        error = max((max(abs(u[i] - s * pg_i) for i, pg_i in enumerate(apply(p, g))) for g, u in translations), default=0.0)
        if best is None or error < best[3]:
            best = (p, s, 1.0, error)
    if best is None:
        raise ValueError("no axis map fits the rest pose")
    p, s, _flip, error = best
    flip = -1.0 if det3(p) < 0 else 1.0
    if rotations:
        def rotation_error(sign):
            worst = 0.0
            for g, u in rotations:
                mapped = [sign * c for c in apply(p, g[:3])] + [g[3]]
                dot = abs(sum(a * b for a, b in zip(mapped, u)))
                worst = max(worst, 1.0 - min(1.0, dot))
            return worst
        flip = min((1.0, -1.0), key=rotation_error)
    return p, s, flip, error


def to_gltf(p, s, flip, t, q):
    """An imported local (t, q xyzw) back in glTF axes."""
    pt = transpose(p)
    tg = [v / s for v in apply(pt, t)]
    xyz = [flip * v for v in apply(pt, q[:3])]
    norm = math.sqrt(sum(v * v for v in xyz) + q[3] * q[3]) or 1.0
    return tg, [v / norm for v in xyz] + [q[3] / norm]


def from_gltf(p, s, flip, t, q):
    """The other way, for tests and for writing poses into an importer's space."""
    return [s * v for v in apply(p, t)], [flip * v for v in apply(p, q[:3])] + [q[3]]


# ---------------------------------------------------------------- the root bone
# An importer may convert the axes once, above the root (the root's local then
# carries the whole conversion, its children keep glTF axes), instead of in every
# bone. The root then needs its own map: imported = A * glTF, A from the rest pose.

def matrix(t, q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), t[0]],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), t[1]],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), t[2]],
            [0.0, 0.0, 0.0, 1.0]]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def inverse(m):
    rows = [list(r) + [float(i == j) for j in range(4)] for i, r in enumerate(m)]
    for c in range(4):
        pivot = max(range(c, 4), key=lambda r: abs(rows[r][c]))
        rows[c], rows[pivot] = rows[pivot], rows[c]
        d = rows[c][c]
        rows[c] = [v / d for v in rows[c]]
        for r in range(4):
            if r != c:
                f = rows[r][c]
                rows[r] = [a - f * b for a, b in zip(rows[r], rows[c])]
    return [r[4:] for r in rows]


def decompose(m):
    """(t, q xyzw) of a rigid 4x4 (rotation part normalised column by column)."""
    t = [m[i][3] for i in range(3)]
    r = [[m[i][j] for j in range(3)] for i in range(3)]
    for j in range(3):
        n = math.sqrt(sum(r[i][j] ** 2 for i in range(3))) or 1.0
        for i in range(3):
            r[i][j] /= n
    trace = r[0][0] + r[1][1] + r[2][2]
    if trace > 0:
        s = math.sqrt(trace + 1.0) * 2
        q = [(r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s, s / 4]
    else:
        i = max(range(3), key=lambda k: r[k][k])
        j, k = (i + 1) % 3, (i + 2) % 3
        s = math.sqrt(max(0.0, 1.0 + r[i][i] - r[j][j] - r[k][k])) * 2
        q = [0.0] * 4
        q[i] = s / 4
        q[3] = (r[k][j] - r[j][k]) / s
        q[j] = (r[j][i] + r[i][j]) / s
        q[k] = (r[k][i] + r[i][k]) / s
    n = math.sqrt(sum(v * v for v in q)) or 1.0
    return t, [v / n for v in q]


def root_map(p, s, flip, gltf_rest, imported_rest, tolerance=1e-3):
    """None when the root follows the bones' axis map, else the matrix A that turns
    the glTF root local into the imported one (a conversion above the root)."""
    t, q = from_gltf(p, s, flip, *gltf_rest)
    if max(abs(a - b) for a, b in zip(t, imported_rest[0])) <= tolerance * max(1.0, s) and \
            1.0 - abs(sum(a * b for a, b in zip(q, imported_rest[1]))) <= tolerance:
        return None
    return mul(matrix(*imported_rest), inverse(matrix(*gltf_rest)))


def root_to_gltf(a, t, q):
    return decompose(mul(inverse(a), matrix(t, q)))
