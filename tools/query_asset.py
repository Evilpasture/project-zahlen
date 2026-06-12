# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


# tools/query_blush_mechanics.py
import bpy


def query_blush_mechanics():
    print("\n" + "=" * 90)
    print(" EXHAUSTIVE BLUSH SOURCE & RIG CONTROLLER DIAGNOSTICS ")
    print("=" * 90)

    # Target keywords to find the blush mechanic
    KEYWORDS = ["blush", "cheek", "smooth", "face", "makeup", "red"]

    # -------------------------------------------------------------------------
    # PART 1: SEARCHING RIG CUSTOM PROPERTIES
    # -------------------------------------------------------------------------
    print("\n--- [PART 1] ARMATURE & POSE BONE CUSTOM PROPERTIES ---")
    rig = bpy.data.objects.get("POMNI_rig") or next(
        (o for o in bpy.data.objects if o.type == "ARMATURE"), None
    )

    if rig:
        print(f"[*] Checking active armature: '{rig.name}'")
        # Check Armature Object Custom Properties
        arm_props = [p for p in rig.keys() if any(kw in p.lower() for kw in KEYWORDS)]
        for prop in arm_props:
            print(f"  [+] Found Rig Property: '{prop}' = {rig[prop]}")

        # Check Pose Bones Custom Properties
        found_bone_prop = False
        for pb in rig.pose.bones:
            bone_props = [
                p for p in pb.keys() if any(kw in p.lower() for kw in KEYWORDS)
            ]
            if bone_props:
                found_bone_prop = True
                print(f"  [+] Bone '{pb.name}' has relevant custom properties:")
                for prop in bone_props:
                    print(f"      * '{prop}' = {pb[prop]}")
        if not found_bone_prop:
            print("  [-] No matching custom properties found on pose bones.")
    else:
        print("[-] No Armature object found to scan.")

    # -------------------------------------------------------------------------
    # PART 2: MATERIAL & SHADER NODE ANALYSIS
    # -------------------------------------------------------------------------
    print("\n--- [PART 2] MATERIAL SHADER NODES & DRIVERS ---")
    head_mesh = bpy.data.objects.get("pomni_head.002")
    materials_to_check = set()

    if head_mesh:
        print(f"[*] Found head mesh: '{head_mesh.name}'")
        for slot in head_mesh.material_slots:
            if slot.material:
                materials_to_check.add(slot.material)
    else:
        # Fallback to check all materials containing "head" or "pomni"
        for mat in bpy.data.materials:
            if any(x in mat.name.lower() for x in ["head", "pomni", "face"]):
                materials_to_check.add(mat)

    for mat in materials_to_check:
        print(f"\n[+] Scanning Material: '{mat.name}'")
        if not mat.use_nodes or not mat.node_tree:
            print("  [-] Nodes are disabled or empty on this material.")
            continue

        # A. Look for relevant nodes
        relevant_nodes = []
        for node in mat.node_tree.nodes:
            # Check name, label, or node type
            node_name_lower = node.name.lower()
            node_label_lower = node.label.lower() if node.label else ""

            if any(kw in node_name_lower or kw in node_label_lower for kw in KEYWORDS):
                relevant_nodes.append(node)
            elif node.type in ("MIX", "MIX_RGB", "VAL_TO_RGB", "ATTRIBUTE"):
                # Also collect mix nodes and color ramps as they often route the blush
                relevant_nodes.append(node)

        print(
            f"  -> Found {len(relevant_nodes)} potential routing nodes in shader tree:"
        )
        for node in relevant_nodes:
            label_str = f" (Label: '{node.label}')" if node.label else ""
            print(f"      * Node: '{node.name}'{label_str} | Type: {node.type}")
            # Print node values or inputs that might be driven
            for input_idx, inp in enumerate(node.inputs):
                if inp.is_linked:
                    # Trace connections
                    links_from = ", ".join(
                        f"'{link.from_node.name}' -> {link.from_socket.name}"
                        for link in inp.links
                    )
                    print(f"        -> Input '{inp.name}' linked from: {links_from}")
                else:
                    val = getattr(inp, "default_value", "N/A")
                    # Formatting values beautifully
                    if isinstance(val, float):
                        print(f"        -> Input '{inp.name}': {val:.4f}")
                    elif hasattr(val, "color") or (
                        isinstance(val, (list, tuple)) and len(val) >= 3
                    ):
                        print(
                            f"        -> Input '{inp.name}': Color/Vector {list(val)[:4]}"
                        )

        # B. Check for active Material Animation/Driver data
        if mat.node_tree.animation_data and mat.node_tree.animation_data.drivers:
            drivers = mat.node_tree.animation_data.drivers
            print(
                f"  [+] Found {len(drivers)} active DRIVERS in this material's shader tree:"
            )
            for d in drivers:
                print(f"      * Driver Target Path: '{d.data_path}'")
                print(
                    f"        -> Expression: '{d.driver.expression}' | Muted: {d.mute}"
                )
                for var in d.driver.variables:
                    print(f"        -> Variable: '{var.name}' | Type: {var.type}")
                    for i, t in enumerate(var.targets):
                        target_id = t.id.name if t.id else "None"
                        bone_str = (
                            f" | Bone: '{t.bone_target}'"
                            if hasattr(t, "bone_target") and t.bone_target
                            else ""
                        )
                        print(
                            f"           Target {i}: ID: '{target_id}'{bone_str} | Data Path: '{t.data_path}'"
                        )
        else:
            print("  [-] No active shader drivers found on this material's node tree.")

    # -------------------------------------------------------------------------
    # PART 3: COLOR ATTRIBUTES & VERTEX COLORS
    # -------------------------------------------------------------------------
    print("\n--- [PART 3] COLOR ATTRIBUTES (VERTEX COLORS) ---")
    if head_mesh:
        if (
            hasattr(head_mesh.data, "color_attributes")
            and head_mesh.data.color_attributes
        ):
            print(f"[*] Scanning Color Attributes on '{head_mesh.name}':")
            for attr in head_mesh.data.color_attributes:
                print(
                    f"  [+] Found Layer: '{attr.name}' | Domain: {attr.domain} | Type: {attr.data_type}"
                )
        else:
            print(f"  [-] No Color Attributes found on '{head_mesh.name}'.")
    else:
        print("  [-] Head mesh missing; skipping color attribute scan.")

    # -------------------------------------------------------------------------
    # PART 4: SHAPE KEYS & CORRESPONDING DRIVERS
    # -------------------------------------------------------------------------
    print("\n--- [PART 4] SHAPE KEY ANALYSIS ---")
    if head_mesh and head_mesh.data.shape_keys:
        print(f"[*] Scanning Shape Keys on '{head_mesh.name}':")
        for kb in head_mesh.data.shape_keys.key_blocks:
            if any(kw in kb.name.lower() for kw in KEYWORDS):
                print(f"  [+] Shape Key: '{kb.name}' | Current Value: {kb.value:.4f}")

        anim_data = head_mesh.data.shape_keys.animation_data
        if anim_data and anim_data.drivers:
            print("  [*] Scanning shape key drivers:")
            for d in anim_data.drivers:
                if any(kw in d.data_path.lower() for kw in KEYWORDS):
                    print(f"    [+] Driven Path: '{d.data_path}'")
                    print(f"        -> Expression: '{d.driver.expression}'")
    else:
        print("  [-] No shape keys found on the head mesh.")

    # -------------------------------------------------------------------------
    # PART 5: DATABASE OBJECT SEARCH (FOR OVERLAY MESHES)
    # -------------------------------------------------------------------------
    print("\n--- [PART 5] SEARCHING SCENE FOR HIDDEN OVERLAY MESHES ---")
    overlay_objects = [
        o for o in bpy.data.objects if any(kw in o.name.lower() for kw in KEYWORDS)
    ]
    if overlay_objects:
        print(f"[*] Found {len(overlay_objects)} scene objects with relevant names:")
        for obj in overlay_objects:
            status_reports = []
            if obj.users_collection:
                for col in obj.users_collection:
                    status_reports.append(col.name)
            col_str = (
                f"In Collections: {status_reports}"
                if status_reports
                else "Orphaned/Not in collections"
            )
            print(f"  [+] Object: '{obj.name}' | Type: {obj.type} | {col_str}")
            print(
                f"      -> Viewport Hidden: {obj.hide_viewport} | Render Hidden: {obj.hide_render}"
            )
    else:
        print("  [-] No separate mesh objects containing keywords found in database.")


if __name__ == "__main__":
    query_blush_mechanics()
    print("\n" + "=" * 90 + "\n")

