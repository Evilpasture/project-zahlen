import bpy
import os
import sys
import json
import importlib

# 1. Dynamically find the .blend file path to append local scripts path [1]
blend_dir = os.path.dirname(bpy.data.filepath)
if not blend_dir:
    raise RuntimeError("Please save your Blender file before running this script.")

if blend_dir not in sys.path:
    sys.path.append(blend_dir)

# 2. Import and automatically reload the anim_math.py module
import anim_math
importlib.reload(anim_math)

# ===================================================================
# RIG SCHEMA MAP (Defines your custom bone naming configuration)
# ===================================================================
RIG_MAP = {
    "TORSO": "torso",
    "CHEST": "chest",
    "HIPS": "hips",
    "HEAD": "head",
    "SPINE_0": "spine_fk",
    "SPINE_1": "spine_fk.001",
    
    "ARM_L": "upper_arm_fk.L",
    "FOREARM_L": "forearm_fk.L",
    "HAND_L": "hand_fk.L",
    
    "ARM_R": "upper_arm_fk.R",
    "FOREARM_R": "forearm_fk.R",
    "HAND_R": "hand_fk.R",
    
    "THIGH_L": "thigh_fk.L",
    "SHIN_L": "shin_fk.L",
    "FOOT_L": "foot_fk.L",
    
    "THIGH_R": "thigh_fk.R",
    "SHIN_R": "shin_fk.R",
    "FOOT_R": "foot_fk.R",
    
    "HAT_L": "hat-start.l",
    "HAT_R": "hat-start.r",
    
    "EYELID_TOP_L": "eyelidTop.l",
    "EYELID_TOP_R": "eyelidTop.r",
    "EYELID_BOT_L": "eyelidBot.l",
    "EYELID_BOT_R": "eyelidBot.r",
    
    "EYE_L": "eye.l",
    "EYE_R": "eye.r",
    
    "ARM_PARENT_L": "upper_arm_parent.L",
    "ARM_PARENT_R": "upper_arm_parent.R",
    "LEG_PARENT_L": "thigh_parent.L",
    "LEG_PARENT_R": "thigh_parent.R",
    
    "FINGER_BASES": ["f_index", "f_middle", "f_ring", "thumb"]
}

# Fine-tuning adjustments for the procedural script
ADJUSTMENTS = {
    "BLINK_TRAVEL_UPPER": -0.1,
    "BLINK_TRAVEL_LOWER": 0.1,
    "FINGER_FLEX_RANGE": 0.08,
    "ARM_SWAY_RANGE": 2.5,
    "WRIST_SWAY_RANGE": 4.0
}

# ===================================================================
# AUTOMATIC TIMELINE SCALER HANDLER
# ===================================================================
def auto_adjust_timeline_range(scene):
    """Automatically snaps the Blender timeline's end frame to match the active state action's length."""
    rig = bpy.context.active_object
    if rig and rig.type == 'ARMATURE' and rig.animation_data and rig.animation_data.action:
        action = rig.animation_data.action
        name = action.name.upper()
        if "_WALKING" in name:
            scene.frame_end = 36
        elif "_RUNNING" in name:
            scene.frame_end = 24
        elif "_STRAFE" in name:
            scene.frame_end = 40
        elif "_IDLE" in name:
            scene.frame_end = 120

# Clean up any existing handler of the same name to prevent duplicates [1]
for h in list(bpy.app.handlers.frame_change_pre):
    if h.__name__ == "auto_adjust_timeline_range":
        bpy.app.handlers.frame_change_pre.remove(h)

# Register the background handler [1]
bpy.app.handlers.frame_change_pre.append(auto_adjust_timeline_range)

# ===================================================================
# BATCH EXPORTER INTERFACE
# ===================================================================
def generate_all_actions():
    """Reads the JSON config database, generates the actions, and saves them [2]."""
    rig = bpy.context.active_object
    if not rig or rig.type != 'ARMATURE':
        print("Please select your character's armature in the 3D Viewport.")
        return

    # 1. Load Decoupled configurations from JSON
    json_path = os.path.join(blend_dir, "state_configs.json")
    try:
        with open(json_path, 'r') as f:
            state_configs = json.load(f)
    except Exception as e:
        print(f"Error loading config JSON: {e}")
        return

    # Extract clean character prefix (e.g. "pomni" if rig is named "Pomni")
    prefix = rig.name.lower()
    if prefix.endswith(".anim") or prefix.endswith(".retarget"):
        prefix = prefix.split(".")[0]

    # Definition of animation states and target durations
    states = {
        'IDLE': 120,
        'WALKING': 36,
        'RUNNING': 24,
        'STRAFE': 40
    }

    # Ensure animation data block exists [2]
    if not rig.animation_data:
        rig.animation_data_create()

    # Unlink active action to clear workspace
    rig.animation_data.action = None

    for state_type, duration in states.items():
        action_name = f"{prefix}_{state_type}"
        
        # Overwrite existing action cleanly if found
        old_action = bpy.data.actions.get(action_name)
        if old_action:
            bpy.data.actions.remove(old_action)

        # Create fresh action [2]
        action = bpy.data.actions.new(name=action_name)
        action.use_fake_user = True  # Keep action inside file even if unassigned [2]
        
        # Link action to rig
        rig.animation_data.action = action
        
        print(f"Generating action: '{action_name}' ({duration} frames)...")
        anim_math.generate_single_state_data(rig, state_type, duration, state_configs[state_type], RIG_MAP, ADJUSTMENTS)

    # Re-assign IDLE as the default active preview action
    idle_action = bpy.data.actions.get(f"{prefix}_IDLE")
    if idle_action:
        rig.animation_data.action = idle_action
        bpy.context.scene.frame_start = 1
        bpy.context.scene.frame_end = 120
        bpy.context.scene.frame_set(1)

    bpy.context.view_layer.update()
    bpy.ops.screen.animation_play()
    print("Batch animation export complete. Actions are saved in your Action Editor.")

# Run the batch exporter
generate_all_actions()