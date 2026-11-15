"""
Poses and scales a Mii body GLB the way Nintendo's editor body does, without
Blender, so the result matches FFL.js bone for bone.

The body GLBs carry the Wii U editor skeleton (skl_root, chest, head, arms,
legs) and its animation clips. Nintendo scales that skeleton per bone from
the Mii's height and build:

  - nn::mii GetBodyScale turns height and build into a scale vector.
  - nn::mii UpdateScale picks, per bone, the whole vector, the vector with X
    and Y swapped (arms, which point sideways), X alone (joints that should
    stay round) or the vector with Y held at 1 or more (the neck).
  - Each scaled bone only moves its children, it never scales them, and the
    bones under skl_root shift so the feet stay on the ground.

FFL.js (helpers/ModelScaleDesc.js and helpers/SkeletonScalingExtensions.js)
reproduces this on three.js skeletons. This module follows FFL.js exactly,
so tools/reference can check it against FFL.js's own render.

Everything here is numpy and the standard library: it runs inside Blender's
Python, in a plain interpreter, and it maps one to one onto the C++ port.
"""
import json
import struct

import numpy as np

# The head is a seventh of the body's world scale (FFL.js headToBodyScale 10/7, times 0.1).
HEAD_TO_BODY = 7.0

# Wii U editor body (MiiBodyMiddle.bfres) bones, from FFL.js editorBodyScaleDesc.
ROOT_BONE = "skl_root"
HEAD_BONE = "head"
YXZ_BONES = {"arm_l1", "arm_l2", "arm_r1", "arm_r2", "elbow_l", "elbow_r"}
SCALAR_BONES = {"wrist_l", "wrist_r", "shoulder_l", "shoulder_r",
                "ankle_l", "ankle_r", "knee_l", "knee_r"}
Y_MIN_1_BONES = {"head"}
UNSCALED_BONES = {"all_root", "body"}

# Limb bones and the bone each one points at. Hands and feet end at a bone
# with nothing after it, and are held to a sphere around it instead.
LIMB_SEGMENTS = {
    "arm_l1": "arm_l2", "arm_l2": "wrist_l", "elbow_l": "wrist_l",
    "arm_r1": "arm_r2", "arm_r2": "wrist_r", "elbow_r": "wrist_r",
    "foot_l1": "foot_l2", "foot_l2": "ankle_l", "knee_l": "ankle_l",
    "foot_r1": "foot_r2", "foot_r2": "ankle_r", "knee_r": "ankle_r",
}
LIMB_ENDS = {"wrist_l", "wrist_r", "ankle_l", "ankle_r"}

_COMPONENTS = {
    5120: (np.int8, 127.0),
    5121: (np.uint8, 255.0),
    5122: (np.int16, 32767.0),
    5123: (np.uint16, 65535.0),
    5125: (np.uint32, None),
    5126: (np.float32, None),
}
_WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def body_scale(height, build):
    """nn::mii GetBodyScale (Mii Maker's FUN_020737b8): the scale vector for
    a height and build of 0 to 127. Z always follows X."""
    x = (build * (height * (0.47 / 128) + 0.4)) / 128 + height * (0.23 / 128) + 0.4
    y = height * (0.77 / 128) + 0.5
    return np.array((x, y, x))


# ---------------------------------------------------------------------------
# GLB reading
# ---------------------------------------------------------------------------

class GLB:
    def __init__(self, path):
        with open(path, "rb") as fh:
            data = fh.read()
        magic, _version, _length = struct.unpack_from("<4sII", data, 0)
        if magic != b"glTF":
            raise ValueError(f"{path} is not a GLB file")
        at = 12
        self.json = None
        self.bin = b""
        while at < len(data):
            size, kind = struct.unpack_from("<I4s", data, at)
            chunk = data[at + 8:at + 8 + size]
            if kind == b"JSON":
                self.json = json.loads(chunk)
            elif kind == b"BIN\x00":
                self.bin = chunk
            at += 8 + size

    def accessor(self, index):
        """An accessor as float64 (normalized integers mapped to -1..1 or
        0..1, as glTF says), or as integers when it is not normalized."""
        acc = self.json["accessors"][index]
        dtype, norm = _COMPONENTS[acc["componentType"]]
        width = _WIDTH[acc["type"]]
        count = acc["count"]
        view = self.json["bufferViews"][acc["bufferView"]]
        start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        item = np.dtype(dtype).itemsize * width
        stride = view.get("byteStride") or item
        raw = np.frombuffer(self.bin, dtype=np.uint8, count=stride * (count - 1) + item, offset=start)
        rows = np.lib.stride_tricks.as_strided(raw, shape=(count, item), strides=(stride, 1))
        values = np.ascontiguousarray(rows).view(dtype).reshape(count, width)
        if acc.get("normalized") and norm:
            return np.maximum(values.astype(np.float64) / norm, -1.0)
        if norm is None:
            return values.astype(np.float64)
        return values


def _quat_matrix(q):
    x, y, z, w = q
    n = x * x + y * y + z * z + w * w
    s = 2.0 / n if n else 0.0
    return np.array((
        (1 - s * (y * y + z * z), s * (x * y - z * w), s * (x * z + y * w)),
        (s * (x * y + z * w), 1 - s * (x * x + z * z), s * (y * z - x * w)),
        (s * (x * z - y * w), s * (y * z + x * w), 1 - s * (x * x + y * y)),
    ))


def _trs(t, r, s):
    m = np.eye(4)
    m[:3, :3] = _quat_matrix(r) * np.asarray(s)[None, :]
    m[:3, 3] = t
    return m


# ---------------------------------------------------------------------------
# The posed, scaled body
# ---------------------------------------------------------------------------

class Body:
    """A body GLB posed with one of its clips and scaled for a Mii.

    parts:  [(name, vertices (n, 3), triangles (m, 3))] in the GLB's world
            space (Y up), skinned with the scaled skeleton.
    head:   the head's world matrix: where FFL.js attaches the head model,
            with the head's own scale (a seventh of the body's).
    """

    def __init__(self, path, height=64, build=64, pose="Finish"):
        glb = GLB(path)
        j = glb.json
        nodes = j["nodes"]
        self.names = [n.get("name", "") for n in nodes]
        parent = [-1] * len(nodes)
        for i, n in enumerate(nodes):
            for c in n.get("children", []):
                parent[c] = i
        self.parent = parent

        trs = [[np.array(n.get("translation", (0.0, 0.0, 0.0)), dtype=np.float64),
                np.array(n.get("rotation", (0.0, 0.0, 0.0, 1.0)), dtype=np.float64),
                np.array(n.get("scale", (1.0, 1.0, 1.0)), dtype=np.float64)] for n in nodes]
        if pose:
            self._apply_pose(glb, trs, pose)
        local = [_trs(*t) for t in trs]

        world = [None] * len(nodes)

        def world_of(i):
            if world[i] is None:
                world[i] = local[i] if parent[i] < 0 else world_of(parent[i]) @ local[i]
            return world[i]

        for i in range(len(nodes)):
            world_of(i)

        scale = body_scale(height, build)
        self.scale = scale
        skins = j["skins"]
        joints = skins[0]["joints"]
        scaled_world = self._scale_skeleton(joints, world, local, scale)
        self.bone_world = scaled_world

        head = joints.index(self.names.index(HEAD_BONE))
        head_world = scaled_world[head]
        rot = head_world[:3, :3] / np.linalg.norm(head_world[:3, :3], axis=0)[None, :]
        self.head = np.eye(4)
        self.head[:3, :3] = rot / HEAD_TO_BODY
        self.head[:3, 3] = head_world[:3, 3]

        self.joint_names = [self.names[node] for node in joints]
        self.parts = []
        self.dominant = []  # per part, the joint each vertex follows most
        for i, n in enumerate(nodes):
            if "mesh" not in n or "skin" not in n:
                continue
            skin = skins[n["skin"]]
            if skin["joints"] != joints:
                raise ValueError("body meshes use different skeletons")
            # Each mesh is quantized on its own, so each skin has its own
            # inverse bind matrices, which also undo that quantization.
            ibm = glb.accessor(skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
            bone_mats = scaled_world @ ibm
            for prim in j["meshes"][n["mesh"]]["primitives"]:
                attrs = prim["attributes"]
                pos = glb.accessor(attrs["POSITION"])
                ji = glb.accessor(attrs["JOINTS_0"]).astype(np.int64)
                w = glb.accessor(attrs["WEIGHTS_0"])
                w = w / np.maximum(w.sum(axis=1, keepdims=True), 1e-12)
                tris = glb.accessor(prim["indices"]).astype(np.int64).reshape(-1, 3)
                ph = np.c_[pos, np.ones(len(pos))]
                out = np.zeros((len(pos), 3))
                for k in range(ji.shape[1]):
                    m = bone_mats[ji[:, k]]
                    out += w[:, k:k + 1] * np.einsum("nij,nj->ni", m, ph)[:, :3]
                self.parts.append((n.get("name", f"node{i}"), out, tris))
                self.dominant.append(ji[np.arange(len(ji)), w.argmax(axis=1)])

    def thicken_limbs(self, min_radius):
        """Push limb surface out to at least min_radius (body units) from the
        bone it hangs on: a capsule along each arm and leg bone, a sphere
        around each wrist and ankle. A short, slight Mii scales its limbs to
        under half their usual thickness while its head stays the same size;
        this holds them at a printable minimum. Limbs already thicker are not
        touched, so lengths and the rest of the silhouette stay exact.
        Returns how many vertices moved."""
        index = {name: k for k, name in enumerate(self.joint_names)}
        joint_pos = self.bone_world[:, :3, 3]
        moved = 0
        for (_name, verts, _tris), dom in zip(self.parts, self.dominant):
            for name, k in index.items():
                if name not in LIMB_SEGMENTS and name not in LIMB_ENDS:
                    continue
                sel = np.nonzero(dom == k)[0]
                if not len(sel):
                    continue
                a = joint_pos[k]
                p = verts[sel] - a
                if name in LIMB_SEGMENTS:
                    d = joint_pos[index[LIMB_SEGMENTS[name]]] - a
                    length = float(np.linalg.norm(d))
                    if length < 1e-9:
                        continue
                    d = d / length
                    t = np.clip(p @ d, 0.0, length)
                    axis = np.outer(t, d)
                else:
                    axis = np.zeros_like(p)
                off = p - axis
                r = np.linalg.norm(off, axis=1)
                thin = (r < min_radius) & (r > 1e-9)
                if thin.any():
                    verts[sel[thin]] = a + axis[thin] + off[thin] * (min_radius / r[thin])[:, None]
                    moved += int(thin.sum())
        return moved

    @staticmethod
    def _apply_pose(glb, trs, pose):
        """The first frame of the clip, as three.js's AnimationMixer.update(0)
        and Blender's glTF import (frame 1) both leave it."""
        clips = glb.json.get("animations", [])
        clip = next((a for a in clips if a.get("name") == pose), None)
        if clip is None:
            raise ValueError(f"no animation named {pose!r}")
        slot = {"translation": 0, "rotation": 1, "scale": 2}
        for ch in clip["channels"]:
            path = ch["target"].get("path")
            if path not in slot or "node" not in ch["target"]:
                continue
            sampler = clip["samplers"][ch["sampler"]]
            value = glb.accessor(sampler["output"])
            if sampler.get("interpolation") == "CUBICSPLINE":
                value = value[1::3]  # skip the tangents around each keyframe
            trs[ch["target"]["node"]][slot[path]] = value[0].astype(np.float64)

    def _scale_skeleton(self, joints, world, local, scale):
        """FFL.js's Skeleton.update override, bone by bone in skin order
        (parents always come first). Returns each joint's scaled world matrix."""
        names = self.names
        world = [w.copy() for w in world]
        children = {}
        for i, p in enumerate(self.parent):
            children.setdefault(p, []).append(i)
        sx, sy, sz = scale

        scaled_world = np.empty((len(joints), 4, 4))
        for b, node in enumerate(joints):
            name = names[node]
            m = world[node]
            if name not in UNSCALED_BONES:
                if name in YXZ_BONES:
                    s = (sy, sx, sz)
                elif name in SCALAR_BONES:
                    s = (sx, sx, sx)
                elif name in Y_MIN_1_BONES:
                    s = (sx, max(sy, 1.0), sz)
                else:
                    s = (sx, sy, sz)
                m = m @ np.diag((*s, 1.0))
                if names[self.parent[node]] == ROOT_BONE:
                    # Bones right under the root spread with the height and
                    # drop so the legs stay planted (world scale 1).
                    t = m[:3, 3]
                    m[:3, 3] = (t[0] * sy, t[1] * sy + (sx - sy), t[2] * sx)
                for c in children.get(node, []):
                    world[c][:3, 3] = (m @ local[c])[:3, 3]
            scaled_world[b] = m
        return scaled_world

    def in_head_frame(self, upright=True):
        """The body parts around the head model's origin, in the head's own
        units (the head GLB's), Y up.

        upright=True keeps the body standing as it is and only moves it under
        the head, leaving the head unrotated. upright=False puts it in the
        head's frame exactly, so the body tilts by however the pose turns the
        head bone (FFL.js renders it that way)."""
        if upright:
            to_head = np.eye(4) * HEAD_TO_BODY
            to_head[3, 3] = 1.0
            to_head[:3, 3] = -self.head[:3, 3] * HEAD_TO_BODY
        else:
            to_head = np.linalg.inv(self.head)
        out = []
        for name, verts, tris in self.parts:
            v = (np.c_[verts, np.ones(len(verts))] @ to_head.T)[:, :3]
            out.append((name, v, tris))
        return out

    def head_rotation(self):
        """The rotation the pose gives the head bone (Y up). FFL.js turns the
        head model with it."""
        return self.head[:3, :3] * HEAD_TO_BODY

    def head_tilt(self):
        """How far the pose turns the head bone from upright, in degrees."""
        rot = self.head_rotation()
        return float(np.degrees(np.arccos(np.clip((np.trace(rot) - 1) / 2, -1.0, 1.0))))


def write_stl(path, parts, z_up=True):
    """Binary STL of [(name, verts, tris)]; z_up turns glTF's Y up into Z up."""
    tri = np.concatenate([v[t] for _n, v, t in parts]).astype(np.float64)
    if z_up:
        tri = np.stack([tri[..., 0], -tri[..., 2], tri[..., 1]], axis=-1)
    normal = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)
    rec = np.zeros(len(tri), dtype=[("n", "<f4", 3), ("v", "<f4", (3, 3)), ("a", "<u2")])
    rec["n"] = normal
    rec["v"] = tri
    with open(path, "wb") as fh:
        fh.write(b"PrintMiiOut body".ljust(80, b"\0"))
        fh.write(struct.pack("<I", len(tri)))
        fh.write(rec.tobytes())


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description="Pose and scale a Mii body GLB to an STL, in head units.")
    ap.add_argument("glb")
    ap.add_argument("out")
    ap.add_argument("--height", type=int, default=64)
    ap.add_argument("--build", type=int, default=64)
    ap.add_argument("--pose", default="Finish")
    ap.add_argument("--tilt", action="store_true", help="put the body in the tilted head's frame, as FFL.js does")
    a = ap.parse_args()
    body = Body(a.glb, a.height, a.build, a.pose)
    write_stl(a.out, body.in_head_frame(upright=not a.tilt))
    print(f"scale {np.round(body.scale, 4).tolist()} head at {np.round(body.head[:3, 3], 3).tolist()} "
          f"tilted {body.head_tilt():.2f} deg")
