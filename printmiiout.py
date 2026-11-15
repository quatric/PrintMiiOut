import sys
import os
import subprocess
import shutil
import math
import time

try:
    import bpy
except ModuleNotFoundError:
    blender_exe = shutil.which("blender") or "/opt/homebrew/bin/blender"
    if not os.path.exists(blender_exe):
        blender_exe = "/Applications/Blender.app/Contents/MacOS/Blender"
    if not os.path.exists(blender_exe):
        print("Error: Blender executable not found.", file=sys.stderr)
        sys.exit(1)
    cmd = [blender_exe, "-b", "--factory-startup", "--python", os.path.abspath(__file__), "--"] + sys.argv[1:]
    sys.exit(subprocess.call(cmd))

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# Mask texture noise leaves islands under ~0.35 GLB units²; real features are 1+.
SPECK_AREA = 0.5

_T0 = time.time()
_T = [_T0]


def _tick(label):
    """Log how long the stage that just ended took."""
    now = time.time()
    print(f"[{now - _T0:6.2f}s] {label}: {now - _T[0]:.2f}s")
    _T[0] = now


def _apply(obj, mod):
    bpy.context.view_layer.objects.active = obj
    try:
        bpy.ops.object.modifier_apply(modifier=mod.name)
    except Exception as e:
        print(f"Warning applying {mod.name} to {obj.name}: {e}")


def _subsurf(obj, levels, name="Subsurf", preserve_corners=True):
    mod = obj.modifiers.new(name=name, type='SUBSURF')
    mod.levels = levels
    mod.subdivision_type = 'CATMULL_CLARK'
    if preserve_corners and hasattr(mod, "boundary_smooth"):
        mod.boundary_smooth = 'PRESERVE_CORNERS'
    return mod


def _coords(mesh):
    import numpy as np
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def _loop_uvs(mesh):
    import numpy as np
    layer = mesh.uv_layers.active
    if layer is None:
        return None
    uv = np.empty(len(mesh.loops) * 2, dtype=np.float32)
    try:
        layer.uv.foreach_get("vector", uv)
    except AttributeError:
        layer.data.foreach_get("uv", uv)
    return uv.reshape(-1, 2).astype(np.float64)


def _material_image(obj):
    for mat in obj.data.materials:
        if mat and mat.node_tree:
            for node in mat.node_tree.nodes:
                if node.type == 'TEX_IMAGE' and node.image:
                    return node.image
    return None


def _image_pixels(img):
    import numpy as np
    width, height = img.size
    px = np.empty(width * height * img.channels, dtype=np.float32)
    img.pixels.foreach_get(px)
    return px.reshape((height, width, img.channels))


def _speck_faces(bm, min_area):
    seen = set()
    specks = []
    for start in bm.faces:
        if start in seen:
            continue
        seen.add(start)
        island = [start]
        for face in island:
            for edge in face.edges:
                for f in edge.link_faces:
                    if f not in seen:
                        seen.add(f)
                        island.append(f)
        if sum(f.calc_area() for f in island) < min_area:
            specks.extend(island)
    return specks


# How the face texture is embossed (core/src/head.cpp does the same). Ink
# (the brightest channel this dark or darker: black and grey lines, dark
# outlines) stands at full height. Colour (hearts, coins, tongues, a coloured
# iris or eyebrow) stands at half height, under the ink.
EMBOSS_INK = 0.5
EMBOSS_COLOUR = 0.2
# Share of a feature's area that must be colour before it gets a plateau, so a
# few tinted edge pixels never lift a black and white feature.
EMBOSS_TONE_SHARE = 0.1
TONE_THICKNESS, TONE_OFFSET = 1.25, 0.2   # 0.75 proud, 0.5 sunk


def _face_islands(face_verts, totals, vertex_count):
    """Island index of each face, faces joining where they share a vertex."""
    import numpy as np
    parent = list(range(vertex_count))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    starts = np.concatenate(([0], np.cumsum(totals)[:-1]))
    fv = face_verts.tolist()
    for s, t in zip(starts.tolist(), totals.tolist()):
        root = find(fv[s])
        for k in range(s + 1, s + t):
            r = find(fv[k])
            if r != root:
                parent[r] = root
    roots = np.array([find(fv[s]) for s in starts.tolist()], dtype=np.int64)
    _, island = np.unique(roots, return_inverse=True)
    return island


def _mesh_from_faces(mesh, keep, loop_verts, totals, name):
    """A new mesh of mesh's faces where keep is set, without loose specks."""
    import bmesh
    import numpy as np
    kept_loops = loop_verts[np.repeat(keep, totals)]
    used, remap = np.unique(kept_loops, return_inverse=True)
    kept_totals = totals[keep]
    co = _coords(mesh)[used]
    if len(kept_totals) and (kept_totals == kept_totals[0]).all():
        faces = remap.reshape(-1, int(kept_totals[0])).tolist()
    else:
        faces = [f.tolist() for f in np.split(remap, np.cumsum(kept_totals)[:-1])]
    out = bpy.data.meshes.new(name)
    out.from_pydata(co.tolist(), [], faces)
    bm = bmesh.new()
    bm.from_mesh(out)
    specks = _speck_faces(bm, SPECK_AREA)
    if specks:
        print(f"Removing {len(specks)} speck faces from {name}")
        bmesh.ops.delete(bm, geom=specks, context='FACES')
    bm.to_mesh(out)
    bm.free()
    return out


def _cull_by_texture(obj, img):
    """Keep only the faces the texture paints as ink, and return a second
    object holding the coloured plateaus (or None). The subdivided mask is
    ~800k faces and most are transparent, so this samples every face at once
    and rebuilds the meshes from the few that stay."""
    import numpy as np

    mesh = obj.data
    uv = _loop_uvs(mesh)
    if uv is None or not len(mesh.polygons):
        return None
    px = _image_pixels(img)
    height, width = px.shape[:2]
    x = (uv[:, 0] * width).astype(np.int64) % width
    y = (uv[:, 1] * height).astype(np.int64) % height
    texel = px[y, x]

    starts = np.empty(len(mesh.polygons), dtype=np.int32)
    totals = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", starts)
    mesh.polygons.foreach_get("loop_total", totals)
    max_alpha = np.maximum.reduceat(texel[:, 3], starts)
    rgb = np.add.reduceat(texel[:, :3], starts, axis=0) / totals[:, None]
    value = rgb.max(axis=1)
    saturation = value - rgb.min(axis=1)
    painted = max_alpha >= 0.1
    ink = painted & (value <= EMBOSS_INK)
    colour = painted & ~ink & (saturation >= EMBOSS_COLOUR)
    light = painted & ~ink & ~colour   # eye whites, teeth, light grey edges
    print(f"Removing {len(ink) - int(ink.sum())} / {len(ink)} transparent faces from {obj.name}")

    loop_verts = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", loop_verts)

    # Which features get a plateau: those that are partly colour, and those
    # painted entirely in white or grey (light eyebrows), which would
    # otherwise vanish. Black and white features (eyes with their whites)
    # get none, so they print exactly as they always have.
    tone = np.zeros(len(ink), dtype=bool)
    if colour.any() or light.any():
        idx = np.nonzero(painted)[0]
        sel = np.repeat(painted, totals)
        island = _face_islands(loop_verts[sel], totals[idx], len(mesh.vertices))
        area = np.empty(len(mesh.polygons), dtype=np.float32)
        mesh.polygons.foreach_get("area", area)
        count = int(island.max()) + 1
        a = area[idx].astype(np.float64)
        total = np.bincount(island, weights=a, minlength=count)
        coloured = np.bincount(island, weights=a * colour[idx], minlength=count)
        inked = np.bincount(island, weights=a * ink[idx], minlength=count)
        plateau = coloured >= EMBOSS_TONE_SHARE * total
        bare = (coloured == 0) & (inked == 0)
        tone[idx] = (((ink | colour)[idx] & plateau[island]) | (light[idx] & bare[island]))

    tone_obj = None
    if tone.any():
        tone_mesh = _mesh_from_faces(mesh, tone, loop_verts, totals, mesh.name + "Tone")
        if len(tone_mesh.polygons):
            tone_obj = bpy.data.objects.new(obj.name + "Tone", tone_mesh)
            for col in obj.users_collection:
                col.objects.link(tone_obj)
        else:
            bpy.data.meshes.remove(tone_mesh)
    obj.data = _mesh_from_faces(mesh, ink, loop_verts, totals, mesh.name)
    bpy.data.meshes.remove(mesh)
    return tone_obj


def _emboss_mask(obj, faceline, img):
    """Turn the painted face texture (eyes, brows, mouth) into raised relief."""
    if faceline:
        sw = obj.modifiers.new(name="Shrinkwrap", type='SHRINKWRAP')
        sw.target = faceline
        sw.wrap_method = 'NEAREST_SURFACEPOINT'
        sw.offset = 0.01

    subsurf = obj.modifiers.new(name="Subsurf", type='SUBSURF')
    subsurf.levels = 6
    for mod in list(obj.modifiers):
        _apply(obj, mod)

    tone = None
    if img and img.channels >= 4:
        tone = _cull_by_texture(obj, img)

    # Ink stands 1.5 proud and sinks 0.5 so it fuses with the face; a
    # plateau stands half as proud, under the ink that outlines it.
    shells = [(obj, 2.0, 0.5)]
    if tone:
        shells.append((tone, TONE_THICKNESS, TONE_OFFSET))
    for shell, thickness, offset in shells:
        sm = shell.modifiers.new(name="Smooth", type='SMOOTH')
        sm.iterations = 25
        sm.factor = 0.6
        _apply(shell, sm)

        solid = shell.modifiers.new(name="Solidify", type='SOLIDIFY')
        solid.thickness = thickness
        solid.offset = offset
        _apply(shell, solid)

        c_smooth = shell.modifiers.new(name="CorrectiveSmooth", type='CORRECTIVE_SMOOTH')
        c_smooth.iterations = 30
        c_smooth.smooth_type = 'LENGTH_WEIGHTED'
        c_smooth.factor = 1.0
        _apply(shell, c_smooth)

        subsurf2 = shell.modifiers.new(name="Subsurf2", type='SUBSURF')
        subsurf2.levels = 1
        _apply(shell, subsurf2)


# Glasses sizes are fractions of head width, like the print sizes further down.
GLASS_ALPHA = 0.5        # texture alpha that counts as frame
GLASS_CELL = 0.5         # grid cell, in fuse voxels
GLASS_REST = 5           # percentile of the frame that may press into the face
GLASS_PROUD = 0.02       # how far the frame stands off the face where it rests
GLASS_MAX_SLIDE = 0.15   # furthest the frame moves back from where the game draws it
GLASS_MIN_DEPTH = 0.02   # thinnest the frame ever gets
GLASS_PLATE = 0.035      # thickness of the frame where the face is further back
GLASS_ARM = 0.04         # width and height of the temple arms
GLASS_ARM_REACH = 0.15   # longest a temple arm gets
GLASS_SINK = 0.01        # how far the back sinks into the head


def _build_glasses(quad, head, unit):
    """The game draws glasses as a flat texture floating in front of the face.
    Cut the frame out of that texture as a thin flat plate, rest it on the
    face and hold its outer ends with temple arms, so it prints attached
    without being bent around the head."""
    import numpy as np
    from mathutils import Vector

    mesh = quad.data
    img = _material_image(quad)
    uv = _loop_uvs(mesh)
    if img is None or img.channels < 4 or uv is None or len(uv) < 3:
        return None
    px = _image_pixels(img)
    height, width = px.shape[:2]

    # Texture space to model space: position = origin + u * du + v * dv.
    loop_verts = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", loop_verts)
    corners = _coords(mesh)[loop_verts].astype(np.float64)
    fit = np.linalg.lstsq(np.c_[uv, np.ones(len(uv))], corners, rcond=None)[0]
    du, dv, origin = fit
    len_u, len_v = np.linalg.norm(du), np.linalg.norm(dv)
    normal = np.cross(du, dv)
    if len_u < 1e-6 or len_v < 1e-6 or np.linalg.norm(normal) < 1e-9:
        return None
    normal /= np.linalg.norm(normal)
    head_co = _coords(head.data)
    if np.dot(normal, corners.mean(axis=0) - head_co.mean(axis=0)) < 0:
        normal = -normal  # always points away from the head

    u0, v0 = uv.min(axis=0)
    u1, v1 = uv.max(axis=0)
    cell = unit * VOXEL_SIZE * GLASS_CELL
    nu = int(min(2048, max(1, math.ceil(len_u * (u1 - u0) / cell))))
    nv = int(min(2048, max(1, math.ceil(len_v * (v1 - v0) / cell))))

    def texel(t, size):
        # The texture is one lens, mirrored across the nose for the other.
        t = np.mod(t, 2.0)
        t = np.where(t > 1.0, 2.0 - t, t)
        return np.minimum((t * size).astype(np.int64), size - 1)

    xs = texel(u0 + (np.arange(nu) + 0.5) / nu * (u1 - u0), width)
    ys = texel(v0 + (np.arange(nv) + 0.5) / nv * (v1 - v0), height)
    keep = px[np.ix_(ys, xs)][:, :, 3].T >= GLASS_ALPHA  # [u, v]
    if not keep.any():
        return None

    gu = u0 + np.arange(nu + 1) / nu * (u1 - u0)
    gv = v0 + np.arange(nv + 1) / nv * (v1 - v0)
    front = origin + gu[:, None, None] * du + gv[None, :, None] * dv  # [u, v, xyz]

    def corners(cells):
        pts = np.zeros((nu + 1, nv + 1), dtype=bool)
        pts[:-1, :-1] |= cells
        pts[1:, :-1] |= cells
        pts[:-1, 1:] |= cells
        pts[1:, 1:] |= cells
        return pts

    used = corners(keep)

    # How far behind the game's glasses plane the head starts, looking straight back.
    reach = unit * 4
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    back_dir = Vector(-normal)
    start = front + normal * reach
    gap = np.full((nu + 1, nv + 1), np.nan)
    for i, j in zip(*np.nonzero(used)):
        hit, loc, _n, _f = head.ray_cast(Vector(start[i, j]), back_dir,
                                         distance=reach * 2, depsgraph=depsgraph)
        if hit:
            gap[i, j] = (loc - Vector(start[i, j])).length - reach
    found = used & ~np.isnan(gap)
    if not found.any():
        return None

    # The game floats the glasses well clear of the face. Slide them straight
    # back until the frame rests on it: flat, standing just proud at its
    # nearest points, with the nose bridge allowed to press in. Hair that
    # hangs in front of the glasses covers them, as it does in the game.
    clear = gap[found & (gap > 0)]
    slide = float(np.percentile(clear, GLASS_REST)) - unit * GLASS_PROUD if len(clear) else 0.0
    slide = float(np.clip(slide, 0.0, unit * GLASS_MAX_SLIDE))
    front = front - normal * slide
    gap = np.where(found, gap - slide, np.inf)

    # The frame itself is a thin plate, fused to the face only where the two
    # touch. Filling every rim back to the face would turn them into tubes.
    sink = unit * GLASS_SINK
    depth = np.clip(gap + sink, unit * GLASS_MIN_DEPTH, unit * GLASS_PLATE)

    # Like real glasses, a short arm runs straight back to the head from each
    # side, as far out as the head is still within reach, so the outer rims
    # are held at both ends.
    cell_gap = np.maximum(np.maximum(gap[:-1, :-1], gap[1:, :-1]),
                          np.maximum(gap[:-1, 1:], gap[1:, 1:]))
    reachable = keep & (cell_gap <= unit * GLASS_ARM_REACH)
    arm_cells = max(1, int(round(unit * GLASS_ARM / cell)))
    mid = nu // 2
    arms = []
    for left in (True, False):
        side = reachable[:mid] if left else reachable[mid:]
        cols = np.nonzero(side.any(axis=1))[0]
        if not len(cols):
            continue
        if left:
            lo = int(cols[0])
        else:
            lo = max(mid, mid + int(cols[-1]) - arm_cells + 1)
        hi = min(nu, lo + arm_cells)
        band = reachable[lo:hi]
        rows = np.nonzero(band.any(axis=0))[0]
        # Aim for the middle of the frame's height, but land on the frame:
        # a square rim has only its top and bottom bars to hold on to there.
        centre = int(rows[np.argmin(np.abs(rows - np.average(rows, weights=band[:, rows].sum(axis=0))))])
        r0 = max(0, centre - arm_cells // 2)
        r1 = min(nv, r0 + arm_cells)
        arm = np.zeros_like(keep)
        arm[lo:hi, r0:r1] = reachable[lo:hi, r0:r1]
        if not arm.any():
            continue
        arm_pts = corners(arm)
        depth = np.where(arm_pts, np.maximum(depth, gap + sink), depth)
        arms.append(float(depth[arm_pts].max()))
    back = front - normal * depth[:, :, None]

    count = (nu + 1) * (nv + 1)
    vid = np.arange(count).reshape(nu + 1, nv + 1)
    i, j = np.nonzero(keep)
    face_front = np.stack([vid[i, j], vid[i + 1, j], vid[i + 1, j + 1], vid[i, j + 1]], axis=1)
    face_back = face_front[:, ::-1] + count
    padded = np.pad(keep, 1)
    faces = [face_front, face_back]
    # Walls wherever a frame cell borders an empty one.
    low, high = padded[:-1, 1:-1], padded[1:, 1:-1]
    i, j = np.nonzero(low != high)
    a, b = vid[i, j], vid[i, j + 1]
    wall = np.stack([a, b, b + count, a + count], axis=1)
    wall[high[i, j]] = wall[high[i, j], ::-1]
    faces.append(wall)
    low, high = padded[1:-1, :-1], padded[1:-1, 1:]
    i, j = np.nonzero(low != high)
    a, b = vid[i, j], vid[i + 1, j]
    wall = np.stack([b, a, a + count, b + count], axis=1)
    wall[high[i, j]] = wall[high[i, j], ::-1]
    faces.append(wall)
    faces = np.concatenate(faces)

    verts = np.concatenate([front.reshape(-1, 3), back.reshape(-1, 3)])
    used_ids, remap = np.unique(faces, return_inverse=True)
    glasses_mesh = bpy.data.meshes.new("Glasses")
    glasses_mesh.from_pydata(verts[used_ids].tolist(), [], remap.reshape(-1, 4).tolist())
    glasses = bpy.data.objects.new("Glasses", glasses_mesh)
    bpy.context.scene.collection.objects.link(glasses)
    print(f"Glasses: {int(keep.sum())} cells on a {nu}x{nv} grid, slid back {slide:.2f}, "
          f"arms {[round(x, 2) for x in arms]}, "
          f"{int((used & ~found).sum())} of {int(used.sum())} points past the head")
    return glasses


def _merge_head(opaque_objs, close_neck):
    """Join the opaque shells (face, forehead, nose, hair, hat) into one mesh,
    weld the seams they share and fill what is left open."""
    import bmesh

    head = opaque_objs[0]
    if len(opaque_objs) > 1:
        bpy.ops.object.select_all(action='DESELECT')
        for o in opaque_objs:
            o.select_set(True)
        bpy.context.view_layer.objects.active = head
        bpy.ops.object.join()
    elif not close_neck:
        return head

    bm = bmesh.new()
    bm.from_mesh(head.data)
    if len(opaque_objs) > 1:
        # Weld vertices touching across part boundaries
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.05)
    open_edges = [e for e in bm.edges if e.is_boundary]
    if open_edges:
        print(f"Filling {len(open_edges)} open edges on the head")
        bmesh.ops.holes_fill(bm, edges=open_edges, sides=10000)
        open_edges = [e for e in bm.edges if e.is_boundary]

    if close_neck:
        if open_edges:
            # If normal fill fails, pinch ONLY the bottom-most boundary loop to a single point.
            # Trace the loop from the lowest vertex to avoid snagging jaw/cheek seams.
            lowest = min({v for e in open_edges for v in e.verts}, key=lambda v: v.co.z)
            loop_verts = set()
            queue = [lowest]
            while queue:
                curr = queue.pop()
                if curr in loop_verts:
                    continue
                loop_verts.add(curr)
                for e in curr.link_edges:
                    if e.is_boundary and e.other_vert(curr) not in loop_verts:
                        queue.append(e.other_vert(curr))
            neck_verts = list(loop_verts)
            centre = sum((v.co for v in neck_verts), neck_verts[0].co * 0) / len(neck_verts)
            for v in neck_verts:
                v.co = centre
            bmesh.ops.remove_doubles(bm, verts=neck_verts, dist=0.1)
            open_edges = [e for e in bm.edges if e.is_boundary]
            if open_edges:
                bmesh.ops.holes_fill(bm, edges=open_edges, sides=10000)
                open_edges = [e for e in bm.edges if e.is_boundary]

    print(f"Head: {len(open_edges)} open edges, {len(bm.faces):,} faces")
    bm.to_mesh(head.data)
    head.data.update()
    bm.free()
    return head


def process_glb(input_glb, output_stl, body_type=None, with_base=True, height=64, build=64):
    """height and build (0-127, as the Mii stores them) scale the body."""
    print(f"Processing: {input_glb} -> {output_stl}")
    input_glb = os.path.abspath(input_glb)
    output_stl = os.path.abspath(output_stl)
    _T[0] = time.time()

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=input_glb)

    import bmesh
    from mathutils import Matrix

    for obj in list(bpy.context.scene.objects):
        if "NoseLine" in obj.name:
            print(f"Removing unnecessary 2D shadow quad: {obj.name}")
            bpy.data.objects.remove(obj, do_unlink=True)

    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    for obj in meshes:
        # Everything below works in world space.
        obj.data.transform(obj.matrix_world)
        obj.matrix_world = Matrix.Identity(4)

    glass_quads = [o for o in meshes if "Glass" in o.name]
    masks = [o for o in meshes if o not in glass_quads and ("Mask" in o.name or "Xlu" in o.name)]
    opaque_objs = [o for o in meshes if o not in glass_quads and o not in masks]
    faceline = bpy.data.objects.get("OpaFaceline")

    for obj in opaque_objs + masks:
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        # Weld UV-seam split vertices but do NOT fill boundary loops -
        # meshes share open seams with each other (face meets hat, etc)
        # and filling them here would seal each shell independently.
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0001)
        bm.to_mesh(obj.data)
        bm.free()

    for obj in opaque_objs:
        _apply(obj, _subsurf(obj, 3))

    cap = bpy.data.objects.get("OpaCap")
    if cap and cap.type == 'MESH':
        print("Embossing stripes on OpaCap...")
        _apply(cap, _subsurf(cap, 1, name="SubsurfCap"))

        cap_img = _material_image(cap)
        if cap_img:
            tex = bpy.data.textures.new("CapTex", type='IMAGE')
            tex.image = cap_img
            displace = cap.modifiers.new(name="DisplaceCap", type='DISPLACE')
            displace.texture = tex
            displace.texture_coords = 'UV'
            displace.mid_level = 0.8745
            displace.strength = -0.8
            _apply(cap, displace)

            smooth_cap = cap.modifiers.new(name="SmoothCap", type='SMOOTH')
            smooth_cap.iterations = 5
            smooth_cap.factor = 0.5
            _apply(cap, smooth_cap)

    hair = bpy.data.objects.get("OpaHair")
    if hair and hair.type == 'MESH':
        print("Embossing hair volume...")
        # Additional subdivision for smoother hair surface
        _apply(hair, _subsurf(hair, 1, name="SubsurfHair"))

        # Use Solidify to uniformly expand the hair surface outward.
        # This gives a subtle embossed relief relative to the face.
        solidify_hair = hair.modifiers.new(name="SolidifyHair", type='SOLIDIFY')
        solidify_hair.thickness = 1.5   # Few-mm extrusion, similar to face features
        solidify_hair.offset = 1.0      # Expand outward only
        solidify_hair.use_even_offset = True
        _apply(hair, solidify_hair)

        smooth_hair = hair.modifiers.new(name="SmoothHair", type='SMOOTH')
        smooth_hair.iterations = 5
        smooth_hair.factor = 0.5
        _apply(hair, smooth_hair)
    _tick("import and smooth shells")

    # An image.png beside the GLB replaces the mask texture embedded in it.
    external_mask = None
    for candidate in [os.path.join(os.path.dirname(input_glb), "image.png"), "image.png"]:
        if os.path.exists(candidate):
            try:
                external_mask = bpy.data.images.load(candidate)
                print(f"Using external emboss mask: {candidate}")
                break
            except Exception as e:
                print(f"Warning loading external mask {candidate}: {e}")

    for obj in masks:
        img = _material_image(obj)
        if "Mask" in obj.name and external_mask:
            img = external_mask
        _emboss_mask(obj, faceline, img)
    _tick("emboss face features")

    # Merge all opaque mesh parts into one, weld shared seam vertices between
    # parts (face/hat/hair boundaries), then fill any remaining true holes.
    head = _merge_head(opaque_objs, close_neck=bool(body_type))
    head.name = 'Head'
    # dimensions would still report the pre-merge faceline bounds here.
    head_xs = _coords(head.data)[:, 0]
    head_width = float(head_xs.max() - head_xs.min())
    _tick("merge head")

    for quad in glass_quads:
        _build_glasses(quad, head, head_width)
        bpy.data.objects.remove(quad, do_unlink=True)
    if glass_quads:
        _tick("glasses")

    has_body = False
    if body_type:
        print(f"\n=== Attaching body (type={body_type}) ===")
        has_body = _attach_body(body_type, head, height, build)
        _tick("body")

    _make_printable(head_width, with_base=has_body and with_base)
    _export_stl(output_stl)
    _tick("export")
    print(f"Done: {output_stl}")


def _export_stl(path):
    try:
        bpy.ops.export_mesh.stl(filepath=path)
    except (AttributeError, RuntimeError):
        bpy.ops.wm.stl_export(filepath=path)


# Sizes below are fractions of head width, so they hold at any import scale.
VOXEL_SIZE = 1 / 240
BASE_RADIUS = 0.45
BASE_HEIGHT = 0.05
BASE_SINK = 0.025
BODY_THICKEN = 0.006
# Thinnest an arm, leg, wrist or ankle may get, as a radius. An average Mii's
# ankles are 0.03 to 0.043 of the head's width, depending on the hair; the
# shortest, slightest Mii scales them to under half that, which snaps once
# printed.
MIN_LIMB_RADIUS = 0.03
BODY_SEAM_WELD = 0.003   # gap between the torso and legs rims that still welds
BODY_POSE = "Finish"     # the editor's standing pose, as Blender imports it
MIN_ISLAND = 0.01
MAX_TRIS = 600_000
REMESH_ADAPTIVITY = 0.03


def _add_base(parts, unit):
    """A disc under the feet, so a top-heavy Mii prints upright and stays standing."""
    import bmesh
    import numpy as np
    from mathutils import Vector

    pts = np.concatenate([_coords(o.data) for o in parts])
    zmin = pts[:, 2].min()
    feet = pts[pts[:, 2] < zmin + unit * 0.05]
    cx, cy = feet[:, 0].mean(), feet[:, 1].mean()

    radius = unit * BASE_RADIUS
    height = unit * BASE_HEIGHT
    bm = bmesh.new()
    # Slight taper hides elephant's foot on the first layers.
    bmesh.ops.create_cone(bm, cap_ends=True, segments=128,
                          radius1=radius, radius2=radius * 0.95, depth=height)
    mesh = bpy.data.meshes.new("Base")
    bm.to_mesh(mesh)
    bm.free()
    base = bpy.data.objects.new("Base", mesh)
    bpy.context.scene.collection.objects.link(base)
    base.location = Vector((cx, cy, zmin + unit * BASE_SINK - height / 2))
    print(f"Base: r={radius:.1f} h={height:.1f} under feet at ({cx:.1f}, {cy:.1f})")
    return base


def _tri_count(mesh):
    return len(mesh.loops) - 2 * len(mesh.polygons)


def _make_printable(unit, with_base):
    """Fuse every shell into one watertight solid, so slicers never have to
    guess at overlaps, gaps or open edges."""
    import bmesh
    import numpy as np

    parts = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    if with_base:
        parts.append(_add_base(parts, unit))

    bpy.ops.object.select_all(action='DESELECT')
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    solid = bpy.context.view_layer.objects.active
    solid.name = "PrintMiiOut"

    remesh = solid.modifiers.new("Fuse", type='REMESH')
    remesh.mode = 'VOXEL'
    remesh.voxel_size = unit * VOXEL_SIZE
    remesh.adaptivity = REMESH_ADAPTIVITY
    bpy.ops.object.modifier_apply(modifier=remesh.name)
    print(f"Fused: {_tri_count(solid.data):,} triangles")
    _tick("fuse")

    # Voxels leave a stair-step on feature edges; two passes sand it off.
    sand = solid.modifiers.new("Sand", type='SMOOTH')
    sand.factor = 0.5
    sand.iterations = 2
    bpy.ops.object.modifier_apply(modifier=sand.name)

    # Specks left over from the texture emboss would print as loose blobs.
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.separate(type='LOOSE')
    bpy.ops.object.mode_set(mode='OBJECT')
    islands = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    volumes = {}
    for o in islands:
        bm = bmesh.new()
        bm.from_mesh(o.data)
        volumes[o] = abs(bm.calc_volume())
        bm.free()
    biggest = max(volumes.values())
    keep = [o for o in islands if volumes[o] >= biggest * MIN_ISLAND]
    for o in islands:
        if o not in keep:
            bpy.data.objects.remove(o, do_unlink=True)
    print(f"Fused into {len(keep)} solid(s), dropped {len(islands) - len(keep)} specks")

    bpy.ops.object.select_all(action='DESELECT')
    for o in keep:
        o.select_set(True)
    bpy.context.view_layer.objects.active = keep[0]
    if len(keep) > 1:
        bpy.ops.object.join()
    solid = bpy.context.view_layer.objects.active
    _tick("drop specks")

    tris = _tri_count(solid.data)
    if tris > MAX_TRIS:
        slim = solid.modifiers.new("Slim", type='DECIMATE')
        slim.ratio = MAX_TRIS / tris
        slim.use_collapse_triangulate = True
        bpy.ops.object.modifier_apply(modifier=slim.name)
        _tick("slim")

    mesh = solid.data
    edge_use = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("edge_index", edge_use)
    open_e = int((np.bincount(edge_use, minlength=len(mesh.edges)) != 2).sum())
    print(f"Printable: {len(mesh.polygons):,} faces, {open_e} non-manifold edges")


def _tilt_head(rotation):
    """Turn everything built so far (head, glasses) about the head's origin,
    the way FFL.js turns the head model with the posed head bone."""
    import numpy as np

    for o in bpy.context.scene.objects:
        if o.type != 'MESH':
            continue
        co = _coords(o.data).astype(np.float64) @ rotation.T
        o.data.vertices.foreach_set("co", co.astype(np.float32).ravel())
        o.data.update()


def _attach_body(body_type, head, height, build):
    """Pose and scale the body as the Mii's height and build make it (FFL.js's
    per-bone scaling, see miibody.py), stand it under the head the way FFL.js
    attaches the head, and close it, overlapping the head so the fuse step
    joins them. Returns False if there is no body."""
    import bmesh
    import numpy as np

    sys.path.insert(0, SCRIPT_DIR)
    import miibody

    body_glb = os.path.join(SCRIPT_DIR, f"miiBody{'M' if body_type == 'm' else 'F'}_wiiu_plain.glb")
    if not os.path.exists(body_glb):
        print(f"Error: Body model not found: {body_glb}")
        return False

    hv = _coords(head.data)
    head_width = float(hv[:, 0].max() - hv[:, 0].min())

    body = miibody.Body(body_glb, height=height, build=build, pose=BODY_POSE)
    moved = body.thicken_limbs(head_width * MIN_LIMB_RADIUS / miibody.HEAD_TO_BODY)
    print(f"Body: height={height} build={build} scale={np.round(body.scale, 3).tolist()}, "
          f"{moved} limb vertices held at the printable minimum, head tilted {body.head_tilt():.1f} deg")

    # glTF is Y up, Blender Z up: (x, y, z) -> (x, -z, y).
    to_z_up = np.array(((1.0, 0.0, 0.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0)))
    _tilt_head(to_z_up @ body.head_rotation() @ to_z_up.T)

    verts, faces, at = [], [], 0
    for _name, v, tris in body.in_head_frame(upright=True):
        verts.append(v @ to_z_up.T)
        faces.append(tris + at)
        at += len(v)
    mesh = bpy.data.meshes.new("Body")
    mesh.from_pydata(np.concatenate(verts).tolist(), [], np.concatenate(faces).tolist())
    body_obj = bpy.data.objects.new("Body", mesh)
    bpy.context.scene.collection.objects.link(body_obj)

    # The torso and legs share a ring of vertices at the waist (or the skirt
    # hem), and the neck is left open inside the head. Weld only open edges,
    # so thin limbs never collapse, then fill whatever is still open.
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=head_width * 1e-5)
    rim = list({v for e in bm.edges if e.is_boundary for v in e.verts})
    bmesh.ops.remove_doubles(bm, verts=rim, dist=head_width * BODY_SEAM_WELD)
    open_e = [e for e in bm.edges if e.is_boundary]
    before = len(open_e)
    if open_e:
        bmesh.ops.holes_fill(bm, edges=open_e, sides=10000)
    after = sum(1 for e in bm.edges if e.is_boundary)
    bm.to_mesh(mesh)
    mesh.update()
    bm.free()
    print(f"Body after waist weld: {before} open edges, {after} after fill")
    body = body_obj

    def close(mesh):
        bm = bmesh.new()
        bm.from_mesh(mesh)
        open_e = [e for e in bm.edges if e.is_boundary]
        if open_e:
            bmesh.ops.holes_fill(bm, edges=open_e, sides=10000)
        after = sum(1 for e in bm.edges if e.is_boundary)
        faces = len(bm.faces)
        bm.to_mesh(mesh)
        mesh.update()
        bm.free()
        return after, faces

    # Subdivide body for smooth surface
    _apply(body, _subsurf(body, 3, name='Sub'))

    # Fill any holes opened by subdivision
    after, faces = close(body.data)
    print(f"  {body.name}: {after} open edges, {faces:,} faces")

    # Thin ankles, wrists and the skirt hem snap once the figure is scaled down.
    thicken = body.modifiers.new('Thicken', type='DISPLACE')
    thicken.mid_level = 0.0
    thicken.strength = head_width * BODY_THICKEN
    _apply(body, thicken)
    return True


if __name__ == "__main__":
    if "--" in sys.argv:
        args = sys.argv[sys.argv.index("--") + 1:]
    else:
        args = sys.argv[1:]

    with_base = "--no-base" not in args
    args = [a for a in args if a != "--no-base"]

    # --height N / --build N: the Mii's own height and build (0-127).
    traits = {"height": 64, "build": 64}
    for key in traits:
        flag = f"--{key}"
        if flag in args:
            at = args.index(flag)
            try:
                traits[key] = max(0, min(127, int(args[at + 1])))
            except (IndexError, ValueError):
                print(f"Warning: {flag} needs a number from 0 to 127")
            del args[at:at + 2]

    # Check if last arg is body type (m or f)
    body_type = None
    if args and args[-1].lower() in ('m', 'f'):
        body_type = args[-1].lower()
        args = args[:-1]

    targets = []
    if args:
        for arg in args:
            path = os.path.expanduser(arg)
            if os.path.isdir(path):
                for entry in sorted(os.listdir(path)):
                    full_path = os.path.join(path, entry)
                    if os.path.isfile(full_path) and entry.lower().endswith((".glb", ".gltf")):
                        targets.append(full_path)
            elif os.path.isfile(path):
                targets.append(path)
            else:
                print(f"Warning: Path not found: {arg}")
    else:
        if os.path.exists("model.glb"):
            targets.append("model.glb")
        else:
            for file in sorted(os.listdir(".")):
                if file.lower().endswith((".glb", ".gltf")):
                    targets.append(file)

    if not targets:
        print("No .glb or .gltf files found to process.")
        sys.exit(0)

    for input_glb in targets:
        output_stl = os.path.splitext(input_glb)[0] + ".stl"
        process_glb(input_glb, output_stl, body_type=body_type, with_base=with_base, **traits)
