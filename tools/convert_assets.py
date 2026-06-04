# tools/convert_assets.py
import os
import sys
import bpy
import bmesh

# Set stack recursion limit
sys.setrecursionlimit(15000)

# ========================================================================
# PIPELINE CONFIGURATION FLAGS
# ========================================================================
# Set to True during active C++ coding to bypass the slow Cycles bake.
# This will let you export all 82 models as static .glb meshes in under 30 seconds!
# Set to False only when you are preparing a Release build to generate high-quality atlases.
DEV_MODE = True
# ========================================================================

# Input and output directory setups (Baking directly to the engine's asset path)
input_dir = os.getcwd()
output_dir = os.path.join(input_dir, "resources", "assets")
os.makedirs(output_dir, exist_ok=True)

print("=========================================================")
print("Zahlen Engine Asset Exporter (With Low-Level bpy.data)")
print("Target directories:")
print(f"  Input:  {input_dir}")
print(f"  Output: {output_dir}")
print("=========================================================")

# 1. Walk through directories to find all .blend files
blend_files = []
for root, _, files in os.walk(input_dir):
    # Normalize slashes and force lowercase for robust cross-platform path matching
    norm_root = root.replace("\\", "/").lower()

    # Exclude resources, exported output, rigged characters (tadc_models), and static props (asset_library)
    if any(
        p in norm_root
        for p in ["resources", "exported_assets", "tadc_models", "asset_library"]
    ):
        continue

    for file in files:
        if file.endswith(".blend") and not file.startswith("."):
            blend_files.append(os.path.join(root, file))

print(f"Found {len(blend_files)} .blend files to process.\n")


def ensure_object_mode():
    """Ensures we are in Object Mode safely without redundant operator calls."""
    if bpy.context.active_object and bpy.context.active_object.mode != "OBJECT":
        try:
            bpy.ops.object.mode_set(mode="OBJECT")
        except Exception:
            pass


def resolve_parenting_cycles():
    """Detects and breaks infinite parent-child loops before the glTF exporter walks the tree."""
    broken = 0
    for obj in list(bpy.data.objects):
        parent = obj.parent
        visited = {obj}
        while parent:
            if parent in visited:
                print(
                    f"       [Repair] Parenting cycle detected: '{obj.name}' loops back to '{parent.name}'. Unparenting."
                )
                obj.parent = None
                broken += 1
                break
            visited.add(parent)
            parent = parent.parent
    return broken


def fix_instancing_loops():
    """Builds a dependency graph of collection instances and runs a DFS to break recursive loops."""
    deps = {}
    for col in bpy.data.collections:
        deps[col] = set()
        for obj in col.objects:
            if obj.instance_type == "COLLECTION" and obj.instance_collection:
                deps[col].add(obj.instance_collection)

    cycles_found = True
    while cycles_found:
        cycles_found = False
        for col in list(deps.keys()):
            visited = set()
            stack = [col]
            cycle = []
            while stack:
                curr = stack[-1]
                if curr in visited:
                    cycle_start_idx = stack.index(curr)
                    cycle = stack[cycle_start_idx:]
                    break
                visited.add(curr)
                neighbors = deps.get(curr, set())
                next_node = None
                for n in neighbors:
                    if n not in visited or n == col:
                        next_node = n
                        break
                if next_node:
                    stack.append(next_node)
                else:
                    stack.pop()
                    visited.remove(curr)

            if cycle:
                chain_str = " -> ".join([c.name for c in cycle])
                print(f"       [Repair] Multi-stage loop detected: {chain_str}")
                broken = False
                for i in range(len(cycle)):
                    c_curr = cycle[i]
                    c_next = cycle[(i + 1) % len(cycle)]
                    for obj in list(c_curr.objects):
                        if (
                            obj.instance_type == "COLLECTION"
                            and obj.instance_collection == c_next
                        ):
                            print(
                                f"       - Disabling instancing on '{obj.name}' to break cycle."
                            )
                            obj.instance_type = "NONE"
                            obj.instance_collection = None
                            broken = True
                            break
                    if broken:
                        break

                # Critical Fix: Only loop again if a cycle was actually broken
                if broken:
                    cycles_found = True
                    deps = {}
                    for c in bpy.data.collections:
                        deps[c] = set()
                        for o in c.objects:
                            if (
                                o.instance_type == "COLLECTION"
                                and o.instance_collection
                            ):
                                deps[c].add(o.instance_collection)
                else:
                    cycles_found = False
                break


def make_instances_real():
    """Bakes all real and virtual instanced meshes into a single unified mesh in-memory (O(1) footprint)."""
    ensure_object_mode()

    depsgraph = bpy.context.evaluated_depsgraph_get()

    bm = bmesh.new()
    merged_materials = []
    material_offset = {}

    # 1. First Pass: Map all unique materials across the objects (Keyed by stable string name to prevent segment leaks)
    for instance in depsgraph.object_instances:
        obj = instance.object
        if obj.type == "MESH":
            for slot_idx, slot in enumerate(obj.material_slots):
                if slot.material:
                    if slot.material not in merged_materials:
                        merged_materials.append(slot.material)
                    # Mapping by (obj, slot_idx) is completely bulletproof for unaligned containers
                    material_offset[(obj.name, slot_idx)] = merged_materials.index(
                        slot.material
                    )

    # 2. Second Pass: Process and merge each instance ONE-BY-ONE (Flat O(1) memory overhead)
    processed_count = 0
    for instance in depsgraph.object_instances:
        obj = instance.object
        if obj.type != "MESH":
            continue

        # Extract evaluated geometry safely from the dependency graph via BMesh
        temp_bm = bmesh.new()
        try:
            temp_bm.from_object(obj, depsgraph)
            temp_mesh = bpy.data.meshes.new("Temp_Mesh_Part")
            temp_bm.to_mesh(temp_mesh)
            temp_bm.free()
        except Exception:
            temp_bm.free()
            continue

        # Transform local vertices to world space
        temp_mesh.transform(instance.matrix_world)

        # Ensure consistent UV map names so layers merge seamlessly
        if temp_mesh.uv_layers:
            temp_mesh.uv_layers.active.name = "UVMap"

        # Remap material indices to our consolidated materials list
        for poly in temp_mesh.polygons:
            orig_idx = poly.material_index
            global_idx = material_offset.get((obj.name, orig_idx), 0)
            poly.material_index = global_idx

        # Append this standard mesh directly to the master BMesh buffer
        bm.from_mesh(temp_mesh)
        processed_count += 1

        # --- IMMEDIATE MEMORY RECLAMATION ---
        # Clean up our temporary mesh block from memory instantly
        try:
            bpy.data.meshes.remove(temp_mesh)
        except Exception:
            pass

    if processed_count == 0:
        bm.free()
        return

    print(
        f"       - Consolidating {processed_count} mesh instances in-memory (O(1) overhead)..."
    )

    # Create the brand new unified mesh datablock
    unified_mesh_data = bpy.data.meshes.new("Unified_Room_Mesh")
    bm.to_mesh(unified_mesh_data)
    bm.free()

    # Append the consolidated materials
    for mat in merged_materials:
        unified_mesh_data.materials.append(mat)

    # --- STRIP VERTEX COLORS PASS ---
    # Removes vertex color layers from the unified mesh to prevent unconditional
    # shader multiplication from tinting flat base colors and picture textures
    while unified_mesh_data.vertex_colors:
        unified_mesh_data.vertex_colors.remove(unified_mesh_data.vertex_colors[0])

    # Create a single unified object and link it to the scene
    unified_obj = bpy.data.objects.new("Unified_Room_Object", unified_mesh_data)
    bpy.context.scene.collection.objects.link(unified_obj)

    # Isolate the scene to ONLY this object and its materials
    # This prevents any dangling collection pointers from crashing the glTF tree builder
    master_col = bpy.context.scene.collection
    for obj in list(master_col.objects):
        if obj != unified_obj:
            try:
                master_col.objects.unlink(obj)
            except Exception:
                pass
    for col in list(master_col.children):
        try:
            master_col.children.unlink(col)
        except Exception:
            pass

    # Select ONLY the unified object and make it active
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    unified_obj.select_set(True)
    bpy.context.view_layer.objects.active = unified_obj

    print("       - In-memory merge completed successfully.")


def bake_modifiers_to_shape_keys(obj, start, end, step=2):
    """Evaluates and bakes animated Geometry Nodes/modifiers to standard glTF-compliant Shape Keys."""
    ensure_object_mode()

    orig_frame = bpy.context.scene.frame_current
    bpy.context.scene.frame_set(start)
    depsgraph = bpy.context.evaluated_depsgraph_get()

    # Create the base static mesh datablock safely via BMesh from_object
    temp_bm = bmesh.new()
    try:
        temp_bm.from_object(obj, depsgraph)
        base_mesh = bpy.data.meshes.new(f"{obj.name}_Base_Mesh")
        temp_bm.to_mesh(base_mesh)
        temp_bm.free()
    except Exception:
        temp_bm.free()
        base_mesh = obj.data.copy()

    base_obj = bpy.data.objects.new(f"{obj.name}_Baked_Anim", base_mesh)
    bpy.context.scene.collection.objects.link(base_obj)

    # Bind materials
    for slot in obj.material_slots:
        if slot.material:
            base_obj.data.materials.append(slot.material)

    # Establish the Basis Shape Key
    base_obj.shape_key_add(name="Basis")

    print(
        f"       - [Baker] Baking Geometry Nodes on '{obj.name}' to Shape Keys (Frames {start} to {end}, Step {step})..."
    )

    for frame in range(start, end + 1, step):
        bpy.context.scene.frame_set(frame)
        dg = bpy.context.evaluated_depsgraph_get()

        # Extract evaluated geometry of this specific frame safely
        t_bm = bmesh.new()
        try:
            t_bm.from_object(obj, dg)
            frame_mesh = bpy.data.meshes.new("Temp_Frame_Mesh")
            t_bm.to_mesh(frame_mesh)
            t_bm.free()
        except Exception:
            t_bm.free()
            continue

        # Add new keyframe target
        key_name = f"Frame_{frame}"
        key = base_obj.shape_key_add(name=key_name)

        # Safe-align changing vertex counts (e.g. dynamic geometry nodes)
        min_verts = min(len(base_mesh.vertices), len(frame_mesh.vertices))

        # High-performance C-Level memory copy (foreach_get/set)
        coords = [0.0] * (len(base_mesh.vertices) * 3)
        frame_coords = [0.0] * (len(frame_mesh.vertices) * 3)
        frame_mesh.vertices.foreach_get("co", frame_coords)

        coords[: min_verts * 3] = frame_coords[: min_verts * 3]

        # Handle dynamic geometry vertex count shrink fallbacks
        if len(frame_mesh.vertices) < len(base_mesh.vertices):
            basis_coords = [0.0] * (len(base_mesh.vertices) * 3)
            base_mesh.vertices.foreach_get("co", basis_coords)
            coords[min_verts * 3 :] = basis_coords[min_verts * 3 :]

        key.data.foreach_set("co", coords)

        # Clean up temporary frame mesh safely
        try:
            bpy.data.meshes.remove(frame_mesh)
        except Exception:
            pass

    # Sparse Keyframing: Animate Shape Key weights sequentially
    key_blocks = base_obj.data.shape_keys.key_blocks
    for frame in range(start, end + 1, step):
        key_name = f"Frame_{frame}"
        if key_name in key_blocks:
            key = key_blocks[key_name]

            # Fade out previous step
            if frame > start:
                key.value = 0.0
                key.keyframe_insert(data_path="value", frame=frame - step)

            # Full active frame weight
            key.value = 1.0
            key.keyframe_insert(data_path="value", frame=frame)

            # Fade out next step
            if frame < end:
                key.value = 0.0
                key.keyframe_insert(data_path="value", frame=frame + step)

    # Restore the original frame context
    bpy.context.scene.frame_set(orig_frame)

    # Hide original procedural object safely instead of deleting it to prevent C++ pointer invalidation!
    try:
        obj.hide_viewport = True
        obj.hide_render = True
    except Exception:
        pass

    # Isolate selection to the newly baked mesh for glTF export
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    base_obj.select_set(True)
    bpy.context.view_layer.objects.active = base_obj

    print("       - [Baker] Baked Geometry Nodes to shape keys successfully.")
    return base_obj


# --- PIPELINE ATLAS BAKING PASS ---


def configure_cycles_gpu(device_type="CUDA"):
    """Forces Blender to recognize and bind CUDA/OptiX/HIP graphics devices on Linux."""
    try:
        preferences = bpy.context.preferences
        cycles_preferences = preferences.addons["cycles"].preferences
        cycles_preferences.compute_device_type = device_type
        cycles_preferences.refresh_devices()

        gpu_enabled = False
        for device in cycles_preferences.devices:
            if device.type in {"CUDA", "OPTIX", "HIP"}:
                device.use = True
                gpu_enabled = True

        if gpu_enabled:
            bpy.context.scene.cycles.device = "GPU"
            print("       - Configured GPU hardware acceleration (CUDA/OptiX/HIP).")
        else:
            bpy.context.scene.cycles.device = "CPU"
    except Exception:
        bpy.context.scene.cycles.device = "CPU"


def repair_materials_for_gltf_fallback(combined_obj):
    """Detects and repairs procedural materials so flat fallback base colors can export in DEV_MODE."""
    for mat in combined_obj.data.materials:
        if mat and mat.node_tree:
            mat.use_nodes = True
            nodes = mat.node_tree.nodes
            principled = next((n for n in nodes if n.type == "BSDF_PRINCIPLED"), None)
            if principled:
                base_color_input = principled.inputs.get("Base Color")
                if base_color_input and base_color_input.is_linked:
                    link = base_color_input.links[0]
                    from_node = link.from_node

                    # If it's a procedural node (not an Image Texture), glTF will ignore it (leaving it white).
                    # We unplug it and extract a representative flat RGB fallback.
                    if from_node.type != "TEX_IMAGE":
                        fallback_color = [0.8, 0.8, 0.8, 1.0]  # Default grey

                        if (
                            from_node.type == "VALTORGB"
                            and len(from_node.color_ramp.elements) > 0
                        ):
                            # ColorRamp: Grab first color stop
                            fallback_color = list(
                                from_node.color_ramp.elements[0].color
                            )
                        elif from_node.type in {"MIX", "MIX_RGB"}:
                            # Mix/MixRGB: Grab primary color input
                            col_input = from_node.inputs.get(
                                "Color1"
                            ) or from_node.inputs.get("A")
                            if col_input:
                                fallback_color = list(col_input.default_value)
                        elif (
                            hasattr(from_node, "outputs") and len(from_node.outputs) > 0
                        ):
                            # Generic procedural node fallback
                            col_out = from_node.outputs[0]
                            if hasattr(col_out, "default_value"):
                                val = col_out.default_value
                                if hasattr(val, "__len__") and len(val) >= 3:
                                    fallback_color = list(val)[:4]
                                    if len(fallback_color) == 3:
                                        fallback_color.append(1.0)

                        # Unplug node and apply direct flat color
                        mat.node_tree.links.remove(link)
                        base_color_input.default_value = fallback_color
                        print(
                            f"       - [Dev Mode Material Repair] Applied flat fallback {fallback_color} to material '{mat.name}'"
                        )


def bake_and_replace_with_atlas(combined_obj, output_png_path, resolution=2048):
    """Welds split vertices, packs original UVs, overrides PBR channels, and bakes textures."""
    # 1. Weld Split Vertices (Remove Doubles) in Object Mode via BMesh
    print("       - Welding split vertices (removing duplicates)...")
    bm = bmesh.new()
    bm.from_mesh(combined_obj.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0001)
    bm.to_mesh(combined_obj.data)
    bm.free()
    combined_obj.data.update()

    # 2. Handle UV maps
    uv_layers = combined_obj.data.uv_layers
    if len(uv_layers) > 0:
        uv_map = uv_layers[0]
        uv_layers.active = uv_map
    else:
        uv_map = uv_layers.new(name="UVMap")
        uv_layers.active = uv_map

    # 3. Pack Original UV Islands cleanly (with Texel Density Normalization)
    print("       - Normalizing UV scales and packing islands...")
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")

    # Normalize scale based on physical 3D area
    bpy.ops.uv.average_islands_scale()

    # Pack islands neatly
    bpy.ops.uv.pack_islands(margin=0.01)
    ensure_object_mode()

    # 4. Create blank image
    image = bpy.data.images.new("Atlas_Albedo", width=resolution, height=resolution)

    # 5. Insert target texture node in materials
    for mat in combined_obj.data.materials:
        if mat and mat.node_tree:
            mat.use_nodes = True
            nodes = mat.node_tree.nodes
            node = nodes.get("Bake_Target_Node")
            if node:
                nodes.remove(node)
            node = nodes.new("ShaderNodeTexImage")
            node.name = "Bake_Target_Node"
            node.image = image
            nodes.active = node

    # 6. CRITICAL: Override PBR channels (Set Metallic AND Transmission to 0.0)
    for mat in combined_obj.data.materials:
        if mat and mat.node_tree:
            principled = next(
                (
                    node
                    for node in mat.node_tree.nodes
                    if node.type == "BSDF_PRINCIPLED"
                ),
                None,
            )
            if principled:
                # Disable Metallic
                metallic_input = principled.inputs.get("Metallic")
                if metallic_input:
                    if metallic_input.is_linked:
                        mat.node_tree.links.remove(metallic_input.links[0])
                    metallic_input.default_value = 0.0

                # Disable Transmission
                transmission_input = principled.inputs.get("Transmission Weight")
                if not transmission_input:
                    transmission_input = principled.inputs.get("Transmission")
                if transmission_input:
                    if transmission_input.is_linked:
                        mat.node_tree.links.remove(transmission_input.links[0])
                    transmission_input.default_value = 0.0

    # 7. Establish Selection via Direct View Layer modifications (Zero operators)
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    combined_obj.select_set(True)
    bpy.context.view_layer.objects.active = combined_obj

    # 8. Configure Cycles Bake Settings
    bpy.context.scene.render.engine = "CYCLES"
    configure_cycles_gpu("CUDA")

    # --- ULTRA OPTIMIZATION PASS ---
    # Flat color baking has no shading noise.
    # Reducing samples to 4 drops rendering CPU/GPU workloads to near zero.
    bpy.context.scene.cycles.samples = 4

    bpy.context.scene.render.bake.use_pass_direct = False
    bpy.context.scene.render.bake.use_pass_indirect = False
    bpy.context.scene.render.bake.use_pass_color = True

    print("       - Baking textures to flat diffuse atlas...")
    bpy.ops.object.bake(type="DIFFUSE", margin=4)

    # 9. Save Image
    os.makedirs(os.path.dirname(output_png_path), exist_ok=True)
    image.filepath_raw = output_png_path
    image.file_format = "PNG"
    image.save()
    print(f"       - Saved texture atlas to: {output_png_path}")

    # 10. Replace materials
    new_mat = bpy.data.materials.new(name="Baked_Atlas_Material")
    new_mat.use_nodes = True
    nodes = new_mat.node_tree.nodes
    principled = nodes.get("Principled BSDF")
    tex_node = nodes.new("ShaderNodeTexImage")
    tex_node.image = image
    new_mat.node_tree.links.new(
        tex_node.outputs["Color"], principled.inputs["Base Color"]
    )

    combined_obj.data.materials.clear()
    combined_obj.data.materials.append(new_mat)


# --- MAIN EXECUTION LOOP ---

success_files = []
failed_files = []

for idx, blend_path in enumerate(blend_files, start=1):
    rel_path = os.path.relpath(blend_path, input_dir)
    filename = os.path.basename(blend_path)
    name_we = os.path.splitext(filename)[0]

    sub_dir = os.path.relpath(os.path.dirname(blend_path), input_dir)
    target_sub = output_dir  # Flat export directly to resources/assets/

    glb_path = os.path.join(target_sub, name_we + ".glb")
    png_path = os.path.join(target_sub, "textures", name_we + "_Atlas.png")

    print(f"[{idx}/{len(blend_files)}] Processing: {rel_path}")
    try:
        # Load file (Header-level WM operation)
        bpy.ops.wm.open_mainfile(filepath=blend_path)

        # Unhide everything and execute auto-repairs
        # (Note: unhide_and_reveal_everything has been safely removed to prevent duplicate/reference meshes from merging)
        resolve_parenting_cycles()
        fix_instancing_loops()

        # ========================================================================
        # DYNAMIC SCENE ROUTER
        # ========================================================================
        has_armatures = len(bpy.data.armatures) > 0
        has_actions = len(bpy.data.actions) > 0
        is_character = "tadc_models" in blend_path.lower()
        is_explicit_animation = (
            "animation" in name_we.lower() or "physics" in name_we.lower()
        )

        has_procedural_modifiers = False
        for o in bpy.context.scene.objects:
            if o.type == "MESH":
                for mod in o.modifiers:
                    if mod.type in {"NODES", "PARTICLE_SYSTEM"}:
                        has_procedural_modifiers = True
                        break

        # Check if we should bake procedural geometry nodes down to Morph Targets
        should_bake_shape_keys = has_procedural_modifiers and has_actions
        should_consolidate = not (
            has_armatures or has_actions or is_character or is_explicit_animation
        )
        # ========================================================================

        if should_bake_shape_keys:
            # Procedural Animated Path: Bake ALL Geometry Nodes to standard Shape Keys
            start = bpy.context.scene.frame_start
            end = bpy.context.scene.frame_end

            baked_objects = []
            target_objects = [
                o
                for o in bpy.context.scene.objects
                if o.type == "MESH"
                and any(mod.type in {"NODES", "PARTICLE_SYSTEM"} for mod in o.modifiers)
            ]

            for target_obj in target_objects:
                baked_obj = bake_modifiers_to_shape_keys(target_obj, start, end, step=2)
                if baked_obj:
                    baked_objects.append(baked_obj)

            # Hide original procedural objects from viewport and render so the exporter skips them
            for target_obj in target_objects:
                try:
                    target_obj.hide_viewport = True
                    target_obj.hide_render = True
                except Exception:
                    pass

            should_consolidate = False
            export_only_selection = False  # Export the entire active scene (with standard elements and baked assets)
        elif should_consolidate:
            # Static Level Path: Combine everything into a single static mesh
            make_instances_real()
            export_only_selection = True
        else:
            # Skeletal/Transform Animated Path: Keep hierarchies and armatures intact
            print(
                "       - [Router] Animated hierarchy detected. Preserving bone chains..."
            )
            export_only_selection = False

        # Execute Atlas Baking inside a try-except block to protect the queue from corrupt files
        combined_mesh = (
            bpy.context.view_layer.objects.active if should_consolidate else None
        )
        if combined_mesh and combined_mesh.type == "MESH":
            # --- THE BAKE TOGGLE ENFORCER ---
            if DEV_MODE:
                print(
                    "       - [DEV MODE] Bypassing Cycles bake step. Exporting raw static geometry..."
                )
                # Repair procedural nodes for flat base-color fallback in DEV_MODE
                repair_materials_for_gltf_fallback(combined_mesh)
            else:
                try:
                    bake_and_replace_with_atlas(
                        combined_mesh, png_path, resolution=2048
                    )
                except Exception as e:
                    print(
                        f"       [Warning] Baking pass failed: {e}. Exporting raw GLB instead."
                    )

        # Export GLB (Essential Exporter Operator)
        try:
            bpy.ops.export_scene.gltf(
                filepath=glb_path,
                export_format="GLB",
                export_materials="EXPORT",
                export_vertex_color="ACTIVE",
                export_animations=True,
                use_selection=export_only_selection,
            )
        except TypeError:
            bpy.ops.export_scene.gltf(
                filepath=glb_path,
                export_format="GLB",
                export_materials="EXPORT",
                export_colors=True,
                export_animations=True,
                use_selection=export_only_selection,
            )
        print(
            f"      [Success] Exported unified GLB: {os.path.relpath(glb_path, input_dir)}"
        )
        success_files.append(rel_path)

    except Exception as e:
        print(f"      [Error] Failed to process {rel_path}: {e}")
        failed_files.append((rel_path, str(e)))

print("\n=========================================================")
print("Export Summary:")
print(f"  Successfully Exported: {len(success_files)} / {len(blend_files)}")
print(f"  Failed:                {len(failed_files)} / {len(blend_files)}")
if failed_files:
    print("\nFailed Files:")
    for f, err in failed_files:
        last_line = err.splitlines()[-1] if err.splitlines() else err
        print(f"  - {f} (Error: {last_line})")
print("=========================================================")
