import sys
import os
import subprocess
import shutil
import math

try:
    import bpy
except ModuleNotFoundError:
    blender_exe = shutil.which("blender") or "/opt/homebrew/bin/blender"
    if not os.path.exists(blender_exe):
        blender_exe = "/Applications/Blender.app/Contents/MacOS/Blender"
    if not os.path.exists(blender_exe):
        print("Error: Blender executable not found.", file=sys.stderr)
        sys.exit(1)
    cmd = [blender_exe, "-b", "--python", os.path.abspath(__file__), "--"] + sys.argv[1:]
    sys.exit(subprocess.call(cmd))

# Mask texture noise leaves islands under ~0.35 GLB units²; real features are 1+.
SPECK_AREA = 0.5


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


def process_glb(input_glb, output_stl, body_type=None):
    print(f"Processing: {input_glb} -> {output_stl}")
    input_glb = os.path.abspath(input_glb)
    output_stl = os.path.abspath(output_stl)
    
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=input_glb)

    import bmesh
    import numpy as np

    faceline = bpy.data.objects.get("OpaFaceline")

    for obj in list(bpy.context.scene.objects):
        if "NoseLine" in obj.name:
            print(f"Removing unnecessary 2D shadow quad: {obj.name}")
            bpy.data.objects.remove(obj, do_unlink=True)

    for obj in list(bpy.context.scene.objects):
        if obj.type == 'MESH':
            bm = bmesh.new()
            bm.from_mesh(obj.data)
            # Weld UV-seam split vertices but do NOT fill boundary loops -
            # meshes share open seams with each other (face meets hat, etc)
            # and filling them here would seal each shell independently.
            bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0001)
            bm.to_mesh(obj.data)
            bm.free()

    for obj in list(bpy.context.scene.objects):
        if obj.type == 'MESH' and not ("Mask" in obj.name or "Xlu" in obj.name):
            subsurf = obj.modifiers.new(name="Subsurf", type='SUBSURF')
            subsurf.levels = 3
            subsurf.subdivision_type = 'CATMULL_CLARK'
            if hasattr(subsurf, "boundary_smooth"):
                subsurf.boundary_smooth = 'PRESERVE_CORNERS'
            bpy.context.view_layer.objects.active = obj
            try:
                bpy.ops.object.modifier_apply(modifier=subsurf.name)
            except Exception as e:
                print(f"Warning applying Subsurf to {obj.name}: {e}")

    cap = bpy.data.objects.get("OpaCap")
    if cap and cap.type == 'MESH':
        print("Embossing stripes on OpaCap...")
        subsurf_cap = cap.modifiers.new(name="SubsurfCap", type='SUBSURF')
        subsurf_cap.levels = 1
        subsurf_cap.subdivision_type = 'CATMULL_CLARK'
        if hasattr(subsurf_cap, "boundary_smooth"):
            subsurf_cap.boundary_smooth = 'PRESERVE_CORNERS'
        bpy.context.view_layer.objects.active = cap
        try:
            bpy.ops.object.modifier_apply(modifier=subsurf_cap.name)
        except Exception as e:
            print(f"Warning applying SubsurfCap: {e}")

        cap_img = None
        if cap.data.materials:
            mat = cap.data.materials[0]
            if mat.use_nodes:
                for node in mat.node_tree.nodes:
                    if node.type == 'TEX_IMAGE' and node.image:
                        cap_img = node.image
                        break
        if cap_img:
            tex = bpy.data.textures.new("CapTex", type='IMAGE')
            tex.image = cap_img
            displace = cap.modifiers.new(name="DisplaceCap", type='DISPLACE')
            displace.texture = tex
            displace.texture_coords = 'UV'
            displace.mid_level = 0.8745
            displace.strength = -0.8
            try:
                bpy.ops.object.modifier_apply(modifier=displace.name)
            except Exception as e:
                print(f"Warning applying DisplaceCap: {e}")

            smooth_cap = cap.modifiers.new(name="SmoothCap", type='SMOOTH')
            smooth_cap.iterations = 5
            smooth_cap.factor = 0.5
            try:
                bpy.ops.object.modifier_apply(modifier=smooth_cap.name)
            except Exception as e:
                pass

    hair = bpy.data.objects.get("OpaHair")
    if hair and hair.type == 'MESH':
        print("Embossing hair volume...")
        # Additional subdivision for smoother hair surface
        subsurf_hair = hair.modifiers.new(name="SubsurfHair", type='SUBSURF')
        subsurf_hair.levels = 1
        subsurf_hair.subdivision_type = 'CATMULL_CLARK'
        if hasattr(subsurf_hair, "boundary_smooth"):
            subsurf_hair.boundary_smooth = 'PRESERVE_CORNERS'
        bpy.context.view_layer.objects.active = hair
        try:
            bpy.ops.object.modifier_apply(modifier=subsurf_hair.name)
        except Exception as e:
            print(f"Warning applying SubsurfHair: {e}")

        # Use Solidify to uniformly expand the hair surface outward.
        # This gives a subtle embossed relief relative to the face.
        solidify_hair = hair.modifiers.new(name="SolidifyHair", type='SOLIDIFY')
        solidify_hair.thickness = 1.5   # Few-mm extrusion, similar to face features
        solidify_hair.offset = 1.0      # Expand outward only
        solidify_hair.use_even_offset = True
        try:
            bpy.ops.object.modifier_apply(modifier=solidify_hair.name)
        except Exception as e:
            print(f"Warning applying SolidifyHair: {e}")

        smooth_hair = hair.modifiers.new(name="SmoothHair", type='SMOOTH')
        smooth_hair.iterations = 5
        smooth_hair.factor = 0.5
        try:
            bpy.ops.object.modifier_apply(modifier=smooth_hair.name)
        except Exception as e:
            pass


    for obj in list(bpy.context.scene.objects):
        if obj.type == 'MESH':
            img = None
            if obj.data.materials:
                mat = obj.data.materials[0]
                if mat.use_nodes:
                    for node in mat.node_tree.nodes:
                        if node.type == 'TEX_IMAGE' and node.image:
                            img = node.image
                            break
            
            if "Mask" in obj.name or "Xlu" in obj.name:
                for candidate in [os.path.join(os.path.dirname(input_glb), "image.png"), "image.png"]:
                    if os.path.exists(candidate):
                        try:
                            img = bpy.data.images.load(candidate)
                            print(f"Using external emboss mask: {candidate}")
                            break
                        except Exception as e:
                            print(f"Warning loading external mask {candidate}: {e}")

                if faceline:
                    sw = obj.modifiers.new(name="Shrinkwrap", type='SHRINKWRAP')
                    sw.target = faceline
                    sw.wrap_method = 'NEAREST_SURFACEPOINT'
                    sw.offset = 0.01

                subsurf = obj.modifiers.new(name="Subsurf", type='SUBSURF')
                subsurf.levels = 6

                bpy.context.view_layer.objects.active = obj
                for mod in list(obj.modifiers):
                    try:
                        bpy.ops.object.modifier_apply(modifier=mod.name)
                    except Exception as e:
                        print(f"Warning applying {mod.name}: {e}")

                if img and img.channels >= 4:
                    width, height = img.size
                    px_data = np.zeros(width * height * img.channels, dtype=np.float32)
                    img.pixels.foreach_get(px_data)
                    px_reshaped = px_data.reshape((height, width, img.channels))
                    alpha_channel = px_reshaped[:, :, 3]
                    lum_channel = 0.2126 * px_reshaped[:, :, 0] + 0.7152 * px_reshaped[:, :, 1] + 0.0722 * px_reshaped[:, :, 2]

                    bm = bmesh.new()
                    bm.from_mesh(obj.data)
                    bm.faces.ensure_lookup_table()
                    uv_layer = bm.loops.layers.uv.active

                    if uv_layer:
                        faces_to_delete = []
                        for face in bm.faces:
                            max_alpha = 0.0
                            avg_lum = 0.0
                            for loop in face.loops:
                                uv = loop[uv_layer].uv
                                x = int(uv[0] * width) % width
                                y = int(uv[1] * height) % height
                                a = alpha_channel[y, x]
                                l = lum_channel[y, x]
                                if a > max_alpha:
                                    max_alpha = a
                                avg_lum += l
                            avg_lum /= len(face.loops)
                            if max_alpha < 0.1 or avg_lum > 0.5:
                                faces_to_delete.append(face)

                        if faces_to_delete:
                            print(f"Removing {len(faces_to_delete)} / {len(bm.faces)} transparent faces from {obj.name}")
                            bmesh.ops.delete(bm, geom=faces_to_delete, context='FACES')
                        specks = _speck_faces(bm, SPECK_AREA)
                        if specks:
                            print(f"Removing {len(specks)} speck faces from {obj.name}")
                            bmesh.ops.delete(bm, geom=specks, context='FACES')
                        bm.to_mesh(obj.data)
                    bm.free()

                sm = obj.modifiers.new(name="Smooth", type='SMOOTH')
                sm.iterations = 25
                sm.factor = 0.6
                try:
                    bpy.ops.object.modifier_apply(modifier=sm.name)
                except Exception as e:
                    print(f"Warning applying Smooth: {e}")

                # Sink 0.5 below the face so features fuse instead of floating on it.
                solid = obj.modifiers.new(name="Solidify", type='SOLIDIFY')
                solid.thickness = 2.0
                solid.offset = 0.5
                try:
                    bpy.ops.object.modifier_apply(modifier=solid.name)
                except Exception as e:
                    print(f"Warning applying Solidify: {e}")

                c_smooth = obj.modifiers.new(name="CorrectiveSmooth", type='CORRECTIVE_SMOOTH')
                c_smooth.iterations = 30
                c_smooth.smooth_type = 'LENGTH_WEIGHTED'
                c_smooth.factor = 1.0
                try:
                    bpy.ops.object.modifier_apply(modifier=c_smooth.name)
                except Exception as e:
                    pass

                subsurf2 = obj.modifiers.new(name="Subsurf2", type='SUBSURF')
                subsurf2.levels = 1
                try:
                    bpy.ops.object.modifier_apply(modifier=subsurf2.name)
                except Exception as e:
                    pass

    # Final pass: merge all opaque mesh parts into a single bmesh,
    # weld shared seam vertices between parts (face/hat/hair boundaries),
    # then fill any remaining true internal holes.
    opaque_objs = [o for o in bpy.context.scene.objects
                   if o.type == 'MESH' and not ('Mask' in o.name or 'Xlu' in o.name)]
    if len(opaque_objs) > 1:
        # Build one combined bmesh from all opaque parts
        combined_bm = bmesh.new()
        for o in opaque_objs:
            temp_bm = bmesh.new()
            temp_bm.from_mesh(o.data)
            # Apply object world matrix
            import mathutils
            temp_bm.transform(o.matrix_world)
            # Merge into combined
            src_verts = [combined_bm.verts.new(v.co) for v in temp_bm.verts]
            for edge in temp_bm.edges:
                try:
                    combined_bm.edges.new([src_verts[edge.verts[0].index], src_verts[edge.verts[1].index]])
                except Exception:
                    pass
            for face in temp_bm.faces:
                try:
                    combined_bm.faces.new([src_verts[v.index] for v in face.verts])
                except Exception:
                    pass
            temp_bm.free()

        # Weld vertices touching across part boundaries
        bmesh.ops.remove_doubles(combined_bm, verts=combined_bm.verts, dist=0.05)
        # Fill any remaining small internal holes
        open_edges = [e for e in combined_bm.edges if e.is_boundary]
        if open_edges:
            print(f"Filling {len(open_edges)} remaining open edges after merge")
            bmesh.ops.holes_fill(combined_bm, edges=open_edges, sides=10000)

        # Write combined mesh back to first opaque object
        combined_bm.to_mesh(opaque_objs[0].data)
        combined_bm.free()

        # Remove the remaining opaque source objects
        for o in opaque_objs[1:]:
            bpy.data.objects.remove(o, do_unlink=True)

        # Verify
        bm_check = bmesh.new()
        bm_check.from_mesh(opaque_objs[0].data)
        remaining_open = [e for e in bm_check.edges if e.is_boundary]
        print(f"Final open edges after merge+weld: {len(remaining_open)}")
        bm_check.free()

    # --- Export head-only STL first ---
    head_stl = output_stl
    try:
        bpy.ops.export_mesh.stl(filepath=head_stl)
    except (AttributeError, RuntimeError):
        bpy.ops.wm.stl_export(filepath=head_stl)

    # --- Attach body if body_type specified ---
    if body_type:
        print(f"\n=== Attaching body (type={body_type}) ===")
        # opaque_objs[0] is our fully processed head
        _attach_body(body_type, opaque_objs[0], output_stl)
    
    print(f"Done: {output_stl}")


def _attach_body(body_type, head, output_stl):
    """Load the body GLB, apply armature pose, scale/position,
    close each shell independently, and export as overlapping manifolds."""
    import bmesh
    import numpy as np

    script_dir = os.path.dirname(os.path.abspath(__file__))
    body_glb = os.path.join(script_dir, f"miiBody{'M' if body_type == 'm' else 'F'}_wiiu_plain.glb")
    if not os.path.exists(body_glb):
        print(f"Error: Body model not found: {body_glb}")
        return

    head.name = 'Head'

    # Close any open edges on the head (neck bottom hole)
    bm_h = bmesh.new()
    bm_h.from_mesh(head.data)
    open_h = [e for e in bm_h.edges if e.is_boundary]
    if open_h:
        print(f"Closing {len(open_h)} open edges on head...")
        bmesh.ops.holes_fill(bm_h, edges=open_h, sides=10000)
        # If normal fill fails, pinch ONLY the bottom-most boundary loop to a single point.
        # We trace the edge loop starting from the absolute lowest Z vertex to avoid snagging jaw/cheek seams.
        open_after = [e for e in bm_h.edges if e.is_boundary]
        if open_after:
            bnd_verts = list(set(v for e in open_after for v in e.verts))
            if bnd_verts:
                lowest_v = min(bnd_verts, key=lambda v: v.co.z)
                # Trace the boundary loop connected to lowest_v
                loop_verts = set()
                queue = [lowest_v]
                while queue:
                    curr = queue.pop(0)
                    if curr not in loop_verts:
                        loop_verts.add(curr)
                        # Find connected boundary vertices
                        for e in curr.link_edges:
                            if e.is_boundary:
                                other = e.other_vert(curr)
                                if other not in loop_verts:
                                    queue.append(other)
                
                neck_verts = list(loop_verts)
                if neck_verts:
                    cx = sum(v.co.x for v in neck_verts) / len(neck_verts)
                    cy = sum(v.co.y for v in neck_verts) / len(neck_verts)
                    cz = sum(v.co.z for v in neck_verts) / len(neck_verts)
                    for v in neck_verts:
                        v.co = (cx, cy, cz)
                    bmesh.ops.remove_doubles(bm_h, verts=neck_verts, dist=0.1)

    hv = np.array([(v.co.x, v.co.y, v.co.z) for v in bm_h.verts])
    bm_h.to_mesh(head.data)
    head.data.update()
    bm_h.free()

    head_cx    = float((hv[:, 0].min() + hv[:, 0].max()) / 2)
    head_cy    = float((hv[:, 1].min() + hv[:, 1].max()) / 2)
    head_zmin  = float(hv[:, 2].min())
    head_width = float(hv[:, 0].max() - hv[:, 0].min())
    print(f"Head: W={head_width:.2f} zmin={head_zmin:.2f} (closed)")

    # --- Import body GLB, bake armature pose ---
    existing_objs = set(bpy.context.scene.objects)
    bpy.ops.import_scene.gltf(filepath=body_glb)
    
    body_objs = [o for o in bpy.context.scene.objects
                 if o not in existing_objs
                 and o.type == 'MESH' 
                 and len(o.data.vertices) >= 100]
    print(f"Body meshes: {[o.name for o in body_objs]}")

    arm_objs = [o for o in bpy.context.scene.objects
                if o not in existing_objs
                and o.type == 'ARMATURE']
    bone_loc = None
    if arm_objs:
        arm = arm_objs[0]
        if 'head' in arm.data.bones:
            bone_loc = arm.matrix_world @ arm.data.bones['head'].head_local
            print(f"Found armature head bone at: {bone_loc}")

    # Bake world transform + apply armature modifier to get posed mesh
    for o in body_objs:
        bpy.context.view_layer.objects.active = o
        o.select_set(True)
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        for m in list(o.modifiers):
            if m.type == 'ARMATURE':
                print(f"  Applying armature on {o.name}...")
                bpy.ops.object.modifier_apply(modifier=m.name)
        o.select_set(False)

    for arm in arm_objs:
        bpy.data.objects.remove(arm, do_unlink=True)

    # Compute combined body bounds for scaling
    all_v = []
    for o in body_objs:
        bm = bmesh.new()
        bm.from_mesh(o.data)
        all_v += [(v.co.x, v.co.y, v.co.z) for v in bm.verts]
        bm.free()
    av = np.array(all_v)
    body_width = float(av[:, 0].max() - av[:, 0].min())
    body_cx = float((av[:, 0].min() + av[:, 0].max()) / 2)
    body_cy = float((av[:, 1].min() + av[:, 1].max()) / 2)
    body_zmax = float(av[:, 2].max())

    # Scale body to 90% of head width
    scale = (head_width * 0.90) / body_width
    if bone_loc is not None:
        # Attach directly to the armature head bone position at origin (0, 0, 0)
        dx = -bone_loc.x * scale
        dy = -bone_loc.y * scale
        dz = -bone_loc.z * scale
        print(f"Body W={body_width:.3f} scale={scale:.4f} attached to head bone: ({dx:.2f}, {dy:.2f}, {dz:.2f})")
    else:
        dx = head_cx - body_cx * scale
        dy = head_cy - body_cy * scale
        dz = head_zmin - body_zmax * scale + 15.0
        print(f"Body W={body_width:.3f} scale={scale:.4f} manual bounds offset")

    for o in body_objs:
        bm = bmesh.new()
        bm.from_mesh(o.data)
        for v in bm.verts:
            v.co.x = v.co.x * scale + dx
            v.co.y = v.co.y * scale + dy
            v.co.z = v.co.z * scale + dz
        bm.to_mesh(o.data)
        o.data.update()
        bm.free()
        o.location = (0, 0, 0); o.rotation_euler = (0, 0, 0); o.scale = (1, 1, 1)

    # --- Merge body pieces, weld torso-legs seam, close body independently ---
    body_bm = bmesh.new()
    for o in body_objs:
        temp = bmesh.new()
        temp.from_mesh(o.data)
        sv = [body_bm.verts.new(v.co) for v in temp.verts]
        for f in temp.faces:
            try:
                body_bm.faces.new([sv[v.index] for v in f.verts])
            except Exception:
                pass
        temp.free()

    bmesh.ops.remove_doubles(body_bm, verts=body_bm.verts, dist=1.5)
    open_b = [e for e in body_bm.edges if e.is_boundary]
    print(f"Body after torso-legs weld: {len(open_b)} open edges")
    if open_b:
        bmesh.ops.holes_fill(body_bm, edges=open_b, sides=10000)
    open_b2 = [e for e in body_bm.edges if e.is_boundary]
    print(f"Body after fill: {len(open_b2)} open edges")
    body_bm.to_mesh(body_objs[0].data)
    body_objs[0].data.update()
    body_bm.free()
    for o in body_objs[1:]:
        bpy.data.objects.remove(o, do_unlink=True)

    # Subdivide body for smooth surface
    bpy.context.view_layer.objects.active = body_objs[0]
    sub = body_objs[0].modifiers.new('Sub', type='SUBSURF')
    sub.levels = 3
    sub.subdivision_type = 'CATMULL_CLARK'
    if hasattr(sub, 'boundary_smooth'):
        sub.boundary_smooth = 'PRESERVE_CORNERS'
    bpy.ops.object.modifier_apply(modifier=sub.name)

    # Fill any holes opened by subdivision + recalc normals on both shells
    for obj in [body_objs[0], head]:
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        open_e = [e for e in bm.edges if e.is_boundary]
        if open_e:
            bmesh.ops.holes_fill(bm, edges=open_e, sides=10000)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bm.to_mesh(obj.data)
        obj.data.update()
        bm.free()

    # Report per-shell status
    for obj in [head, body_objs[0]]:
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        oe = [e for e in bm.edges if e.is_boundary]
        print(f"  {obj.name}: {len(oe)} open edges, {len(bm.faces):,} faces")
        bm.free()

    # Export both closed shells in one STL (slicer unions overlapping shells)
    try:
        bpy.ops.export_mesh.stl(filepath=output_stl)
    except (AttributeError, RuntimeError):
        bpy.ops.wm.stl_export(filepath=output_stl)


if __name__ == "__main__":
    if "--" in sys.argv:
        args = sys.argv[sys.argv.index("--") + 1:]
    else:
        args = sys.argv[1:]

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
        process_glb(input_glb, output_stl, body_type=body_type)
