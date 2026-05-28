import bpy
import math
from mathutils import Vector, Euler

# ===================================================================
# CUSTOM RIG ADJUSTMENTS
# ===================================================================
BLINK_TRAVEL_UPPER = 0.1   # Top eyelid travel down (Positive local Y)
BLINK_TRAVEL_LOWER = -0.1  # Bottom eyelid travel up (Negative local Y)

FINGER_FLEX_RANGE = 0.08    # Base finger flex amplitude
ARM_SWAY_RANGE = 2.5        # Base arm swing amplitude (degrees)
WRIST_SWAY_RANGE = 4.0      # Base wrist flop amplitude (degrees)

EYE_SACCADE_SCALE = 4.0     # Subtle scale multiplier for the eye target controller

# ===================================================================
# RIG SCHEMA DEFINITIONS
# ===================================================================
class RigSchema:
    """Central structural schema to prevent magic string lookup bugs."""
    TORSO = "torso"
    CHEST = "chest"
    HIPS = "hips"
    HEAD = "head"
    SPINE_0 = "spine_fk"
    SPINE_1 = "spine_fk.001"
    
    UP_ARM_L = "upper_arm_fk.L"
    FORE_ARM_L = "forearm_fk.L"
    HAND_L = "hand_fk.L"
    UP_ARM_R = "upper_arm_fk.R"
    FORE_ARM_R = "forearm_fk.R"
    HAND_R = "hand_fk.R"
    
    THIGH_L = "thigh_fk.L"
    SHIN_L = "shin_fk.L"
    FOOT_L = "foot_fk.L"
    THIGH_R = "thigh_fk.R"
    SHIN_R = "shin_fk.R"
    FOOT_R = "foot_fk.R"
    
    HAT_L = "hat-start.l"
    HAT_R = "hat-start.r"
    
    EYELID_TOP_L = "eyelidTop.l"
    EYELID_TOP_R = "eyelidTop.r"
    EYELID_BOT_L = "eyelidBot.l"
    EYELID_BOT_R = "eyelidBot.r"
    
    EYE_CONTROL = "eyeControl"  # Central master target eye controller
    
    # Rigify IK/FK Switch Constraints
    ARM_PARENT_L = "upper_arm_parent.L"
    ARM_PARENT_R = "upper_arm_parent.R"
    LEG_PARENT_L = "thigh_parent.L"
    LEG_PARENT_R = "thigh_parent.R"


class BoneData:
    """Wraps a PoseBone to cache initial offsets and handle missing bones gracefully."""
    def __init__(self, pose_bone):
        self.bone = pose_bone
        self.base_loc = pose_bone.location.copy() if pose_bone else Vector((0.0, 0.0, 0.0))
        self.base_rot = pose_bone.rotation_euler.copy() if pose_bone else Euler((0.0, 0.0, 0.0))

    def keyframe_insert(self, data_path, frame):
        if self.bone:
            self.bone.keyframe_insert(data_path=data_path, frame=frame)

    @property
    def location(self):
        return self.bone.location if self.bone else Vector((0.0, 0.0, 0.0))

    @location.setter
    def location(self, val):
        if self.bone:
            self.bone.location = val

    @property
    def rotation_euler(self):
        return self.bone.rotation_euler if self.bone else Euler((0.0, 0.0, 0.0))

    @rotation_euler.setter
    def rotation_euler(self, val):
        if self.bone:
            self.bone.rotation_euler = val


class CharacterRig:
    """Active character container handling structures and default overrides safely."""
    def __init__(self, rig, is_locomotion=False):
        self.rig = rig
        self.pose_bones = rig.pose.bones
        
        # Populate bone schema mappings
        self.torso = BoneData(self.pose_bones.get(RigSchema.TORSO))
        self.chest = BoneData(self.pose_bones.get(RigSchema.CHEST))
        self.hips = BoneData(self.pose_bones.get(RigSchema.HIPS))
        self.head = BoneData(self.pose_bones.get(RigSchema.HEAD))
        self.spine_0 = BoneData(self.pose_bones.get(RigSchema.SPINE_0))
        self.spine_1 = BoneData(self.pose_bones.get(RigSchema.SPINE_1))
        
        self.up_arm_l = BoneData(self.pose_bones.get(RigSchema.UP_ARM_L))
        self.fore_arm_l = BoneData(self.pose_bones.get(RigSchema.FORE_ARM_L))
        self.hand_l = BoneData(self.pose_bones.get(RigSchema.HAND_L))
        self.up_arm_r = BoneData(self.pose_bones.get(RigSchema.UP_ARM_R))
        self.fore_arm_r = BoneData(self.pose_bones.get(RigSchema.FORE_ARM_R))
        self.hand_r = BoneData(self.pose_bones.get(RigSchema.HAND_R))
        
        self.thigh_l = BoneData(self.pose_bones.get(RigSchema.THIGH_L))
        self.shin_l = BoneData(self.pose_bones.get(RigSchema.SHIN_L))
        self.foot_l = BoneData(self.pose_bones.get(RigSchema.FOOT_L))
        self.thigh_r = BoneData(self.pose_bones.get(RigSchema.THIGH_R))
        self.shin_r = BoneData(self.pose_bones.get(RigSchema.SHIN_R))
        self.foot_r = BoneData(self.pose_bones.get(RigSchema.FOOT_R))
        
        self.hat_l = BoneData(self.pose_bones.get(RigSchema.HAT_L))
        self.hat_r = BoneData(self.pose_bones.get(RigSchema.HAT_R))
        
        self.eyelid_top_l = BoneData(self.pose_bones.get(RigSchema.EYELID_TOP_L))
        self.eyelid_top_r = BoneData(self.pose_bones.get(RigSchema.EYELID_TOP_R))
        self.eyelid_bot_l = BoneData(self.pose_bones.get(RigSchema.EYELID_BOT_L))
        self.eyelid_bot_r = BoneData(self.pose_bones.get(RigSchema.EYELID_BOT_R))
        
        self.eye_control = BoneData(self.pose_bones.get(RigSchema.EYE_CONTROL))

        # Direct structural overrides
        if self.up_arm_l.bone:
            self.up_arm_l.base_rot = Euler((0.0, 0.0, math.radians(-70)))
        if self.fore_arm_l.bone:
            self.fore_arm_l.base_rot = Euler((math.radians(25), 0.0, 0.0))
        if self.up_arm_r.bone:
            self.up_arm_r.base_rot = Euler((0.0, 0.0, math.radians(70)))
        if self.fore_arm_r.bone:
            self.fore_arm_r.base_rot = Euler((math.radians(25), 0.0, 0.0))

        th_rot = math.radians(-5) if is_locomotion else 0.0
        sh_rot = math.radians(5) if is_locomotion else 0.0
        ft_rot = math.radians(-5) if is_locomotion else 0.0

        for side in ["l", "r"]:
            thigh = getattr(self, f"thigh_{side}")
            shin = getattr(self, f"shin_{side}")
            foot = getattr(self, f"foot_{side}")
            if thigh.bone:
                thigh.base_rot = Euler((th_rot, 0.0, 0.0))
            if shin.bone:
                shin.base_rot = Euler((sh_rot, 0.0, 0.0))
            if foot.bone:
                foot.base_rot = Euler((ft_rot, 0.0, 0.0))

# ===================================================================
# ORGANIC MATH CALCULATIONS (Progress-Relative)
# ===================================================================
def get_organic_breath_curve(phase):
    """Generates an asymmetric, double-exponential breathing curve."""
    skewed_phase = phase + 0.4 * math.sin(phase)
    raw_val = math.exp(math.sin(skewed_phase))
    min_val = math.exp(-1) # ~0.3679
    max_val = math.exp(1)  # ~2.7183
    normalized = (raw_val - min_val) / (max_val - min_val)
    return normalized * 2.0 - 1.0

def get_blink_factor_relative(progress, duration):
    """Adjusts blink frequency based on animation length to prevent fluttering."""
    if duration < 60:
        if 0.45 <= progress <= 0.55:
            p_blink = (progress - 0.45) / 0.10
            return math.sin(p_blink * math.pi)
    else:
        if 0.22 <= progress <= 0.28:
            p_blink = (progress - 0.22) / 0.06
            return math.sin(p_blink * math.pi)
        if 0.72 <= progress <= 0.78:
            p_blink = (progress - 0.72) / 0.06
            return math.sin(p_blink * math.pi)
    return 0.0

def get_eye_saccade_relative(progress):
    """Eye saccades scaled relative to timeline progress."""
    if 0.35 <= progress <= 0.60:
        return (-0.015, 0.0)
    if 0.80 <= progress <= 0.95:
        return (0.01, 0.01)
    return (0.0, 0.0)

# ===================================================================
# RIG DATA SETUP HELPERS
# ===================================================================
def _prepare_rig_for_animation(rig):
    """Initializes bone rotation modes and triggers FK constraints on active bones."""
    pose_bones = rig.pose.bones
    for side in ["L", "R"]:
        arm_parent = pose_bones.get(getattr(RigSchema, f"ARM_PARENT_{side}"))
        if arm_parent: arm_parent["IK_FK"] = 1.0
        leg_parent = pose_bones.get(getattr(RigSchema, f"LEG_PARENT_{side}"))
        if leg_parent: leg_parent["IK_FK"] = 1.0

    active_bones = [
        RigSchema.TORSO, RigSchema.CHEST, RigSchema.HIPS, RigSchema.HEAD,
        RigSchema.SPINE_0, RigSchema.SPINE_1,
        RigSchema.UP_ARM_L, RigSchema.FORE_ARM_L, RigSchema.HAND_L,
        RigSchema.UP_ARM_R, RigSchema.FORE_ARM_R, RigSchema.HAND_R,
        RigSchema.THIGH_L, RigSchema.SHIN_L, RigSchema.FOOT_L,
        RigSchema.THIGH_R, RigSchema.SHIN_R, RigSchema.FOOT_R,
        RigSchema.HAT_L, RigSchema.HAT_R
    ]
    for b_name in active_bones:
        b = pose_bones.get(b_name)
        if b: b.rotation_mode = 'XYZ'

def _reset_pose_to_rest(rig):
    """Programmatically clears pose transformations on major structural bones, leaving facial bones intact."""
    pose_bones = rig.pose.bones
    
    # Exclude Eyelids/Eyes but include EYE_CONTROL to center its base pose
    bones_to_reset = [
        RigSchema.TORSO, RigSchema.CHEST, RigSchema.HIPS, RigSchema.HEAD,
        RigSchema.SPINE_0, RigSchema.SPINE_1,
        RigSchema.UP_ARM_L, RigSchema.FORE_ARM_L, RigSchema.HAND_L,
        RigSchema.UP_ARM_R, RigSchema.FORE_ARM_R, RigSchema.HAND_R,
        RigSchema.THIGH_L, RigSchema.SHIN_L, RigSchema.FOOT_L,
        RigSchema.THIGH_R, RigSchema.SHIN_R, RigSchema.FOOT_R,
        RigSchema.HAT_L, RigSchema.HAT_R,
        RigSchema.EYE_CONTROL
    ]
    
    for b_name in bones_to_reset:
        bone = pose_bones.get(b_name)
        if bone:
            bone.location = Vector((0.0, 0.0, 0.0))
            if bone.rotation_mode == 'QUATERNION':
                bone.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
            elif bone.rotation_mode == 'AXIS_ANGLE':
                bone.rotation_axis_angle = (0.0, 0.0, 1.0, 0.0)
            else:
                bone.rotation_euler = Euler((0.0, 0.0, 0.0))
            bone.scale = Vector((1.0, 1.0, 1.0))
            
    # Update Blender's internal matrices before caching base values
    bpy.context.view_layer.update()

def _disable_creases_and_eyebrows(rig):
    """Searches and disables eyebrows and crease properties/control bones on frame 1 of all actions."""
    # Custom property targets
    targets = [rig, rig.data]
    for b_name in ["metarig.anim : MENU", "torso", "root", "head", "hips"]:
        b = rig.pose.bones.get(b_name)
        if b:
            targets.append(b)
            
    for target in targets:
        for key in list(target.keys()):
            key_lower = key.lower()
            if "crease" in key_lower or "eyebrow" in key_lower:
                try:
                    target[key] = 0.0
                    from rna_prop_ui import rna_idprop_ui_prop_update
                    rna_idprop_ui_prop_update(target, key)
                except Exception:
                    pass
                
                try:
                    target.keyframe_insert(data_path=f'["{key}"]', frame=1)
                except Exception:
                    pass

    # Process scales for direct bone-based toggle controls
    for bone in rig.pose.bones:
        b_name_lower = bone.name.lower()
        if ("crease" in b_name_lower or "eyebrow" in b_name_lower) and ("switch" in b_name_lower or "toggle" in b_name_lower or "on" in b_name_lower or "off" in b_name_lower or "ctrl" in b_name_lower or "menu" in b_name_lower):
            try:
                bone.scale = Vector((0.0, 0.0, 0.0))
                bone.keyframe_insert(data_path="scale", frame=1)
            except Exception:
                pass

# ===================================================================
# KEYFRAMING UTILITY HELPERS
# ===================================================================
def _keyframe_legs(rig, l_leg_swing, r_leg_swing, l_leg_sway, r_leg_sway, l_knee_bend, r_knee_bend, l_foot_flex, r_foot_flex, frame):
    """Sets standard dynamic keyframes on the leg chains."""
    if rig.thigh_l.bone:
        rig.thigh_l.rotation_euler = Euler((rig.thigh_l.base_rot.x - l_leg_swing, rig.thigh_l.base_rot.y, rig.thigh_l.base_rot.z + l_leg_sway))
        rig.thigh_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.shin_l.bone:
        rig.shin_l.rotation_euler = Euler((rig.shin_l.base_rot.x + l_knee_bend, rig.shin_l.base_rot.y, rig.shin_l.base_rot.z))
        rig.shin_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.foot_l.bone:
        rig.foot_l.rotation_euler = Euler((rig.foot_l.base_rot.x + l_foot_flex, rig.foot_l.base_rot.y, rig.foot_l.base_rot.z))
        rig.foot_l.keyframe_insert("rotation_euler", frame=frame)

    if rig.thigh_r.bone:
        rig.thigh_r.rotation_euler = Euler((rig.thigh_r.base_rot.x - r_leg_swing, rig.thigh_r.base_rot.y, rig.thigh_r.base_rot.z + (-1.0 * r_leg_sway)))
        rig.thigh_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.shin_r.bone:
        rig.shin_r.rotation_euler = Euler((rig.shin_r.base_rot.x + r_knee_bend, rig.shin_r.base_rot.y, rig.shin_r.base_rot.z))
        rig.shin_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.foot_r.bone:
        rig.foot_r.rotation_euler = Euler((rig.foot_r.base_rot.x + r_foot_flex, rig.foot_r.base_rot.y, rig.foot_r.base_rot.z))
        rig.foot_r.keyframe_insert("rotation_euler", frame=frame)

def _keyframe_arms_idle(rig, breath_phase, arm_sway_rad, wrist_sway_rad, frame):
    """Calculates and sets passive organic arm breathing movements."""
    delayed_breath = get_organic_breath_curve(breath_phase - 0.5)
    
    if rig.up_arm_l.bone:
        z_offset = delayed_breath * arm_sway_rad
        rig.up_arm_l.rotation_euler = Euler((rig.up_arm_l.base_rot.x, rig.up_arm_l.base_rot.y, rig.up_arm_l.base_rot.z + z_offset))
        rig.up_arm_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.fore_arm_l.bone:
        elbow_offset = get_organic_breath_curve(breath_phase - 0.7) * arm_sway_rad
        rig.fore_arm_l.rotation_euler = Euler((rig.fore_arm_l.base_rot.x + elbow_offset, rig.fore_arm_l.base_rot.y, rig.fore_arm_l.base_rot.z))
        rig.fore_arm_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.hand_l.bone:
        wrist_offset = get_organic_breath_curve(breath_phase - 1.0) * wrist_sway_rad
        rig.hand_l.rotation_euler = Euler((rig.hand_l.base_rot.x + wrist_offset, rig.hand_l.base_rot.y, rig.hand_l.base_rot.z))
        rig.hand_l.keyframe_insert("rotation_euler", frame=frame)

    if rig.up_arm_r.bone:
        z_offset = delayed_breath * arm_sway_rad
        rig.up_arm_r.rotation_euler = Euler((rig.up_arm_r.base_rot.x, rig.up_arm_r.base_rot.y, rig.up_arm_r.base_rot.z - z_offset))
        rig.up_arm_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.fore_arm_r.bone:
        elbow_offset = get_organic_breath_curve(breath_phase - 0.7) * arm_sway_rad
        rig.fore_arm_r.rotation_euler = Euler((rig.fore_arm_r.base_rot.x + elbow_offset, rig.fore_arm_r.base_rot.y, rig.fore_arm_r.base_rot.z))
        rig.fore_arm_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.hand_r.bone:
        wrist_offset = get_organic_breath_curve(breath_phase - 1.0) * wrist_sway_rad
        rig.hand_r.rotation_euler = Euler((rig.hand_r.base_rot.x + wrist_offset, rig.hand_r.base_rot.y, rig.hand_r.base_rot.z))
        rig.hand_r.keyframe_insert("rotation_euler", frame=frame)

def _keyframe_arms_locomotion(rig, walk_phase, swing_amp, sway_amp, frame):
    """Applies rhythmic swing and sway parameters to standard locomotion chains."""
    l_swing = math.sin(walk_phase) * swing_amp
    r_swing = -math.sin(walk_phase) * swing_amp
    z_offset = math.cos(walk_phase) * sway_amp

    if rig.up_arm_l.bone:
        rig.up_arm_l.rotation_euler = Euler((rig.up_arm_l.base_rot.x + l_swing, rig.up_arm_l.base_rot.y, rig.up_arm_l.base_rot.z + z_offset))
        rig.up_arm_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.fore_arm_l.bone:
        elbow_offset = math.sin(walk_phase - 0.5) * swing_amp * 0.4
        rig.fore_arm_l.rotation_euler = Euler((rig.fore_arm_l.base_rot.x + elbow_offset, rig.fore_arm_l.base_rot.y, rig.fore_arm_l.base_rot.z))
        rig.fore_arm_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.hand_l.bone:
        wrist_offset = math.cos(walk_phase - 1.0) * swing_amp * 0.15
        rig.hand_l.rotation_euler = Euler((rig.hand_l.base_rot.x + wrist_offset, rig.hand_l.base_rot.y, rig.hand_l.base_rot.z))
        rig.hand_l.keyframe_insert("rotation_euler", frame=frame)

    if rig.up_arm_r.bone:
        get_z = rig.up_arm_r.base_rot.z - z_offset if rig.up_arm_r.base_rot else -z_offset
        rig.up_arm_r.rotation_euler = Euler((rig.up_arm_r.base_rot.x + r_swing, rig.up_arm_r.base_rot.y, get_z))
        rig.up_arm_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.fore_arm_r.bone:
        elbow_offset = math.sin(walk_phase - 0.5 - math.pi) * swing_amp * 0.4
        rig.fore_arm_r.rotation_euler = Euler((rig.fore_arm_r.base_rot.x + elbow_offset, rig.fore_arm_r.base_rot.y, rig.fore_arm_r.base_rot.z))
        rig.fore_arm_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.hand_r.bone:
        wrist_offset = math.cos(walk_phase - 1.0 - math.pi) * swing_amp * 0.15
        rig.hand_r.rotation_euler = Euler((rig.hand_r.base_rot.x + wrist_offset, rig.hand_r.base_rot.y, rig.hand_r.base_rot.z))
        rig.hand_r.keyframe_insert("rotation_euler", frame=frame)

def _keyframe_fingers(rig, breath_phase, flex_offset_is_locomotion, finger_base, frame):
    """Processes dynamic finger curls, handling parent IK/FK holds cleanly."""
    pose_bones = rig.pose_bones
    finger_bases = ["f_index", "f_middle", "f_ring", "thumb"]
    finger_delays = {"f_index": 0.0, "f_middle": -0.2, "f_ring": -0.4, "thumb": -0.6}

    for side in ["L", "R"]:
        # Standardize negative rotation so fingers curl inwards on both mirrored sides
        flip = -1.0

        arm_parent = pose_bones.get(getattr(RigSchema, f"ARM_PARENT_{side}"))
        if arm_parent: arm_parent.keyframe_insert(data_path='["IK_FK"]', frame=frame)
        leg_parent = pose_bones.get(getattr(RigSchema, f"LEG_PARENT_{side}"))
        if leg_parent: leg_parent.keyframe_insert(data_path='["IK_FK"]', frame=frame)

        for f_base in finger_bases:
            delay = finger_delays[f_base]
            if flex_offset_is_locomotion:
                flex_offset = math.sin(breath_phase + delay) * FINGER_FLEX_RANGE
            else:
                flex_offset = get_organic_breath_curve(breath_phase + delay) * FINGER_FLEX_RANGE

            for seg in ["01", "02", "03"]:
                bone_name = f"{f_base}.{seg}.{side}"
                bone = pose_bones.get(bone_name)
                if bone:
                    bone.rotation_mode = 'XYZ'
                    if f_base == "thumb":
                        target_rot_x = flip * (math.radians(10) + flex_offset * 0.5)
                        target_rot_y = flip * math.radians(5)
                        bone.rotation_euler = Euler((target_rot_x, target_rot_y, 0.0))
                    else:
                        base_angle = math.radians(finger_base * 80 * (1.0 if seg == "01" else 1.2))
                        bone.rotation_euler = Euler((flip * (base_angle + flex_offset), 0.0, 0.0))
                    bone.keyframe_insert(data_path="rotation_euler", frame=frame)

def _keyframe_eyelids(rig, blink_factor, frame):
    """Sets stylized location offsets so eyelids are open by default and close when blinking."""
    # When blink_factor is 0.0 (default), offset is at 100% of BLINK_TRAVEL (eyes open).
    # When blink_factor is 1.0 (blinking), offset is 0.0 (eyes closed to rest pose).
    open_factor = 1.0 - blink_factor
    upper_offset = BLINK_TRAVEL_UPPER * open_factor
    lower_offset = BLINK_TRAVEL_LOWER * open_factor

    for side in ["l", "r"]:
        top = getattr(rig, f"eyelid_top_{side}")
        bot = getattr(rig, f"eyelid_bot_{side}")
        if top.bone:
            # Shift along local vertical Y-axis and clamp defensively to the rig's physical [0.0, 1.0] limits
            target_y = max(0.0, min(1.0, upper_offset))
            top.location = Vector((0.0, target_y, 0.0))
            top.keyframe_insert("location", frame=frame)
        if bot.bone:
            # Shift along local vertical Y-axis and clamp defensively to the rig's physical [-1.0, 0.0] limits
            target_y = max(-1.0, min(0.0, lower_offset))
            bot.location = Vector((0.0, target_y, 0.0))
            bot.keyframe_insert("location", frame=frame)

def _keyframe_eyes(rig, progress, eye_activity, frame):
    """Translates the eye target controller to simulate saccadic movements."""
    if not rig.eye_control.bone:
        return

    saccade_x, saccade_z = get_eye_saccade_relative(progress)
    
    # Scale saccades up for the master target bone (eyeControl)
    offset_x = saccade_x * eye_activity * EYE_SACCADE_SCALE
    offset_z = saccade_z * eye_activity * EYE_SACCADE_SCALE

    # Translate along local X (horizontal) and Z (vertical)
    rig.eye_control.location = rig.eye_control.base_loc + Vector((offset_x, 0.0, offset_z))
    rig.eye_control.keyframe_insert("location", frame=frame)

def _keyframe_hat_sway(rig, breath_phase, walk_phase, is_locomotion, frame):
    """Handles shared hat sway mathematics over both passive breathing and physical movement."""
    sway_offset = math.cos(breath_phase) * math.radians(2.5)
    walk_sway = (math.cos(walk_phase * 2) * math.radians(1.5)) if is_locomotion else 0.0

    if rig.hat_l.bone:
        rig.hat_l.rotation_euler = Euler((rig.hat_l.base_rot.x, rig.hat_l.base_rot.y + sway_offset + walk_sway, rig.hat_l.base_rot.z))
        rig.hat_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.hat_r.bone:
        rig.hat_r.rotation_euler = Euler((rig.hat_r.base_rot.x, rig.hat_r.base_rot.y - sway_offset - walk_sway, rig.hat_r.base_rot.z))
        rig.hat_r.keyframe_insert("rotation_euler", frame=frame)


# ===================================================================
# STATE GENERATORS
# ===================================================================
def generate_idle(rig, duration):
    """Generates the IDLE resting breathing animation action."""
    _prepare_rig_for_animation(rig)
    _reset_pose_to_rest(rig)
    rig_data = CharacterRig(rig, is_locomotion=False)

    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = duration

    arm_sway_rad = math.radians(ARM_SWAY_RANGE)
    wrist_sway_rad = math.radians(WRIST_SWAY_RANGE)

    for frame in range(1, duration + 1):
        bpy.context.scene.frame_set(frame)
        progress = (frame - 1) / duration

        sway_phase = progress * 2 * math.pi
        breath_phase = progress * 2 * math.pi
        organic_breath = get_organic_breath_curve(breath_phase)
        delayed_breath = get_organic_breath_curve(breath_phase - 0.5)

        # On frame 1, robustly disable creases and eyebrows to make them OFF by default
        if frame == 1:
            _disable_creases_and_eyebrows(rig)

        # A. Torso Translation & Lean
        if rig_data.torso.bone:
            x_sway = math.sin(sway_phase) * 0.015
            y_sway = math.cos(sway_phase) * 0.006
            z_breath = organic_breath * 0.012
            rig_data.torso.location = rig_data.torso.base_loc + Vector((x_sway, y_sway, z_breath))
            rig_data.torso.rotation_euler = Euler((math.radians(4.0), 0.0, rig_data.torso.base_rot.z))
            rig_data.torso.keyframe_insert("location", frame=frame)
            rig_data.torso.keyframe_insert("rotation_euler", frame=frame)

        # B. Hips Tilt
        if rig_data.hips.bone:
            hips_tilt = organic_breath * math.radians(1.0)
            rig_data.hips.rotation_euler = Euler((rig_data.hips.base_rot.x - hips_tilt, rig_data.hips.base_rot.y, rig_data.hips.base_rot.z))
            rig_data.hips.keyframe_insert("rotation_euler", frame=frame)

        # C. Spine & Chest
        if rig_data.spine_0.bone:
            s0_pitch = rig_data.spine_0.base_rot.x + organic_breath * math.radians(0.5)
            rig_data.spine_0.rotation_euler = Euler((s0_pitch, rig_data.spine_0.base_rot.y, rig_data.spine_0.base_rot.z))
            rig_data.spine_0.keyframe_insert("rotation_euler", frame=frame)
        if rig_data.spine_1.bone:
            s1_pitch = rig_data.spine_1.base_rot.x + organic_breath * math.radians(0.3)
            rig_data.spine_1.rotation_euler = Euler((s1_pitch, rig_data.spine_1.base_rot.y, rig_data.spine_1.base_rot.z))
            rig_data.spine_1.keyframe_insert("rotation_euler", frame=frame)
        if rig_data.chest.bone:
            chest_tilt = delayed_breath * math.radians(1.8)
            rig_data.chest.rotation_euler = Euler((rig_data.chest.base_rot.x + chest_tilt, rig_data.chest.base_rot.y, rig_data.chest.base_rot.z))
            rig_data.chest.keyframe_insert("rotation_euler", frame=frame)

        # D. Head Compensation
        if rig_data.head.bone:
            head_compensate = organic_breath * math.radians(0.8)
            rig_data.head.rotation_euler = Euler((rig_data.head.base_rot.x - head_compensate, rig_data.head.base_rot.y, rig_data.head.base_rot.z))
            rig_data.head.keyframe_insert("rotation_euler", frame=frame)

        # E. FK Arm Breathing Sway
        _keyframe_arms_idle(rig_data, breath_phase, arm_sway_rad, wrist_sway_rad, frame)

        # F. Legs (static, lock default rotation)
        for side in ["l", "r"]:
            for joint in ["thigh", "shin", "foot"]:
                b = getattr(rig_data, f"{joint}_{side}")
                if b.bone:
                    b.rotation_euler = b.base_rot.copy()
                    b.keyframe_insert("rotation_euler", frame=frame)

        # G. Fingers, Eye Blinks, & Eye Saccades
        _keyframe_fingers(rig_data, breath_phase, flex_offset_is_locomotion=False, finger_base=0.25, frame=frame)
        _keyframe_eyelids(rig_data, get_blink_factor_relative(progress, duration), frame)
        _keyframe_eyes(rig_data, progress, eye_activity=1.0, frame=frame)

        # J. Hat Secondary Sway
        _keyframe_hat_sway(rig_data, breath_phase, 0.0, is_locomotion=False, frame=frame)


def generate_walking(rig, duration):
    """Generates the dynamic WALKING loop action."""
    _prepare_rig_for_animation(rig)
    _reset_pose_to_rest(rig)
    rig_data = CharacterRig(rig, is_locomotion=True)

    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = duration

    for frame in range(1, duration + 1):
        bpy.context.scene.frame_set(frame)
        progress = (frame - 1) / duration

        walk_phase = progress * 2 * math.pi
        sway_phase = walk_phase
        breath_phase = walk_phase
        organic_breath = math.sin(breath_phase)
        delayed_breath = math.sin(breath_phase - 0.5)

        # On frame 1, robustly disable creases and eyebrows to make them OFF by default
        if frame == 1:
            _disable_creases_and_eyebrows(rig)

        # A. Torso Translation & Lean
        if rig_data.torso.bone:
            x_sway = math.sin(sway_phase) * 0.035
            z_bounce = (abs(math.cos(walk_phase)) - 0.5) * 2.0 * 0.045
            y_sway = -0.05 + (abs(math.cos(walk_phase)) - 0.5) * 2.0 * 0.015
            z_breath = organic_breath * 0.015

            rig_data.torso.location = rig_data.torso.base_loc + Vector((x_sway, y_sway, z_breath + z_bounce))
            rig_data.torso.rotation_euler = Euler((math.radians(10.0), 0.0, rig_data.torso.base_rot.z))
            rig_data.torso.keyframe_insert("location", frame=frame)
            rig_data.torso.keyframe_insert("rotation_euler", frame=frame)

        # B. Hips & Spine Core twisting
        if rig_data.hips.bone:
            hips_tilt = organic_breath * math.radians(1.0)
            hips_pitch = rig_data.hips.base_rot.x - hips_tilt + math.cos(walk_phase * 2) * 0.035
            hips_roll = rig_data.hips.base_rot.y + math.sin(walk_phase) * 0.08
            hips_yaw = rig_data.hips.base_rot.z + math.sin(walk_phase) * 0.12
            rig_data.hips.rotation_euler = Euler((hips_pitch, hips_roll, hips_yaw))
            rig_data.hips.keyframe_insert("rotation_euler", frame=frame)

        if rig_data.spine_0.bone:
            s0_pitch = rig_data.spine_0.base_rot.x + math.cos(walk_phase * 2) * 0.02
            s0_yaw = rig_data.spine_0.base_rot.z + math.sin(walk_phase) * -0.03
            rig_data.spine_0.rotation_euler = Euler((s0_pitch, rig_data.spine_0.base_rot.y, s0_yaw))
            rig_data.spine_0.keyframe_insert("rotation_euler", frame=frame)
        if rig_data.spine_1.bone:
            s1_pitch = rig_data.spine_1.base_rot.x + math.cos(walk_phase * 2 - 0.5) * (0.02 * 0.7)
            s1_yaw = rig_data.spine_1.base_rot.z + math.sin(walk_phase - 0.5) * (-0.03 * 0.7)
            rig_data.spine_1.rotation_euler = Euler((s1_pitch, rig_data.spine_1.base_rot.y, s1_yaw))
            rig_data.spine_1.keyframe_insert("rotation_euler", frame=frame)

        if rig_data.chest.bone:
            chest_tilt = delayed_breath * math.radians(1.8)
            chest_pitch = rig_data.chest.base_rot.x + chest_tilt + math.cos(walk_phase * 2) * 0.02
            chest_roll = rig_data.chest.base_rot.y - math.sin(walk_phase) * 0.05
            chest_yaw = rig_data.chest.base_rot.z + math.sin(walk_phase) * -0.08
            rig_data.chest.rotation_euler = Euler((chest_pitch, chest_roll, chest_yaw))
            rig_data.chest.keyframe_insert("rotation_euler", frame=frame)

        # D. Head Compensation
        if rig_data.head.bone:
            head_compensate = organic_breath * math.radians(0.8)
            walk_stabilize = math.sin(walk_phase * 2) * math.radians(1.0)
            rig_data.head.rotation_euler = Euler((rig_data.head.base_rot.x - head_compensate + walk_stabilize, rig_data.head.base_rot.y, rig_data.head.base_rot.z))
            rig_data.head.keyframe_insert("rotation_euler", frame=frame)

        # E. Dynamic Arm Swing
        _keyframe_arms_locomotion(rig_data, walk_phase, swing_amp=0.40, sway_amp=0.05, frame=frame)

        # F. Leg Stride Setup & Movement
        l_leg_sway = 0.0
        r_leg_sway = 0.0
        l_leg_swing = math.sin(walk_phase) * 0.58
        r_leg_swing = -math.sin(walk_phase) * 0.58
        l_knee_bend = max(0, math.cos(walk_phase)) * 0.55
        r_knee_bend = max(0, -math.cos(walk_phase)) * 0.55
        l_foot_flex = -math.sin(walk_phase) * 0.25
        r_foot_flex = math.sin(walk_phase) * 0.25

        _keyframe_legs(rig_data, l_leg_swing, r_leg_swing, l_leg_sway, r_leg_sway, l_knee_bend, r_knee_bend, l_foot_flex, r_foot_flex, frame)

        # G. Fingers, Eye Blinks (Disabled), & Eye Saccades
        _keyframe_fingers(rig_data, breath_phase, flex_offset_is_locomotion=True, finger_base=0.35, frame=frame)
        _keyframe_eyelids(rig_data, 0.0, frame)
        _keyframe_eyes(rig_data, progress, eye_activity=0.3, frame=frame)

        # J. Hat Secondary Sway
        _keyframe_hat_sway(rig_data, breath_phase, walk_phase, is_locomotion=True, frame=frame)


def generate_running(rig, duration):
    """Generates the energetic RUNNING cycle action."""
    _prepare_rig_for_animation(rig)
    _reset_pose_to_rest(rig)
    rig_data = CharacterRig(rig, is_locomotion=True)

    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = duration

    for frame in range(1, duration + 1):
        bpy.context.scene.frame_set(frame)
        progress = (frame - 1) / duration

        walk_phase = progress * 2 * math.pi
        sway_phase = walk_phase
        breath_phase = walk_phase
        organic_breath = math.sin(breath_phase)
        delayed_breath = math.sin(breath_phase - 0.5)

        # On frame 1, robustly disable creases and eyebrows to make them OFF by default
        if frame == 1:
            _disable_creases_and_eyebrows(rig)

        # A. Torso Translation & Athletic Forward Lean
        if rig_data.torso.bone:
            x_sway = math.sin(sway_phase) * 0.05
            z_bounce = (abs(math.cos(walk_phase)) - 0.5) * 2.0 * 0.10
            y_sway = -0.14 + (abs(math.cos(walk_phase)) - 0.5) * 2.0 * 0.035
            z_breath = organic_breath * 0.025

            rig_data.torso.location = rig_data.torso.base_loc + Vector((x_sway, y_sway, z_breath + z_bounce))
            rig_data.torso.rotation_euler = Euler((math.radians(22.0), 0.0, rig_data.torso.base_rot.z))
            rig_data.torso.keyframe_insert("location", frame=frame)
            rig_data.torso.keyframe_insert("rotation_euler", frame=frame)

        # B. Highly Aggressive Pelvic/Spine Pitch and Roll
        if rig_data.hips.bone:
            hips_tilt = organic_breath * math.radians(1.0)
            hips_pitch = rig_data.hips.base_rot.x - hips_tilt + math.cos(walk_phase * 2) * 0.12
            hips_roll = rig_data.hips.base_rot.y + math.sin(walk_phase) * 0.14
            hips_yaw = rig_data.hips.base_rot.z + math.sin(walk_phase) * 0.24
            rig_data.hips.rotation_euler = Euler((hips_pitch, hips_roll, hips_yaw))
            rig_data.hips.keyframe_insert("rotation_euler", frame=frame)

        if rig_data.spine_0.bone:
            s0_pitch = rig_data.spine_0.base_rot.x + math.cos(walk_phase * 2) * 0.05
            s0_yaw = rig_data.spine_0.base_rot.z + math.sin(walk_phase) * -0.07
            rig_data.spine_0.rotation_euler = Euler((s0_pitch, rig_data.spine_0.base_rot.y, s0_yaw))
            rig_data.spine_0.keyframe_insert("rotation_euler", frame=frame)
        if rig_data.spine_1.bone:
            s1_pitch = rig_data.spine_1.base_rot.x + math.cos(walk_phase * 2 - 0.5) * (0.05 * 0.7)
            s1_yaw = rig_data.spine_1.base_rot.z + math.sin(walk_phase - 0.5) * (-0.07 * 0.7)
            rig_data.spine_1.rotation_euler = Euler((s1_pitch, rig_data.spine_1.base_rot.y, s1_yaw))
            rig_data.spine_1.keyframe_insert("rotation_euler", frame=frame)

        if rig_data.chest.bone:
            chest_tilt = delayed_breath * math.radians(1.8)
            chest_pitch = rig_data.chest.base_rot.x + chest_tilt + math.cos(walk_phase * 2) * 0.06
            chest_roll = rig_data.chest.base_rot.y - math.sin(walk_phase) * 0.09
            chest_yaw = rig_data.chest.base_rot.z + math.sin(walk_phase) * -0.16
            rig_data.chest.rotation_euler = Euler((chest_pitch, chest_roll, chest_yaw))
            rig_data.chest.keyframe_insert("rotation_euler", frame=frame)

        # D. Head Compensation
        if rig_data.head.bone:
            head_compensate = organic_breath * math.radians(0.8)
            walk_stabilize = math.sin(walk_phase * 2) * math.radians(1.0)
            rig_data.head.rotation_euler = Euler((rig_data.head.base_rot.x - head_compensate + walk_stabilize, rig_data.head.base_rot.y, rig_data.head.base_rot.z))
            rig_data.head.keyframe_insert("rotation_euler", frame=frame)

        # E. Fast Athletic Arm Pumps
        _keyframe_arms_locomotion(rig_data, walk_phase, swing_amp=0.80, sway_amp=0.08, frame=frame)

        # F. Powerful Stride Legs
        l_leg_sway = 0.0
        r_leg_sway = 0.0
        l_leg_swing = math.sin(walk_phase) * 1.15
        r_leg_swing = -math.sin(walk_phase) * 1.15
        l_knee_bend = max(0, math.cos(walk_phase)) * 1.10
        r_knee_bend = max(0, -math.cos(walk_phase)) * 1.10
        l_foot_flex = -math.sin(walk_phase) * 0.45
        r_foot_flex = math.sin(walk_phase) * 0.45

        _keyframe_legs(rig_data, l_leg_swing, r_leg_swing, l_leg_sway, r_leg_sway, l_knee_bend, r_knee_bend, l_foot_flex, r_foot_flex, frame)

        # G. Loose Fist Fingers, Blinks (Disabled), & Focus Eye Saccades
        _keyframe_fingers(rig_data, breath_phase, flex_offset_is_locomotion=True, finger_base=0.65, frame=frame)
        _keyframe_eyelids(rig_data, 0.0, frame)
        _keyframe_eyes(rig_data, progress, eye_activity=0.1, frame=frame)

        # J. Hat Secondary Sway
        _keyframe_hat_sway(rig_data, breath_phase, walk_phase, is_locomotion=True, frame=frame)


def generate_strafe(rig, duration):
    """Generates the sidestepping STRAFE loop action."""
    _prepare_rig_for_animation(rig)
    _reset_pose_to_rest(rig)
    rig_data = CharacterRig(rig, is_locomotion=True)

    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = duration

    for frame in range(1, duration + 1):
        bpy.context.scene.frame_set(frame)
        progress = (frame - 1) / duration

        walk_phase = progress * 2 * math.pi
        sway_phase = walk_phase
        breath_phase = walk_phase
        organic_breath = math.sin(breath_phase)
        delayed_breath = math.sin(breath_phase - 0.5)

        # On frame 1, robustly disable creases and eyebrows to make them OFF by default
        if frame == 1:
            _disable_creases_and_eyebrows(rig)

        # A. Torso Translation & Lateral Weight Sway
        if rig_data.torso.bone:
            x_sway = math.sin(sway_phase) * 0.10
            z_bounce = (abs(math.cos(walk_phase)) - 0.5) * 2.0 * 0.035
            y_sway = (abs(math.cos(walk_phase)) - 0.5) * 2.0 * 0.008
            z_breath = organic_breath * 0.018

            rig_data.torso.location = rig_data.torso.base_loc + Vector((x_sway, y_sway, z_breath + z_bounce))
            
            torso_roll = math.sin(sway_phase) * math.radians(5.0)
            rig_data.torso.rotation_euler = Euler((0.0, torso_roll, rig_data.torso.base_rot.z))
            
            rig_data.torso.keyframe_insert("location", frame=frame)
            rig_data.torso.keyframe_insert("rotation_euler", frame=frame)

        # B. Hips Twist & Spine Side-bending Stabilizers
        if rig_data.hips.bone:
            hips_tilt = organic_breath * math.radians(1.0)
            hips_pitch = rig_data.hips.base_rot.x - hips_tilt + math.cos(walk_phase * 2) * 0.02
            hips_roll = rig_data.hips.base_rot.y + math.sin(walk_phase) * 0.09
            hips_yaw = rig_data.hips.base_rot.z + math.sin(walk_phase) * 0.03
            rig_data.hips.rotation_euler = Euler((hips_pitch, hips_roll, hips_yaw))
            rig_data.hips.keyframe_insert("rotation_euler", frame=frame)

        if rig_data.spine_0.bone:
            s0_pitch = rig_data.spine_0.base_rot.x + math.cos(walk_phase * 2) * 0.01
            s0_yaw = rig_data.spine_0.base_rot.z
            rig_data.spine_0.rotation_euler = Euler((s0_pitch, rig_data.spine_0.base_rot.y, s0_yaw))
            rig_data.spine_0.keyframe_insert("rotation_euler", frame=frame)
        if rig_data.spine_1.bone:
            s1_pitch = rig_data.spine_1.base_rot.x + math.cos(walk_phase * 2 - 0.5) * (0.01 * 0.7)
            s1_yaw = rig_data.spine_1.base_rot.z
            rig_data.spine_1.rotation_euler = Euler((s1_pitch, rig_data.spine_1.base_rot.y, s1_yaw))
            rig_data.spine_1.keyframe_insert("rotation_euler", frame=frame)

        if rig_data.chest.bone:
            chest_tilt = delayed_breath * math.radians(1.8)
            chest_pitch = rig_data.chest.base_rot.x + chest_tilt + math.cos(walk_phase * 2) * 0.01
            chest_roll = rig_data.chest.base_rot.y - math.sin(walk_phase) * 0.06
            chest_yaw = rig_data.chest.base_rot.z
            rig_data.chest.rotation_euler = Euler((chest_pitch, chest_roll, chest_yaw))
            rig_data.chest.keyframe_insert("rotation_euler", frame=frame)

        # D. Head Compensation
        if rig_data.head.bone:
            head_compensate = organic_breath * math.radians(0.8)
            walk_stabilize = math.sin(walk_phase * 2) * math.radians(1.0)
            rig_data.head.rotation_euler = Euler((rig_data.head.base_rot.x - head_compensate + walk_stabilize, rig_data.head.base_rot.y, rig_data.head.base_rot.z))
            rig_data.head.keyframe_insert("rotation_euler", frame=frame)

        # E. Arms Balanced Sway
        _keyframe_arms_locomotion(rig_data, walk_phase, swing_amp=0.12, sway_amp=0.14, frame=frame)

        # F. Sidestepping Leg Swings
        l_leg_sway = max(0, math.sin(walk_phase)) * 0.32
        r_leg_sway = max(0, -math.sin(walk_phase)) * 0.32
        l_leg_swing = 0.0
        r_leg_swing = 0.0
        l_knee_bend = max(0, math.sin(walk_phase)) * 0.38
        r_knee_bend = max(0, -math.sin(walk_phase)) * 0.38
        l_foot_flex = -math.sin(walk_phase) * 0.12
        r_foot_flex = math.sin(walk_phase) * 0.12

        _keyframe_legs(rig_data, l_leg_swing, r_leg_swing, l_leg_sway, r_leg_sway, l_knee_bend, r_knee_bend, l_foot_flex, r_foot_flex, frame)

        # G. Fingers, Eye Blinks (Disabled), & Strafe Eye Saccades
        _keyframe_fingers(rig_data, breath_phase, flex_offset_is_locomotion=True, finger_base=0.30, frame=frame)
        _keyframe_eyelids(rig_data, 0.0, frame)
        _keyframe_eyes(rig_data, progress, eye_activity=0.6, frame=frame)

        # J. Hat Secondary Sway
        _keyframe_hat_sway(rig_data, breath_phase, walk_phase, is_locomotion=True, frame=frame)


# ===================================================================
# AUTOMATIC TIMELINE SCALER HANDLER
# ===================================================================
def auto_adjust_timeline_range(scene):
    """Snaps the active timeline end frame based on active state name."""
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

for h in list(bpy.app.handlers.frame_change_pre):
    if h.__name__ == "auto_adjust_timeline_range":
        bpy.app.handlers.frame_change_pre.remove(h)

bpy.app.handlers.frame_change_pre.append(auto_adjust_timeline_range)


# ===================================================================
# BATCH EXPORTER INTERFACE
# ===================================================================
def generate_all_actions():
    """Builds and links each separate state loop to the armature."""
    rig = bpy.context.active_object
    if not rig or rig.type != 'ARMATURE':
        print("Please select your character's armature in the 3D Viewport.")
        return

    prefix = rig.name.lower()
    if prefix.endswith(".anim") or prefix.endswith(".retarget"):
        prefix = prefix.split(".")[0]

    states = {
        'IDLE': (generate_idle, 120),
        'WALKING': (generate_walking, 36),
        'RUNNING': (generate_running, 24),
        'STRAFE': (generate_strafe, 40)
    }

    if not rig.animation_data:
        rig.animation_data_create()

    rig.animation_data.action = None

    for state_type, (gen_func, duration) in states.items():
        action_name = f"{prefix}_{state_type}"
        
        old_action = bpy.data.actions.get(action_name)
        if old_action:
            bpy.data.actions.remove(old_action)

        action = bpy.data.actions.new(name=action_name)
        action.use_fake_user = True
        
        rig.animation_data.action = action
        
        print(f"Generating action: '{action_name}' ({duration} frames)...")
        gen_func(rig, duration)

    # Re-assign IDLE as default preview
    idle_action = bpy.data.actions.get(f"{prefix}_IDLE")
    if idle_action:
        rig.animation_data.action = idle_action
        bpy.context.scene.frame_start = 1
        bpy.context.scene.frame_end = 120
        bpy.context.scene.frame_set(1)

    bpy.context.view_layer.update()
    bpy.ops.screen.animation_play()
    print("Batch animation export complete. Actions are saved in your Action Editor.")

# Run batch exporter
generate_all_actions()