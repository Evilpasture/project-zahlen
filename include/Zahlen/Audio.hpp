// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Common.h>
#include <memory>
#include <string>

namespace ZHLN {

class Engine;

struct AudioConfig {
	bool enableSpatialization = true;
};

class ZHLN_API AudioContext {
  public:
	AudioContext(const AudioConfig& cfg = {});
	~AudioContext();

	AudioContext(const AudioContext&) = delete;
	AudioContext& operator=(const AudioContext&) = delete;

	// Listener Positioning for 3D Audio
	void UpdateListener(const JPH::Vec3& position, const JPH::Vec3& direction,
						const JPH::Vec3& up = JPH::Vec3::sAxisY());

	// Simple fire-and-forget one-shots (2D)
	void PlayOneShot(const std::string& filepath, float volume = 1.0f);

	// Simple fire-and-forget one-shots (3D)
	void PlayOneShot3D(const std::string& filepath, const JPH::Vec3& position, float volume = 1.0f);

	// Explicit Sound Instancing API (keeps miniaudio.h completely private)
	auto CreateSoundInstance(const std::string& filepath, bool spatialized = true) -> void*;
	void DestroySoundInstance(void* soundHandle);
	void PlaySoundInstance(void* soundHandle);
	void StopSoundInstance(void* soundHandle);
	void SetSoundInstancePosition(void* soundHandle, const JPH::Vec3& position);
	void SetSoundInstanceVolume(void* soundHandle, float volume);
	void SetSoundInstanceLooping(void* soundHandle, bool looping);
	auto IsSoundInstancePlaying(void* soundHandle) -> bool;

	// Procedural sound generation (for UI beeps, jump indicators, etc.)
	void PlayProceduralBeep(float frequency = 440.0f, float duration = 0.5f, float volume = 0.2f);

	struct Impl;
	[[nodiscard]] auto GetImpl() const -> Impl* { return _impl.get(); }

  private:
	std::unique_ptr<Impl> _impl;
};

// ECS Audio Update System
ZHLN_API void AudioSystem(Engine& engine, float dt);

} // namespace ZHLN
