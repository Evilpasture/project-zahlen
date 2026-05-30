# tools/query_eyelids.py
import bpy
import fnmatch


def run_glob_query():
    patterns = ["pomni_eyelids*", "pomni_pupil*", "pomni_eyes*"]

    print("\n" + "=" * 70)
    print(" GLOB PATTERN QUERY: EYELIDS, PUPILS & EYES ")
    print("=" * 70)

    # Gather matching objects
    matched_objects = []
    for pattern in patterns:
        for obj in bpy.data.objects:
            if fnmatch.fnmatch(obj.name.lower(), pattern.lower()):
                if obj not in matched_objects:
                    matched_objects.append(obj)

    if not matched_objects:
        print("[-] No objects matched the glob patterns.")
        return

    for obj in matched_objects:
        print(f"\n[+] Object: {obj.name} ({obj.type})")
        print(f"    Parent Object: '{obj.parent.name if obj.parent else 'None'}'")
        print(f"    Parent Type: '{obj.parent_type if obj.parent else 'N/A'}'")
        if obj.parent_type == "BONE":
            print(f"    Parent Bone: '{obj.parent_bone}'")

        # Transform State
        print(
            f"    Transform: Loc={obj.location} | Rot={obj.rotation_euler} | Scale={obj.scale}"
        )
        print(
            f"    Visibility: Viewport={obj.hide_viewport} | Render={obj.hide_render}"
        )

        if obj.type == "MESH":
            print(f"    Vertices: {len(obj.data.vertices)}")
            print(f"    Vertex Groups: {[vg.name for vg in obj.vertex_groups]}")

            # Modifier Details
            print("    Modifiers (Top to Bottom):")
            if not obj.modifiers:
                print("      (No modifiers)")
            for i, mod in enumerate(obj.modifiers):
                target = getattr(mod, "target", None)
                target_str = f"Target: '{target.name}'" if target else ""
                vgroup = getattr(mod, "vertex_group", "")
                vgroup_str = f"VGroup: '{vgroup}'" if vgroup else ""
                state = "[ON ]" if mod.show_viewport else "[OFF]"
                print(
                    f"      {i}. {state} {mod.name} ({mod.type}) {target_str} {vgroup_str}"
                )

            # Shape Keys, Current Values, and Active Drivers
            if obj.data.shape_keys:
                print("    Shape Keys & Drivers:")
                for kb in obj.data.shape_keys.key_blocks:
                    driver_str = ""
                    # Locate active drivers associated with this shape key channel
                    skeys = obj.data.shape_keys
                    if skeys.animation_data and skeys.animation_data.drivers:
                        for d in skeys.animation_data.drivers:
                            # Match drivers deforming this key block path
                            if kb.name in d.data_path:
                                vars_str = ", ".join(
                                    [f"{v.name} ({v.type})" for v in d.driver.variables]
                                )
                                driver_str = f" | [DRIVEN] expression: '{d.driver.expression}' variables: [{vars_str}]"
                    print(f"      - '{kb.name}': value={kb.value:.4f}{driver_str}")
            else:
                print("    Shape Keys: None")


if __name__ == "__main__":
    run_glob_query()
    print("\n" + "=" * 70 + "\n")
