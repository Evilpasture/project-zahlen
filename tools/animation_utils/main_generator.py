import bpy
import math
from mathutils import Vector, Euler

# ===================================================================
# CUSTOM RIG ADJUSTMENTS
# ===================================================================
BLINK_TRAVEL_UPPER = 0.1  # Top eyelid travel down (Positive local Y)
BLINK_TRAVEL_LOWER = -0.1  # Bottom eyelid travel up (Negative local Y)

FINGER_FLEX_RANGE = 0.08  # Base finger flex amplitude
ARM_SWAY_RANGE = 2.5  # Base arm swing amplitude (degrees)
WRIST_SWAY_RANGE = 4.0  # Base wrist flop amplitude (degrees)

EYE_SACCADE_SCALE = 4.0  # Subtle scale multiplier for the eye target controller


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
        self.base_loc = (
            pose_bone.location.copy() if pose_bone else Vector((0.0, 0.0, 0.0))
        )
        self.base_rot = (
            pose_bone.rotation_euler.copy() if pose_bone else Euler((0.0, 0.0, 0.0))
        )

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
            self.up_arm_l.base_rot = Euler((0.0, 0.0, math.radians(-82)))
        if self.fore_arm_l.bone:
            # A soft 15-degree resting elbow bend is often more natural than 25 degrees
            self.fore_arm_l.base_rot = Euler((math.radians(15), 0.0, 0.0))

        if self.up_arm_r.bone:
            self.up_arm_r.base_rot = Euler((0.0, 0.0, math.radians(82)))
        if self.fore_arm_r.bone:
            self.fore_arm_r.base_rot = Euler((math.radians(15), 0.0, 0.0))
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
    min_val = math.exp(-1)  # ~0.3679
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
# DECLARATIVE MOTION PROFILES
# ===================================================================
MOTION_PROFILES = {
    "IDLE": {
        "feel": "relaxed",
        "breath": {
            "rate": 1.0,
            "depth": 0.6,
            "shape": "organic",
        },
        "torso": {
            "style": "loop",
            "sway_x": 0.4,
            "sway_y": 0.2,
            "bob_z": 0.3,
            "lean": 4.0,
        },
        "arms": {
            "style": "passive",
            "sway": 0.5,
            "delay": 0.5,
            "flare": 2.0,  # 2-degree subtle rest offset
        },
        "fingers": {
            "curl": 0.25,
            "style": "organic",
        },
        "eyes": {
            "blink": True,
            "saccade": 1.0,
        },
        "legs": {
            "style": "static",
            "amplitude": 0.0,
        },
        "spine": {
            "flex": 0.4,  # Adds organic spine-flex to remove rigidity
            "hips_sway": 0.2,  # Hip weight-shift amplitude
        },
        "head": {
            "stabilize": 0.8,  # Smooths head tilt relative to core
        },
    },
    "WALKING": {
        "feel": "casual",
        "breath": {"rate": 1.0, "depth": 0.5, "shape": "sine"},
        "torso": {
            "style": "loop",
            "sway_x": 0.7,
            "sway_y": 0.3,
            "bob_z": 0.4,
            "lean": 10.0,
        },
        "arms": {"style": "swing", "sway": 0.6, "delay": 0.0, "flare": 0.0},
        "fingers": {"curl": 0.35, "style": "rhythmic"},
        "eyes": {"blink": False, "saccade": 0.3},
        "legs": {"style": "stride", "amplitude": 0.58},
        "spine": {"flex": 0.5, "hips_sway": 0.5},
        "head": {"stabilize": 0.85},
    },
    "RUNNING": {
        "feel": "athletic",
        "breath": {"rate": 1.0, "depth": 0.8, "shape": "sine"},
        "torso": {
            "style": "loop",
            "sway_x": 1.0,
            "sway_y": 0.7,
            "bob_z": 1.0,
            "lean": 22.0,
        },
        "arms": {"style": "pump", "sway": 1.0, "delay": 0.0, "flare": 0.0},
        "fingers": {"curl": 0.65, "style": "rhythmic"},
        "eyes": {"blink": False, "saccade": 0.1},
        "legs": {"style": "stride", "amplitude": 1.15},
        "spine": {"flex": 1.0, "hips_sway": 1.0},
        "head": {"stabilize": 0.90},
    },
    "STRAFE": {
        "feel": "cautious",
        "breath": {"rate": 1.0, "depth": 0.55, "shape": "sine"},
        "torso": {
            "style": "loop",
            "sway_x": 1.2,
            "sway_y": 0.15,
            "bob_z": 0.35,
            "lean": 0.0,
        },
        "arms": {
            "style": "out",
            "sway": 0.9,
            "delay": 0.0,
            "flare": 4.0,  # Reduced from 18.0 to prevent the stiff robotic look
        },
        "fingers": {"curl": 0.30, "style": "rhythmic"},
        "eyes": {"blink": False, "saccade": 0.6},
        "legs": {"style": "sidestep", "amplitude": 0.35},
        "spine": {"flex": 0.3, "hips_sway": 0.3},
        "head": {"stabilize": 0.80},
    },
    "JUMPING": {
        "feel": "energetic",
        "breath": {"rate": 0.0, "depth": 0.0, "shape": "sine"},
        "torso": {
            "style": "jump",
            "sway_x": 0.0,
            "sway_y": 0.0,
            "bob_z": 0.0,
            "lean": 0.0,
        },
        "arms": {"style": "jump", "sway": 1.0, "delay": 0.0, "flare": 0.0},
        "fingers": {"curl": 0.35, "style": "rhythmic"},
        "eyes": {"blink": False, "saccade": 0.4},
        "legs": {"style": "jump", "amplitude": 1.0},
        "spine": {"flex": 1.0, "hips_sway": 0.0},
        "head": {"stabilize": 1.0},
    },
    "LANDING": {
        "feel": "heavy",
        "breath": {"rate": 0.0, "depth": 0.0, "shape": "sine"},
        "torso": {
            "style": "land",
            "sway_x": 0.0,
            "sway_y": 0.0,
            "bob_z": 0.0,
            "lean": 0.0,
        },
        "arms": {"style": "land", "sway": 1.0, "delay": 0.0, "flare": 0.0},
        "fingers": {"curl": 0.45, "style": "rhythmic"},
        "eyes": {"blink": False, "saccade": 0.2},
        "legs": {"style": "land", "amplitude": 1.0},
        "spine": {"flex": 1.0, "hips_sway": 0.0},
        "head": {"stabilize": 1.0},
    },
}


# ===================================================================
# PROFILE RESOLVER — config → raw motion parameters
# ===================================================================
_TORSO_SWAY_X_MAX = 0.05  # metres, full-scale lateral
_TORSO_SWAY_Y_MAX = 0.015  # metres, full-scale fore-aft
_TORSO_BOB_Z_MAX = 0.10  # metres, full-scale vertical
_ARM_SWING_MAX = 0.80  # radians, full-scale upper arm swing
_ARM_SWAY_MAX = 0.14  # radians, full-scale arm sway
_LEG_STRIDE_MAX = 1.15  # radians, full-scale thigh swing
_SPINE_FLEX_MAX = 0.08  # radians, full-scale spine sway
_HIPS_SWAY_MAX = 0.24  # radians, full-scale pelvic counter-swing


class ResolvedProfile:
    """Translates a declarative profile dict into ready-to-use motion scalars."""

    def __init__(self, profile: dict):
        p = profile

        # Breath
        b = p["breath"]
        self.breath_rate = b["rate"]
        self.breath_depth = b["depth"]
        self.breath_shape = b["shape"]

        # Torso
        t = p["torso"]
        self.torso_style = t.get("style", "loop")
        self.torso_sway_x = t["sway_x"] * _TORSO_SWAY_X_MAX
        self.torso_sway_y = t["sway_y"] * _TORSO_SWAY_Y_MAX
        self.torso_bob_z = t["bob_z"] * _TORSO_BOB_Z_MAX
        self.torso_lean = math.radians(t["lean"])

        # Arms
        a = p["arms"]
        self.arm_style = a["style"]
        self.arm_swing = a["sway"] * _ARM_SWING_MAX
        self.arm_sway = a["sway"] * _ARM_SWAY_MAX
        self.arm_delay = a["delay"]
        self.arm_flare = math.radians(
            a.get("flare", 0.0)
        )  # Resolves outward abduction offset

        # Fingers
        f = p["fingers"]
        self.finger_curl = f["curl"]
        self.finger_style = f["style"]

        # Eyes
        e = p["eyes"]
        self.eye_blink = e["blink"]
        self.eye_saccade = e["saccade"]

        # Legs
        l = p["legs"]
        self.leg_style = l["style"]
        self.leg_amplitude = l["amplitude"] * _LEG_STRIDE_MAX

        # Spine Flex & Hips
        s = p.get("spine", {"flex": 0.5, "hips_sway": 0.4})
        self.spine_flex = s["flex"] * _SPINE_FLEX_MAX
        self.hips_sway = s["hips_sway"] * _HIPS_SWAY_MAX

        # Head stabilization
        h = p.get("head", {"stabilize": 0.6})
        self.head_stabilize = h["stabilize"]


def get_breath(shape: str, phase: float) -> float:
    """Calculates breathing patterns dynamically based on profile intent."""
    if shape == "organic":
        return get_organic_breath_curve(phase)
    elif shape == "held":
        t = phase % (2 * math.pi) / (2 * math.pi)
        if t < 0.4:
            return math.sin(t / 0.4 * math.pi / 2)
        elif t < 0.7:
            return 1.0
        else:
            return math.cos((t - 0.7) / 0.3 * math.pi / 2)
    else:  # "sine"
        return math.sin(phase)


# ===================================================================
# SYSTEM CORE UTILITY HELPERS
# ===================================================================
def _prepare_rig_for_animation(rig):
    """Initializes bone rotation modes and triggers FK constraints on active bones."""
    pose_bones = rig.pose.bones
    for side in ["L", "R"]:
        arm_parent = pose_bones.get(getattr(RigSchema, f"ARM_PARENT_{side}"))
        if arm_parent:
            arm_parent["IK_FK"] = 1.0
        leg_parent = pose_bones.get(getattr(RigSchema, f"LEG_PARENT_{side}"))
        if leg_parent:
            leg_parent["IK_FK"] = 1.0

    active_bones = [
        RigSchema.TORSO,
        RigSchema.CHEST,
        RigSchema.HIPS,
        RigSchema.HEAD,
        RigSchema.SPINE_0,
        RigSchema.SPINE_1,
        RigSchema.UP_ARM_L,
        RigSchema.FORE_ARM_L,
        RigSchema.HAND_L,
        RigSchema.UP_ARM_R,
        RigSchema.FORE_ARM_R,
        RigSchema.HAND_R,
        RigSchema.THIGH_L,
        RigSchema.SHIN_L,
        RigSchema.FOOT_L,
        RigSchema.THIGH_R,
        RigSchema.SHIN_R,
        RigSchema.FOOT_R,
        RigSchema.HAT_L,
        RigSchema.HAT_R,
    ]
    for b_name in active_bones:
        b = pose_bones.get(b_name)
        if b:
            b.rotation_mode = "XYZ"


def _reset_pose_to_rest(rig):
    """Programmatically clears pose transformations on major structural bones, leaving facial bones intact."""
    pose_bones = rig.pose.bones

    bones_to_reset = [
        RigSchema.TORSO,
        RigSchema.CHEST,
        RigSchema.HIPS,
        RigSchema.HEAD,
        RigSchema.SPINE_0,
        RigSchema.SPINE_1,
        RigSchema.UP_ARM_L,
        RigSchema.FORE_ARM_L,
        RigSchema.HAND_L,
        RigSchema.UP_ARM_R,
        RigSchema.FORE_ARM_R,
        RigSchema.HAND_R,
        RigSchema.THIGH_L,
        RigSchema.SHIN_L,
        RigSchema.FOOT_L,
        RigSchema.THIGH_R,
        RigSchema.SHIN_R,
        RigSchema.FOOT_R,
        RigSchema.HAT_L,
        RigSchema.HAT_R,
        RigSchema.EYE_CONTROL,
    ]

    for b_name in bones_to_reset:
        bone = pose_bones.get(b_name)
        if bone:
            bone.location = Vector((0.0, 0.0, 0.0))
            if bone.rotation_mode == "QUATERNION":
                bone.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
            elif bone.rotation_mode == "AXIS_ANGLE":
                bone.rotation_axis_angle = (0.0, 0.0, 1.0, 0.0)
            else:
                bone.rotation_euler = Euler((0.0, 0.0, 0.0))
            bone.scale = Vector((1.0, 1.0, 1.0))

    bpy.context.view_layer.update()


def _disable_creases_and_eyebrows(rig):
    """Searches and disables eyebrows and crease properties/control bones on frame 1 of all actions."""
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

    for bone in rig.pose.bones:
        b_name_lower = bone.name.lower()
        if ("crease" in b_name_lower or "eyebrow" in b_name_lower) and (
            "switch" in b_name_lower
            or "toggle" in b_name_lower
            or "on" in b_name_lower
            or "off" in b_name_lower
            or "ctrl" in b_name_lower
            or "menu" in b_name_lower
        ):
            try:
                bone.scale = Vector((0.0, 0.0, 0.0))
                bone.keyframe_insert(data_path="scale", frame=1)
            except Exception:
                pass


# ===================================================================
# KEYFRAMING UTILITY HELPERS
# ===================================================================
def _keyframe_legs(
    rig,
    l_leg_swing,
    r_leg_swing,
    l_leg_sway,
    r_leg_sway,
    l_knee_bend,
    r_knee_bend,
    l_foot_flex,
    r_foot_flex,
    frame,
):
    """Sets standard dynamic keyframes on the leg chains."""
    if rig.thigh_l.bone:
        rig.thigh_l.rotation_euler = Euler(
            (
                rig.thigh_l.base_rot.x - l_leg_swing,
                rig.thigh_l.base_rot.y,
                rig.thigh_l.base_rot.z + l_leg_sway,
            )
        )
        rig.thigh_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.shin_l.bone:
        rig.shin_l.rotation_euler = Euler(
            (
                rig.shin_l.base_rot.x + l_knee_bend,
                rig.shin_l.base_rot.y,
                rig.shin_l.base_rot.z,
            )
        )
        rig.shin_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.foot_l.bone:
        rig.foot_l.rotation_euler = Euler(
            (
                rig.foot_l.base_rot.x + l_foot_flex,
                rig.foot_l.base_rot.y,
                rig.foot_l.base_rot.z,
            )
        )
        rig.foot_l.keyframe_insert("rotation_euler", frame=frame)

    if rig.thigh_r.bone:
        rig.thigh_r.rotation_euler = Euler(
            (
                rig.thigh_r.base_rot.x - r_leg_swing,
                rig.thigh_r.base_rot.y,
                rig.thigh_r.base_rot.z + (-1.0 * r_leg_sway),
            )
        )
        rig.thigh_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.shin_r.bone:
        rig.shin_r.rotation_euler = Euler(
            (
                rig.shin_r.base_rot.x + r_knee_bend,
                rig.shin_r.base_rot.y,
                rig.shin_r.base_rot.z,
            )
        )
        rig.shin_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.foot_r.bone:
        rig.foot_r.rotation_euler = Euler(
            (
                rig.foot_r.base_rot.x + r_foot_flex,
                rig.foot_r.base_rot.y,
                rig.foot_r.base_rot.z,
            )
        )
        rig.foot_r.keyframe_insert("rotation_euler", frame=frame)


def _keyframe_arms_idle(
    rig, breath_phase, arm_sway_rad, wrist_sway_rad, frame, arm_flare=0.0
):
    """Calculates and sets passive organic arm breathing movements symmetrically around the flared rest angle."""
    delayed_breath = get_organic_breath_curve(breath_phase - 0.5)

    # Standard symmetric breathing sway
    z_offset = delayed_breath * arm_sway_rad

    if rig.up_arm_l.bone:
        # Left Arm: Adds flare to rotate outward (+ direction)
        rig.up_arm_l.rotation_euler = Euler(
            (
                rig.up_arm_l.base_rot.x,
                rig.up_arm_l.base_rot.y,
                rig.up_arm_l.base_rot.z + arm_flare + z_offset,
            )
        )
        rig.up_arm_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.fore_arm_l.bone:
        elbow_offset = get_organic_breath_curve(breath_phase - 0.7) * arm_sway_rad
        rig.fore_arm_l.rotation_euler = Euler(
            (
                rig.fore_arm_l.base_rot.x + elbow_offset,
                rig.fore_arm_l.base_rot.y,
                rig.fore_arm_l.base_rot.z,
            )
        )
        rig.fore_arm_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.hand_l.bone:
        wrist_offset = get_organic_breath_curve(breath_phase - 1.0) * wrist_sway_rad
        rig.hand_l.rotation_euler = Euler(
            (
                rig.hand_l.base_rot.x + wrist_offset,
                rig.hand_l.base_rot.y,
                rig.hand_l.base_rot.z,
            )
        )
        rig.hand_l.keyframe_insert("rotation_euler", frame=frame)

    if rig.up_arm_r.bone:
        # Right Arm: Subtracts flare to rotate outward (- direction)
        rig.up_arm_r.rotation_euler = Euler(
            (
                rig.up_arm_r.base_rot.x,
                rig.up_arm_r.base_rot.y,
                rig.up_arm_r.base_rot.z - arm_flare - z_offset,
            )
        )
        rig.up_arm_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.fore_arm_r.bone:
        elbow_offset = get_organic_breath_curve(breath_phase - 0.7) * arm_sway_rad
        rig.fore_arm_r.rotation_euler = Euler(
            (
                rig.fore_arm_r.base_rot.x + elbow_offset,
                rig.fore_arm_r.base_rot.y,
                rig.fore_arm_r.base_rot.z,
            )
        )
        rig.fore_arm_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.hand_r.bone:
        wrist_offset = get_organic_breath_curve(breath_phase - 1.0) * wrist_sway_rad
        rig.hand_r.rotation_euler = Euler(
            (
                rig.hand_r.base_rot.x + wrist_offset,
                rig.hand_r.base_rot.y,
                rig.hand_r.base_rot.z,
            )
        )
        rig.hand_r.keyframe_insert("rotation_euler", frame=frame)


def _keyframe_arms_locomotion(rig, walk_phase, swing_amp, sway_amp, frame):
    """Applies rhythmic swing and sway parameters to standard locomotion chains."""
    l_swing = math.sin(walk_phase) * swing_amp
    r_swing = -math.sin(walk_phase) * swing_amp
    z_offset = math.cos(walk_phase) * sway_amp

    if rig.up_arm_l.bone:
        rig.up_arm_l.rotation_euler = Euler(
            (
                rig.up_arm_l.base_rot.x + l_swing,
                rig.up_arm_l.base_rot.y,
                rig.up_arm_l.base_rot.z + z_offset,
            )
        )
        rig.up_arm_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.fore_arm_l.bone:
        elbow_offset = math.sin(walk_phase - 0.5) * swing_amp * 0.4
        rig.fore_arm_l.rotation_euler = Euler(
            (
                rig.fore_arm_l.base_rot.x + elbow_offset,
                rig.fore_arm_l.base_rot.y,
                rig.fore_arm_l.base_rot.z,
            )
        )
        rig.fore_arm_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.hand_l.bone:
        wrist_offset = math.cos(walk_phase - 1.0) * swing_amp * 0.15
        rig.hand_l.rotation_euler = Euler(
            (
                rig.hand_l.base_rot.x + wrist_offset,
                rig.hand_l.base_rot.y,
                rig.hand_l.base_rot.z,
            )
        )
        rig.hand_l.keyframe_insert("rotation_euler", frame=frame)

    if rig.up_arm_r.bone:
        get_z = (
            rig.up_arm_r.base_rot.z - z_offset if rig.up_arm_r.base_rot else -z_offset
        )
        rig.up_arm_r.rotation_euler = Euler(
            (rig.up_arm_r.base_rot.x + r_swing, rig.up_arm_r.base_rot.y, get_z)
        )
        rig.up_arm_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.fore_arm_r.bone:
        elbow_offset = math.sin(walk_phase - 0.5 - math.pi) * swing_amp * 0.4
        rig.fore_arm_r.rotation_euler = Euler(
            (
                rig.fore_arm_r.base_rot.x + elbow_offset,
                rig.fore_arm_r.base_rot.y,
                rig.fore_arm_r.base_rot.z,
            )
        )
        rig.fore_arm_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.hand_r.bone:
        wrist_offset = math.cos(walk_phase - 1.0 - math.pi) * swing_amp * 0.15
        rig.hand_r.rotation_euler = Euler(
            (
                rig.hand_r.base_rot.x + wrist_offset,
                rig.hand_r.base_rot.y,
                rig.hand_r.base_rot.z,
            )
        )
        rig.hand_r.keyframe_insert("rotation_euler", frame=frame)


def _keyframe_fingers(rig, breath_phase, flex_offset_is_locomotion, finger_base, frame):
    """Processes dynamic finger curls, handling parent IK/FK holds cleanly."""
    pose_bones = rig.pose_bones
    finger_bases = ["f_index", "f_middle", "f_ring", "thumb"]
    finger_delays = {"f_index": 0.0, "f_middle": -0.2, "f_ring": -0.4, "thumb": -0.6}

    for side in ["L", "R"]:
        flip = -1.0

        arm_parent = pose_bones.get(getattr(RigSchema, f"ARM_PARENT_{side}"))
        if arm_parent:
            arm_parent.keyframe_insert(data_path='["IK_FK"]', frame=frame)
        leg_parent = pose_bones.get(getattr(RigSchema, f"LEG_PARENT_{side}"))
        if leg_parent:
            leg_parent.keyframe_insert(data_path='["IK_FK"]', frame=frame)

        for f_base in finger_bases:
            delay = finger_delays[f_base]
            if flex_offset_is_locomotion:
                flex_offset = math.sin(breath_phase + delay) * FINGER_FLEX_RANGE
            else:
                flex_offset = (
                    get_organic_breath_curve(breath_phase + delay) * FINGER_FLEX_RANGE
                )

            for seg in ["01", "02", "03"]:
                bone_name = f"{f_base}.{seg}.{side}"
                bone = pose_bones.get(bone_name)
                if bone:
                    bone.rotation_mode = "XYZ"
                    if f_base == "thumb":
                        target_rot_x = flip * (math.radians(10) + flex_offset * 0.5)
                        target_rot_y = flip * math.radians(5)
                        bone.rotation_euler = Euler((target_rot_x, target_rot_y, 0.0))
                    else:
                        base_angle = math.radians(
                            finger_base * 80 * (1.0 if seg == "01" else 1.2)
                        )
                        bone.rotation_euler = Euler(
                            (flip * (base_angle + flex_offset), 0.0, 0.0)
                        )
                    bone.keyframe_insert(data_path="rotation_euler", frame=frame)


def _keyframe_eyelids(rig, blink_factor, frame):
    """Sets stylized location offsets so eyelids are open by default and close when blinking."""
    open_factor = 1.0 - blink_factor
    upper_offset = BLINK_TRAVEL_UPPER * open_factor
    lower_offset = BLINK_TRAVEL_LOWER * open_factor

    for side in ["l", "r"]:
        top = getattr(rig, f"eyelid_top_{side}")
        bot = getattr(rig, f"eyelid_bot_{side}")
        if top.bone:
            target_y = max(0.0, min(1.0, upper_offset))
            top.location = Vector((0.0, target_y, 0.0))
            top.keyframe_insert("location", frame=frame)
        if bot.bone:
            target_y = max(-1.0, min(0.0, lower_offset))
            bot.location = Vector((0.0, target_y, 0.0))
            bot.keyframe_insert("location", frame=frame)


def _keyframe_eyes(rig, progress, eye_activity, frame):
    """Translates the eye target controller to simulate saccadic movements."""
    if not rig.eye_control.bone:
        return

    saccade_x, saccade_z = get_eye_saccade_relative(progress)
    offset_x = saccade_x * eye_activity * EYE_SACCADE_SCALE
    offset_z = saccade_z * eye_activity * EYE_SACCADE_SCALE

    rig.eye_control.location = rig.eye_control.base_loc + Vector(
        (offset_x, 0.0, offset_z)
    )
    rig.eye_control.keyframe_insert("location", frame=frame)


def _keyframe_hat_sway(rig, breath_phase, walk_phase, is_locomotion, frame):
    """Handles shared hat sway mathematics over both passive breathing and physical movement."""
    sway_offset = math.cos(breath_phase) * math.radians(2.5)
    walk_sway = (math.cos(walk_phase * 2) * math.radians(1.5)) if is_locomotion else 0.0

    if rig.hat_l.bone:
        rig.hat_l.rotation_euler = Euler(
            (
                rig.hat_l.base_rot.x,
                rig.hat_l.base_rot.y + sway_offset + walk_sway,
                rig.hat_l.base_rot.z,
            )
        )
        rig.hat_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.hat_r.bone:
        rig.hat_r.rotation_euler = Euler(
            (
                rig.hat_r.base_rot.x,
                rig.hat_r.base_rot.y - sway_offset - walk_sway,
                rig.hat_r.base_rot.z,
            )
        )
        rig.hat_r.keyframe_insert("rotation_euler", frame=frame)


# ===================================================================
# UNIVERSAL GENERATOR ENGINE
# ===================================================================
def generate_state(rig, state_name: str, duration: int):
    """Universal generator: completely parameterized and driven by MOTION_PROFILES."""
    profile = ResolvedProfile(MOTION_PROFILES[state_name])
    is_loco = state_name != "IDLE"

    _prepare_rig_for_animation(rig)
    _reset_pose_to_rest(rig)
    rig_data = CharacterRig(rig, is_locomotion=is_loco)

    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = duration

    for frame in range(1, duration + 1):
        bpy.context.scene.frame_set(frame)
        progress = (frame - 1) / (duration - 1) if duration > 1 else 0.0
        phase = (
            progress
            * 2
            * math.pi
            * (profile.breath_rate if profile.breath_rate > 0.0 else 1.0)
        )

        if frame == 1:
            _disable_creases_and_eyebrows(rig)

        # 1. Resolve breathing values
        breath = get_breath(profile.breath_shape, phase) * profile.breath_depth
        delayed = get_breath(profile.breath_shape, phase - 0.5) * profile.breath_depth

        # 2. Transient vs. Cyclic State Pre-computation
        if profile.torso_style == "jump":
            if progress < 0.3:
                t = progress / 0.3
                t_sin = math.sin(t * math.pi / 2.0)
                torso_z = -0.09 * t_sin
                torso_y = -0.03 * t_sin
                torso_pitch = math.radians(16.0) * t_sin
                knee_bend = math.radians(45.0) * t_sin
                thigh_swing = math.radians(24.0) * t_sin
                foot_flex = math.radians(15.0) * t_sin
                arm_swing = math.radians(-32.0) * t_sin
            elif progress < 0.48:
                t = (progress - 0.3) / 0.18
                t_sin = math.sin(t * math.pi / 2.0)
                torso_z = -0.09 + (0.16 - (-0.09)) * t_sin
                torso_y = -0.03 + (0.01 - (-0.03)) * t_sin
                torso_pitch = (
                    math.radians(16.0) * (1.0 - t_sin) - math.radians(6.0) * t_sin
                )
                knee_bend = math.radians(45.0) * (1.0 - t_sin)
                thigh_swing = (
                    math.radians(24.0) * (1.0 - t_sin) - math.radians(12.0) * t_sin
                )
                foot_flex = (
                    math.radians(15.0) * (1.0 - t_sin) - math.radians(28.0) * t_sin
                )
                arm_swing = (
                    math.radians(-32.0) * (1.0 - t_sin) + math.radians(48.0) * t_sin
                )
            elif progress < 0.78:
                t = (progress - 0.48) / 0.30
                t_sin = math.sin(t * math.pi)
                torso_z = 0.16 + 0.08 * t_sin
                torso_y = 0.01 * (1.0 - t) - 0.01 * t
                torso_pitch = math.radians(-6.0) * (1.0 - t) + math.radians(8.0) * t
                knee_bend = math.radians(22.0) * t_sin
                thigh_swing = math.radians(16.0) * t_sin
                foot_flex = math.radians(-28.0) * (1.0 - t) + math.radians(8.0) * t
                arm_swing = math.radians(48.0) * (1.0 - t) + math.radians(12.0) * t
            else:
                t = (progress - 0.78) / 0.22
                t_sin = math.sin(t * math.pi / 2.0)
                torso_z = 0.16 * (1.0 - t_sin)
                torso_y = -0.01 + 0.01 * t_sin
                torso_pitch = math.radians(8.0) + math.radians(4.0) * t_sin
                knee_bend = math.radians(12.0) * (1.0 - t_sin)
                thigh_swing = math.radians(-8.0) * t_sin
                foot_flex = (
                    math.radians(8.0) * (1.0 - t_sin) + math.radians(18.0) * t_sin
                )
                arm_swing = (
                    math.radians(12.0) * (1.0 - t_sin) + math.radians(24.0) * t_sin
                )
            torso_x = 0.0

        elif profile.torso_style == "land":
            if progress < 0.35:
                t = progress / 0.35
                t_sin = math.sin(t * math.pi / 2.0)
                torso_z = -0.13 * t_sin
                torso_y = -0.05 * t_sin
                torso_pitch = math.radians(22.0) * t_sin
                knee_bend = math.radians(58.0) * t_sin
                thigh_swing = math.radians(34.0) * t_sin
                foot_flex = math.radians(22.0) * t_sin
                arm_swing = math.radians(-26.0) * t_sin
            elif progress < 0.55:
                t = (progress - 0.35) / 0.20
                t_cos = math.cos(t * math.pi)
                torso_z = -0.13 - 0.01 * (1.0 - t_cos)
                torso_y = -0.05
                torso_pitch = math.radians(22.0) + math.radians(1.5) * (1.0 - t_cos)
                knee_bend = math.radians(58.0) + math.radians(2.0) * (1.0 - t_cos)
                thigh_swing = math.radians(34.0)
                foot_flex = math.radians(22.0)
                arm_swing = math.radians(-26.0)
            else:
                t = (progress - 0.55) / 0.45
                interp = (1.0 - math.cos(t * math.pi)) / 2.0
                torso_z = -0.14 * (1.0 - interp)
                torso_y = -0.05 * (1.0 - interp)
                torso_pitch = math.radians(23.5) * (1.0 - interp)
                knee_bend = math.radians(60.0) * (1.0 - interp)
                thigh_swing = math.radians(34.0) * (1.0 - interp)
                foot_flex = math.radians(22.0) * (1.0 - interp)
                arm_swing = math.radians(-26.0) * (1.0 - interp)
            torso_x = 0.0

        else:
            # Standard Loop / Cyclic calculations
            torso_x = math.sin(phase) * profile.torso_sway_x
            torso_y = math.cos(phase) * profile.torso_sway_y
            torso_z = breath * profile.torso_bob_z
            torso_pitch = profile.torso_lean

        # 3. Apply Torso Coordinate Space
        if rig_data.torso.bone:
            rig_data.torso.location = rig_data.torso.base_loc + Vector(
                (torso_x, torso_y, torso_z)
            )
            rig_data.torso.rotation_euler = Euler(
                (
                    rig_data.torso.base_rot.x + torso_pitch,
                    0.0,
                    rig_data.torso.base_rot.z,
                )
            )
            rig_data.torso.keyframe_insert("location", frame=frame)
            rig_data.torso.keyframe_insert("rotation_euler", frame=frame)

        # 4. Pelvis & Spine (Unified weight-shift solving)
        if rig_data.hips.bone:
            if profile.torso_style in ("jump", "land"):
                hips_pitch = rig_data.hips.base_rot.x - (torso_pitch * 0.45)
                hips_roll = rig_data.hips.base_rot.y
                hips_yaw = rig_data.hips.base_rot.z
            else:
                hips_pitch = rig_data.hips.base_rot.x - (
                    breath * 0.02 * profile.spine_flex
                )
                hips_roll = (
                    rig_data.hips.base_rot.y + math.sin(phase) * profile.hips_sway * 0.5
                )
                hips_yaw = (
                    rig_data.hips.base_rot.z + math.sin(phase) * profile.hips_sway
                )
            rig_data.hips.rotation_euler = Euler((hips_pitch, hips_roll, hips_yaw))
            rig_data.hips.keyframe_insert("rotation_euler", frame=frame)

        if rig_data.spine_0.bone:
            if profile.torso_style in ("jump", "land"):
                s0_pitch = rig_data.spine_0.base_rot.x + (torso_pitch * 0.25)
                s0_yaw = rig_data.spine_0.base_rot.z
            else:
                s0_pitch = (
                    rig_data.spine_0.base_rot.x
                    + math.cos(phase * 2) * profile.spine_flex * 0.4
                )
                s0_yaw = (
                    rig_data.spine_0.base_rot.z
                    + math.sin(phase) * profile.spine_flex * -0.3
                )
            rig_data.spine_0.rotation_euler = Euler(
                (s0_pitch, rig_data.spine_0.base_rot.y, s0_yaw)
            )
            rig_data.spine_0.keyframe_insert("rotation_euler", frame=frame)

        if rig_data.spine_1.bone:
            if profile.torso_style in ("jump", "land"):
                s1_pitch = rig_data.spine_1.base_rot.x + (torso_pitch * 0.18)
                s1_yaw = rig_data.spine_1.base_rot.z
            else:
                s1_pitch = (
                    rig_data.spine_1.base_rot.x
                    + math.cos(phase * 2 - 0.5) * profile.spine_flex * 0.3
                )
                s1_yaw = (
                    rig_data.spine_1.base_rot.z
                    + math.sin(phase - 0.5) * profile.spine_flex * -0.2
                )
            rig_data.spine_1.rotation_euler = Euler(
                (s1_pitch, rig_data.spine_1.base_rot.y, s1_yaw)
            )
            rig_data.spine_1.keyframe_insert("rotation_euler", frame=frame)

        if rig_data.chest.bone:
            if profile.torso_style in ("jump", "land"):
                chest_pitch = rig_data.chest.base_rot.x + (torso_pitch * 0.35)
                chest_roll = rig_data.chest.base_rot.y
                chest_yaw = rig_data.chest.base_rot.z
            else:
                chest_pitch = (
                    rig_data.chest.base_rot.x
                    + delayed * 0.03
                    + math.cos(phase * 2) * profile.spine_flex * 0.4
                )
                chest_roll = (
                    rig_data.chest.base_rot.y
                    - math.sin(phase) * profile.spine_flex * 0.5
                )
                chest_yaw = (
                    rig_data.chest.base_rot.z
                    + math.sin(phase) * profile.spine_flex * -0.8
                )
            rig_data.chest.rotation_euler = Euler((chest_pitch, chest_roll, chest_yaw))
            rig_data.chest.keyframe_insert("rotation_euler", frame=frame)

        # 5. Head Stabilization Adjustments
        if rig_data.head.bone:
            if profile.torso_style in ("jump", "land"):
                head_pitch = rig_data.head.base_rot.x - (torso_pitch * 0.55)
                head_yaw = rig_data.head.base_rot.z
            else:
                head_pitch = (
                    rig_data.head.base_rot.x
                    - torso_pitch * profile.head_stabilize
                    - breath * 0.02
                )
                head_yaw = (
                    rig_data.head.base_rot.z
                    - (math.sin(phase) * profile.spine_flex * -0.4)
                    * profile.head_stabilize
                )
            rig_data.head.rotation_euler = Euler(
                (head_pitch, rig_data.head.base_rot.y, head_yaw)
            )
            rig_data.head.keyframe_insert("rotation_euler", frame=frame)

        # 6. Arms Dispatch
        if profile.arm_style in ("jump", "land"):
            for side in ["l", "r"]:
                up_arm = getattr(rig_data, f"up_arm_{side}")
                fore_arm = getattr(rig_data, f"fore_arm_{side}")
                hand = getattr(rig_data, f"hand_{side}")
                if up_arm.bone:
                    up_arm.rotation_euler = Euler(
                        (
                            up_arm.base_rot.x + arm_swing,
                            up_arm.base_rot.y,
                            up_arm.base_rot.z,
                        )
                    )
                    up_arm.keyframe_insert("rotation_euler", frame=frame)
                if fore_arm.bone:
                    fore_arm.rotation_euler = Euler(
                        (
                            fore_arm.base_rot.x + abs(arm_swing) * 0.25,
                            fore_arm.base_rot.y,
                            fore_arm.base_rot.z,
                        )
                    )
                    fore_arm.keyframe_insert("rotation_euler", frame=frame)
                if hand.bone:
                    hand.rotation_euler = Euler(
                        (hand.base_rot.x, hand.base_rot.y, hand.base_rot.z)
                    )
                    hand.keyframe_insert("rotation_euler", frame=frame)
        elif profile.arm_style in ("passive", "out"):
            # Resolve the raw subtle passive sways directly using the profile Multiplier to ensure clear motion
            sway_mult = profile.arm_sway / _ARM_SWAY_MAX if _ARM_SWAY_MAX > 0.0 else 1.0

            # Replaced math.radians(2.5) with direct raw scaled parameters to ensure clear visual viewport movement
            _keyframe_arms_idle(
                rig_data,
                phase + profile.arm_delay,
                profile.arm_sway,  # Subtle Upper Arm breathing sway (e.g. 4.0 deg in IDLE)
                profile.arm_sway * 1.5,  # Subtle Wrist sway (e.g. 6.0 deg in IDLE)
                frame,
                arm_flare=profile.arm_flare,
            )
        elif profile.arm_style in ("swing", "pump"):
            _keyframe_arms_locomotion(
                rig_data,
                phase,
                swing_amp=profile.arm_swing,
                sway_amp=profile.arm_sway,
                frame=frame,
            )

        # 7. Legs Dispatch
        if profile.leg_style in ("jump", "land"):
            _keyframe_legs(
                rig_data,
                thigh_swing,
                thigh_swing,
                0.0,
                0.0,
                knee_bend,
                knee_bend,
                foot_flex,
                foot_flex,
                frame,
            )
        elif profile.leg_style == "stride":
            amp = profile.leg_amplitude
            _keyframe_legs(
                rig_data,
                math.sin(phase) * amp,
                -math.sin(phase) * amp,
                0.0,
                0.0,
                max(0, math.cos(phase)) * amp * 0.55,
                max(0, -math.cos(phase)) * amp * 0.55,
                -math.sin(phase) * amp * 0.25,
                math.sin(phase) * amp * 0.25,
                frame,
            )
        elif profile.leg_style == "sidestep":
            amp = profile.leg_amplitude
            _keyframe_legs(
                rig_data,
                0.0,
                0.0,
                max(0, math.sin(phase)) * amp * 0.32,
                max(0, -math.sin(phase)) * amp * 0.32,
                max(0, math.sin(phase)) * amp * 0.38,
                max(0, -math.sin(phase)) * amp * 0.38,
                -math.sin(phase) * 0.12,
                math.sin(phase) * 0.12,
                frame,
            )
        else:  # static
            for side in ["l", "r"]:
                for joint in ["thigh", "shin", "foot"]:
                    b = getattr(rig_data, f"{joint}_{side}")
                    if b.bone:
                        b.rotation_euler = b.base_rot.copy()
                        b.keyframe_insert("rotation_euler", frame=frame)

        # 8. Hands, Fingers, Eyes, and Secondary items
        _keyframe_fingers(
            rig_data,
            phase,
            flex_offset_is_locomotion=is_loco,
            finger_base=profile.finger_curl,
            frame=frame,
        )
        blink = (
            get_blink_factor_relative(progress, duration) if profile.eye_blink else 0.0
        )
        _keyframe_eyelids(rig_data, blink, frame)
        _keyframe_eyes(
            rig_data, progress, eye_activity=profile.eye_saccade, frame=frame
        )
        _keyframe_hat_sway(rig_data, phase, phase, is_locomotion=is_loco, frame=frame)


# ===================================================================
# AUTOMATIC TIMELINE SCALER HANDLER
# ===================================================================
def auto_adjust_timeline_range(scene):
    """Snaps the active timeline end frame based on active state name."""
    rig = bpy.context.active_object
    if (
        rig
        and rig.type == "ARMATURE"
        and rig.animation_data
        and rig.animation_data.action
    ):
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
        elif "_JUMPING" in name:
            scene.frame_end = 30
        elif "_LANDING" in name:
            scene.frame_end = 20


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
    if not rig or rig.type != "ARMATURE":
        print("Please select your character's armature in the 3D Viewport.")
        return

    prefix = rig.name.lower()
    if prefix.endswith(".anim") or prefix.endswith(".retarget"):
        prefix = prefix.split(".")[0]

    states = {
        "IDLE": 120,
        "WALKING": 36,
        "RUNNING": 24,
        "STRAFE": 40,
        "JUMPING": 30,
        "LANDING": 20,
    }

    if not rig.animation_data:
        rig.animation_data_create()

    rig.animation_data.action = None

    for state_type, duration in states.items():
        action_name = f"{prefix}_{state_type}"

        old_action = bpy.data.actions.get(action_name)
        if old_action:
            bpy.data.actions.remove(old_action)

        action = bpy.data.actions.new(name=action_name)
        action.use_fake_user = True

        rig.animation_data.action = action

        print(f"Generating declarative action: '{action_name}' ({duration} frames)...")
        generate_state(rig, state_type, duration)

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
