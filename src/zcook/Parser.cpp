// src/zcook/Parser.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Parser.hpp"

#include <charconv>
#include <cstdlib>
#include <print>

namespace {
std::string DecodeJSONString(std::string_view raw) {
	std::string result;
	result.reserve(raw.size());
	size_t i = 0;
	while (i < raw.size()) {
		if (raw[i] == '\\' && i + 1 < raw.size()) {
			char next = raw[i + 1];
			if (next == 'u' && i + 5 < raw.size()) {
				std::string hexStr(raw.substr(i + 2, 4));
				uint32_t codepoint = 0;
				auto [ptr, ec] =
					std::from_chars(hexStr.data(), hexStr.data() + hexStr.size(), codepoint, 16);
				if (ec == std::errc{}) {
					if (codepoint <= 0x7F) {
						result.push_back(static_cast<char>(codepoint));
					} else if (codepoint <= 0x7FF) {
						result.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
						result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
					} else if (codepoint <= 0xFFFF) {
						result.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
						result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
						result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
					}
					i += 6;
					continue;
				}
			}
			switch (next) {
				case '"':
					result.push_back('"');
					break;
				case '\\':
					result.push_back('\\');
					break;
				case '/':
					result.push_back('/');
					break;
				case 'b':
					result.push_back('\b');
					break;
				case 'f':
					result.push_back('\f');
					break;
				case 'n':
					result.push_back('\n');
					break;
				case 'r':
					result.push_back('\r');
					break;
				case 't':
					result.push_back('\t');
					break;
				default:
					result.push_back(next);
					break;
			}
			i += 2;
		} else {
			result.push_back(raw[i]);
			i++;
		}
	}
	return result;
}

void PrintParserErrorContext(std::string_view source, size_t targetLine) {
	std::println(stderr, "[zcook] Error Context around line {}:", targetLine);
	size_t currentLine = 1;
	size_t pos = 0;
	while (pos < source.size()) {
		size_t next_nl = source.find('\n', pos);
		std::string_view lineStr = (next_nl == std::string_view::npos)
									   ? source.substr(pos)
									   : source.substr(pos, next_nl - pos);

		if (currentLine >= (targetLine > 3 ? targetLine - 3 : 1) && currentLine <= targetLine + 3) {
			const char* marker = (currentLine == targetLine) ? ">>> " : "    ";
			std::println(stderr, "{}{:4}: {}", marker, currentLine, lineStr);
		}
		if (next_nl == std::string_view::npos)
			break;
		pos = next_nl + 1;
		currentLine++;
	}
}
} // namespace

namespace Compiler {

IRManifest Parser::Parse() {
	IRManifest manifest;
	Expect(TokenType::BeginObject);
	while (!Match(TokenType::EndObject)) {
		Token key = Expect(TokenType::String);
		Expect(TokenType::Colon);
		if (key.value == "scene_info")
			ParseSceneInfo(manifest);
		else if (key.value == "meshes")
			ParseMeshes(manifest);
		else if (key.value == "nodes")
			ParseNodes(manifest);
		else if (key.value == "lights")
			ParseLights(manifest);
		else if (key.value == "materials")
			ParseMaterials(manifest);
		else if (key.value == "animations")
			ParseAnimations(manifest);
		else if (key.value == "skins")
			ParseSkins(manifest);
		else
			SkipValue();

		if (!Peek(TokenType::EndObject))
			Expect(TokenType::Comma);
	}
	return manifest;
}

Token Parser::Peek() const {
	return m_cursor >= m_tokens.size() ? Token{.type = TokenType::EndOfFile, .value = {}}
									   : m_tokens[m_cursor];
}
bool Parser::Peek(TokenType type) const {
	return Peek().type == type;
}
Token Parser::Advance() {
	return m_cursor >= m_tokens.size() ? Token{.type = TokenType::EndOfFile, .value = {}}
									   : m_tokens[m_cursor++];
}
bool Parser::Match(TokenType type) {
	if (Peek(type)) {
		Advance();
		return true;
	}
	return false;
}
Token Parser::Expect(TokenType type) {
	Token tok = Advance();
	if (tok.type != type) {
		std::println(
			stderr,
			"\n[zcook Parser Error] Expected token type {} ({}), but found '{}' ({}) on line {}",
			static_cast<int>(type), GetTokenTypeName(type), tok.value, static_cast<int>(tok.type),
			tok.line);
		PrintParserErrorContext(m_source, tok.line);
		std::exit(1);
	}
	return tok;
}

void Parser::ParseSceneInfo(IRManifest& manifest) {
	Expect(TokenType::BeginObject);
	while (!Match(TokenType::EndObject)) {
		Token key = Expect(TokenType::String);
		Expect(TokenType::Colon);
		if (key.value == "name")
			manifest.levelName = DecodeJSONString(Expect(TokenType::String).value);
		else
			SkipValue();
		if (!Peek(TokenType::EndObject))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseMeshes(IRManifest& manifest) {
	Expect(TokenType::BeginArray);
	while (!Match(TokenType::EndArray)) {
		IRMesh mesh;
		Expect(TokenType::BeginObject);
		while (!matchMeshField(mesh)) {
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
		manifest.meshes.push_back(mesh);
		if (!Peek(TokenType::EndArray))
			Expect(TokenType::Comma);
	}
}

bool Parser::matchMeshField(IRMesh& mesh) {
	if (Match(TokenType::EndObject))
		return true;
	Token key = Expect(TokenType::String);
	Expect(TokenType::Colon);
	if (key.value == "id")
		mesh.id = DecodeJSONString(Expect(TokenType::String).value);
	else if (key.value == "layout")
		mesh.layout = DecodeJSONString(Expect(TokenType::String).value);
	else if (key.value == "buffers")
		ParseBuffers(mesh);
	else if (key.value == "primitives")
		ParsePrimitives(mesh);
	else if (key.value == "morph_targets")
		ParseMorphTargets(mesh);
	else
		SkipValue();
	return false;
}

void Parser::ParseBuffers(IRMesh& mesh) {
	Expect(TokenType::BeginObject);
	while (!Match(TokenType::EndObject)) {
		Token key = Expect(TokenType::String);
		Expect(TokenType::Colon);
		if (key.value == "bin_file")
			mesh.binFile = DecodeJSONString(Expect(TokenType::String).value);
		else if (key.value == "vertex_buffer")
			ParseBufferBound(mesh.vertexBuffer);
		else
			SkipValue();
		if (!Peek(TokenType::EndObject))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseBufferBound(IRBuffer& buf) {
	Expect(TokenType::BeginObject);
	while (!Match(TokenType::EndObject)) {
		Token key = Expect(TokenType::String);
		Expect(TokenType::Colon);
		if (key.value == "byte_offset")
			buf.byteOffset = std::stoul(std::string(Expect(TokenType::Number).value));
		else if (key.value == "byte_length")
			buf.byteLength = std::stoul(std::string(Expect(TokenType::Number).value));
		else
			SkipValue();
		if (!Peek(TokenType::EndObject))
			Expect(TokenType::Comma);
	}
}

void Parser::ParsePrimitives(IRMesh& mesh) {
	Expect(TokenType::BeginArray);
	while (!Match(TokenType::EndArray)) {
		IRPrimitive prim;
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "material_id") {
				Token val = Advance();
				if (val.type == TokenType::String)
					prim.materialId = DecodeJSONString(val.value);
			} else if (key.value == "vertex_offset") {
				prim.vertexOffset = std::stoul(std::string(Expect(TokenType::Number).value));
			} else if (key.value == "vertex_count") {
				prim.vertexCount = std::stoul(std::string(Expect(TokenType::Number).value));
			} else {
				SkipValue();
			}
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
		mesh.primitives.push_back(prim);
		if (!Peek(TokenType::EndArray))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseNodes(IRManifest& manifest) {
	Expect(TokenType::BeginArray);
	while (!Match(TokenType::EndArray)) {
		IRNode node;
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "id") {
				node.id = DecodeJSONString(Expect(TokenType::String).value);
			} else if (key.value == "parent_id") {
				Token val = Advance();
				if (val.type == TokenType::String)
					node.parentId = DecodeJSONString(val.value);
			} else if (key.value == "visible") {
				node.visible = (Advance().type == TokenType::True);
			} else if (key.value == "transform") {
				ParseTransform(node);
			} else if (key.value == "refs") {
				ParseRefs(node);
			} else {
				SkipValue();
			}
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
		manifest.nodes.push_back(node);
		if (!Peek(TokenType::EndArray))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseTransform(IRNode& node) {
	Expect(TokenType::BeginObject);
	while (!Match(TokenType::EndObject)) {
		Token key = Expect(TokenType::String);
		Expect(TokenType::Colon);
		if (key.value == "local") {
			Expect(TokenType::BeginArray);
			for (int i = 0; i < 16; ++i) {
				node.localMatrix[i] = std::stof(std::string(Expect(TokenType::Number).value));
				if (i < 15)
					Expect(TokenType::Comma);
			}
			Expect(TokenType::EndArray);
		} else if (key.value == "world") {
			Expect(TokenType::BeginArray);
			for (int i = 0; i < 16; ++i) {
				node.worldMatrix[i] = std::stof(std::string(Expect(TokenType::Number).value));
				if (i < 15)
					Expect(TokenType::Comma);
			}
			Expect(TokenType::EndArray);
		} else {
			SkipValue();
		}
		if (!Peek(TokenType::EndObject))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseRefs(IRNode& node) {
	Expect(TokenType::BeginObject);
	while (!Match(TokenType::EndObject)) {
		Token key = Expect(TokenType::String);
		Expect(TokenType::Colon);
		if (key.value == "mesh_id") {
			Token val = Advance();
			if (val.type == TokenType::String)
				node.meshId = DecodeJSONString(val.value);
		} else if (key.value == "light_id") {
			Token val = Advance();
			if (val.type == TokenType::String)
				node.lightId = DecodeJSONString(val.value);
		} else if (key.value == "skin_id") {
			Token val = Advance();
			if (val.type == TokenType::String)
				node.skinId = DecodeJSONString(val.value);
		} else {
			SkipValue();
		}
		if (!Peek(TokenType::EndObject))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseLights(IRManifest& manifest) {
	Expect(TokenType::BeginArray);
	while (!Match(TokenType::EndArray)) {
		IRLight l;
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "id")
				l.id = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "type")
				l.type = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "intensity")
				l.intensity = std::stof(std::string(Expect(TokenType::Number).value));
			else if (key.value == "color") {
				Expect(TokenType::BeginArray);
				for (int i = 0; i < 3; ++i) {
					l.color[i] = std::stof(std::string(Expect(TokenType::Number).value));
					if (i < 2)
						Expect(TokenType::Comma);
				}
				Expect(TokenType::EndArray);
			} else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
		manifest.lights.push_back(l);
		if (!Peek(TokenType::EndArray))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseMaterials(IRManifest& manifest) {
	Expect(TokenType::BeginArray);
	while (!Match(TokenType::EndArray)) {
		IRMaterial mat;
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "id")
				mat.id = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "double_sided")
				mat.doubleSided = (Advance().type == TokenType::True);
			else if (key.value == "pbr")
				ParsePBR(mat);
			else if (key.value == "maps")
				ParseMaps(mat);
			else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
		manifest.materials.push_back(mat);
		if (!Peek(TokenType::EndArray))
			Expect(TokenType::Comma);
	}
}

void Parser::ParsePBR(IRMaterial& mat) {
	Expect(TokenType::BeginObject);
	while (!Match(TokenType::EndObject)) {
		Token key = Expect(TokenType::String);
		Expect(TokenType::Colon);
		if (key.value == "base_color") {
			Expect(TokenType::BeginArray);
			for (int i = 0; i < 4; ++i) {
				mat.baseColor[i] = std::stof(std::string(Expect(TokenType::Number).value));
				if (i < 3)
					Expect(TokenType::Comma);
			}
			Expect(TokenType::EndArray);
		} else if (key.value == "metallic")
			mat.metallic = std::stof(std::string(Expect(TokenType::Number).value));
		else if (key.value == "roughness")
			mat.roughness = std::stof(std::string(Expect(TokenType::Number).value));
		else if (key.value == "emissive_factor") {
			Expect(TokenType::BeginArray);
			for (int i = 0; i < 3; ++i) {
				mat.emissiveFactor[i] = std::stof(std::string(Expect(TokenType::Number).value));
				if (i < 2)
					Expect(TokenType::Comma);
			}
			Expect(TokenType::EndArray);
		} else if (key.value == "emissive_strength")
			mat.emissiveStrength = std::stof(std::string(Expect(TokenType::Number).value));
		else
			SkipValue();
		if (!Peek(TokenType::EndObject))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseMaps(IRMaterial& mat) {
	Expect(TokenType::BeginObject);
	while (!Match(TokenType::EndObject)) {
		Token key = Expect(TokenType::String);
		Expect(TokenType::Colon);
		Token val = Advance();
		std::string fileStr = (val.type == TokenType::String) ? DecodeJSONString(val.value) : "";
		if (key.value == "albedo" && !fileStr.empty())
			mat.albedoMap = fileStr;
		else if (key.value == "normal" && !fileStr.empty())
			mat.normalMap = fileStr;
		else if (key.value == "metallic_roughness" && !fileStr.empty())
			mat.metallicRoughnessMap = fileStr;
		else if (key.value == "emissive" && !fileStr.empty())
			mat.emissiveMap = fileStr;
		if (!Peek(TokenType::EndObject))
			Expect(TokenType::Comma);
	}
}

void Parser::SkipValue() {
	Token tok = Advance();
	if (tok.type == TokenType::BeginObject) {
		int depth = 1;
		while (depth > 0) {
			Token t = Advance();
			if (t.type == TokenType::BeginObject)
				depth++;
			else if (t.type == TokenType::EndObject)
				depth--;
		}
	} else if (tok.type == TokenType::BeginArray) {
		int depth = 1;
		while (depth > 0) {
			Token t = Advance();
			if (t.type == TokenType::BeginArray)
				depth++;
			else if (t.type == TokenType::EndArray)
				depth--;
		}
	}
}

void Parser::ParseAnimations(IRManifest& manifest) {
	Expect(TokenType::BeginArray);
	while (!Match(TokenType::EndArray)) {
		IRAnimation anim;
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "id")
				anim.id = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "name")
				anim.name = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "duration")
				anim.duration = std::stof(std::string(Expect(TokenType::Number).value));
			else if (key.value == "loop")
				anim.loop = (Advance().type == TokenType::True);
			else if (key.value == "channels")
				ParseAnimChannels(anim);
			else if (key.value == "samplers")
				ParseAnimSamplers(anim);
			else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
		manifest.animations.push_back(anim);
		if (!Peek(TokenType::EndArray))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseAnimChannels(IRAnimation& anim) {
	Expect(TokenType::BeginArray);
	while (!Match(TokenType::EndArray)) {
		IRAnimationChannel chan;
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "target_node_id")
				chan.targetNodeId = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "target_path")
				chan.targetPath = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "sampler_id")
				chan.samplerId = std::stoul(std::string(Expect(TokenType::Number).value));
			else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
		anim.channels.push_back(chan);
		if (!Peek(TokenType::EndArray))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseAnimSamplers(IRAnimation& anim) {
	Expect(TokenType::BeginArray);
	while (!Match(TokenType::EndArray)) {
		IRAnimationSampler samp;
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "interpolation")
				samp.interpolation = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "input_offset")
				samp.inputOffset = std::stoul(std::string(Expect(TokenType::Number).value));
			else if (key.value == "input_length")
				samp.inputLength = std::stoul(std::string(Expect(TokenType::Number).value));
			else if (key.value == "output_offset")
				samp.outputOffset = std::stoul(std::string(Expect(TokenType::Number).value));
			else if (key.value == "output_length")
				samp.outputLength = std::stoul(std::string(Expect(TokenType::Number).value));
			else if (key.value == "bin_file")
				samp.binFile = DecodeJSONString(Expect(TokenType::String).value);
			else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
		anim.samplers.push_back(samp);
		if (!Peek(TokenType::EndArray))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseSkins(IRManifest& manifest) {
	Expect(TokenType::BeginArray);
	while (!Match(TokenType::EndArray)) {
		IRSkin skin;
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "id")
				skin.id = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "name")
				skin.name = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "joints") {
				Expect(TokenType::BeginArray);
				while (!Match(TokenType::EndArray)) {
					skin.joints.push_back(DecodeJSONString(Expect(TokenType::String).value));
					if (!Peek(TokenType::EndArray))
						Expect(TokenType::Comma);
				}
			} else if (key.value == "parents") {
				Expect(TokenType::BeginArray);
				while (!Match(TokenType::EndArray)) {
					skin.parents.push_back(DecodeJSONString(Expect(TokenType::String).value));
					if (!Peek(TokenType::EndArray))
						Expect(TokenType::Comma);
				}
			} else if (key.value == "inverse_bind_matrices") {
				Expect(TokenType::BeginArray);
				while (!Match(TokenType::EndArray)) {
					skin.inverseBindMatrices.push_back(
						std::stof(std::string(Expect(TokenType::Number).value)));
					if (!Peek(TokenType::EndArray))
						Expect(TokenType::Comma);
				}
			} else if (key.value == "rest_pose") {
				Expect(TokenType::BeginArray);
				while (!Match(TokenType::EndArray)) {
					skin.restPose.push_back(
						std::stof(std::string(Expect(TokenType::Number).value)));
					if (!Peek(TokenType::EndArray))
						Expect(TokenType::Comma);
				}
			} else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
		manifest.skins.push_back(skin);
		if (!Peek(TokenType::EndArray))
			Expect(TokenType::Comma);
	}
}

void Parser::ParseMorphTargets(IRMesh& mesh) {
	Expect(TokenType::BeginArray);
	while (!Match(TokenType::EndArray)) {
		IRMorphTarget target;
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "name")
				target.name = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "bin_file")
				target.binFile = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "byte_offset")
				target.byteOffset = std::stoul(std::string(Expect(TokenType::Number).value));
			else if (key.value == "byte_length")
				target.byteLength = std::stoul(std::string(Expect(TokenType::Number).value));
			else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
		mesh.morphTargets.push_back(target);
		if (!Peek(TokenType::EndArray))
			Expect(TokenType::Comma);
	}
}

} // namespace Compiler
