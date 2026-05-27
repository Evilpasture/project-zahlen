import bpy
import sys
import argparse


def bake_model_to_atlas(model_path, output_glb_path, output_png_path, resolution=2048):
    # 1. Clear default scene objects (cube, camera, light)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    # 2. Open .blend or Import glTF/GLB based on extension
    if model_path.lower().endswith(".blend"):
        print(f"Opening Blend file: {model_path}...")
        bpy.ops.wm.open_mainfile(filepath=model_path)
        try:
            # Unpack embedded textures so Cycles can find them on disk during baking
            bpy.ops.file.unpack_all(method="USE_LOCAL")
            print("       - Unpacked embedded textures from .blend file.")
        except Exception as e:
            print(f"       [Warning] Texture unpacking skipped/failed: {e}")
    else:
        print(f"Importing glTF/GLB: {model_path}...")
        bpy.ops.import_scene.gltf(filepath=model_path)

    # 3. Collect all meshes (filtering out armatures, empties, and checking view layer)
    meshes = []
    for obj in bpy.context.scene.objects:
        if obj.type == "MESH":
            # ONLY include meshes that are actually active in the View Layer
            if obj.name in bpy.context.view_layer.objects:
                meshes.append(obj)

    if not meshes:
        print("Error: No meshes found in active View Layer!")
        return

    # 4. Deselect all objects first to prevent armatures ("POMNI_rig")
    # or other non-mesh helper nodes from interfering with mesh-only operations.
    print("Deselecting non-mesh objects...")
    bpy.ops.object.select_all(action="DESELECT")

    # 5. Set the active object to the first valid mesh, then select only the mesh objects safely
    valid_active_set = False
    for mesh in meshes:
        try:
            mesh.select_set(True)
            if not valid_active_set:
                bpy.context.view_layer.objects.active = mesh
                valid_active_set = True
        except Exception as e:
            print(
                f"       [Warning] Bypassing non-selectable helper mesh '{mesh.name}': {e}"
            )

    # 6. Join all submeshes into a single unified mesh (simulate Ctrl+J)
    print("Joining submeshes...")
    bpy.ops.object.join()
    combined_obj = bpy.context.active_object

    # 7. WELD SPLIT VERTICES (REMOVE DOUBLES)
    print("Welding split vertices (removing duplicates)...")
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.remove_doubles(threshold=0.0001)
    bpy.ops.object.mode_set(mode="OBJECT")

    # 8. CLEAN UP EMPTY MATERIAL SLOTS
    print("Cleaning up empty material slots...")
    slots = combined_obj.material_slots
    for i in reversed(range(len(slots))):
        if not slots[i].material:
            combined_obj.active_material_index = i
            bpy.ops.object.material_slot_remove()

    # 9. Ensure the combined UV Map is selected and active.
    print("Activating original UV Map...")
    uv_layers = combined_obj.data.uv_layers
    if len(uv_layers) > 0:
        uv_map = uv_layers[0]
        uv_layers.active = uv_map
    else:
        # Fallback if no UV map was imported
        uv_map = uv_layers.new(name="UVMap")
        uv_layers.active = uv_map

    # 10. PRESERVE ORIGINAL UV ISLANDS AND PACK THEM WITHOUT OVERLAP
    print("Packing existing UV islands into a single atlas layout...")
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")

    # 1. Normalize island scales based on physical 3D area (no blurry clothes!)
    bpy.ops.uv.average_islands_scale()

    # 2. Pack the islands into the 0-1 space neatly
    bpy.ops.uv.pack_islands(margin=0.01)
    bpy.ops.object.mode_set(mode="OBJECT")

    # 11. Create the destination blank image for the atlas
    print(f"Allocating blank {resolution}x{resolution} texture sheet...")
    image = bpy.data.images.new("Atlas_Albedo", width=resolution, height=resolution)

    # 12. Insert an active Texture Node in all existing materials.
    for mat in combined_obj.data.materials:
        if mat:
            mat.use_nodes = True  # Force nodes active so node_tree is populated
            nodes = mat.node_tree.nodes

            # Clean up any leftover bake nodes from previous runs
            if "Bake_Target_Node" in nodes:
                nodes.remove(nodes["Bake_Target_Node"])

            node = nodes.new("ShaderNodeTexImage")
            node.name = "Bake_Target_Node"
            node.image = image
            nodes.active = node  # Set active so the bake uses it

    # 13. PRE-BAKE CRITICAL FIX: Force Metallic to 0 on all materials.
    print("Forcing metallic values to 0.0 for diffuse color baking...")
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

    # 14. Ensure *only* the combined mesh is active/selected before baking.
    bpy.ops.object.select_all(action="DESELECT")
    combined_obj.select_set(True)
    bpy.context.view_layer.objects.active = combined_obj

    # 15. Configure Cycles renderer for CPU/GPU baking
    print("Configuring baking engine...")
    bpy.context.scene.render.engine = "CYCLES"

    try:
        bpy.context.scene.cycles.device = "GPU"
    except:
        bpy.context.scene.cycles.device = "CPU"

    # Bake purely flat color maps (Diffuse Color), ignoring lights/shadows
    bpy.context.scene.render.bake.use_pass_direct = False
    bpy.context.scene.render.bake.use_pass_indirect = False
    bpy.context.scene.render.bake.use_pass_color = True

    # 16. Execute the bake
    print("Executing texture bake (this may take a few seconds)...")
    bpy.ops.object.bake(type="DIFFUSE", margin=4)

    # 17. Save the compiled atlas texture sheet
    image.filepath_raw = output_png_path
    image.file_format = "PNG"
    image.save()
    print(f"Saved texture atlas to: {output_png_path}")

    # 18. Finalize UV Map
    print("Finalizing UV Map...")
    combined_obj.data.uv_layers[0].name = "UVMap"

    # 19. Clean up the mesh materials and assign the single, newly-baked material
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

    # 20. Export back to GLB
    print(f"Exporting unified GLB to: {output_glb_path}")
    bpy.ops.export_scene.gltf(filepath=output_glb_path, export_format="GLB")
    print("Process complete!")


# CLI Entrypoint wrapping
if __name__ == "__main__":
    if "--" in sys.argv:
        args_list = sys.argv[sys.argv.index("--") + 1 :]
    else:
        args_list = []

    parser = argparse.ArgumentParser(
        description="Bake multi-material GLB into a single-mesh atlas."
    )
    parser.add_argument("--input", required=True, help="Input GLB file path")
    parser.add_argument(
        "--output_mesh", required=True, help="Output baked GLB file path"
    )
    parser.add_argument(
        "--output_texture", required=True, help="Output PNG atlas file path"
    )
    parser.add_argument(
        "--resolution", type=int, default=2048, help="Texture resolution"
    )

    parsed_args = parser.parse_args(args_list)

    bake_model_to_atlas(
        parsed_args.input,
        parsed_args.output_mesh,
        parsed_args.output_texture,
        parsed_args.resolution,
    )
