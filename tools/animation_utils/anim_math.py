import bpy
import math
from mathutils import Vector, Euler

def get_organic_breath_curve(phase):
    """Generates an asymmetric, double-exponential breathing curve."""
    skewed_phase = phase + 0.4 * math.sin(phase)
    raw_val = math.exp(math.sin(skewed_phase))
    min_val = math.exp(-1)
    max_val = math.exp(1)
    normalized = (raw_val - min_val) / (max_val - min_val)
    return normalized * 2.0 - 1.0

def get_blink_factor_relative(progress, duration):
    """Adjusts blink frequency based on animation length."""
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

def generate_single_state_data(rig, state_type, duration, config, rig_map, adjust):
    pose_bones = rig.pose.bones

    # Reference bones using Rig Schema Map
    torso = pose_bones.get(rig_map["TORSO"])
    chest = pose_bones.get(rig_map["CHEST"])
    hips = pose_bones.get(rig_map["HIPS"])
    head = pose_bones.get(rig_map["HEAD"])
    spine_0 = pose_bones.get(rig_map["SPINE_0"])
    spine_1 = pose_bones.get(rig_map["SPINE_1"])
    
    up_arm_l = pose_bones.get(rig_map["ARM_L"])
    fore_arm_l = pose_bones.get(rig_map["FOREARM_L"])
    hand_l = pose_bones.get(rig_map["HAND_L"])
    up_arm_r = pose_bones.get(rig_map["ARM_R"])
    fore_arm_r = pose_bones.get(rig_map["FOREARM_R"])
    hand_r = pose_bones.get(rig_map["HAND_R"])
    
    thigh_l = pose_bones.get(rig_map["THIGH_L"])
    shin_l = pose_bones.get(rig_map["SHIN_L"])
    foot_l = pose_bones.get(rig_map["FOOT_L"])
    thigh_r = pose_bones.get(rig_map["THIGH_R"])
    shin_r = pose_bones.get(rig_map["SHIN_R"])
    foot_r = pose_bones.get(rig_map["FOOT_R"])
    
    hat_l = pose_bones.get(rig_map["HAT_L"])
    hat_r = pose_bones.get(rig_map["HAT_R"])
    eyelid_top_l = pose_bones.get(rig_map["EYELID_TOP_L"])
    eyelid_top_r = pose_bones.get(rig_map["EYELID_TOP_R"])
    eyelid_bot_l = pose_bones.get(rig_map["EYELID_BOT_L"])
    eyelid_bot_r = pose_bones.get(rig_map["EYELID_BOT_R"])
    eye_l = pose_bones.get(rig_map["EYE_L"])
    eye_r = pose_bones.get(rig_map["EYE_R"])

    # Configure FK Switches [2]
    for side in ["L", "R"]:
        arm_parent = pose_bones.get(rig_map[f"ARM_PARENT_{side}"])
        if arm_parent: arm_parent["IK_FK"] = 1.0
        leg_parent = pose_bones.get(rig_map[f"LEG_PARENT_{side}"])
        if leg_parent: leg_parent["IK_FK"] = 1.0

    # Cache starting transforms
    base_torso_loc = torso.location.copy() if torso else Vector((0,0,0))
    if torso: torso.rotation_mode = 'XYZ'
    base_torso_rot = torso.rotation_euler.copy() if torso else Euler((0,0,0))
    
    if chest: chest.rotation_mode = 'XYZ'
    base_chest_rot = chest.rotation_euler.copy() if chest else Euler((0,0,0))
    if hips: hips.rotation_mode = 'XYZ'
    base_hips_rot = hips.rotation_euler.copy() if hips else Euler((0,0,0))
    if head: head.rotation_mode = 'XYZ'
    base_head_rot = head.rotation_euler.copy() if head else Euler((0,0,0))
    
    if spine_0: spine_0.rotation_mode = 'XYZ'
    base_spine_0_rot = spine_0.rotation_euler.copy() if spine_0 else Euler((0,0,0))
    if spine_1: spine_1.rotation_mode = 'XYZ'
    base_spine_1_rot = spine_1.rotation_euler.copy() if spine_1 else Euler((0,0,0))
    
    # Arm rotation defaults
    if up_arm_l: up_arm_l.rotation_mode = 'XYZ'
    base_up_arm_l_rot = Euler((0, 0, math.radians(-70))) if up_arm_l else Euler((0,0,0))
    if fore_arm_l: fore_arm_l.rotation_mode = 'XYZ'
    base_fore_arm_l_rot = Euler((math.radians(25), 0, 0)) if fore_arm_l else Euler((0,0,0))
    if hand_l: hand_l.rotation_mode = 'XYZ'
    base_hand_l_rot = hand_l.rotation_euler.copy() if hand_l else Euler((0,0,0))

    if up_arm_r: up_arm_r.rotation_mode = 'XYZ'
    base_up_arm_r_rot = Euler((0, 0, math.radians(70))) if up_arm_r else Euler((0,0,0))
    if fore_arm_r: fore_arm_r.rotation_mode = 'XYZ'
    base_fore_arm_r_rot = Euler((math.radians(25), 0, 0)) if fore_arm_r else Euler((0,0,0))
    if hand_r: hand_r.rotation_mode = 'XYZ'
    base_hand_r_rot = hand_r.rotation_euler.copy() if hand_r else Euler((0,0,0))

    # Leg rotation defaults
    th_rot = math.radians(-5) if config['walk_freq'] > 0 else 0.0
    sh_rot = math.radians(5) if config['walk_freq'] > 0 else 0.0
    ft_rot = math.radians(-5) if config['walk_freq'] > 0 else 0.0

    if thigh_l: thigh_l.rotation_mode = 'XYZ'
    base_thigh_l_rot = Euler((th_rot, 0, 0)) if thigh_l else Euler((0,0,0))
    if shin_l: shin_l.rotation_mode = 'XYZ'
    base_shin_l_rot = Euler((sh_rot, 0, 0)) if shin_l else Euler((0,0,0))
    if foot_l: foot_l.rotation_mode = 'XYZ'
    base_foot_l_rot = Euler((ft_rot, 0, 0)) if foot_l else Euler((0,0,0))

    if thigh_r: thigh_r.rotation_mode = 'XYZ'
    base_thigh_r_rot = Euler((th_rot, 0, 0)) if thigh_r else Euler((0,0,0))
    if shin_r: shin_r.rotation_mode = 'XYZ'
    base_shin_r_rot = Euler((sh_rot, 0, 0)) if shin_r else Euler((0,0,0))
    if foot_r: foot_r.rotation_mode = 'XYZ'
    base_foot_r_rot = Euler((ft_rot, 0, 0)) if foot_r else Euler((0,0,0))

    # Secondary defaults
    base_eyelid_top_l = eyelid_top_l.location.copy() if eyelid_top_l else Vector((0,0,0))
    base_eyelid_top_r = eyelid_top_r.location.copy() if eyelid_top_r else Vector((0,0,0))
    base_eyelid_bot_l = eyelid_bot_l.location.copy() if eyelid_bot_l else Vector((0,0,0))
    base_eyelid_bot_r = eyelid_bot_r.location.copy() if eyelid_bot_r else Vector((0,0,0))
    base_eye_l = eye_l.location.copy() if eye_l else Vector((0,0,0))
    base_eye_r = eye_r.location.copy() if eye_r else Vector((0,0,0))

    if hat_l: hat_l.rotation_mode = 'XYZ'
    base_hat_l_rot = hat_l.rotation_euler.copy() if hat_l else Euler((0,0,0))
    if hat_r: hat_r.rotation_mode = 'XYZ'
    base_hat_r_rot = hat_r.rotation_euler.copy() if hat_r else Euler((0,0,0))

    loop_divisor = duration
    for frame in range(1, duration + 1):
        bpy.context.scene.frame_set(frame)
        progress = (frame - 1) / loop_divisor
        
        if config['walk_freq'] > 0:
            walk_phase = progress * 2 * math.pi
            sway_phase = walk_phase
            breath_phase = walk_phase * config['breath_speed_mult']
            organic_breath = math.sin(breath_phase)
            delayed_breath = math.sin(breath_phase - 0.5)
        else:
            walk_phase = 0
            sway_phase = progress * 2 * math.pi
            breath_phase = progress * 2 * math.pi * config['breath_speed_mult']
            organic_breath = get_organic_breath_curve(breath_phase)
            delayed_breath = get_organic_breath_curve(breath_phase - 0.5)

        # --- A. Torso Translation & Dynamic Momentum Lean ---
        if torso:
            x_sway = math.sin(sway_phase) * config['body_sway_x']
            if config['walk_freq'] > 0:
                z_bounce = (abs(math.cos(walk_phase)) - 0.5) * 2.0 * config['z_bounce']
                y_sway = config['y_forward'] + (abs(math.cos(walk_phase)) - 0.5) * 2.0 * config['body_sway_y']
            else:
                z_bounce = 0
                y_sway = math.cos(sway_phase) * config['body_sway_y']
                
            z_breath = organic_breath * config['breath_lift']
            torso.location = base_torso_loc + Vector((x_sway, y_sway, z_breath + z_bounce))
            
            # Lean rotation
            torso_pitch = math.radians(config['torso_pitch'])
            torso_roll = math.radians(config['torso_roll'])
            if state_type == 'STRAFE':
                torso_roll = math.sin(sway_phase) * math.radians(config['torso_roll'])
                
            torso.rotation_euler = Euler((torso_pitch, torso_roll, base_torso_rot.z))
            
            torso.keyframe_insert(data_path="location", frame=frame)
            torso.keyframe_insert(data_path="rotation_euler", frame=frame)

        # --- B. Hips & Chest Multi-Axis Rotation ---
        if hips:
            hips_tilt = organic_breath * math.radians(1.0)
            hips_pitch_loc = base_hips_rot.x - hips_tilt
            if config['walk_freq'] > 0:
                hips_pitch_loc += math.cos(walk_phase * 2) * config['hip_pitch_x']
            hips_roll_loc = base_hips_rot.y
            if config['walk_freq'] > 0:
                hips_roll_loc += math.sin(walk_phase) * config['hip_roll_y']
            hips_yaw_loc = base_hips_rot.z
            if config['walk_freq'] > 0:
                hips_yaw_loc += math.sin(walk_phase) * config['hip_yaw']
                
            hips.rotation_euler = Euler((hips_pitch_loc, hips_roll_loc, hips_yaw_loc))
            hips.keyframe_insert(data_path="rotation_euler", frame=frame)
            
        # --- C. Intermediate Spine Bones [3] ---
        if spine_0:
            s0_pitch = base_spine_0_rot.x + (math.cos(walk_phase * 2) * config['spine_pitch_x'] if config['walk_freq'] > 0 else organic_breath * math.radians(0.5))
            s0_yaw = base_spine_0_rot.z + (math.sin(walk_phase) * config['spine_yaw_z'] if config['walk_freq'] > 0 else 0)
            spine_0.rotation_euler = Euler((s0_pitch, base_spine_0_rot.y, s0_yaw))
            spine_0.keyframe_insert(data_path="rotation_euler", frame=frame)
            
        if spine_1:
            s1_pitch = base_spine_1_rot.x + (math.cos(walk_phase * 2 - 0.5) * (config['spine_pitch_x'] * 0.7) if config['walk_freq'] > 0 else organic_breath * math.radians(0.3))
            s1_yaw = base_spine_1_rot.z + (math.sin(walk_phase - 0.5) * (config['spine_yaw_z'] * 0.7) if config['walk_freq'] > 0 else 0)
            spine_1.rotation_euler = Euler((s1_pitch, base_spine_1_rot.y, s1_yaw))
            spine_1.keyframe_insert(data_path="rotation_euler", frame=frame)

        if chest:
            chest_tilt = delayed_breath * math.radians(1.8)
            chest_pitch_loc = base_chest_rot.x + chest_tilt
            if config['walk_freq'] > 0:
                chest_pitch_loc += math.cos(walk_phase * 2) * config['chest_pitch_x']
            chest_roll_loc = base_chest_rot.y
            if config['walk_freq'] > 0:
                chest_roll_loc -= math.sin(walk_phase) * config['chest_roll_y']
            chest_yaw_loc = base_chest_rot.z
            if config['walk_freq'] > 0:
                chest_yaw_loc += math.sin(walk_phase) * config['chest_yaw']
                
            chest.rotation_euler = Euler((chest_pitch_loc, chest_roll_loc, chest_yaw_loc))
            chest.keyframe_insert(data_path="rotation_euler", frame=frame)

        # --- D. Head Leveling Compensation ---
        if head:
            head_compensate = organic_breath * math.radians(0.8)
            walk_stabilize = (math.sin(walk_phase * 2) * math.radians(1.0)) if config['walk_freq'] > 0 else 0
            
            head.rotation_euler = Euler((base_head_rot.x - head_compensate + walk_stabilize, base_head_rot.y, base_head_rot.z))
            head.keyframe_insert(data_path="rotation_euler", frame=frame)

        # --- E. Dynamic FK Arm and Hand Sway ---
        arm_sway_rad = math.radians(adjust['ARM_SWAY_RANGE'])
        wrist_sway_rad = math.radians(adjust['WRIST_SWAY_RANGE'])
        
        l_swing = math.sin(walk_phase) * config['arm_swing_x'] if config['walk_freq'] > 0 else 0
        r_swing = -math.sin(walk_phase) * config['arm_swing_x'] if config['walk_freq'] > 0 else 0
        
        # Left Arm Sway
        if up_arm_l:
            z_offset = (math.cos(walk_phase) * config['arm_sway_z']) if config['walk_freq'] > 0 else (delayed_breath * arm_sway_rad)
            up_arm_l.rotation_euler = Euler((base_up_arm_l_rot.x + l_swing, base_up_arm_l_rot.y, base_up_arm_l_rot.z + z_offset))
            up_arm_l.keyframe_insert(data_path="rotation_euler", frame=frame)
        if fore_arm_l:
            elbow_offset = (math.sin(walk_phase - 0.5) * config['arm_swing_x'] * 0.4) if config['walk_freq'] > 0 else (get_organic_breath_curve(breath_phase - 0.7) * arm_sway_rad)
            fore_arm_l.rotation_euler = Euler((base_fore_arm_l_rot.x + elbow_offset, base_fore_arm_l_rot.y, base_fore_arm_l_rot.z))
            fore_arm_l.keyframe_insert(data_path="rotation_euler", frame=frame)
        if hand_l:
            wrist_offset = (math.cos(walk_phase - 1.0) * config['arm_swing_x'] * 0.15) if config['walk_freq'] > 0 else (get_organic_breath_curve(breath_phase - 1.0) * wrist_sway_rad)
            hand_l.rotation_euler = Euler((base_hand_l_rot.x + wrist_offset, base_hand_l_rot.y, base_hand_l_rot.z))
            hand_l.keyframe_insert(data_path="rotation_euler", frame=frame)

        # Right Arm Sway
        if up_arm_r:
            z_offset = (math.cos(walk_phase) * config['arm_sway_z']) if config['walk_freq'] > 0 else (delayed_breath * arm_sway_rad)
            up_arm_r.rotation_euler = Euler((base_up_arm_r_rot.x + r_swing, base_up_arm_r_rot.y, base_up_arm_r_rot.z - z_offset))
            up_arm_r.keyframe_insert(data_path="rotation_euler", frame=frame)
        if fore_arm_r:
            elbow_offset = (math.sin(walk_phase - 0.5 - math.pi) * config['arm_swing_x'] * 0.4) if config['walk_freq'] > 0 else (get_organic_breath_curve(breath_phase - 0.7) * arm_sway_rad)
            fore_arm_r.rotation_euler = Euler((base_fore_arm_r_rot.x + elbow_offset, base_fore_arm_r_rot.y, base_fore_arm_r_rot.z))
            fore_arm_r.keyframe_insert(data_path="rotation_euler", frame=frame)
        if hand_r:
            wrist_offset = (math.cos(walk_phase - 1.0 - math.pi) * config['arm_swing_x'] * 0.15) if config['walk_freq'] > 0 else (get_organic_breath_curve(breath_phase - 1.0) * wrist_sway_rad)
            hand_r.rotation_euler = Euler((base_hand_r_rot.x + wrist_offset, base_hand_r_rot.y, base_hand_r_rot.z))
            hand_r.keyframe_insert(data_path="rotation_euler", frame=frame)

        # --- F. FK Leg Swing ---
        if state_type == 'STRAFE':
            l_leg_sway = max(0, math.sin(walk_phase)) * config['leg_swing_z']
            r_leg_sway = max(0, -math.sin(walk_phase)) * config['leg_swing_z']
            l_leg_swing, r_leg_swing = 0.0, 0.0
            l_knee_bend = max(0, math.sin(walk_phase)) * config['knee_bend_x']
            r_knee_bend = max(0, -math.sin(walk_phase)) * config['knee_bend_x']
        else:
            l_leg_sway = math.cos(walk_phase) * config['leg_swing_z']
            r_leg_sway = -math.cos(walk_phase) * config['leg_swing_z']
            l_leg_swing = math.sin(walk_phase) * config['leg_swing_x']
            r_leg_swing = -math.sin(walk_phase) * config['leg_swing_x']
            l_knee_bend = max(0, math.cos(walk_phase)) * config['knee_bend_x']
            r_knee_bend = max(0, -math.cos(walk_phase)) * config['knee_bend_x']
            
        l_foot_flex = -math.sin(walk_phase) * config['foot_flex_x'] if config['walk_freq'] > 0 else 0
        r_foot_flex = math.sin(walk_phase) * config['foot_flex_x'] if config['walk_freq'] > 0 else 0
        
        # Left Leg
        if thigh_l:
            thigh_l.rotation_euler = Euler((base_thigh_l_rot.x - l_leg_swing, base_thigh_l_rot.y, base_thigh_l_rot.z + l_leg_sway))
            thigh_l.keyframe_insert(data_path="rotation_euler", frame=frame)
        if shin_l:
            shin_l.rotation_euler = Euler((base_shin_l_rot.x + l_knee_bend, base_shin_l_rot.y, base_shin_l_rot.z))
            shin_l.keyframe_insert(data_path="rotation_euler", frame=frame)
        if foot_l:
            foot_l.rotation_euler = Euler((base_foot_l_rot.x + l_foot_flex, base_foot_l_rot.y, base_foot_l_rot.z))
            foot_l.keyframe_insert(data_path="rotation_euler", frame=frame)
            
        # Right Leg
        if thigh_r:
            thigh_r.rotation_euler = Euler((base_thigh_r_rot.x - r_leg_swing, base_thigh_r_rot.y, base_thigh_r_rot.z + (-1.0 * r_leg_sway)))
            thigh_r.keyframe_insert(data_path="rotation_euler", frame=frame)
        if shin_r:
            shin_r.rotation_euler = Euler((base_shin_r_rot.x + r_knee_bend, base_shin_r_rot.y, base_shin_r_rot.z))
            shin_r.keyframe_insert(data_path="rotation_euler", frame=frame)
        if foot_r:
            foot_r.rotation_euler = Euler((base_foot_r_rot.x + r_foot_flex, base_foot_r_rot.y, base_foot_r_rot.z))
            foot_r.keyframe_insert(data_path="rotation_euler", frame=frame)

        # --- G. Finger curling ---
        finger_bases = rig_map.get("FINGER_BASES", ["f_index", "f_middle", "f_ring", "thumb"])
        for side in ["L", "R"]:
            flip = 1.0 if side == "L" else -1.0
            
            # Keyframe active settings
            arm_parent = pose_bones.get(rig_map[f"ARM_PARENT_{side}"])
            if arm_parent: arm_parent.keyframe_insert(data_path='["IK_FK"]', frame=frame)
            leg_parent = pose_bones.get(rig_map[f"LEG_PARENT_{side}"])
            if leg_parent: leg_parent.keyframe_insert(data_path='["IK_FK"]', frame=frame)
                
            for base in finger_bases:
                delay = finger_delays[base]
                if config['walk_freq'] > 0:
                    flex_offset = math.sin(breath_phase + delay) * adjust['FINGER_FLEX_RANGE']
                else:
                    flex_offset = get_organic_breath_curve(breath_phase + delay) * adjust['FINGER_FLEX_RANGE']
                
                for seg in ["01", "02", "03"]:
                    bone_name = f"{base}.{seg}.{side}"
                    bone = pose_bones.get(bone_name)
                    if bone:
                        bone.rotation_mode = 'XYZ'
                        if base == "thumb":
                            target_rot_x = flip * (math.radians(10) + flex_offset * 0.5)
                            target_rot_y = flip * math.radians(5)
                            bone.rotation_euler = (target_rot_x, target_rot_y, 0)
                        else:
                            base_angle = math.radians(config['finger_base'] * 80 * (1 if seg == "01" else 1.2))
                            bone.rotation_euler = (flip * (base_angle + flex_offset), 0, 0)
                        bone.keyframe_insert(data_path="rotation_euler", frame=frame)

        # --- H. Eyelids ---
        if config['allow_blink']:
            blink_factor = get_blink_factor_relative(progress, duration)
        else:
            blink_factor = 0.0
            
        upper_offset = adjust['BLINK_TRAVEL_UPPER'] * blink_factor
        lower_offset = adjust['BLINK_TRAVEL_LOWER'] * blink_factor
        
        if eyelid_top_l:
            eyelid_top_l.location = base_eyelid_top_l + Vector((0, upper_offset, upper_offset))
            eyelid_top_l.keyframe_insert(data_path="location", frame=frame)
        if eyelid_top_r:
            eyelid_top_r.location = base_eyelid_top_r + Vector((0, upper_offset, upper_offset))
            eyelid_top_r.keyframe_insert(data_path="location", frame=frame)
        if eyelid_bot_l:
            eyelid_bot_l.location = base_eyelid_bot_l + Vector((0, lower_offset, -lower_offset))
            eyelid_bot_l.keyframe_insert(data_path="location", frame=frame)
        if eyelid_bot_r:
            eyelid_bot_r.location = base_eyelid_bot_r + Vector((0, lower_offset, -lower_offset))
            eyelid_bot_r.keyframe_insert(data_path="location", frame=frame)

        # --- I. Saccades ---
        saccade_x, saccade_z = get_eye_saccade_relative(progress)
        saccade_x *= config['eye_activity']
        saccade_z *= config['eye_activity']
        
        if eye_l:
            eye_l.location = base_eye_l + Vector((saccade_x, 0, saccade_z))
            eye_l.keyframe_insert(data_path="location", frame=frame)
        if eye_r:
            eye_r.location = base_eye_r + Vector((saccade_x, 0, saccade_z))
            eye_r.keyframe_insert(data_path="location", frame=frame)

        # --- J. Hat Sway ---
        sway_offset = math.cos(breath_phase) * math.radians(2.5)
        walk_sway = (math.cos(walk_phase * 2) * math.radians(1.5)) if config['walk_freq'] > 0 else 0
        
        if hat_l:
            hat_l.rotation_euler = Euler((base_hat_l_rot.x, base_hat_l_rot.y + sway_offset + walk_sway, base_hat_l_rot.z))
            hat_l.keyframe_insert(data_path="rotation_euler", frame=frame)
        if hat_r:
            hat_r.rotation_euler = Euler((base_hat_r_rot.x, base_hat_r_rot.y - sway_offset - walk_sway, base_hat_r_rot.z))
            hat_r.keyframe_insert(data_path="rotation_euler", frame=frame)