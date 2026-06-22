// src/zcook/Cook.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Cook.hpp"

#include "BinaryReader.hpp"
#include "GLB.hpp"
#include "Transform.hpp"
#include "threading/TaskSystem.hpp"
#include "threading/Thread.hpp"

#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Types.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <print>
#include <vector>

namespace fs = std::filesystem;

namespace ZHLN {

int CookMesh(int argc, char** argv) {
	std::string metaPath, meshId, outPath, inPath;
	for (int i = 0; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--meta" && i + 1 < argc)
			metaPath = argv[++i];
		else if (arg == "--id" && i + 1 < argc)
			meshId = argv[++i];
		else if (arg == "-i" && i + 1 < argc)
			inPath = argv[++i];
		else if (arg == "-o" && i + 1 < argc)
			outPath = argv[++i];
	}

	if (metaPath.empty() || meshId.empty() || inPath.empty() || outPath.empty()) {
		std::println(stderr, "[zcook] ERROR: Missing arguments for mesh subcommand.");
		return 1;
	}

	// Safely patch the extension incase the build system passed .json instead of .bin
	std::string binMetaPath = fs::path(metaPath).replace_extension(".bin").string();

	Compiler::BinaryReader reader(binMetaPath);
	Compiler::IRManifest manifest = reader.Parse();

	auto it = std::ranges::find_if(manifest.meshes, [&](const auto& m) { return m.id == meshId; });
	if (it == manifest.meshes.end())
		return 1;

	CompiledMesh compiled = CompileRawMesh(*it, inPath);

	CookedMeshHeader meshHeader{};
	meshHeader.magic = 0x3048534D;
	meshHeader.version = 2;

	if (compiled.glbVertices.empty()) {
		meshHeader.boundingBoxMin[0] = meshHeader.boundingBoxMin[1] = meshHeader.boundingBoxMax[0] =
			meshHeader.boundingBoxMax[1] = meshHeader.boundingBoxMax[2] = 0.0f;
		meshHeader.vertexCount = meshHeader.indexCount = 0;
	} else {
		meshHeader.boundingBoxMin[0] = compiled.minB[0];
		meshHeader.boundingBoxMin[1] = compiled.minB[1];
		meshHeader.boundingBoxMin[2] = compiled.minB[2];
		meshHeader.boundingBoxMax[0] = compiled.maxB[0];
		meshHeader.boundingBoxMax[1] = compiled.maxB[1];
		meshHeader.boundingBoxMax[2] = compiled.maxB[2];
		meshHeader.vertexCount = static_cast<uint32_t>(compiled.glbVertices.size() / 16);
		meshHeader.indexCount = static_cast<uint32_t>(compiled.indices.size());
	}

	std::vector<Vertex> finalVerts(meshHeader.vertexCount);
	for (uint32_t i = 0; i < meshHeader.vertexCount; ++i) {
		Vertex& v = finalVerts[i];
		const float* flts = &compiled.glbVertices[static_cast<size_t>(i) * 16];
		v.position[0] = flts[0];
		v.position[1] = flts[1];
		v.position[2] = flts[2];
		v.normal = Math::PackNormal(flts[3], flts[4], flts[5]);
		v.tangent = Math::PackNormal(flts[6], flts[7], flts[8], flts[9]);
		v.uv = Math::PackUV(flts[10], flts[11]);
		v.color = Math::PackColor(flts[12], flts[13], flts[14], flts[15]);

		if (compiled.isSkinned) {
			v.joints[0] = compiled.joints[i * 4 + 0];
			v.joints[1] = compiled.joints[i * 4 + 1];
			v.joints[2] = compiled.joints[i * 4 + 2];
			v.joints[3] = compiled.joints[i * 4 + 3];
			v.weights[0] = compiled.weights[i * 4 + 0];
			v.weights[1] = compiled.weights[i * 4 + 1];
			v.weights[2] = compiled.weights[i * 4 + 2];
			v.weights[3] = compiled.weights[i * 4 + 3];
		}
	}

	fs::create_directories(fs::path(outPath).parent_path());
	FILE* out = std::fopen(outPath.c_str(), "wb");
	if (out == nullptr)
		return 1;

	std::fwrite(&meshHeader, 1, sizeof(CookedMeshHeader), out);
	if (!finalVerts.empty())
		std::fwrite(finalVerts.data(), 1, finalVerts.size() * sizeof(Vertex), out);
	if (!compiled.indices.empty())
		std::fwrite(compiled.indices.data(), 1, compiled.indices.size() * sizeof(uint32_t), out);
	std::fclose(out);
	return 0;
}

int CookTexture(int argc, char** argv) {
	std::string inPath, outPath;
	for (int i = 0; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "-i" && i + 1 < argc)
			inPath = argv[++i];
		else if (arg == "-o" && i + 1 < argc)
			outPath = argv[++i];
	}
	if (inPath.empty() || outPath.empty())
		return 1;

	FILE* in = std::fopen(inPath.c_str(), "rb");
	if (!in)
		return 1;
	std::fseek(in, 0, SEEK_END);
	long size = std::ftell(in);
	std::fseek(in, 0, SEEK_SET);
	std::vector<char> fileData(size);
	std::fread(fileData.data(), 1, size, in);
	std::fclose(in);

	fs::create_directories(fs::path(outPath).parent_path());
	FILE* out = std::fopen(outPath.c_str(), "wb");
	if (!out)
		return 1;
	std::fwrite(fileData.data(), 1, size, out);
	std::fclose(out);
	return 0;
}

int CookAnimation(int argc, char** argv) {
	std::string metaPath, outPath, animId;
	for (int i = 0; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--meta" && i + 1 < argc)
			metaPath = argv[++i];
		else if (arg == "--id" && i + 1 < argc)
			animId = argv[++i];
		else if (arg == "-o" && i + 1 < argc)
			outPath = argv[++i];
	}
	if (metaPath.empty() || animId.empty() || outPath.empty())
		return 1;

	std::string binMetaPath = fs::path(metaPath).replace_extension(".bin").string();
	Compiler::BinaryReader reader(binMetaPath);
	Compiler::IRManifest manifest = reader.Parse();

	auto it =
		std::ranges::find_if(manifest.animations, [&](const auto& a) { return a.id == animId; });
	if (it == manifest.animations.end())
		return 1;

	const auto& anim = *it;
	std::string levelFolder = fs::path(metaPath).parent_path().string();

	std::vector<CookedAnimTrack> tracks;
	std::vector<uint8_t> payloadData;

	auto appendPayload = [&](const std::string& binFile, uint32_t offset,
							 uint32_t length) -> uint32_t {
		std::string fullBinPath = levelFolder + "/" + binFile;
		FILE* bf = std::fopen(fullBinPath.c_str(), "rb");
		if (!bf)
			return 0;
		std::vector<uint8_t> temp(length);
		std::fseek(bf, offset, SEEK_SET);
		std::fread(temp.data(), 1, length, bf);
		std::fclose(bf);
		while (payloadData.size() % 4 != 0)
			payloadData.push_back(0);
		auto localOffset = static_cast<uint32_t>(payloadData.size());
		payloadData.insert(payloadData.end(), temp.begin(), temp.end());
		return localOffset;
	};

	for (const auto& channel : anim.channels) {
		if (channel.samplerId >= anim.samplers.size())
			continue;
		const auto& sampler = anim.samplers[channel.samplerId];

		CookedAnimTrack track{};
		track.targetNodeHash = HashAssetPath(channel.targetNodeId);

		if (channel.targetPath == "translation")
			track.pathType = 0;
		else if (channel.targetPath == "rotation")
			track.pathType = 1;
		else if (channel.targetPath == "scale")
			track.pathType = 2;
		else
			continue;

		track.keyCount = sampler.inputLength / sizeof(float);
		track.timeOffset = appendPayload(sampler.binFile, sampler.inputOffset, sampler.inputLength);
		track.valueOffset =
			appendPayload(sampler.binFile, sampler.outputOffset, sampler.outputLength);
		tracks.push_back(track);
	}

	CookedAnimHeader header{};
	header.duration = anim.duration;
	header.loop = anim.loop ? 1 : 0;
	header.trackCount = static_cast<uint32_t>(tracks.size());

	fs::create_directories(fs::path(outPath).parent_path());
	FILE* out = std::fopen(outPath.c_str(), "wb");
	if (!out)
		return 1;

	std::fwrite(&header, sizeof(CookedAnimHeader), 1, out);
	if (!tracks.empty())
		std::fwrite(tracks.data(), sizeof(CookedAnimTrack), tracks.size(), out);
	if (!payloadData.empty())
		std::fwrite(payloadData.data(), 1, payloadData.size(), out);
	std::fclose(out);
	return 0;
}

int PackArchive(int argc, char** argv) {
	std::string outPath, manifestPath;
	for (int i = 0; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "-o" && i + 1 < argc)
			outPath = argv[++i];
		else if (arg == "-i" && i + 1 < argc)
			manifestPath = argv[++i];
	}
	if (outPath.empty() || manifestPath.empty())
		return 1;

	fs::create_directories(fs::path(outPath).parent_path());
	std::ifstream ifs(manifestPath);
	if (!ifs.is_open())
		return 1;

	struct ManifestEntry {
		std::string vpath, rpath;
		std::vector<char> data;
		size_t size = 0, alignedOffset = 0;
		int errorCode = 0;
		bool success = false;
	};

	std::vector<ManifestEntry> manifestEntries;
	std::string line;
	while (std::getline(ifs, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		auto pos = line.find('=');
		if (pos == std::string::npos)
			continue;
		manifestEntries.push_back({line.substr(0, pos), line.substr(pos + 1)});
	}
	ifs.close();

	const auto totalFiles = static_cast<uint32_t>(manifestEntries.size());
	if (!manifestEntries.empty()) {
		std::atomic<uint32_t> loadedCount{0};
		ZHLN::Mutex printMutex{};

		ZHLN::TaskSystem::ParallelFor(totalFiles, 1, [&](uint32_t start, uint32_t end, uint32_t) {
			for (uint32_t i = start; i < end; ++i) {
				auto& entry = manifestEntries[i];
				FILE* f = std::fopen(entry.rpath.c_str(), "rb");
				if (f == nullptr) {
					entry.errorCode = errno;
					entry.success = false;
				} else {
					std::fseek(f, 0, SEEK_END);
					long size = std::ftell(f);
					std::fseek(f, 0, SEEK_SET);
					entry.data.resize(size);
					if (size > 0)
						entry.size = std::fread(entry.data.data(), 1, size, f);
					entry.success = true;
					std::fclose(f);
				}
				const uint32_t current = loadedCount.fetch_add(1, std::memory_order_relaxed) + 1;
				if (current % 10 == 0 || current == totalFiles) {
					std::lock_guard<ZHLN::Mutex> lock(printMutex);
					std::print("\r[zcook] Loading assets: {}/{} ({:.1f}%)", current, totalFiles,
							   (static_cast<float>(current) / totalFiles) * 100.0f);
					std::fflush(stdout);
				}
			}
		});
		std::println("");
	}

	uint64_t totalBytesToWrite = sizeof(PakHeader);
	uint64_t successfulCount = 0;
	for (const auto& entry : manifestEntries) {
		if (entry.success) {
			size_t padding = (16 - (totalBytesToWrite % 16)) % 16;
			totalBytesToWrite += padding + entry.size;
			successfulCount++;
		}
	}
	totalBytesToWrite += successfulCount * sizeof(PakEntry);

	FILE* out = std::fopen(outPath.c_str(), "wb");
	if (!out)
		return 1;

	std::vector<char> streamBuffer(1024 * 1024);
	std::setvbuf(out, streamBuffer.data(), _IOFBF, streamBuffer.size());

	uint64_t totalBytesWritten = 0;
	auto writeAndTrack = [&](const void* ptr, size_t size) {
		if (size == 0)
			return;
		std::fwrite(ptr, 1, size, out);
		totalBytesWritten += size;
	};

	PakHeader dummyHeader{};
	writeAndTrack(&dummyHeader, sizeof(PakHeader));

	std::vector<PakEntry> entries;
	uint64_t currentPayloadSize = 0;

	auto lastProgressTime = std::chrono::steady_clock::now();
	auto startWriteTime = lastProgressTime;

	auto updateProgress = [&](bool force) {
		auto now = std::chrono::steady_clock::now();
		if (force ||
			std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgressTime).count() >=
				100) {
			double writtenMB = totalBytesWritten / 1048576.0;
			double totalMB = totalBytesToWrite / 1048576.0;
			double percentage =
				totalBytesToWrite > 0 ? (totalBytesWritten * 100.0 / totalBytesToWrite) : 100.0;
			double speedMBs =
				(std::chrono::duration_cast<std::chrono::milliseconds>(now - startWriteTime)
					 .count() > 0)
					? (writtenMB /
					   std::chrono::duration_cast<std::chrono::milliseconds>(now - startWriteTime)
						   .count() *
					   1000.0)
					: 0.0;
			std::print("\r[zcook] Writing archive: {:.2f} / {:.2f} MB ({:.1f}%) | {:.1f} MB/s",
					   writtenMB, totalMB, percentage, speedMBs);
			std::fflush(stdout);
			lastProgressTime = now;
		}
	};

	for (auto& entry : manifestEntries) {
		if (!entry.success)
			continue;
		size_t padding = (16 - (currentPayloadSize % 16)) % 16;
		if (padding > 0) {
			static const char zeroPadding[16] = {0};
			writeAndTrack(zeroPadding, padding);
			currentPayloadSize += padding;
		}

		PakEntry pakEntry{};
		pakEntry.pathHash = HashAssetPath(entry.vpath);
		pakEntry.offset = sizeof(PakHeader) + currentPayloadSize;
		pakEntry.compressedSize = entry.size;
		pakEntry.uncompressedSize = entry.size;

		entries.push_back(pakEntry);
		if (entry.size > 0) {
			writeAndTrack(entry.data.data(), entry.size);
			currentPayloadSize += entry.size;
		}
		updateProgress(false);
	}

	uint64_t tocOffset = sizeof(PakHeader) + currentPayloadSize;
	if (!entries.empty())
		writeAndTrack(entries.data(), entries.size() * sizeof(PakEntry));

	PakHeader header{};
	std::memcpy(header.magic, "ZPAK", 4);
	header.version = 1;
	header.entryCount = entries.size();
	header.tocOffset = tocOffset;

	std::fseek(out, 0, SEEK_SET);
	std::fwrite(&header, 1, sizeof(PakHeader), out);

	totalBytesWritten = totalBytesToWrite;
	updateProgress(true);
	std::println("");
	std::fclose(out);

	std::println("[zcook] Successfully packed {}/{} assets into '{}'", entries.size(), totalFiles,
				 outPath);
	return 0;
}

int CookGLB(int argc, char** argv) {
	std::string metaPath, outPath;
	for (int i = 0; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--meta" && i + 1 < argc)
			metaPath = argv[++i];
		else if (arg == "-o" && i + 1 < argc)
			outPath = argv[++i];
	}
	if (metaPath.empty() || outPath.empty())
		return 1;

	std::string binMetaPath = fs::path(metaPath).replace_extension(".bin").string();
	Compiler::BinaryReader reader(binMetaPath);
	Compiler::IRManifest manifest = reader.Parse();

	std::string levelFolder = fs::path(metaPath).parent_path().string();
	if (!GLB::EmitGLB(manifest, levelFolder, outPath))
		return 1;
	return 0;
}

} // namespace ZHLN
