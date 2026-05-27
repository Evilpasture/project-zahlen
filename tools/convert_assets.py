import os
import sys
import bpy

# Set stack recursion limit
sys.setrecursionlimit(15000)

# ========================================================================
# PIPELINE CONFIGURATION FLAGS
# ========================================================================
# Set to True during active C++ coding to bypass the slow Cycles bake.
# This will let you export all 78 models as static .glb meshes in under 30 seconds!
# Set to False only when you are preparing a Release build to generate high-quality atlases.
DEV_MODE = True
# ========================================================================

# Input and output directory setups
input_dir = os.getcwd()
output_dir = os.path.join(input_dir, "exported_assets")
os.makedirs(output_dir, exist_ok=True)

print("=========================================================")
print("Zahlen Engine Asset Exporter (With Automatic Atlas Baking)")
print("Target directories:")
print(f"  Input:  {input_dir}")
print(f"  Output: {output_dir}")
print("=========================================================")

# 1. Walk through directories to find all .blend files
blend_files = []
for root, _, files in os.walk(input_dir):
    if "exported_assets" in root:
        continue
    for file in files:
        if file.endswith(".blend") and not file.startswith("."):
            blend_files.append(os.path.join(root, file))

print(f"Found {len(blend_files)} .blend files to process.\n")

# --- ADVANCED AUTO-REPAIR PASSES ---


def unhide_and_reveal_everything():
    """Unhides all objects so operators can select and process them."""
    for obj in bpy.data.objects:
        obj.hide_viewport = False
        try:
            obj.hide_set(False)
        except Exception:
            pass


def resolve_parenting_cycles():
    """Detects and breaks infinite parent-child loops."""
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
    """Builds a dependency graph of collection instances and runs a DFS to break loops."""
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

                cycles_found = True
                deps = {}
                for c in bpy.data.collections:
                    deps[c] = set()
                    for o in c.objects:
                        if o.instance_type == "COLLECTION" and o.instance_collection:
                            deps[c].add(o.instance_collection)
                break


def flatten_parenting_tree():
    """Unparents meshes while baking their absolute world positions."""
    active_obj = bpy.context.view_layer.objects.active
    if active_obj and active_obj.hide_viewport:
        valid_active = None
        for obj in bpy.context.scene.objects:
            if not obj.hide_viewport:
                valid_active = obj
                break
        bpy.context.view_layer.objects.active = valid_active

    if bpy.ops.object.mode_set.poll():
        try:
            bpy.ops.object.mode_set(mode="OBJECT")
        except Exception:
            pass

    bpy.ops.object.select_all(action="DESELECT")

    count = 0
    for obj in bpy.context.scene.objects:
        if obj.parent and obj.type in {"MESH", "EMPTY"}:
            try:
                obj.select_set(True)
                count += 1
            except Exception:
                pass

    if count > 0:
        try:
            bpy.ops.object.parent_clear(type="CLEAR_KEEP_TRANSFORM")
            print(
                f"       - Flattened parenting tree for {count} objects (baked world transforms)."
            )
        except Exception:
            pass


def make_instances_real():
    """Converts virtual/instanced assets to static meshes and joins them."""
    active_obj = bpy.context.view_layer.objects.active
    if active_obj and active_obj.hide_viewport:
        valid_active = None
        for obj in bpy.context.scene.objects:
            if not obj.hide_viewport:
                valid_active = obj
                break
        bpy.context.view_layer.objects.active = valid_active

    if bpy.ops.object.mode_set.poll():
        try:
            bpy.ops.object.mode_set(mode="OBJECT")
        except Exception:
            pass

    bpy.ops.object.select_all(action="DESELECT")
    for obj in bpy.context.scene.objects:
        try:
            obj.select_set(True)
        except Exception:
            pass

    try:
        bpy.ops.object.duplicates_make_real()
        print(
            "       - Successfully converted all virtual instances to static objects."
        )

        # Consolidation Pass
        selected_objs = [
            o for o in bpy.context.scene.objects if o.select_get() and o.type == "MESH"
        ]
        if len(selected_objs) > 1:
            bpy.context.view_layer.objects.active = selected_objs[0]
            try:
                bpy.ops.object.join()
                print(
                    f"       - Consolidated {len(selected_objs)} duplicate meshes into a single mesh."
                )
            except Exception as e:
                print(f"       [Warning] Duplicate consolidation failed: {e}")
    except Exception:
        pass


def cleanup_original_emitters():
    """Strips instancing settings and procedural modifiers from original emitters."""
    disabled_instancing = 0
    removed_modifiers = 0

    for obj in list(bpy.data.objects):
        if obj.instance_type != "NONE":
            obj.instance_type = "NONE"
            obj.instance_collection = None
            disabled_instancing += 1

        for mod in list(obj.modifiers):
            if mod.type in {"PARTICLE_SYSTEM", "NODES"}:
                try:
                    obj.modifiers.remove(mod)
                    removed_modifiers += 1
                except Exception:
                    pass

    if disabled_instancing > 0 or removed_modifiers > 0:
        print(
            f"       - Stripped original emitters: disabled {disabled_instancing} instancers, removed {removed_modifiers} procedural modifiers."
        )


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


def bake_and_replace_with_atlas(combined_obj, output_png_path, resolution=2048):
    """Welds split vertices, packs original UVs, overrides PBR channels, and bakes textures."""
    # 1. Weld Split Vertices (Remove Doubles)
    print("       - Welding split vertices (removing duplicates)...")
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.remove_doubles(threshold=0.0001)
    bpy.ops.object.mode_set(mode="OBJECT")

    # 2. Clean Up Empty Material Slots
    slots = combined_obj.material_slots
    for i in reversed(range(len(slots))):
        if not slots[i].material:
            combined_obj.active_material_index = i
            bpy.ops.object.material_slot_remove()

    # 3. Handle UV maps
    uv_layers = combined_obj.data.uv_layers
    if len(uv_layers) > 0:
        uv_map = uv_layers[0]
        uv_layers.active = uv_map
    else:
        uv_map = uv_layers.new(name="UVMap")
        uv_layers.active = uv_map

    # 4. Pack Original UV Islands cleanly (with Texel Density Normalization)
    print("       - Normalizing UV scales and packing islands...")
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")

    # Normalize scale based on physical 3D area
    bpy.ops.uv.average_islands_scale()

    # Pack islands neatly
    bpy.ops.uv.pack_islands(margin=0.01)
    bpy.ops.object.mode_set(mode="OBJECT")

    # 5. Create blank image
    image = bpy.data.images.new("Atlas_Albedo", width=resolution, height=resolution)

    # 6. Insert target texture node in materials
    for mat in combined_obj.data.materials:
        if mat and mat.node_tree:
            mat.use_nodes = True
            nodes = mat.node_tree.nodes
            if "Bake_Target_Node" in nodes:
                nodes.remove(nodes["Bake_Target_Node"])
            node = nodes.new("ShaderNodeTexImage")
            node.name = "Bake_Target_Node"
            node.image = image
            nodes.active = node

    # 7. CRITICAL: Override PBR channels (Set Metallic AND Transmission to 0.0)
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

    # 8. Establish Selection
    bpy.ops.object.select_all(action="DESELECT")
    combined_obj.select_set(True)
    bpy.context.view_layer.objects.active = combined_obj

    # 9. Configure Cycles Bake Settings
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

    # 10. Save Image
    os.makedirs(os.path.dirname(output_png_path), exist_ok=True)
    image.filepath_raw = output_png_path
    image.file_format = "PNG"
    image.save()
    print(f"       - Saved texture atlas to: {output_png_path}")

    # 11. Replace materials
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
    target_sub = os.path.join(output_dir, sub_dir)
    os.makedirs(target_sub, exist_ok=True)

    glb_path = os.path.join(target_sub, name_we + ".glb")
    png_path = os.path.join(target_sub, "textures", name_we + "_Atlas.png")

    print(f"[{idx}/{len(blend_files)}] Processing: {rel_path}")
    try:
        # Load file
        bpy.ops.wm.open_mainfile(filepath=blend_path)

        # Unhide everything and execute auto-repairs
        unhide_and_reveal_everything()
        p_cycles = resolve_parenting_cycles()
        fix_instancing_loops()
        flatten_parenting_tree()

        make_instances_real()  # Generates static geometry and joins it into one mesh
        cleanup_original_emitters()

        # Execute Atlas Baking inside a try-except block to protect the queue from corrupt files
        combined_mesh = bpy.context.view_layer.objects.active
        if combined_mesh and combined_mesh.type == "MESH":
            # --- THE BAKE TOGGLE ENFORCER ---
            if DEV_MODE:
                print(
                    "       - [DEV MODE] Bypassing Cycles bake step. Exporting raw static geometry..."
                )
            else:
                try:
                    bake_and_replace_with_atlas(
                        combined_mesh, png_path, resolution=2048
                    )
                except Exception as e:
                    print(
                        f"       [Warning] Baking pass failed: {e}. Exporting raw GLB instead."
                    )

        # Export GLB
        try:
            bpy.ops.export_scene.gltf(
                filepath=glb_path,
                export_format="GLB",
                export_materials="EXPORT",
                export_vertex_color="ACTIVE",
                export_animations=True,
            )
        except TypeError:
            bpy.ops.export_scene.gltf(
                filepath=glb_path,
                export_format="GLB",
                export_materials="EXPORT",
                export_colors=True,
                export_animations=True,
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
