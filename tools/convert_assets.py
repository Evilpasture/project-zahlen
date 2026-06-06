# tools/convert_assets.py
import os
import sys
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed

# Safeguard imports to run under native Python (dispatcher) and Blender Python (worker)
try:
    import bpy
    import bmesh

    INSIDE_BLENDER = True
except ImportError:
    INSIDE_BLENDER = False

# Set stack recursion limit (applied inside worker instances)
if INSIDE_BLENDER:
    sys.setrecursionlimit(15000)

# ========================================================================
# PIPELINE CONFIGURATION FLAGS
# ========================================================================
# Set to True during active C++ coding to bypass the slow Cycles bake.
# Set to False only when preparing a Release build to generate high-quality atlases.
DEV_MODE = True
# ========================================================================


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
    """Converts visible instances to real objects natively and cleans up hidden objects to prevent export bloat."""
    ensure_object_mode()

    # 1. Purge all hidden objects immediately to free RAM and avoid processing them
    for obj in list(bpy.context.scene.objects):
        try:
            if not obj.visible_get():
                bpy.data.objects.remove(obj, do_unlink=True)
        except Exception:
            pass

    # 2. Select all remaining visible objects
    for obj in bpy.context.scene.objects:
        obj.select_set(True)

    if bpy.context.scene.objects:
        bpy.context.view_layer.objects.active = bpy.context.scene.objects[0]

    # 3. Natively make all visible instances real
    try:
        bpy.ops.object.duplicates_make_real()
    except Exception as e:
        print(f"       - [Notice] duplicates_make_real: {e}")

    ensure_object_mode()

    # 4. Clean up any remaining non-mesh helper objects (Empties, curves, lights, cameras)
    for obj in list(bpy.context.scene.objects):
        if obj.type not in {"MESH", "ARMATURE"}:
            try:
                bpy.data.objects.remove(obj, do_unlink=True)
            except Exception:
                pass

    # Force Python GC to free memory immediately
    import gc

    gc.collect()

    print("       - Converted instances to real mesh objects natively.")


def bake_modifiers_to_shape_keys(obj, start, end, step=2):
    """Evaluates and bakes animated Geometry Nodes/modifiers to standard glTF-compliant Shape Keys."""
    ensure_object_mode()

    orig_frame = bpy.context.scene.frame_current
    bpy.context.scene.frame_set(start)
    depsgraph = bpy.context.evaluated_depsgraph_get()

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

    # Copy parent and transformations natively
    base_obj.parent = obj.parent
    base_obj.matrix_parent_inverse = obj.matrix_parent_inverse.copy()
    base_obj.matrix_local = obj.matrix_local.copy()

    # Copy visibility settings
    base_obj.hide_viewport = obj.hide_viewport
    base_obj.hide_render = obj.hide_render

    # Copy animation data natively
    if obj.animation_data and obj.animation_data.action:
        base_obj.animation_data_create()
        base_obj.animation_data.action = obj.animation_data.action

    for slot in obj.material_slots:
        if slot.material:
            base_obj.data.materials.append(slot.material)

    base_obj.shape_key_add(name="Basis")

    print(
        f"       - [Baker] Baking Geometry Nodes on '{obj.name}' to Shape Keys (Frames {start} to {end}, Step {step})..."
    )

    for frame in range(start, end + 1, step):
        bpy.context.scene.frame_set(frame)
        dg = bpy.context.evaluated_depsgraph_get()

        t_bm = bmesh.new()
        try:
            t_bm.from_object(obj, dg)
            frame_mesh = bpy.data.meshes.new("Temp_Frame_Mesh")
            t_bm.to_mesh(frame_mesh)
            t_bm.free()
        except Exception:
            t_bm.free()
            continue

        key_name = f"Frame_{frame}"
        key = base_obj.shape_key_add(name=key_name)

        min_verts = min(len(base_mesh.vertices), len(frame_mesh.vertices))

        coords = [0.0] * (len(base_mesh.vertices) * 3)
        frame_coords = [0.0] * (len(frame_mesh.vertices) * 3)
        frame_mesh.vertices.foreach_get("co", frame_coords)

        coords[: min_verts * 3] = frame_coords[: min_verts * 3]

        if len(frame_mesh.vertices) < len(base_mesh.vertices):
            basis_coords = [0.0] * (len(base_mesh.vertices) * 3)
            base_mesh.vertices.foreach_get("co", basis_coords)
            coords[min_verts * 3 :] = basis_coords[min_verts * 3 :]

        key.data.foreach_set("co", coords)

        try:
            bpy.data.meshes.remove(frame_mesh)
        except Exception:
            pass

    key_blocks = base_obj.data.shape_keys.key_blocks
    for frame in range(start, end + 1, step):
        key_name = f"Frame_{frame}"
        if key_name in key_blocks:
            key = key_blocks[key_name]

            if frame > start:
                key.value = 0.0
                key.keyframe_insert(data_path="value", frame=frame - step)

            key.value = 1.0
            key.keyframe_insert(data_path="value", frame=frame)

            if frame < end:
                key.value = 0.0
                key.keyframe_insert(data_path="value", frame=frame + step)

    bpy.context.scene.frame_set(orig_frame)

    try:
        obj.hide_viewport = True
        obj.hide_render = True
    except Exception:
        pass

    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    base_obj.select_set(True)
    bpy.context.view_layer.objects.active = base_obj

    print("       - [Baker] Baked Geometry Nodes to shape keys successfully.")
    return base_obj


def configure_cycles_gpu(device_type="CUDA"):
    """Forces Blender to recognize and bind CUDA/OptiX/HIP graphics devices on Linux/Windows."""
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

                    if from_node.type != "TEX_IMAGE":
                        fallback_color = [0.8, 0.8, 0.8, 1.0]

                        if (
                            from_node.type == "VALTORGB"
                            and len(from_node.color_ramp.elements) > 0
                        ):
                            fallback_color = list(
                                from_node.color_ramp.elements[0].color
                            )
                        elif from_node.type in {"MIX", "MIX_RGB"}:
                            col_input = from_node.inputs.get(
                                "Color1"
                            ) or from_node.inputs.get("A")
                            if col_input:
                                fallback_color = list(col_input.default_value)
                        elif (
                            hasattr(from_node, "outputs") and len(from_node.outputs) > 0
                        ):
                            col_out = from_node.outputs[0]
                            if hasattr(col_out, "default_value"):
                                val = col_out.default_value
                                if hasattr(val, "__len__") and len(val) >= 3:
                                    fallback_color = list(val)[:4]
                                    if len(fallback_color) == 3:
                                        fallback_color.append(1.0)

                        mat.node_tree.links.remove(link)
                        base_color_input.default_value = fallback_color
                        print(
                            f"       - [Dev Mode Material Repair] Applied flat fallback {fallback_color} to material '{mat.name}'"
                        )


def bake_and_replace_with_atlas(combined_obj, output_png_path, resolution=2048):
    """Welds split vertices, packs original UVs, overrides PBR channels, and bakes textures."""
    print("       - Welding split vertices (removing duplicates)...")
    bm = bmesh.new()
    bm.from_mesh(combined_obj.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0001)
    bm.to_mesh(combined_obj.data)
    bm.free()
    combined_obj.data.update()

    uv_layers = combined_obj.data.uv_layers
    if len(uv_layers) > 0:
        uv_map = uv_layers[0]
        uv_layers.active = uv_map
    else:
        uv_map = uv_layers.new(name="UVMap")
        uv_layers.active = uv_map

    print("       - Normalizing UV scales and packing islands...")
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.average_islands_scale()
    bpy.ops.uv.pack_islands(margin=0.01)
    ensure_object_mode()

    image = bpy.data.images.new("Atlas_Albedo", width=resolution, height=resolution)

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
                metallic_input = principled.inputs.get("Metallic")
                if metallic_input:
                    if metallic_input.is_linked:
                        mat.node_tree.links.remove(metallic_input.links[0])
                    metallic_input.default_value = 0.0

                transmission_input = principled.inputs.get("Transmission Weight")
                if not transmission_input:
                    transmission_input = principled.inputs.get("Transmission")
                if transmission_input:
                    if transmission_input.is_linked:
                        mat.node_tree.links.remove(transmission_input.links[0])
                    transmission_input.default_value = 0.0

    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    combined_obj.select_set(True)
    bpy.context.view_layer.objects.active = combined_obj

    bpy.context.scene.render.engine = "CYCLES"
    configure_cycles_gpu("CUDA")

    bpy.context.scene.cycles.samples = 4
    bpy.context.scene.render.bake.use_pass_direct = False
    bpy.context.scene.render.bake.use_pass_indirect = False
    bpy.context.scene.render.bake.use_pass_color = True

    print("       - Baking textures to flat diffuse atlas...")
    bpy.ops.object.bake(type="DIFFUSE", margin=4)

    os.makedirs(os.path.dirname(output_png_path), exist_ok=True)
    image.filepath_raw = output_png_path
    image.file_format = "PNG"
    image.save()
    print(f"       - Saved texture atlas to: {output_png_path}")

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


# --- SINGLE WORKER EXECUTION FLOW (Runs inside spawned Blender) ---


def run_worker_pipeline(blend_path, dev_mode=True):
    """Processes exactly one .blend file. Called within individual Blender background processes."""
    blend_path = os.path.abspath(blend_path)
    input_dir = os.getcwd()
    output_dir = os.path.join(input_dir, "resources", "assets")

    filename = os.path.basename(blend_path)
    name_we = os.path.splitext(filename)[0]

    glb_path = os.path.join(output_dir, name_we + ".glb")
    png_path = os.path.join(output_dir, "textures", name_we + "_Atlas.png")

    try:
        # Load file cleanly as data (bypasses viewport shader compilation hangs)
        bpy.ops.wm.open_mainfile(filepath=blend_path, load_ui=False, use_scripts=False)

        resolve_parenting_cycles()
        fix_instancing_loops()

        # Identify if this is explicitly a character asset or a dedicated animation file
        is_character = (
            "tadc_models" in blend_path.lower()
            or "character" in name_we.lower()
            or "pomni" in name_we.lower()
            or "zooble" in name_we.lower()
        )
        is_explicit_animation = (
            "animation" in name_we.lower() or "physics" in name_we.lower()
        )

        has_actions = len(bpy.data.actions) > 0
        has_procedural_modifiers = False
        for o in bpy.context.scene.objects:
            if o.type == "MESH":
                for mod in o.modifiers:
                    if mod.type in {"NODES", "PARTICLE_SYSTEM"}:
                        has_procedural_modifiers = True
                        break

        # Always consolidate environments/rooms so modifiers inside collection instances
        # are baked, and procedural materials are repaired.
        should_consolidate = not (is_character or is_explicit_animation)
        should_bake_shape_keys = (
            has_procedural_modifiers and has_actions and not should_consolidate
        )

        if should_bake_shape_keys:
            start = bpy.context.scene.frame_start
            end = bpy.context.scene.frame_end

            target_objects = [
                o
                for o in bpy.context.scene.objects
                if o.type == "MESH"
                and any(mod.type in {"NODES", "PARTICLE_SYSTEM"} for mod in o.modifiers)
            ]

            for target_obj in target_objects:
                bake_modifiers_to_shape_keys(target_obj, start, end, step=2)

            for target_obj in target_objects:
                try:
                    target_obj.hide_viewport = True
                    target_obj.hide_render = True
                except Exception:
                    pass

            should_consolidate = False
            export_only_selection = False
        elif should_consolidate:
            make_instances_real()
            export_only_selection = False  # Export full scene natively containing only our real visible meshes
        else:
            print("       - Preserve animated hierarchies.")
            export_only_selection = False

        if should_consolidate:
            if dev_mode:
                # Natively evaluate and repair flat fallback materials on all remaining meshes individually
                for obj in bpy.context.scene.objects:
                    if obj.type == "MESH":
                        repair_materials_for_gltf_fallback(obj)
            else:
                # In Release mode, join them into a single mesh and bake the texture atlas
                ensure_object_mode()
                for o in bpy.context.view_layer.objects:
                    o.select_set(False)

                mesh_objs = [o for o in bpy.context.scene.objects if o.type == "MESH"]
                for o in mesh_objs:
                    o.select_set(True)

                if mesh_objs:
                    bpy.context.view_layer.objects.active = mesh_objs[0]
                    try:
                        bpy.ops.object.join()
                        combined_mesh = bpy.context.view_layer.objects.active
                        bake_and_replace_with_atlas(
                            combined_mesh, png_path, resolution=2048
                        )
                    except Exception as e:
                        print(
                            f"       [Warning] Baking pass failed: {e}. Defaulting to raw export."
                        )

        try:
            bpy.ops.export_scene.gltf(
                filepath=glb_path,
                export_format="GLB",
                export_materials="EXPORT",
                export_vertex_color="ACTIVE",
                export_animations=not should_consolidate,  # Disable animation baking for static consolidated rooms
                use_selection=export_only_selection,
            )
        except TypeError:
            bpy.ops.export_scene.gltf(
                filepath=glb_path,
                export_format="GLB",
                export_materials="EXPORT",
                export_colors=True,
                export_animations=not should_consolidate,  # Disable animation baking for static consolidated rooms
                use_selection=export_only_selection,
            )

        print(f"      [Success] Exported unified GLB: {os.path.basename(glb_path)}")
        print(
            "[PipelineStatus] Success"
        )  # Token verified by dispatcher to confirm completion
        return True

    except Exception as e:
        print(f"      [Error] Processing failed: {e}")
        import traceback

        traceback.print_exc()
        return False


# --- MASTER DISPATCHER SYSTEM (Runs inside native OS python command prompt) ---


def find_blender():
    """Locates the Blender system executable dynamically."""
    if "BLENDER_PATH" in os.environ:
        return os.environ["BLENDER_PATH"]

    import shutil

    shutil_path = shutil.which("blender")
    if shutil_path:
        return shutil_path

    if sys.platform == "win32":
        paths = [
            r"C:\Program Files\Blender Foundation\Blender 4.2\blender.exe",
            r"C:\Program Files\Blender Foundation\Blender 4.1\blender.exe",
            r"C:\Program Files\Blender Foundation\Blender 4.0\blender.exe",
            r"C:\Program Files\Blender Foundation\Blender 3.6\blender.exe",
        ]
        for p in paths:
            if os.path.exists(p):
                return p
    elif sys.platform == "darwin":
        path = "/Applications/Blender.app/Contents/MacOS/Blender"
        if os.path.exists(path):
            return path

    raise FileNotFoundError(
        "Could not automatically locate the 'blender' executable on this system.\n"
        "Please add Blender to your system PATH or configure the 'BLENDER_PATH' environment variable."
    )


def run_worker_process(blender_bin, script_path, blend_path, dev_mode):
    """Launches an isolated headless Blender CLI instance and streams its output in real-time."""
    cmd = [
        blender_bin,
        "--background",
        "--python",
        script_path,
        "--",
        "--worker",
        blend_path,
    ]
    if dev_mode:
        cmd.append("--dev")
    else:
        cmd.append("--release")

    filename = os.path.basename(blend_path)
    prefix = f"[{filename}]"

    # Start the background process and route both standard outputs chronologically to our listener
    process = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="ignore",
        bufsize=1,
    )

    full_stdout = []
    # Read and print the console logs line by line as Blender writes them
    for line in iter(process.stdout.readline, ""):
        stripped = line.strip()
        if stripped:
            print(f"  {prefix} {stripped}")
            full_stdout.append(line)

    process.stdout.close()
    return_code = process.wait()

    success = (return_code == 0) and (
        any("[PipelineStatus] Success" in l for l in full_stdout)
    )
    return blend_path, success, "".join(full_stdout), ""


def get_safer_worker_limit():
    """Calculates a safe concurrency limit to prevent running out of RAM on large assets."""
    cpu_limit = max(1, (os.cpu_count() or 4) - 1)

    # Check system memory to dynamically size the pool safely
    import sys

    if sys.platform == "linux":
        try:
            with open("/proc/meminfo", "r") as f:
                for line in f:
                    if "MemTotal" in line:
                        total_kb = int(line.split()[1])
                        total_gb = total_kb / (1024 * 1024)
                        # Allocate roughly 6.5GB per Blender instance to be extremely safe
                        mem_limit = max(1, int(total_gb // 6.5))
                        return min(cpu_limit, mem_limit)
        except Exception:
            pass

    # Default fallback limit to prevent extreme RAM spikes on average desktop systems
    return min(cpu_limit, 4)


def run_master_dispatcher():
    """Scans the directory layout and schedules parallel worker commands using native thread pools."""
    input_dir = os.getcwd()
    output_dir = os.path.join(input_dir, "resources", "assets")
    os.makedirs(output_dir, exist_ok=True)

    print("=========================================================")
    print("Zahlen Engine Asset Exporter (Parallel Multiprocessing)")
    print("Target directories:")
    print(f"  Input:  {input_dir}")
    print(f"  Output: {output_dir}")
    print("=========================================================")

    # 1. Discover target files
    blend_files = []
    for root, _, files in os.walk(input_dir):
        norm_root = root.replace("\\", "/").lower()
        if any(
            p in norm_root
            for p in ["resources", "exported_assets", "tadc_models", "asset_library"]
        ):
            continue
        for file in files:
            if file.endswith(".blend") and not file.startswith("."):
                # Dynamically skip computationally impossible procedural loops
                if "void" in file.lower():
                    print(
                        f"  [Skip] Intentionally bypassing computationally heavy scene: {file}"
                    )
                    continue
                blend_files.append(os.path.join(root, file))

    total_files = len(blend_files)
    print(f"Found {total_files} .blend files to process.\n")

    if total_files == 0:
        return

    try:
        blender_bin = find_blender()
        print(f"Using Blender executable: {blender_bin}")
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

    # 2. Configure Worker Pool Limit safely based on system RAM
    max_workers = get_safer_worker_limit()
    print(f"Spawning thread pool to orchestrate {max_workers} background workers...\n")

    script_path = os.path.abspath(__file__)
    success_files = []
    failed_files = []

    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        futures = {
            executor.submit(
                run_worker_process, blender_bin, script_path, blend, DEV_MODE
            ): blend
            for blend in blend_files
        }

        completed_count = 0
        for future in as_completed(futures):
            blend_path = futures[future]
            completed_count += 1
            rel_path = os.path.relpath(blend_path, input_dir)

            try:
                _, success, stdout, stderr = future.result()
                if success:
                    print(
                        f"[{completed_count}/{total_files}] [Success] Exported: {rel_path}"
                    )
                    success_files.append(rel_path)
                else:
                    print(
                        f"[{completed_count}/{total_files}] [Error] Failed to process: {rel_path}"
                    )
                    failed_files.append((rel_path, "See printed real-time logs above."))
            except Exception as e:
                print(
                    f"[{completed_count}/{total_files}] [System Error] Process crashed on: {rel_path}: {e}"
                )
                failed_files.append((rel_path, f"       System failure: {str(e)}"))

    print("\n=========================================================")
    print("Parallel Export Summary:")
    print(f"  Successfully Exported: {len(success_files)} / {total_files}")
    print(f"  Failed:                {len(failed_files)} / {total_files}")
    if failed_files:
        print("\nFailed Files Details:")
        for f, err in failed_files:
            print(f"  - {f}:")
            print(err)
    print("=========================================================")


# --- ENTRY ROUTING POINT ---

if __name__ == "__main__":
    if INSIDE_BLENDER:
        # Worker Entry: Execute logic on single target command argument
        args = []
        if "--" in sys.argv:
            args = sys.argv[sys.argv.index("--") + 1 :]
        else:
            args = sys.argv[1:]

        worker_file = None
        cli_dev_mode = True

        if "--worker" in args:
            try:
                idx = args.index("--worker")
                worker_file = args[idx + 1]
            except IndexError:
                print("[Worker Error] No target file path argument followed '--worker'")
                sys.exit(1)

        if "--release" in args:
            cli_dev_mode = False

        if worker_file:
            ok = run_worker_pipeline(worker_file, dev_mode=cli_dev_mode)
            sys.exit(0 if ok else 1)
        else:
            print("[Worker Error] Invalid arguments passed to Blender worker process.")
            sys.exit(1)
    else:
        # Master Entry: Run orchestrator using system Python interpreter
        run_master_dispatcher()
