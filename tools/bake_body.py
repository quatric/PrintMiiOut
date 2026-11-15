"""
Re-saves a Mii body GLB in a form any glTF importer reads, for the
in-browser generator (core/), whose importer (assimp) has no support for
KHR_mesh_quantization.

  python tools/bake_body.py miiBodyM_wiiu_plain.glb static/client/miiBodyM.glb

The output keeps the skeleton (node names, hierarchy, inverse bind
matrices) and the skinned meshes, with:
  - positions and weights as plain floats instead of quantized integers,
  - the Finish clip's first frame written into the node transforms, so
    the pose needs no animation, and no animations at all.
The body is then scaled per Mii at runtime, exactly as miibody.py does.
"""
import json
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import miibody  # noqa: E402


def bake(src, dst, pose="Finish"):
    glb = miibody.GLB(src)
    j = glb.json
    nodes = j["nodes"]
    trs = [[np.array(n.get("translation", (0.0, 0.0, 0.0)), dtype=np.float64),
            np.array(n.get("rotation", (0.0, 0.0, 0.0, 1.0)), dtype=np.float64),
            np.array(n.get("scale", (1.0, 1.0, 1.0)), dtype=np.float64)] for n in nodes]
    miibody.Body._apply_pose(glb, trs, pose)

    blob = bytearray()
    views, accessors = [], []

    def add(array, target=None, kind=None, component=None, minmax=False):
        array = np.ascontiguousarray(array)
        while len(blob) % 4:
            blob.append(0)
        view = {"buffer": 0, "byteOffset": len(blob), "byteLength": array.nbytes}
        if target:
            view["target"] = target
        blob.extend(array.tobytes())
        views.append(view)
        acc = {"bufferView": len(views) - 1, "componentType": component,
               "count": int(array.shape[0]), "type": kind}
        if minmax:
            acc["min"] = array.min(axis=0).tolist()
            acc["max"] = array.max(axis=0).tolist()
        accessors.append(acc)
        return len(accessors) - 1

    out_nodes = []
    for i, n in enumerate(nodes):
        node = {"name": n.get("name", f"node{i}")}
        t, r, s = trs[i]
        if np.any(t != 0):
            node["translation"] = t.tolist()
        if np.any(r != (0.0, 0.0, 0.0, 1.0)):
            node["rotation"] = (r / np.linalg.norm(r)).tolist()
        if np.any(s != 1):
            node["scale"] = s.tolist()
        if "children" in n:
            node["children"] = n["children"]
        out_nodes.append(node)

    out_skins = []
    for skin in j["skins"]:
        ibm = glb.accessor(skin["inverseBindMatrices"]).astype(np.float32)
        out_skins.append({"joints": skin["joints"],
                          "inverseBindMatrices": add(ibm, kind="MAT4", component=5126)})

    out_meshes = []
    for i, n in enumerate(nodes):
        if "mesh" not in n:
            continue
        prims = []
        for prim in j["meshes"][n["mesh"]]["primitives"]:
            a = prim["attributes"]
            pos = glb.accessor(a["POSITION"]).astype(np.float32)
            joints = glb.accessor(a["JOINTS_0"]).astype(np.uint16)
            weights = glb.accessor(a["WEIGHTS_0"]).astype(np.float32)
            weights /= np.maximum(weights.sum(axis=1, keepdims=True), 1e-12)
            index = glb.accessor(prim["indices"]).astype(np.uint16).reshape(-1, 1)
            prims.append({
                "attributes": {
                    "POSITION": add(pos, 34962, "VEC3", 5126, minmax=True),
                    "JOINTS_0": add(joints, 34962, "VEC4", 5123),
                    "WEIGHTS_0": add(weights, 34962, "VEC4", 5126),
                },
                "indices": add(index, 34963, "SCALAR", 5123),
                "mode": 4,
            })
        out_meshes.append({"name": n.get("name", f"mesh{i}"), "primitives": prims})
        out_nodes[i]["mesh"] = len(out_meshes) - 1
        out_nodes[i]["skin"] = n["skin"]

    while len(blob) % 4:
        blob.append(0)
    doc = {
        "asset": {"version": "2.0", "generator": "PrintMiiOut tools/bake_body.py"},
        "scene": 0,
        "scenes": j["scenes"],
        "nodes": out_nodes,
        "meshes": out_meshes,
        "skins": out_skins,
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob)}],
    }
    text = json.dumps(doc, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    with open(dst, "wb") as fh:
        fh.write(struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(text) + 8 + len(blob)))
        fh.write(struct.pack("<I4s", len(text), b"JSON") + text)
        fh.write(struct.pack("<I4s", len(blob), b"BIN\x00") + bytes(blob))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    bake(sys.argv[1], sys.argv[2])
