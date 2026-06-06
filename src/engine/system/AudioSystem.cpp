// src/audio/AudioSystem.cpp
#include "Zahlen/alife/Types.hpp"
#include "ecs/ECS.hpp"
#include "physics/Physics.hpp"

#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN {

ZHLN_API void AudioSystem(Engine& engine, float dt) {
	auto& reg = engine.GetRegistry();
	auto& audio = engine.GetAudioContext();
	const auto& world = engine.GetPhysicsContext().GetWorld();

	// 1. Sync the Audio Listener with the Main Camera
	auto& cam = engine.GetCamera();

	float yawRad = JPH::DegreesToRadians(cam.yaw);
	float pitchRad = JPH::DegreesToRadians(cam.pitch);

	// Correct Z axis coordinate maps to: Sin(yaw) * Cos(pitch)
	JPH::Vec3 direction(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad),
						JPH::Sin(yawRad) * JPH::Cos(pitchRad));
	direction = direction.Normalized();

	audio.UpdateListener(cam.position, direction, JPH::Vec3::sAxisY());

	// 2. Process all entities with an AudioSourceComponent
	auto entities = reg.GetEntitiesWith<AudioSourceComponent>();
	auto audioSources = reg.GetRawArray<AudioSourceComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		AudioSourceComponent& source = audioSources[i];

		// Instantiate native sound if missing
		if (source.nativeSound == nullptr) {
			source.nativeSound = audio.CreateSoundInstance(source.filepath, source.isSpatialized);
			if (source.nativeSound != nullptr) {
				audio.SetSoundInstanceLooping(source.nativeSound, source.isLooping);
				audio.SetSoundInstanceVolume(source.nativeSound, source.volume);
				if (source.playOnStart) {
					audio.PlaySoundInstance(source.nativeSound);
				}
			}
		}

		if (source.nativeSound != nullptr) {
			// Find world-space position (Defaults to Origin)
			JPH::Vec3 position = JPH::Vec3::sZero();
			if (auto* phys = reg.Get<PhysicsComponent>(e)) {
				uint32_t dense = world.slotToDense[phys->physicsHandle.index];
				const size_t base = static_cast<size_t>(dense) * 4;
				position = JPH::Vec3((float)world.positions[base], (float)world.positions[base + 1],
									 (float)world.positions[base + 2]);
			} else if (auto* alifeComp = reg.Get<ALife::ALifeComponent>(e)) {
				position = JPH::Vec3(alifeComp->position);
			}

			// Sync dynamic parameters down to the active stream
			audio.SetSoundInstancePosition(source.nativeSound, position);
			audio.SetSoundInstanceVolume(source.nativeSound, source.volume);
			audio.SetSoundInstanceLooping(source.nativeSound, source.isLooping);
		}
	}
}

} // namespace ZHLN
