# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

import math
import bpy
from mathutils import Euler, Vector

# ===================================================================
# CUSTOM RIG ADJUSTMENTS & TUNING
# ===================================================================
LEG_STANCE_SPREAD = 0.045  # Lateral foot spread distance in meters

FINGER_FLEX_RANGE = 0.08  # Base finger flex amplitude
ARM_SWAY_RANGE = 2.5  # Base arm swing amplitude (degrees)
WRIST_SWAY_RANGE = 4.0  # Base wrist flop amplitude (degrees)

# Visor Eyelid Movement Offsets (Distance in meters)
EYELID_TRAVEL_TOP = -0.035  # Distance top eyelid moves DOWN to close
EYELID_TRAVEL_BOT = 0.035  # Distance bottom eyelid moves UP to close
EYE_SACCADE_SCALE = 0.005  # Digital eye pupil movement scale

# Strict action whitelist for exports
VALID_ACTION_NAMES = {
    "Uzi_IDLE",
    "Uzi_WALKING",
    "Uzi_RUNNING",
    "Uzi_STRAFE",
    "Uzi_JUMPING",
    "Uzi_LANDING",
}


# ===================================================================
# UZI / TOXILISK HUMANOID RIG SCHEMA DEFINITIONS
# ===================================================================
class UziRigSchema:
    """Structural schema mapped directly to Toxilisk's Uzi Rig & Facial_Rig."""

    TORSO = "CTR-Torso"
    CHEST = "CTR-Chest"
    HIPS = "CTR-Hips"
    HEAD = "CTR-Neck"

    # Arms (FK)
    UP_ARM_L = "FK-Upper_arm.L"
    FORE_ARM_L = "FK-Forearm.L"
    HAND_L = "CTR-Hand.L"
    UP_ARM_R = "FK-Upper_arm.R"
    FORE_ARM_R = "FK-Forearm.R"
    HAND_R = "CTR-Hand.R"

    # Legs (FK & IK)
    THIGH_L = "FK-Thigh.L"
    SHIN_L = "FK-Shin.L"
    FOOT_L = "FK-Foot.L"
    THIGH_R = "FK-Thigh.R"
    SHIN_R = "FK-Shin.R"
    FOOT_R = "FK-Foot.R"

    # Visor Facial Bones (Facial_Rig)
    EYELID_TOP_L = "CTR-Top_Eyelid.L"
    EYELID_TOP_R = "CTR-Top_Eyelid.R"
    EYELID_BOT_L = "CTR-Bottom_Eyelid.L"
    EYELID_BOT_R = "CTR-Bottom_Eyelid.R"
    EYE_L = "CTR-Eye.L"
    EYE_R = "CTR-Eye.R"

    # Toxilisk Data-level IK/FK Property Keys
    IK_FK_PROPS = ["IK/FK_Arm.L", "IK/FK_Arm.R", "IK/FK_Leg.L", "IK/FK_Leg.R"]


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

    def __init__(self, body_rig, face_rig, is_locomotion=False):
        self.rig = body_rig
        self.pose_bones = body_rig.pose.bones
        face_bones = face_rig.pose.bones if face_rig else self.pose_bones

        # Core
        self.torso = BoneData(self.pose_bones.get(UziRigSchema.TORSO))
        self.chest = BoneData(self.pose_bones.get(UziRigSchema.CHEST))
        self.hips = BoneData(self.pose_bones.get(UziRigSchema.HIPS))
        self.head = BoneData(self.pose_bones.get(UziRigSchema.HEAD))

        # Arms
        self.up_arm_l = BoneData(self.pose_bones.get(UziRigSchema.UP_ARM_L))
        self.fore_arm_l = BoneData(self.pose_bones.get(UziRigSchema.FORE_ARM_L))
        self.hand_l = BoneData(self.pose_bones.get(UziRigSchema.HAND_L))
        self.up_arm_r = BoneData(self.pose_bones.get(UziRigSchema.UP_ARM_R))
        self.fore_arm_r = BoneData(self.pose_bones.get(UziRigSchema.FORE_ARM_R))
        self.hand_r = BoneData(self.pose_bones.get(UziRigSchema.HAND_R))

        # Legs (Check both FK-Foot and CTR-Foot)
        foot_l_bone = self.pose_bones.get("FK-Foot.L") or self.pose_bones.get("CTR-Foot.L")
        foot_r_bone = self.pose_bones.get("FK-Foot.R") or self.pose_bones.get("CTR-Foot.R")

        self.thigh_l = BoneData(self.pose_bones.get(UziRigSchema.THIGH_L))
        self.shin_l = BoneData(self.pose_bones.get(UziRigSchema.SHIN_L))
        self.foot_l = BoneData(foot_l_bone)
        self.thigh_r = BoneData(self.pose_bones.get(UziRigSchema.THIGH_R))
        self.shin_r = BoneData(self.pose_bones.get(UziRigSchema.SHIN_R))
        self.foot_r = BoneData(foot_r_bone)

        # Facial Visor Bones
        self.eyelid_top_l = BoneData(face_bones.get(UziRigSchema.EYELID_TOP_L))
        self.eyelid_top_r = BoneData(face_bones.get(UziRigSchema.EYELID_TOP_R))
        self.eyelid_bot_l = BoneData(face_bones.get(UziRigSchema.EYELID_BOT_L))
        self.eyelid_bot_r = BoneData(face_bones.get(UziRigSchema.EYELID_BOT_R))
        self.eye_l = BoneData(face_bones.get(UziRigSchema.EYE_L))
        self.eye_r = BoneData(face_bones.get(UziRigSchema.EYE_R))

        # Direct resting pose offsets
        if self.up_arm_l.bone:
            self.up_arm_l.base_rot = Euler((0.0, 0.0, math.radians(-82)))
        if self.fore_arm_l.bone:
            self.fore_arm_l.base_rot = Euler((math.radians(15), 0.0, 0.0))

        if self.up_arm_r.bone:
            self.up_arm_r.base_rot = Euler((0.0, 0.0, math.radians(82)))
        if self.fore_arm_r.bone:
            self.fore_arm_r.base_rot = Euler((math.radians(15), 0.0, 0.0))

        th_rot = math.radians(-5) if is_locomotion else 0.0
        sh_rot = math.radians(5) if is_locomotion else 0.0
        ft_rot = math.radians(-5) if is_locomotion else 0.0
        stance_angle = math.radians(4.0)

        # Apply leg stance abduction (flares thighs outward away from center)
        if self.thigh_l.bone:
            self.thigh_l.base_rot = Euler((th_rot, 0.0, -stance_angle))
        if self.shin_l.bone:
            self.shin_l.base_rot = Euler((sh_rot, 0.0, 0.0))
        if self.foot_l.bone:
            self.foot_l.base_rot = Euler((ft_rot, 0.0, stance_angle))

        if self.thigh_r.bone:
            self.thigh_r.base_rot = Euler((th_rot, 0.0, stance_angle))
        if self.shin_r.bone:
            self.shin_r.base_rot = Euler((sh_rot, 0.0, 0.0))
        if self.foot_r.bone:
            self.foot_r.base_rot = Euler((ft_rot, 0.0, -stance_angle))


# ===================================================================
# ORGANIC MATH CALCULATIONS
# ===================================================================
def get_organic_breath_curve(phase):
    """Generates an asymmetric, double-exponential breathing curve."""
    skewed_phase = phase + 0.4 * math.sin(phase)
    raw_val = math.exp(math.sin(skewed_phase))
    min_val = math.exp(-1)
    max_val = math.exp(1)
    normalized = (raw_val - min_val) / (max_val - min_val)
    return normalized * 2.0 - 1.0


def get_blink_factor_relative(progress, duration):
    """Calculates blink factor (0.0 = open, 1.0 = fully closed)."""
    if duration >= 60:
        if 0.22 <= progress <= 0.28:
            p_blink = (progress - 0.22) / 0.06
            return math.sin(p_blink * math.pi)
        if 0.72 <= progress <= 0.78:
            p_blink = (progress - 0.72) / 0.06
            return math.sin(p_blink * math.pi)
    else:
        if 0.45 <= progress <= 0.55:
            p_blink = (progress - 0.45) / 0.10
            return math.sin(p_blink * math.pi)
    return 0.0


# ===================================================================
# DECLARATIVE MOTION PROFILES
# ===================================================================
MOTION_PROFILES = {
    "IDLE": {
        "feel": "relaxed",
        "breath": {"rate": 1.0, "depth": 0.6, "shape": "organic"},
        "torso": {
            "style": "loop",
            "sway_x": 0.4,
            "sway_y": 0.2,
            "bob_z": 0.3,
            "lean": 4.0,
        },
        "arms": {"style": "passive", "sway": 0.5, "delay": 0.5, "flare": 2.0},
        "fingers": {"curl": 0.25, "style": "organic"},
        "eyes": {"blink": True, "saccade": 1.0},
        "legs": {"style": "static", "amplitude": 0.0},
        "spine": {"flex": 0.4, "hips_sway": 0.2},
        "head": {"stabilize": 0.8},
    },
    "WALKING": {
        "feel": "casual",
        "breath": {"rate": 1.0, "depth": 0.5, "shape": "sine"},
        "torso": {
            "style": "loop",
            "sway_x": 0.7,
            "sway_y": 0.3,
            "bob_z": 0.4,
            "lean": 8.0,
        },
        "arms": {"style": "swing", "sway": 0.6, "delay": 0.0, "flare": 4.0},
        "fingers": {"curl": 0.35, "style": "rhythmic"},
        "eyes": {"blink": False, "saccade": 0.3},
        "legs": {"style": "stride", "amplitude": 0.50},
        "spine": {"flex": 0.5, "hips_sway": 0.5},
        "head": {"stabilize": 0.85},
    },
    "RUNNING": {
        "feel": "athletic",
        "breath": {"rate": 1.0, "depth": 0.8, "shape": "sine"},
        "torso": {
            "style": "loop",
            "sway_x": 0.8,
            "sway_y": 0.5,
            "bob_z": 0.7,
            "lean": 14.0,
        },
        "arms": {"style": "pump", "sway": 0.8, "delay": 0.0, "flare": 8.0},  # 8-deg outward body clearance
        "fingers": {"curl": 0.65, "style": "rhythmic"},
        "eyes": {"blink": False, "saccade": 0.1},
        "legs": {"style": "stride", "amplitude": 0.72},
        "spine": {"flex": 0.8, "hips_sway": 0.8},
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
        "arms": {"style": "out", "sway": 0.9, "delay": 0.0, "flare": 4.0},
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

_TORSO_SWAY_X_MAX = 0.05
_TORSO_SWAY_Y_MAX = 0.015
_TORSO_BOB_Z_MAX = 0.10
_ARM_SWING_MAX = 0.80
_ARM_SWAY_MAX = 0.14
_LEG_STRIDE_MAX = 1.0
_SPINE_FLEX_MAX = 0.08
_HIPS_SWAY_MAX = 0.24


class ResolvedProfile:
    """Translates a declarative profile dict into ready-to-use motion scalars."""

    def __init__(self, profile: dict):
        p = profile
        b = p["breath"]
        self.breath_rate = b["rate"]
        self.breath_depth = b["depth"]
        self.breath_shape = b["shape"]

        t = p["torso"]
        self.torso_style = t.get("style", "loop")
        self.torso_sway_x = t["sway_x"] * _TORSO_SWAY_X_MAX
        self.torso_sway_y = t["sway_y"] * _TORSO_SWAY_Y_MAX
        self.torso_bob_z = t["bob_z"] * _TORSO_BOB_Z_MAX
        self.torso_lean = math.radians(t["lean"])

        a = p["arms"]
        self.arm_style = a["style"]
        self.arm_swing = a["sway"] * _ARM_SWING_MAX
        self.arm_sway = a["sway"] * _ARM_SWAY_MAX
        self.arm_delay = a["delay"]
        self.arm_flare = math.radians(a.get("flare", 0.0))

        f = p["fingers"]
        self.finger_curl = f["curl"]

        e = p.get("eyes", {"blink": True, "saccade": 1.0})
        self.eye_blink = e["blink"]
        self.eye_saccade = e["saccade"]

        l = p["legs"]
        self.leg_style = l["style"]
        self.leg_amplitude = l["amplitude"] * _LEG_STRIDE_MAX

        s = p.get("spine", {"flex": 0.5, "hips_sway": 0.4})
        self.spine_flex = s["flex"] * _SPINE_FLEX_MAX
        self.hips_sway = s["hips_sway"] * _HIPS_SWAY_MAX

        h = p.get("head", {"stabilize": 0.6})
        self.head_stabilize = h["stabilize"]


def get_breath(shape: str, phase: float) -> float:
    if shape == "organic":
        return get_organic_breath_curve(phase)
    return math.sin(phase)


# ===================================================================
# SYSTEM CORE UTILITY HELPERS
# ===================================================================
def _prepare_rig_for_animation(rig, face_rig):
    """Sets FK switches on rig.data (Toxilisk UI architecture)."""
    props = rig.data
    for prop_name in UziRigSchema.IK_FK_PROPS:
        if prop_name in props:
            props[prop_name] = 1.0
            try:
                rig.data.keyframe_insert(data_path=f'["{prop_name}"]', frame=1)
            except Exception:
                pass

    for b in rig.pose.bones:
        b.rotation_mode = "XYZ"

    if face_rig:
        for b in face_rig.pose.bones:
            b.rotation_mode = "XYZ"


def _reset_pose_to_rest(rig, face_rig):
    """Clears transformations safely on rig and Facial_Rig."""
    rigs = [r for r in [rig, face_rig] if r]
    for r in rigs:
        for b in r.pose.bones:
            b.location = Vector((0.0, 0.0, 0.0))
            b.rotation_euler = Euler((0.0, 0.0, 0.0))
            b.scale = Vector((1.0, 1.0, 1.0))
    bpy.context.view_layer.update()


# ===================================================================
# KEYFRAMING UTILITY HELPERS
# ===================================================================
def _keyframe_eyelids(rig, blink_factor, frame):
    """Translates Uzi's top and bottom visor eyelids to close eyes when blinking."""
    top_offset = EYELID_TRAVEL_TOP * blink_factor
    bot_offset = EYELID_TRAVEL_BOT * blink_factor

    for side in ["l", "r"]:
        top = getattr(rig, f"eyelid_top_{side}")
        bot = getattr(rig, f"eyelid_bot_{side}")

        if top.bone:
            top.location = top.base_loc + Vector((0.0, 0.0, top_offset))
            top.keyframe_insert("location", frame=frame)
        if bot.bone:
            bot.location = bot.base_loc + Vector((0.0, 0.0, bot_offset))
            bot.keyframe_insert("location", frame=frame)


def _keyframe_eyes(rig, phase, saccade_mult, frame):
    """Adds subtle digital pupil shifts to Uzi's CTR-Eye.L and CTR-Eye.R."""
    shift_x = math.sin(phase * 0.5) * EYE_SACCADE_SCALE * saccade_mult
    shift_z = math.cos(phase * 0.3) * EYE_SACCADE_SCALE * saccade_mult * 0.5

    for side in ["l", "r"]:
        eye = getattr(rig, f"eye_{side}")
        if eye.bone:
            eye.location = eye.base_loc + Vector((shift_x, 0.0, shift_z))
            eye.keyframe_insert("location", frame=frame)


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
    """Applies stance offset and keyframes leg joints with realistic knee articulation."""
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
        rig.foot_l.location = rig.foot_l.base_loc + Vector((-LEG_STANCE_SPREAD, 0.0, 0.0))
        rig.foot_l.rotation_euler = Euler(
            (
                rig.foot_l.base_rot.x + l_foot_flex,
                rig.foot_l.base_rot.y,
                rig.foot_l.base_rot.z,
            )
        )
        rig.foot_l.keyframe_insert("location", frame=frame)
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
        rig.foot_r.location = rig.foot_r.base_loc + Vector((LEG_STANCE_SPREAD, 0.0, 0.0))
        rig.foot_r.rotation_euler = Euler(
            (
                rig.foot_r.base_rot.x + r_foot_flex,
                rig.foot_r.base_rot.y,
                rig.foot_r.base_rot.z,
            )
        )
        rig.foot_r.keyframe_insert("location", frame=frame)
        rig.foot_r.keyframe_insert("rotation_euler", frame=frame)


def _keyframe_arms_idle(
    rig, breath_phase, arm_sway_rad, wrist_sway_rad, frame, arm_flare=0.0
):
    delayed_breath = get_organic_breath_curve(breath_phase - 0.5)
    z_offset = delayed_breath * arm_sway_rad

    if rig.up_arm_l.bone:
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


def _keyframe_arms_locomotion(
    rig, walk_phase, swing_amp, sway_amp, frame, arm_style="swing", arm_flare=0.0
):
    """Calculates athletic running arm mechanics with 65-85 deg elbow flex and body clearance."""
    l_swing = math.sin(walk_phase) * swing_amp
    r_swing = -math.sin(walk_phase) * swing_amp
    z_offset = math.cos(walk_phase) * sway_amp

    if arm_style == "pump":
        # Sprinting pump mechanics: deep 65° base elbow bend + 20° flex curve
        base_elbow = math.radians(65.0)
        elbow_flex_amp = math.radians(20.0)
        clearance_flare = math.radians(8.0) + arm_flare
    else:
        # Walking swing mechanics: 25° base elbow bend + 12° flex curve
        base_elbow = math.radians(25.0)
        elbow_flex_amp = math.radians(12.0)
        clearance_flare = math.radians(4.0) + arm_flare

    # Left Arm
    if rig.up_arm_l.bone:
        rig.up_arm_l.rotation_euler = Euler(
            (
                rig.up_arm_l.base_rot.x + l_swing,
                rig.up_arm_l.base_rot.y,
                rig.up_arm_l.base_rot.z + clearance_flare + z_offset,
            )
        )
        rig.up_arm_l.keyframe_insert("rotation_euler", frame=frame)
    if rig.fore_arm_l.bone:
        l_elbow = base_elbow + math.sin(walk_phase - 0.5) * elbow_flex_amp
        rig.fore_arm_l.rotation_euler = Euler(
            (
                rig.fore_arm_l.base_rot.x + l_elbow,
                rig.fore_arm_l.base_rot.y,
                rig.fore_arm_l.base_rot.z,
            )
        )
        rig.fore_arm_l.keyframe_insert("rotation_euler", frame=frame)

    # Right Arm
    if rig.up_arm_r.bone:
        rig.up_arm_r.rotation_euler = Euler(
            (
                rig.up_arm_r.base_rot.x + r_swing,
                rig.up_arm_r.base_rot.y,
                rig.up_arm_r.base_rot.z - clearance_flare - z_offset,
            )
        )
        rig.up_arm_r.keyframe_insert("rotation_euler", frame=frame)
    if rig.fore_arm_r.bone:
        r_elbow = base_elbow + math.sin(walk_phase - 0.5 - math.pi) * elbow_flex_amp
        rig.fore_arm_r.rotation_euler = Euler(
            (
                rig.fore_arm_r.base_rot.x + r_elbow,
                rig.fore_arm_r.base_rot.y,
                rig.fore_arm_r.base_rot.z,
            )
        )
        rig.fore_arm_r.keyframe_insert("rotation_euler", frame=frame)


def _keyframe_fingers(rig, breath_phase, finger_base, frame):
    """Keyframes Uzi's finger Curl controls (CTR-Thumb_Curl.L, etc.)."""
    pb = rig.pose_bones
    finger_names = ["Thumb", "Index", "Middle", "Pinky"]

    for side in ["L", "R"]:
        for f_name in finger_names:
            curl_bone_name = f"CTR-{f_name}_Curl.{side}"
            curl_bone = pb.get(curl_bone_name)
            if curl_bone:
                flex_offset = (
                    get_organic_breath_curve(breath_phase) * FINGER_FLEX_RANGE
                )
                curl_angle = (finger_base * 0.5) + flex_offset
                curl_bone.rotation_euler = Euler((curl_angle, 0.0, 0.0))
                curl_bone.keyframe_insert("rotation_euler", frame=frame)


# ===================================================================
# UNIVERSAL GENERATOR ENGINE
# ===================================================================
def generate_state(body_rig, face_rig, state_name: str, duration: int):
    profile = ResolvedProfile(MOTION_PROFILES[state_name])
    is_loco = state_name != "IDLE"

    _prepare_rig_for_animation(body_rig, face_rig)
    _reset_pose_to_rest(body_rig, face_rig)
    rig_data = CharacterRig(body_rig, face_rig, is_locomotion=is_loco)

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

        breath = get_breath(profile.breath_shape, phase) * profile.breath_depth

        # Smooth 4-Phase Jumping Mechanics
        if profile.torso_style == "jump":
            if progress < 0.25:
                t = progress / 0.25
                t_sin = math.sin(t * math.pi / 2.0)
                torso_z = -0.09 * t_sin
                torso_y = -0.03 * t_sin
                torso_pitch = math.radians(16.0) * t_sin
                knee_bend = math.radians(45.0) * t_sin
                thigh_swing = math.radians(24.0) * t_sin
                foot_flex = math.radians(15.0) * t_sin
                arm_swing = math.radians(-32.0) * t_sin

            elif progress < 0.48:
                t = (progress - 0.25) / 0.23
                t_sin = math.sin(t * math.pi / 2.0)
                torso_z = -0.09 + (0.16 - (-0.09)) * t_sin
                torso_y = -0.03 * (1.0 - t_sin)
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
                t_sin = math.sin(t * math.pi / 2.0)
                torso_z = 0.16 - 0.14 * t_sin
                torso_y = 0.0
                torso_pitch = math.radians(-6.0) * (1.0 - t_sin) + math.radians(8.0) * t_sin
                knee_bend = math.radians(18.0) * t_sin
                thigh_swing = math.radians(-12.0) * (1.0 - t_sin) + math.radians(10.0) * t_sin
                foot_flex = math.radians(-28.0) * (1.0 - t_sin) + math.radians(10.0) * t_sin
                arm_swing = math.radians(48.0) * (1.0 - t_sin) + math.radians(12.0) * t_sin

            else:
                t = (progress - 0.78) / 0.22
                t_sin = math.sin(t * math.pi / 2.0)
                torso_z = 0.02 * (1.0 - t_sin)
                torso_y = 0.0
                torso_pitch = math.radians(8.0) * (1.0 - t_sin)
                knee_bend = math.radians(18.0) * (1.0 - t_sin)
                thigh_swing = math.radians(10.0) * (1.0 - t_sin)
                foot_flex = math.radians(10.0) * (1.0 - t_sin)
                arm_swing = math.radians(12.0) * (1.0 - t_sin)

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
            else:
                t = (progress - 0.35) / 0.65
                interp = (1.0 - math.cos(t * math.pi)) / 2.0
                torso_z = -0.13 * (1.0 - interp)
                torso_y = -0.05 * (1.0 - interp)
                torso_pitch = math.radians(22.0) * (1.0 - interp)
                knee_bend = math.radians(58.0) * (1.0 - interp)
                thigh_swing = math.radians(34.0) * (1.0 - interp)
                foot_flex = math.radians(22.0) * (1.0 - interp)
                arm_swing = math.radians(-26.0) * (1.0 - interp)
            torso_x = 0.0

        else:
            torso_x = math.sin(phase) * profile.torso_sway_x
            torso_y = math.cos(phase) * profile.torso_sway_y
            torso_z = breath * profile.torso_bob_z
            torso_pitch = profile.torso_lean

        # Apply Torso
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

        # Apply Hips
        if rig_data.hips.bone:
            hips_roll = (
                rig_data.hips.base_rot.y + math.sin(phase) * profile.hips_sway * 0.5
            )
            hips_yaw = rig_data.hips.base_rot.z + math.sin(phase) * profile.hips_sway
            rig_data.hips.rotation_euler = Euler(
                (rig_data.hips.base_rot.x, hips_roll, hips_yaw)
            )
            rig_data.hips.keyframe_insert("rotation_euler", frame=frame)

        # Apply Chest
        if rig_data.chest.bone:
            chest_pitch = (
                rig_data.chest.base_rot.x
                + breath * 0.03
                + math.cos(phase * 2) * profile.spine_flex * 0.4
            )
            rig_data.chest.rotation_euler = Euler(
                (chest_pitch, rig_data.chest.base_rot.y, rig_data.chest.base_rot.z)
            )
            rig_data.chest.keyframe_insert("rotation_euler", frame=frame)

        # Apply Head
        if rig_data.head.bone:
            head_pitch = (
                rig_data.head.base_rot.x
                - torso_pitch * profile.head_stabilize
                - breath * 0.02
            )
            rig_data.head.rotation_euler = Euler(
                (head_pitch, rig_data.head.base_rot.y, rig_data.head.base_rot.z)
            )
            rig_data.head.keyframe_insert("rotation_euler", frame=frame)

        # Arms Dispatch
        if profile.arm_style in ("jump", "land"):
            for side in ["l", "r"]:
                up_arm = getattr(rig_data, f"up_arm_{side}")
                fore_arm = getattr(rig_data, f"fore_arm_{side}")
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

        elif profile.arm_style in ("passive", "out"):
            _keyframe_arms_idle(
                rig_data,
                phase + profile.arm_delay,
                profile.arm_sway,
                profile.arm_sway * 1.5,
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
                arm_style=profile.arm_style,
                arm_flare=profile.arm_flare,
            )

        # Legs Dispatch
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
            l_swing = math.sin(phase) * amp
            r_swing = -math.sin(phase) * amp

            # Natural knee articulation: 15° base flex + deep 60° folding during recovery
            l_knee_bend = math.radians(15.0) + max(0.0, math.sin(phase - 0.7)) * (amp * 1.0)
            r_knee_bend = math.radians(15.0) + max(0.0, math.sin(phase + math.pi - 0.7)) * (amp * 1.0)

            l_foot_flex = -math.sin(phase) * amp * 0.3
            r_foot_flex = math.sin(phase) * amp * 0.3

            _keyframe_legs(
                rig_data,
                l_swing,
                r_swing,
                0.0,
                0.0,
                l_knee_bend,
                r_knee_bend,
                l_foot_flex,
                r_foot_flex,
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
                max(0, math.sin(phase)) * amp * 0.38 + math.radians(10.0),
                max(0, -math.sin(phase)) * amp * 0.38 + math.radians(10.0),
                -math.sin(phase) * 0.12,
                math.sin(phase) * 0.12,
                frame,
            )
        else:  # static (IDLE)
            _keyframe_legs(
                rig_data,
                l_leg_swing=0.0,
                r_leg_swing=0.0,
                l_leg_sway=0.0,
                r_leg_sway=0.0,
                l_knee_bend=math.radians(8.0),
                r_knee_bend=math.radians(8.0),
                l_foot_flex=0.0,
                r_foot_flex=0.0,
                frame=frame,
            )

        # Secondary Facial & Appendage Dispatch
        _keyframe_fingers(rig_data, phase, profile.finger_curl, frame=frame)

        blink = (
            get_blink_factor_relative(progress, duration)
            if profile.eye_blink
            else 0.0
        )
        _keyframe_eyelids(rig_data, blink, frame=frame)
        _keyframe_eyes(
            rig_data, phase, saccade_mult=profile.eye_saccade, frame=frame
        )


# ===================================================================
# AUTOMATIC TIMELINE SCALER HANDLER
# ===================================================================
def auto_adjust_timeline_range(scene):
    """Snaps active timeline end frame when you pick an action in the Action Editor."""
    rig = (
        bpy.data.objects.get("Rig")
        or bpy.data.objects.get("Rig.001")
        or bpy.context.active_object
    )
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
# ACTION OVERWRITE & BATCH EXPORTER (Blender 5.2.0 Slotted Action Compatible)
# ===================================================================
def purge_messy_actions(rigs):
    """Unassigns and purges all actions except the exact 6 whitelist actions."""
    for r in rigs:
        if r and r.animation_data:
            r.animation_data.action = None

    for action in list(bpy.data.actions):
        if action.name not in VALID_ACTION_NAMES or "." in action.name:
            bpy.data.actions.remove(action)


def generate_all_actions():
    body_rig = (
        bpy.data.objects.get("Rig")
        or bpy.data.objects.get("Rig.001")
        or bpy.context.active_object
    )
    face_rig = bpy.data.objects.get("Facial_Rig")

    if not body_rig or body_rig.type != "ARMATURE":
        print("Please select Uzi's main Armature ('Rig') in the 3D Viewport.")
        return

    # Clean up any messy actions or .001 duplicates currently in memory
    purge_messy_actions([body_rig, face_rig])

    bpy.context.view_layer.objects.active = body_rig
    body_rig.select_set(True)

    states = {
        "IDLE": 120,
        "WALKING": 36,
        "RUNNING": 24,
        "STRAFE": 40,
        "JUMPING": 30,
        "LANDING": 20,
    }

    if not body_rig.animation_data:
        body_rig.animation_data_create()

    if face_rig and not face_rig.animation_data:
        face_rig.animation_data_create()

    for state_type, duration in states.items():
        action_name = f"Uzi_{state_type}"

        # Unassign and remove existing action block to ensure clean overwrite
        existing = bpy.data.actions.get(action_name)
        if existing:
            bpy.data.actions.remove(existing)

        # Create fresh action with exact name (no .001 suffix)
        action = bpy.data.actions.new(name=action_name)
        action.use_fake_user = True

        body_rig.animation_data.action = action
        if face_rig:
            face_rig.animation_data.action = action

        print(f"Generating clean Uzi Action: '{action_name}' ({duration} frames)...")
        generate_state(body_rig, face_rig, state_type, duration)

    # Preview Uzi_IDLE
    idle_action = bpy.data.actions.get("Uzi_IDLE")
    if idle_action:
        body_rig.animation_data.action = idle_action
        if face_rig:
            face_rig.animation_data.action = idle_action

        bpy.context.scene.frame_start = 1
        bpy.context.scene.frame_end = 120
        bpy.context.scene.frame_set(1)

    bpy.context.view_layer.update()
    bpy.ops.screen.animation_play()
    print("Export complete! Running arms updated with natural elbow bends.")


# Run generator
generate_all_actions()