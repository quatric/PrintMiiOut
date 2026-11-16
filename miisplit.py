"""Splitting a Mii figure into pieces that snap together, in plain numpy:
plane cuts through the body that follow its bones, round pins with crush
ribs and the sockets they press into, and the pieces laid out for printing.

printmiiout.py does the rest in Blender (fusing each piece, cutting the
sockets); core/src/split.cpp and pipeline.cpp do the same in C++.

Meshes here are triangle arrays: vertices (n, 3), faces (m, 3) and, for the
body, a tag per face saying which piece it belongs to. Sizes are in model
units, which are millimetres when the figure prints at 100%.
"""

import math

import numpy as np

from miibody import HEAD_BONE

TAG_CAP, TAG_TORSO, TAG_ARM_L, TAG_ARM_R, TAG_LEGS, TAG_NECK = range(6)

PIN_CLEARANCE = 0.1     # between a pin's body and its socket, each side
PIN_RIB = 0.22          # how far the crush ribs stand off the pin
PIN_END_GAP = 0.25      # left free at the bottom of each socket
SLIDE_CLEARANCE = 0.15  # between pieces that slide past each other
NECK_SOCKET_DEPTH = 6.0  # how deep the neck's pin goes into the torso and head, at least


def wall_for(r):
    """The material to leave around a bore of radius r."""
    return min(max(0.5 * r, 0.6), 1.5)


def good_depth(r):
    """How deep a bore of radius r should go to hold its pin firmly."""
    return min(max(3.2 * r, 3.0), 8.0)


def face_tags(body):
    """Which piece each face of a miibody.Body goes to, per part: the arm
    the arm, elbow and wrist bones move, the legs (the whole legs mesh, the
    tops tucked under the shirt or skirt included), the neck (the head bone)
    and the torso, which keeps the shoulders."""
    def tag_of(name):
        if name == HEAD_BONE:
            return TAG_NECK
        if name.startswith(("arm_", "elbow_", "wrist_")):
            return TAG_ARM_L if "_l" in name else TAG_ARM_R
        if name.startswith(("foot_", "knee_", "ankle_")):
            return TAG_LEGS
        return TAG_TORSO

    by_joint = np.array([tag_of(n) for n in body.joint_names])
    out = []
    for (name, _v, tris), dom in zip(body.parts, body.dominant):
        if name.startswith("legs"):
            out.append(np.full(len(tris), TAG_LEGS))
            continue
        t = by_joint[dom][tris]
        # The tag most of the corners have; with three different, the first.
        out.append(np.where(t[:, 1] == t[:, 2], t[:, 1], t[:, 0]))
    return out


def unit(v):
    v = np.asarray(v, dtype=np.float64)
    return v / max(np.linalg.norm(v), 1e-300)


def plane_basis(n):
    """Two directions spanning the plane with this normal."""
    a = np.array((1.0, 0.0, 0.0)) if abs(n[0]) < 0.9 else np.array((0.0, 1.0, 0.0))
    e1 = unit(np.cross(n, a))
    return e1, np.cross(n, e1)


def bone_distance(points, chain):
    """How far each point is from a chain of bones [(start, end), ...]."""
    best = np.full(len(points), np.inf)
    for a, b in chain:
        ab = b - a
        t = np.clip((points - a) @ ab / max(ab @ ab, 1e-12), 0.0, 1.0)
        best = np.minimum(best, np.linalg.norm(points - (a + t[:, None] * ab), axis=1))
    return best


def components(n, a, b):
    """Connected components of n nodes joined by edges a[i]-b[i]: a label
    per node, the same for nodes that are connected."""
    lab = np.arange(n)
    if not len(a):
        return lab
    while True:
        la, lb = lab[a], lab[b]
        if np.array_equal(la, lb):
            return lab
        # Hook the larger root under the smaller, then flatten.
        np.minimum.at(lab, np.maximum(la, lb), np.minimum(la, lb))
        while True:
            nl = lab[lab]
            if np.array_equal(nl, lab):
                break
            lab = nl


def _edge_keys(F):
    a = F.ravel()
    b = np.roll(F, -1, axis=1).ravel()
    lo, hi = np.minimum(a, b), np.maximum(a, b)
    return a, b, lo, hi, lo.astype(np.int64) * (int(F.max()) + 1) + hi


def face_pairs(F):
    """Faces that share an edge, as (face a, face b, vertex, vertex)."""
    if not len(F):
        e = np.zeros(0, dtype=np.int64)
        return e, e, e, e
    _a, _b, lo, hi, key = _edge_keys(F)
    face = np.repeat(np.arange(len(F)), 3)
    order = np.argsort(key, kind="stable")
    k = key[order]
    same = np.nonzero(k[1:] == k[:-1])[0]
    return face[order][same], face[order][same + 1], lo[order][same], hi[order][same]


def compact(V, F, T=None):
    """Drops the vertices no face uses."""
    used, inv = np.unique(F, return_inverse=True)
    return V[used], inv.reshape(-1, 3), T


def split_by_plane(V, F, T, point, normal):
    """Splits every face the plane crosses, so each lies on one side of it.
    Returns the new mesh and each vertex's signed distance from the plane."""
    d = (V - point) @ normal
    scale = max(float(np.linalg.norm(V.max(0) - V.min(0))), 1e-9)
    snap = np.abs(d) < 1e-9 * scale
    V = V.copy()
    V[snap] -= np.outer(d[snap], normal)
    d = np.where(snap, 0.0, d)
    s = np.sign(d)[F]
    crossing = np.nonzero((s > 0).any(1) & (s < 0).any(1))[0]
    if not len(crossing):
        return V, F, T, d
    new_v, cut_at = [], {}
    nv = len(V)

    def cut_vertex(a, b):
        key = (a, b) if a < b else (b, a)
        idx = cut_at.get(key)
        if idx is None:
            a, b = key  # the same point from either face
            t = d[a] / (d[a] - d[b])
            idx = nv + len(new_v)
            new_v.append(V[a] + (V[b] - V[a]) * t)
            cut_at[key] = idx
        return idx

    F = F.copy()
    extra_f, extra_t = [], []
    for f, tri, sg in zip(crossing.tolist(), F[crossing].tolist(), s[crossing].tolist()):
        if 0 in sg:
            z = sg.index(0)
            a, b, c = tri[z], tri[(z + 1) % 3], tri[(z + 2) % 3]
            x = cut_vertex(b, c)
            F[f] = (a, b, x)
            extra_f.append((a, x, c))
            extra_t.append(T[f])
        else:
            lone = next(k for k in range(3) if sg[k] != sg[(k + 1) % 3] and sg[k] != sg[(k + 2) % 3])
            a, b, c = tri[lone], tri[(lone + 1) % 3], tri[(lone + 2) % 3]
            x, y = cut_vertex(a, b), cut_vertex(a, c)
            F[f] = (a, x, y)
            extra_f += [(x, b, c), (x, c, y)]
            extra_t += [T[f], T[f]]
    V = np.vstack([V, np.array(new_v)])
    d = np.concatenate([d, np.zeros(len(new_v))])
    F = np.vstack([F, np.array(extra_f, dtype=F.dtype)])
    T = np.concatenate([T, np.array(extra_t, dtype=T.dtype)])
    return V, F, T, d


def flood(F, d, seed, pairs):
    """The faces beyond the plane (d > 0 at a corner) that faces marked in
    seed reach without crossing it, or None if no seed is beyond it. Faces
    the plane crosses need not be split first: two faces on an edge are
    joined beyond the plane when the edge reaches past it."""
    beyond = d[F].max(1) > 0
    seeds = beyond & seed
    if not seeds.any():
        return None
    fa, fb, va, vb = pairs
    da, db = d[va], d[vb]
    link = beyond[fa] & beyond[fb] & ((np.maximum(da, db) > 0) | ((da == 0) & (db == 0)))
    lab = components(len(F), fa[link], fb[link])
    return np.isin(lab, np.unique(lab[seeds]))


def _loops(next_of):
    """Chains {vertex: [next vertices]} into closed loops."""
    loops = []
    for start in list(next_of):
        while next_of.get(start):
            loop = [start]
            cur = next_of[start].pop()
            guard = len(next_of) + 1
            while cur != start and guard:
                guard -= 1
                loop.append(cur)
                nxt = next_of.get(cur)
                if not nxt:
                    break
                cur = nxt.pop()
            if cur == start and len(loop) >= 3:
                loops.append(loop)
    return loops


def plane_holes(V, F, point, normal, eps):
    """The open loops of a mesh that lie in the plane, as outlines."""
    a, b, _lo, _hi, key = _edge_keys(F)
    uniq, inv, count = np.unique(key, return_inverse=True, return_counts=True)
    on = np.abs((V - point) @ normal) <= eps
    bd = (count[inv] == 1) & on[a] & on[b]
    next_of = {}
    for x, y in zip(b[bd].tolist(), a[bd].tolist()):  # the hole runs the other way
        next_of.setdefault(x, []).append(y)
    return [V[loop] for loop in _loops(next_of)]


def cut_off(V, F, T, point, normal, seed_tags, forbidden=None):
    """Cuts a tagged mesh along a plane whose normal points to the side being
    cut off. The part beyond the plane that faces tagged seed_tags reach
    without crossing it comes off. Returns (near, off, outlines): the two
    meshes (each (V, F, T), open where the cut went) and the cut's outlines
    around the part that came off. None when no seed is beyond the plane, or
    forbidden(tags, centres) is true for a face of the part reached, which
    means the plane does not cut it off cleanly."""
    normal = unit(normal)
    V2, F2, T2, d = split_by_plane(V, F, T, point, normal)
    reached = flood(F2, d, np.isin(T2, seed_tags), face_pairs(F2))
    if reached is None:
        return None
    if forbidden is not None and forbidden(T2[reached], V2[F2[reached]].mean(1)).any():
        return None
    near = compact(V2, F2[~reached], T2[~reached])
    off = compact(V2, F2[reached], T2[reached])
    scale = max(float(np.linalg.norm(V.max(0) - V.min(0))), 1e-9)
    outlines = plane_holes(off[0], off[1], point, normal, 1e-7 * scale)
    return near, off, outlines


def slice_loops(V, F, point, normal):
    """The closed outlines where a plane meets a mesh."""
    d = (V - point) @ normal
    d = np.where(d == 0, 1e-12, d)  # on the plane: count it as above
    up = d[F] > 0
    crossing = up.any(1) & ~up.all(1)
    Fc, Uc = F[crossing], up[crossing]
    ends = []
    for k in range(3):
        a, b = Fc[:, k], Fc[:, (k + 1) % 3]
        ends.append((Uc[:, k] != Uc[:, (k + 1) % 3], np.minimum(a, b), np.maximum(a, b)))
    where, links = {}, {}
    for i in range(len(Fc)):
        keys = [(int(lo[i]), int(hi[i])) for cut, lo, hi in ends if cut[i]]
        if len(keys) != 2:
            continue
        for lo, hi in keys:
            if (lo, hi) not in where:
                t = d[lo] / (d[lo] - d[hi])
                where[(lo, hi)] = V[lo] + (V[hi] - V[lo]) * t
        links.setdefault(keys[0], []).append(keys[1])
        links.setdefault(keys[1], []).append(keys[0])
    out, used = [], set()
    for start in links:
        if start in used:
            continue
        loop, prev, cur = [], None, start
        while cur not in used:
            used.add(cur)
            loop.append(where[cur])
            nxt = next((k for k in links[cur] if k != prev and k not in used), None)
            if nxt is None:
                break
            prev, cur = cur, nxt
        if len(loop) >= 3:
            out.append(np.array(loop))
    return out


def outline_area(outline, normal):
    p = outline - outline.mean(0)
    return abs(float(np.cross(p, np.roll(p, -1, axis=0)).sum(0) @ normal)) / 2


# -- Pins --------------------------------------------------------------------

def _inside_polygon(P, poly):
    """Crossing-number test of points P (k, 2) against a polygon (n, 2)."""
    a, b = poly, np.roll(poly, -1, axis=0)
    py = P[:, 1:2]
    straddle = (a[None, :, 1] > py) != (b[None, :, 1] > py)
    dy = np.where(b[:, 1] == a[:, 1], 1e-300, b[:, 1] - a[:, 1])
    x = (b[None, :, 0] - a[None, :, 0]) * (py - a[None, :, 1]) / dy[None, :] + a[None, :, 0]
    return ((straddle & (P[:, 0:1] < x)).sum(1) % 2) == 1


def _outline_distance(P, poly):
    a, b = poly, np.roll(poly, -1, axis=0)
    ab = b - a
    len2 = np.maximum((ab * ab).sum(1), 1e-300)
    t = np.clip(((P[:, None, :] - a[None]) * ab[None]).sum(2) / len2[None], 0.0, 1.0)
    e = a[None] + t[..., None] * ab[None] - P[:, None, :]
    return np.sqrt((e * e).sum(2).min(1))


def _widest_point(poly, split, axis, side):
    """The point of poly farthest from its outline (within the half plane
    side * (p - split) . axis >= 0 when side is not 0), and how far that is."""
    lo, hi = poly.min(0), poly.max(0)

    def score(P):
        r = np.where(_inside_polygon(P, poly), _outline_distance(P, poly), -1.0)
        if side:
            h = ((P - split) @ axis) * side
            r = np.where(h < 0, -1.0, np.minimum(r, h))  # stay clear of the other pin's half
        return r

    best = (lo + hi) / 2
    best_r = float(score(best[None])[0])
    c, half = best.copy(), (hi - lo) / 2
    g = np.linspace(-1.0, 1.0, 17)
    for _round in range(4):
        P = np.stack(np.meshgrid(c[0] + g * half[0], c[1] + g * half[1]), -1).reshape(-1, 2)
        r = score(P)
        i = int(np.argmax(r))
        if r[i] > best_r:
            best_r, best = float(r[i]), P[i]
        c = best.copy()
        half = half / 4
    return best, max(best_r, 0.0)


def pin_spots(outline, normal, min_d, max_d, prefer_two):
    """Where pins go through a cut [(centre, diameter)]: one for a roughly
    round cross-section, two side by side for a long one (or, with
    prefer_two, whenever two fit, however thin), each as thick as the walls
    around it allow."""
    if len(outline) < 3:
        return []
    normal = unit(normal)
    e1, e2 = plane_basis(normal)
    origin = outline[0]
    poly = np.stack([(outline - origin) @ e1, (outline - origin) @ e2], 1)

    def to_model(p):
        return origin + e1 * p[0] + e2 * p[1]

    def diameter_for(room):
        # A pin as thick as leaves a wall around it about a third of the room.
        wall = min(max(0.35 * room, 0.8), 2.0)
        return min(max(2 * (room - wall - PIN_CLEARANCE), min_d), max_d)

    # The outline's long axis, from the spread of its points.
    c = poly.mean(0)
    q = poly - c
    sxx, sxy, syy = (q[:, 0] ** 2).sum(), (q[:, 0] * q[:, 1]).sum(), (q[:, 1] ** 2).sum()
    ang = 0.5 * math.atan2(2 * sxy, sxx - syy)
    major = np.array((math.cos(ang), math.sin(ang)))
    minor = np.array((-major[1], major[0]))
    a, b = q @ major, q @ minor
    one, radius = _widest_point(poly, c, major, 0)
    if prefer_two or (a.max() - a.min()) > 1.7 * (b.max() - b.min()):
        # Long: a pin in each half keeps the piece from turning.
        pa, ra = _widest_point(poly, c, major, 1)
        pb, rb = _widest_point(poly, c, major, -1)
        room = min(ra, rb)
        fits = 2 * (room - min(max(0.35 * room, 0.8), 2.0) - PIN_CLEARANCE) >= min_d
        if fits:
            return [(to_model(pa), diameter_for(room)), (to_model(pb), diameter_for(room))]
    return [(to_model(one), diameter_for(radius))]


def make_pin(diameter, length):
    """A pin lying on one of its flats, along X from x = 0, standing on z = 0:
    four crush ribs that press into the socket wall, chamfered ends that find
    the socket and a flat top and bottom, so it prints lying down without
    support."""
    r = diameter / 2
    flat = 0.84 * r
    chamfer = min(0.4, 0.2 * diameter)
    ramp = max(0.0, min(1.0, (length - 2 * chamfer) / 4))
    N = 96  # a multiple of 8, so the ribs at 45 degrees get a sample
    th = np.arange(N) * (2 * math.pi / N)
    rib_width = max(0.35 / r, 2.2 * 2 * math.pi / N)  # half width, radians
    centres = math.pi / 4 + np.arange(4) * math.pi / 2
    dth = np.abs(np.remainder(th[:, None] - centres[None] + math.pi, 2 * math.pi) - math.pi)
    rib_at = np.maximum(1 - dth / rib_width, 0).max(1)
    xs = (0, chamfer, chamfer + ramp, length - chamfer - ramp, length - chamfer, length)
    shrink = (chamfer, 0, 0, 0, 0, chamfer)
    ribs = (0, 0, 1, 1, 0, 0)
    V = []
    for x, sh, rb in zip(xs, shrink, ribs):
        rad = r - sh + PIN_RIB * rb * rib_at
        V.append(np.stack([np.full(N, x), rad * np.cos(th), np.clip(rad * np.sin(th), -flat, flat) + flat], 1))
    V = np.vstack(V + [np.array(((0, 0, flat), (length, 0, flat)))])
    j = np.arange(N)
    F = []
    for ring in range(5):
        a, b = ring * N + j, ring * N + (j + 1) % N
        F += [np.stack([a, b, b + N], 1), np.stack([a, b + N, a + N], 1)]
    c0, c1 = 6 * N, 6 * N + 1
    F += [np.stack([np.full(N, c0), (j + 1) % N, j], 1),
          np.stack([np.full(N, c1), 5 * N + j, 5 * N + (j + 1) % N], 1)]
    return V, np.vstack(F)


def socket_mesh(mouth, axis, radius, depth, segments=48):
    """A blind hole for a pin, as a solid to cut away: a bore from mouth
    along axis, chamfered at the mouth and ending in a cone that prints
    without support, running on 0.8 past the mouth so it opens cleanly
    through the joint face."""
    axis = unit(axis)
    e1, e2 = plane_basis(axis)
    chamfer = min(0.3, 0.25 * radius)
    profile = ((-0.8, radius + chamfer), (0.0, radius + chamfer), (chamfer, radius), (depth, radius))
    th = np.arange(segments) * (2 * math.pi / segments)
    ring = np.cos(th)[:, None] * e1 + np.sin(th)[:, None] * e2
    V = [mouth + axis * t + ring * r for t, r in profile]
    V = np.vstack(V + [np.array([mouth - axis * 0.8, mouth + axis * (depth + radius)])])
    j = np.arange(segments)
    n = segments
    F = []
    for k in range(len(profile) - 1):
        a, b = k * n + j, k * n + (j + 1) % n
        F += [np.stack([a, b, b + n], 1), np.stack([a, b + n, a + n], 1)]
    start, tip = len(profile) * n, len(profile) * n + 1
    last = (len(profile) - 1) * n
    F += [np.stack([np.full(n, start), (j + 1) % n, j], 1),
          np.stack([np.full(n, tip), last + j, last + (j + 1) % n], 1)]
    return V, np.vstack(F)


def cylinder_mesh(centre, axis, radius, half_height, segments=64):
    """A closed cylinder around centre, half_height either way along axis."""
    axis = unit(axis)
    e1, e2 = plane_basis(axis)
    th = np.arange(segments) * (2 * math.pi / segments)
    ring = (np.cos(th)[:, None] * e1 + np.sin(th)[:, None] * e2) * radius
    V = np.vstack([centre - axis * half_height + ring, centre + axis * half_height + ring,
                   [centre - axis * half_height, centre + axis * half_height]])
    n, j = segments, np.arange(segments)
    F = [np.stack([j, (j + 1) % n, (j + 1) % n + n], 1), np.stack([j, (j + 1) % n + n, j + n], 1),
         np.stack([np.full(n, 2 * n), (j + 1) % n, j], 1), np.stack([np.full(n, 2 * n + 1), j + n, (j + 1) % n + n], 1)]
    return V, np.vstack(F)


def offset_polygon(pts, delta):
    """A closed polygon (n, 2) grown outward by delta, counterclockwise."""
    p = np.asarray(pts, dtype=np.float64)
    q = np.roll(p, -1, axis=0)
    if (p[:, 0] * q[:, 1] - q[:, 0] * p[:, 1]).sum() < 0:
        p = p[::-1]
    e_in = p - np.roll(p, 1, axis=0)
    e_out = np.roll(p, -1, axis=0) - p
    n_in = np.stack([e_in[:, 1], -e_in[:, 0]], 1) / np.maximum(np.linalg.norm(e_in, axis=1), 1e-12)[:, None]
    n_out = np.stack([e_out[:, 1], -e_out[:, 0]], 1) / np.maximum(np.linalg.norm(e_out, axis=1), 1e-12)[:, None]
    m = n_in + n_out
    m /= np.maximum(np.linalg.norm(m, axis=1), 1e-12)[:, None]
    return p + m * (delta / np.maximum((m * n_in).sum(1), 0.3))[:, None]


def prism_mesh(pts, z0, z1):
    """An upright prism: a counterclockwise outline (n, 2) carried from z0 up
    to z1. Faces are lists, the two ends one polygon each."""
    n = len(pts)
    V = np.vstack([np.c_[pts, np.full(n, z0)], np.c_[pts, np.full(n, z1)]])
    F = [[i, (i + 1) % n, (i + 1) % n + n, i + n] for i in range(n)]
    F.append(list(range(n - 1, -1, -1)))
    F.append(list(range(n, 2 * n)))
    return V, F


def lay_out(pieces, gap=6.0, width=180.0):
    """Moves pieces [(V, F)] side by side on the bed, each standing on z = 0,
    in rows no wider than width, deepest first, all centred on the origin."""
    lo = [V.min(0) for V, _F in pieces]
    hi = [V.max(0) for V, _F in pieces]
    order = sorted(range(len(pieces)), key=lambda i: -(hi[i][1] - lo[i][1]))
    shifts = [None] * len(pieces)
    x = y = row_depth = 0.0
    for i in order:
        w, dpt = hi[i][0] - lo[i][0], hi[i][1] - lo[i][1]
        if x > 0 and x + w > width:
            x, y, row_depth = 0.0, y + row_depth + gap, 0.0
        shifts[i] = np.array((x - lo[i][0], y - lo[i][1], -lo[i][2]))
        x += w + gap
        row_depth = max(row_depth, dpt)
    a = np.min([lo[i] + shifts[i] for i in order], axis=0)
    b = np.max([hi[i] + shifts[i] for i in order], axis=0)
    mid = np.array(((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, 0.0))
    return [(V + shifts[i] - mid, F) for i, (V, F) in enumerate(pieces)]


# -- Where to cut --------------------------------------------------------------

def find_arm_cut(V, F, T, side, joints, unit_size, raycaster, log=print):
    """How far down the upper arm to cut it off the body, square to it: past
    where it comes free (the first cut going out from the shoulder that takes
    nothing far from the arm's bones), at the cut that leaves room for the
    thickest pin as deep as it should go in both the shoulder and the arm.
    raycaster(V, F) gives a function (origin, direction, distance) that finds
    how far that mesh's surface is (or None). Returns (point, axis,
    forbidden) for cut_off."""
    tag, other = (TAG_ARM_R, TAG_ARM_L) if side else (TAG_ARM_L, TAG_ARM_R)
    s = "r" if side else "l"
    shoulder, elbow, wrist = joints[f"arm_{s}1"], joints[f"arm_{s}2"], joints[f"wrist_{s}"]
    axis = unit(elbow - shoulder)
    chain = [(shoulder, elbow), (elbow, wrist)]
    arm_verts = np.unique(F[T == tag])
    if not len(arm_verts):
        raise RuntimeError("split: the body has no arm")
    # The arm's own faces are within its thickness of its bones (the hand a
    # little past the wrist); a cut that leaks takes body far beyond.
    thick = float(bone_distance(V[arm_verts], chain).max())
    reach = max(0.12 * unit_size, 1.5 * thick)

    def forbidden(tags, centres):
        return np.isin(tags, (TAG_LEGS, TAG_NECK, other)) | (bone_distance(centres, chain) > reach)

    # Only faces near the arm can be reached without reaching one too far
    # away first, so the search floods just those.
    centres = V[F].mean(1)
    near = bone_distance(centres, chain) < reach + 0.03 * unit_size
    Fr, Tr = F[near], T[near]
    bad = forbidden(Tr, centres[near])
    pairs = face_pairs(Fr)
    seed = Tr == tag

    def attempt(at):
        point = shoulder + axis * at
        reached = flood(Fr, (V - point) @ axis, seed, pairs)
        if reached is None or (reached & bad).any():
            return None
        return point, reached

    free, at = None, 0.02 * unit_size
    while at < 0.25 * unit_size and free is None:
        if attempt(at):
            free = at
        at += 0.005 * unit_size
    if free is None:
        raise RuntimeError("split: could not cut off an arm")

    # The pin runs up the arm into the shoulder. A little further down the
    # arm the shoulder may hold a thicker one; take the cut that takes the
    # thickest pin as deep as it should go.
    e1, e2 = plane_basis(axis)

    # Into the shoulder, the body as it is (an arm cut off its far side is
    # out of the way); into the arm, the arm alone, as dress or skirt may
    # sit inside it.
    torso = raycaster(V, F)

    def room_along(raycast, mouth, direction, r):
        def depth(o):
            t = raycast(o + direction * 0.01, direction, 50.0)
            return 50.0 if t is None else t
        wall = wall_for(r)
        room = depth(mouth) - r - wall
        for q in range(8):
            ring = e1 * math.cos(q * math.pi / 4) + e2 * math.sin(q * math.pi / 4)
            room = min(room, depth(mouth + ring * (r + wall)) - wall)
        return room

    best_score, best_at = -math.inf, None
    for k in range(13):
        at = free + 0.5 * k
        res = attempt(at)
        if not res:
            continue
        point, reached = res
        loops = slice_loops(V, Fr[reached], point, axis)
        if not loops:
            continue
        outline = max(loops, key=lambda o: outline_area(o, axis))
        centre, diameter = pin_spots(outline, axis, 1.6, 5.0, False)[0]
        arm = raycaster(*compact(V, Fr[reached])[:2])
        # The thickest pin both sides take deep enough; failing that, how
        # close the thinnest comes.
        score, r = -1.0, diameter / 2 + PIN_CLEARANCE
        while r >= 0.8 + PIN_CLEARANCE - 1e-9:
            room = min(room_along(torso, centre, -axis, r), room_along(arm, centre, axis, r))
            want = good_depth(r)
            if room >= want:
                score = r
                break
            score = max(score, room / want - 1)
            r -= 0.1
        if score > best_score + 0.05:
            best_score, best_at = score, at
    if best_at is None:
        raise RuntimeError("split: could not cut off an arm")
    log(f"{'Right' if side else 'Left'} shoulder: cut {best_at:.2f} down the arm")
    return shoulder + axis * best_at, axis, forbidden
