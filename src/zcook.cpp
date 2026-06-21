// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/zcook.cpp
#include "threading/TaskSystem.hpp"
#include "threading/Thread.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Types.hpp>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace ZHLN {

// locale-safe checks to prevent glibc lookup table crashes
inline bool IsDigit(char c) noexcept {
	return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

inline bool IsAlpha(char c) noexcept {
	return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

// Decodes standard JSON Unicode escape sequences (\uXXXX) and control characters to UTF-8
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
					i += 6; // skip \uXXXX
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
			i += 2; // skip escape char and modifier
		} else {
			result.push_back(raw[i]);
			i++;
		}
	}
	return result;
}

// ============================================================================
// Phase 1: High-Performance Lexical Analyzer (Lexer)
// ============================================================================
namespace Compiler {

enum class TokenType : uint8_t {
	BeginObject,
	EndObject,
	BeginArray,
	EndArray,
	Colon,
	Comma,
	String,
	Number,
	True,
	False,
	Null,
	EndOfFile
};

struct Token {
	TokenType type;
	std::string_view value;
	size_t line = 1;
};

inline const char* GetTokenTypeName(TokenType type) {
	switch (type) {
		case TokenType::BeginObject:
			return "BeginObject ('{')";
		case TokenType::EndObject:
			return "EndObject ('}')";
		case TokenType::BeginArray:
			return "BeginArray ('[')";
		case TokenType::EndArray:
			return "EndArray (']')";
		case TokenType::Colon:
			return "Colon (':')";
		case TokenType::Comma:
			return "Comma (',')";
		case TokenType::String:
			return "String";
		case TokenType::Number:
			return "Number";
		case TokenType::True:
			return "True";
		case TokenType::False:
			return "False";
		case TokenType::Null:
			return "Null";
		case TokenType::EndOfFile:
			return "EndOfFile";
		default:
			return "Unknown";
	}
}

class Lexer {
  public:
	explicit Lexer(std::string_view source) : m_source(source) {}

	std::vector<Token> Tokenize() {
		std::vector<Token> tokens;
		while (true) {
			Token tok = NextToken();
			tokens.push_back(tok);
			if (tok.type == TokenType::EndOfFile) {
				break;
			}
		}
		return tokens;
	}

  private:
	std::string_view m_source;
	size_t m_cursor = 0;
	size_t m_line = 1;

	[[nodiscard]] char Peek() const {
		return m_cursor >= m_source.length() ? '\0' : m_source[m_cursor];
	}
	char Advance() {
		if (m_cursor >= m_source.length()) {
			return '\0';
		}
		char c = m_source[m_cursor++];
		if (c == '\n') {
			m_line++;
		}
		return c;
	}

	void SkipWhitespace() {
		while (true) {
			char c = Peek();
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
				Advance();
			} else if (c == '/' && m_cursor + 1 < m_source.length() &&
					   m_source[m_cursor + 1] == '/') {
				while (Peek() != '\n' && Peek() != '\0') {
					Advance();
				}
			} else {
				break;
			}
		}
	}

	Token NextToken() {
		SkipWhitespace();
		size_t startLine = m_line;
		char c = Peek();

		if (c == '\0') {
			return Token{.type = TokenType::EndOfFile, .value = {}, .line = startLine};
		}

		switch (c) {
			case '{':
				Advance();
				return Token{.type = TokenType::BeginObject,
							 .value = m_source.substr(m_cursor - 1, 1),
							 .line = startLine};
			case '}':
				Advance();
				return Token{.type = TokenType::EndObject,
							 .value = m_source.substr(m_cursor - 1, 1),
							 .line = startLine};
			case '[':
				Advance();
				return Token{.type = TokenType::BeginArray,
							 .value = m_source.substr(m_cursor - 1, 1),
							 .line = startLine};
			case ']':
				Advance();
				return Token{.type = TokenType::EndArray,
							 .value = m_source.substr(m_cursor - 1, 1),
							 .line = startLine};
			case ':':
				Advance();
				return Token{.type = TokenType::Colon,
							 .value = m_source.substr(m_cursor - 1, 1),
							 .line = startLine};
			case ',':
				Advance();
				return Token{.type = TokenType::Comma,
							 .value = m_source.substr(m_cursor - 1, 1),
							 .line = startLine};
			case '"':
				return ParseString();
		}

		if (IsDigit(c) || c == '-') {
			return ParseNumber();
		}
		if (IsAlpha(c)) {
			return ParseKeyword();
		}

		Advance();
		return Token{
			.type = TokenType::Null, .value = m_source.substr(m_cursor - 1, 1), .line = startLine};
	}

	Token ParseString() {
		size_t startLine = m_line;
		Advance();
		size_t startIdx = m_cursor;
		while (Peek() != '"' && Peek() != '\0') {
			if (Peek() == '\\') {
				Advance();
			}
			Advance();
		}
		size_t len = m_cursor - startIdx;
		Advance();
		return Token{
			.type = TokenType::String, .value = m_source.substr(startIdx, len), .line = startLine};
	}

	Token ParseNumber() {
		size_t startLine = m_line;
		size_t startIdx = m_cursor;
		if (Peek() == '-') {
			Advance();
		}
		while (IsDigit(Peek())) {
			Advance();
		}
		if (Peek() == '.') {
			Advance();
			while (IsDigit(Peek())) {
				Advance();
			}
		}
		return Token{.type = TokenType::Number,
					 .value = m_source.substr(startIdx, m_cursor - startIdx),
					 .line = startLine};
	}

	Token ParseKeyword() {
		size_t startLine = m_line;
		size_t startIdx = m_cursor;
		while (IsAlpha(Peek())) {
			Advance();
		}
		std::string_view word = m_source.substr(startIdx, m_cursor - startIdx);
		if (word == "true") {
			return Token{.type = TokenType::True, .value = word, .line = startLine};
		}
		if (word == "false") {
			return Token{.type = TokenType::False, .value = word, .line = startLine};
		}
		return Token{.type = TokenType::Null, .value = word, .line = startLine};
	}
};

// ============================================================================
// Phase 2: Schema-Directed Recursive Descent Parser
// ============================================================================
struct IRBuffer {
	uint32_t byteOffset = 0;
	uint32_t byteLength = 0;
};
struct IRPrimitive {
	std::string materialId;
	uint32_t vertexOffset;
	uint32_t vertexCount;
};
struct IRMesh {
	std::string id, layout, binFile;
	IRBuffer vertexBuffer;
	std::vector<IRPrimitive> primitives;
};
struct IRNode {
	std::string id, meshId, lightId;
	float localMatrix[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
							 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
	float worldMatrix[16] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
							 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
	bool visible = true;
};
struct IRLight {
	std::string id, type;
	float color[3] = {1.f, 1.f, 1.f};
	float intensity = 1.0f;
};
struct IRMaterial {
	std::string id, albedoMap, normalMap, metallicRoughnessMap, emissiveMap;
	float baseColor[4] = {1.f, 1.f, 1.f, 1.f};
	float metallic = 0.f, roughness = 0.5f;
	float emissiveFactor[3] = {0.f, 0.f, 0.f};
	float emissiveStrength = 1.0f;
	bool doubleSided = false;
};
struct IRManifest {
	std::string levelName;
	std::vector<IRMesh> meshes;
	std::vector<IRNode> nodes;
	std::vector<IRLight> lights;
	std::vector<IRMaterial> materials;
};

inline void PrintParserErrorContext(std::string_view source, size_t targetLine) {
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

		if (next_nl == std::string_view::npos) {
			break;
		}
		pos = next_nl + 1;
		currentLine++;
	}
}

class Parser {
  public:
	explicit Parser(const std::vector<Token>& tokens, std::string_view source)
		: m_tokens(tokens), m_source(source) {}

	IRManifest Parse() {
		IRManifest manifest;
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "scene_info") {
				ParseSceneInfo(manifest);
			} else if (key.value == "meshes") {
				ParseMeshes(manifest);
			} else if (key.value == "nodes") {
				ParseNodes(manifest);
			} else if (key.value == "lights") {
				ParseLights(manifest);
			} else if (key.value == "materials") {
				ParseMaterials(manifest);
			} else {
				SkipValue();
			}
			if (!Peek(TokenType::EndObject)) {
				Expect(TokenType::Comma);
			}
		}
		return manifest;
	}

  private:
	const std::vector<Token>& m_tokens;
	size_t m_cursor = 0;
	std::string_view m_source;

	[[nodiscard]] Token Peek() const {
		return m_cursor >= m_tokens.size() ? Token{.type = TokenType::EndOfFile, .value = {}}
										   : m_tokens[m_cursor];
	}
	[[nodiscard]] bool Peek(TokenType type) const { return Peek().type == type; }
	Token Advance() {
		return m_cursor >= m_tokens.size() ? Token{.type = TokenType::EndOfFile, .value = {}}
										   : m_tokens[m_cursor++];
	}
	bool Match(TokenType type) {
		if (Peek(type)) {
			Advance();
			return true;
		}
		return false;
	}
	Token Expect(TokenType type) {
		Token tok = Advance();
		if (tok.type != type) {
			std::println(stderr,
						 "\n[zcook Parser Error] Expected token type {} ({}), but found '{}' ({}) "
						 "on line {}",
						 static_cast<int>(type), GetTokenTypeName(type), tok.value,
						 static_cast<int>(tok.type), tok.line);
			PrintParserErrorContext(m_source, tok.line);
			std::exit(1);
		}
		return tok;
	}

	void ParseSceneInfo(IRManifest& manifest) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "name") {
				manifest.levelName = DecodeJSONString(Expect(TokenType::String).value);
			} else {
				SkipValue();
			}
			if (!Peek(TokenType::EndObject)) {
				Expect(TokenType::Comma);
			}
		}
	}

	void ParseMeshes(IRManifest& manifest) {
		Expect(TokenType::BeginArray);
		while (!Match(TokenType::EndArray)) {
			IRMesh mesh;
			Expect(TokenType::BeginObject);
			while (!matchMeshField(mesh)) {
				if (!Peek(TokenType::EndObject)) {
					Expect(TokenType::Comma);
				}
			}
			manifest.meshes.push_back(mesh);
			if (!Peek(TokenType::EndArray)) {
				Expect(TokenType::Comma);
			}
		}
	}

	bool matchMeshField(IRMesh& mesh) {
		if (Match(TokenType::EndObject)) {
			return true;
		}
		Token key = Expect(TokenType::String);
		Expect(TokenType::Colon);
		if (key.value == "id") {
			mesh.id = DecodeJSONString(Expect(TokenType::String).value);
		} else if (key.value == "layout") {
			mesh.layout = DecodeJSONString(Expect(TokenType::String).value);
		} else if (key.value == "buffers") {
			ParseBuffers(mesh);
		} else if (key.value == "primitives") {
			ParsePrimitives(mesh);
		} else {
			SkipValue();
		}
		return false;
	}

	void ParseBuffers(IRMesh& mesh) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "bin_file") {
				mesh.binFile = DecodeJSONString(Expect(TokenType::String).value);
			} else if (key.value == "vertex_buffer") {
				ParseBufferBound(mesh.vertexBuffer);
			} else {
				SkipValue();
			}
			if (!Peek(TokenType::EndObject)) {
				Expect(TokenType::Comma);
			}
		}
	}

	void ParseBufferBound(IRBuffer& buf) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "byte_offset") {
				buf.byteOffset = std::stoul(std::string(Expect(TokenType::Number).value));
			} else if (key.value == "byte_length") {
				buf.byteLength = std::stoul(std::string(Expect(TokenType::Number).value));
			} else {
				SkipValue();
			}
			if (!Peek(TokenType::EndObject)) {
				Expect(TokenType::Comma);
			}
		}
	}

	void ParsePrimitives(IRMesh& mesh) {
		Expect(TokenType::BeginArray);
		while (!Match(TokenType::EndArray)) {
			IRPrimitive prim;
			Expect(TokenType::BeginObject);
			while (!Match(TokenType::EndObject)) {
				Token key = Expect(TokenType::String);
				Expect(TokenType::Colon);
				if (key.value == "material_id") {
					Token val = Advance();
					if (val.type == TokenType::String) {
						prim.materialId = DecodeJSONString(val.value);
					}
				} else if (key.value == "vertex_offset") {
					prim.vertexOffset = std::stoul(std::string(Expect(TokenType::Number).value));
				} else if (key.value == "vertex_count") {
					prim.vertexCount = std::stoul(std::string(Expect(TokenType::Number).value));
				} else {
					SkipValue();
				}
				if (!Peek(TokenType::EndObject)) {
					Expect(TokenType::Comma);
				}
			}
			mesh.primitives.push_back(prim);
			if (!Peek(TokenType::EndArray)) {
				Expect(TokenType::Comma);
			}
		}
	}

	void ParseNodes(IRManifest& manifest) {
		Expect(TokenType::BeginArray);
		while (!Match(TokenType::EndArray)) {
			IRNode node;

			Expect(TokenType::BeginObject);
			while (!Match(TokenType::EndObject)) {
				Token key = Expect(TokenType::String);
				Expect(TokenType::Colon);
				if (key.value == "id") {
					node.id = DecodeJSONString(Expect(TokenType::String).value);
				} else if (key.value == "visible") {
					node.visible = (Advance().type == TokenType::True);
				} else if (key.value == "transform") {
					ParseTransform(node);
				} else if (key.value == "refs") {
					ParseRefs(node);
				} else {
					SkipValue();
				}
				if (!Peek(TokenType::EndObject)) {
					Expect(TokenType::Comma);
				}
			}
			manifest.nodes.push_back(node);
			if (!Peek(TokenType::EndArray)) {
				Expect(TokenType::Comma);
			}
		}
	}

	void ParseTransform(IRNode& node) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "local") {
				Expect(TokenType::BeginArray);
				for (int i = 0; i < 16; ++i) {
					node.localMatrix[i] = std::stof(std::string(Expect(TokenType::Number).value));
					if (i < 15) {
						Expect(TokenType::Comma);
					}
				}
				Expect(TokenType::EndArray);
			} else if (key.value == "world") {
				Expect(TokenType::BeginArray);
				for (int i = 0; i < 16; ++i) {
					node.worldMatrix[i] = std::stof(std::string(Expect(TokenType::Number).value));
					if (i < 15) {
						Expect(TokenType::Comma);
					}
				}
				Expect(TokenType::EndArray);
			} else {
				SkipValue();
			}
			if (!Peek(TokenType::EndObject)) {
				Expect(TokenType::Comma);
			}
		}
	}

	void ParseRefs(IRNode& node) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "mesh_id") {
				Token val = Advance();
				if (val.type == TokenType::String) {
					node.meshId = DecodeJSONString(val.value);
				}
			} else if (key.value == "light_id") {
				Token val = Advance();
				if (val.type == TokenType::String) {
					node.lightId = DecodeJSONString(val.value);
				}
			} else {
				SkipValue();
			}
			if (!Peek(TokenType::EndObject)) {
				Expect(TokenType::Comma);
			}
		}
	}

	void ParseLights(IRManifest& manifest) {
		Expect(TokenType::BeginArray);
		while (!Match(TokenType::EndArray)) {
			IRLight l;
			Expect(TokenType::BeginObject);
			while (!Match(TokenType::EndObject)) {
				Token key = Expect(TokenType::String);
				Expect(TokenType::Colon);
				if (key.value == "id") {
					l.id = DecodeJSONString(Expect(TokenType::String).value);
				} else if (key.value == "type") {
					l.type = DecodeJSONString(Expect(TokenType::String).value);
				} else if (key.value == "intensity") {
					l.intensity = std::stof(std::string(Expect(TokenType::Number).value));
				} else if (key.value == "color") {
					Expect(TokenType::BeginArray);
					for (int i = 0; i < 3; ++i) {
						l.color[i] = std::stof(std::string(Expect(TokenType::Number).value));
						if (i < 2) {
							Expect(TokenType::Comma);
						}
					}
					Expect(TokenType::EndArray);
				} else {
					SkipValue();
				}
				if (!Peek(TokenType::EndObject)) {
					Expect(TokenType::Comma);
				}
			}
			manifest.lights.push_back(l);
			if (!Peek(TokenType::EndArray)) {
				Expect(TokenType::Comma);
			}
		}
	}

	void ParseMaterials(IRManifest& manifest) {
		Expect(TokenType::BeginArray);
		while (!Match(TokenType::EndArray)) {
			IRMaterial mat;
			Expect(TokenType::BeginObject);
			while (!Match(TokenType::EndObject)) {
				Token key = Expect(TokenType::String);
				Expect(TokenType::Colon);
				if (key.value == "id") {
					mat.id = DecodeJSONString(Expect(TokenType::String).value);
				} else if (key.value == "double_sided") {
					Token val = Advance();
					mat.doubleSided = (val.type == TokenType::True);
				} else if (key.value == "pbr") {
					ParsePBR(mat);
				} else if (key.value == "maps") {
					ParseMaps(mat);
				} else {
					SkipValue();
				}
				if (!Peek(TokenType::EndObject)) {
					Expect(TokenType::Comma);
				}
			}
			manifest.materials.push_back(mat);
			if (!Peek(TokenType::EndArray)) {
				Expect(TokenType::Comma);
			}
		}
	}

	void ParsePBR(IRMaterial& mat) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "base_color") {
				Expect(TokenType::BeginArray);
				for (int i = 0; i < 4; ++i) {
					mat.baseColor[i] = std::stof(std::string(Expect(TokenType::Number).value));
					if (i < 3) {
						Expect(TokenType::Comma);
					}
				}
				Expect(TokenType::EndArray);
			} else if (key.value == "metallic") {
				mat.metallic = std::stof(std::string(Expect(TokenType::Number).value));
			} else if (key.value == "roughness") {
				mat.roughness = std::stof(std::string(Expect(TokenType::Number).value));
			} else if (key.value == "emissive_factor") {
				Expect(TokenType::BeginArray);
				for (int i = 0; i < 3; ++i) {
					mat.emissiveFactor[i] = std::stof(std::string(Expect(TokenType::Number).value));
					if (i < 2) {
						Expect(TokenType::Comma);
					}
				}
				Expect(TokenType::EndArray);
			} else if (key.value == "emissive_strength") {
				mat.emissiveStrength = std::stof(std::string(Expect(TokenType::Number).value));
			} else {
				SkipValue();
			}
			if (!Peek(TokenType::EndObject)) {
				Expect(TokenType::Comma);
			}
		}
	}

	void ParseMaps(IRMaterial& mat) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			Token val = Advance();
			std::string fileStr =
				(val.type == TokenType::String) ? DecodeJSONString(val.value) : "";
			if (key.value == "albedo" && !fileStr.empty()) {
				mat.albedoMap = fileStr;
			} else if (key.value == "normal" && !fileStr.empty()) {
				mat.normalMap = fileStr;
			} else if (key.value == "metallic_roughness" && !fileStr.empty()) {
				mat.metallicRoughnessMap = fileStr;
			} else if (key.value == "emissive" && !fileStr.empty()) {
				mat.emissiveMap = fileStr;
			}
			if (!Peek(TokenType::EndObject)) {
				Expect(TokenType::Comma);
			}
		}
	}

	void SkipValue() {
		Token tok = Advance();
		if (tok.type == TokenType::BeginObject) {
			int depth = 1;
			while (depth > 0) {
				Token t = Advance();
				if (t.type == TokenType::BeginObject) {
					depth++;
				} else if (t.type == TokenType::EndObject) {
					depth--;
				}
			}
		} else if (tok.type == TokenType::BeginArray) {
			int depth = 1;
			while (depth > 0) {
				Token t = Advance();
				if (t.type == TokenType::BeginArray) {
					depth++;
				} else if (t.type == TokenType::EndArray) {
					depth--;
				}
			}
		}
	}
};
} // namespace Compiler

// ============================================================================
// Compilation Logic (Moved from Python to C++)
// ============================================================================

struct RawLoopVertex {
	uint32_t v_idx;
	float px, py, pz;
	float nx, ny, nz;
	float u, v;
	float r, g, b, a;
	uint16_t joints[4];
	float weights[4];

	bool operator==(const RawLoopVertex& o) const {
		if (v_idx != o.v_idx) {
			return false;
		}

		// Exact float equality is fine here because these are read verbatim from the Python export
		// array
		if (u != o.u || v != o.v) {
			return false;
		}
		if (nx != o.nx || ny != o.ny || nz != o.nz) {
			return false;
		}
		if (r != o.r || g != o.g || b != o.b || a != o.a) {
			return false;
		}
		return true;
	}
};

struct RawLoopVertexHash {
	size_t operator()(const RawLoopVertex& v) const {
		size_t h = std::hash<uint32_t>{}(v.v_idx);
		auto combine = [&](float val) {
			uint32_t bits = 0;
			std::memcpy(&bits, &val, 4);
			h ^= bits + 0x9e3779b9 + (h << 6) + (h >> 2);
		};
		combine(v.u);
		combine(v.v);
		combine(v.nx);
		combine(v.ny);
		combine(v.nz);
		return h;
	}
};

struct CompiledMesh {
	std::vector<float> glbVertices; // EXACTLY 16 uncompressed standard floats per vertex
	std::vector<uint32_t> indices;
	std::vector<uint16_t> joints;
	std::vector<float> weights;
	std::vector<Compiler::IRPrimitive> primitives;
	float minB[3] = {1e30f, 1e30f, 1e30f};
	float maxB[3] = {-1e30f, -1e30f, -1e30f};
	bool isSkinned = false;
};

CompiledMesh CompileRawMesh(const Compiler::IRMesh& mesh, const std::string& binPath) {
	CompiledMesh result;

	FILE* bf = std::fopen(binPath.c_str(), "rb");
	if (bf == nullptr) {
		std::println(stderr, "[zcook] ERROR: Failed to open intermediate bin file '{}': {}",
					 binPath, std::strerror(errno));
		return result;
	}

	std::fseek(bf, 0, SEEK_END);
	long binSize = std::ftell(bf);
	std::fseek(bf, 0, SEEK_SET);
	std::vector<float> rawFloats(binSize / sizeof(float));
	std::fread(rawFloats.data(), sizeof(float), rawFloats.size(), bf);
	std::fclose(bf);

	bool hasSkin = mesh.layout.contains("_J4W4");
	result.isSkinned = hasSkin;
	size_t stride = hasSkin ? 21 : 13;

	std::unordered_map<RawLoopVertex, uint32_t, RawLoopVertexHash> uniqueMap;
	std::vector<RawLoopVertex> rawVerts;

	// Phase 1: Vertex Deduplication
	for (const auto& prim : mesh.primitives) {
		uint32_t startLoop = prim.vertexOffset;
		uint32_t loopCount = prim.vertexCount;

		Compiler::IRPrimitive outPrim;
		outPrim.materialId = prim.materialId;
		// glTF GLB counts offsets in BYTEs relative to the start of the bufferView
		outPrim.vertexOffset = static_cast<uint32_t>(result.indices.size() * sizeof(uint32_t));
		outPrim.vertexCount = loopCount; // This is the total number of indices to draw!

		for (uint32_t i = 0; i < loopCount; ++i) {
			size_t offset = (startLoop + i) * stride;
			if (offset + stride > rawFloats.size()) {
				break;
			}

			RawLoopVertex rv{};
			rv.v_idx = static_cast<uint32_t>(rawFloats[offset + 0]);
			rv.px = rawFloats[offset + 1];
			rv.py = rawFloats[offset + 2];
			rv.pz = rawFloats[offset + 3];
			rv.nx = rawFloats[offset + 4];
			rv.ny = rawFloats[offset + 5];
			rv.nz = rawFloats[offset + 6];
			rv.u = rawFloats[offset + 7];
			rv.v = rawFloats[offset + 8];
			rv.r = rawFloats[offset + 9];
			rv.g = rawFloats[offset + 10];
			rv.b = rawFloats[offset + 11];
			rv.a = rawFloats[offset + 12];

			if (hasSkin) {
				rv.joints[0] = static_cast<uint16_t>(rawFloats[offset + 13]);
				rv.joints[1] = static_cast<uint16_t>(rawFloats[offset + 14]);
				rv.joints[2] = static_cast<uint16_t>(rawFloats[offset + 15]);
				rv.joints[3] = static_cast<uint16_t>(rawFloats[offset + 16]);
				rv.weights[0] = rawFloats[offset + 17];
				rv.weights[1] = rawFloats[offset + 18];
				rv.weights[2] = rawFloats[offset + 19];
				rv.weights[3] = rawFloats[offset + 20];
			} else {
				rv.joints[0] = rv.joints[1] = rv.joints[2] = rv.joints[3] = 0;
				rv.weights[0] = rv.weights[1] = rv.weights[2] = rv.weights[3] = 0.0f;
			}

			auto it = uniqueMap.find(rv);
			if (it != uniqueMap.end()) {
				result.indices.push_back(it->second);
			} else {
				auto newIdx = static_cast<uint32_t>(rawVerts.size());
				uniqueMap[rv] = newIdx;
				result.indices.push_back(newIdx);
				rawVerts.push_back(rv);
			}
		}
		result.primitives.push_back(outPrim);
	}

	// Phase 2: Tangent Calculation (MikkTSpace-lite orthogonalization)
	std::vector<JPH::Vec3> tangents(rawVerts.size(), JPH::Vec3::sZero());
	std::vector<JPH::Vec3> bitangents(rawVerts.size(), JPH::Vec3::sZero());

	for (size_t i = 0; i + 2 < result.indices.size(); i += 3) {
		uint32_t i0 = result.indices[i + 0];
		uint32_t i1 = result.indices[i + 1];
		uint32_t i2 = result.indices[i + 2];

		if (i0 >= rawVerts.size() || i1 >= rawVerts.size() || i2 >= rawVerts.size()) {
			continue;
		}

		auto& v0 = rawVerts[i0];
		auto& v1 = rawVerts[i1];
		auto& v2 = rawVerts[i2];

		JPH::Vec3 p0(v0.px, v0.py, v0.pz);
		JPH::Vec3 p1(v1.px, v1.py, v1.pz);
		JPH::Vec3 p2(v2.px, v2.py, v2.pz);

		JPH::Vec3 e1 = p1 - p0;
		JPH::Vec3 e2 = p2 - p0;

		float du1 = v1.u - v0.u;
		float dv1 = v1.v - v0.v;
		float du2 = v2.u - v0.u;
		float dv2 = v2.v - v0.v;

		float det = du1 * dv2 - dv1 * du2;
		float r = (det != 0.0f) ? 1.0f / det : 0.0f;

		JPH::Vec3 t = (e1 * dv2 - e2 * dv1) * r;
		JPH::Vec3 b = (e2 * du1 - e1 * du2) * r;

		tangents[i0] += t;
		tangents[i1] += t;
		tangents[i2] += t;
		bitangents[i0] += b;
		bitangents[i1] += b;
		bitangents[i2] += b;
	}

	// Phase 3: Pack into pure uncompressed 64-byte float array blocks for GLB
	result.glbVertices.resize(rawVerts.size() * 16);
	if (hasSkin) {
		result.joints.resize(rawVerts.size() * 4);
		result.weights.resize(rawVerts.size() * 4);
	}

	for (size_t i = 0; i < rawVerts.size(); ++i) {
		auto& rv = rawVerts[i];
		float* f = &result.glbVertices[i * 16];

		f[0] = rv.px;
		f[1] = rv.py;
		f[2] = rv.pz;
		f[3] = rv.nx;
		f[4] = rv.ny;
		f[5] = rv.nz;

		JPH::Vec3 n(rv.nx, rv.ny, rv.nz);
		JPH::Vec3 t = tangents[i];
		if (t.LengthSq() > 1e-6f) {
			JPH::Vec3 tangent = (t - n * n.Dot(t)).Normalized();
			float sign = n.Cross(t).Dot(bitangents[i]) < 0.0f ? -1.0f : 1.0f;
			f[6] = tangent.GetX();
			f[7] = tangent.GetY();
			f[8] = tangent.GetZ();
			f[9] = sign;
		} else {
			JPH::Vec3 absN(std::abs(n.GetX()), std::abs(n.GetY()), std::abs(n.GetZ()));
			JPH::Vec3 fallbackT =
				(absN.GetX() < 0.999f) ? JPH::Vec3(1.0f, 0.0f, 0.0f) : JPH::Vec3(0.0f, 1.0f, 0.0f);
			JPH::Vec3 tangent = (fallbackT - n * n.Dot(fallbackT)).Normalized();
			f[6] = tangent.GetX();
			f[7] = tangent.GetY();
			f[8] = tangent.GetZ();
			f[9] = 1.0f;
		}

		f[10] = rv.u;
		f[11] = rv.v;
		f[12] = rv.r;
		f[13] = rv.g;
		f[14] = rv.b;
		f[15] = rv.a;

		if (hasSkin) {
			result.joints[i * 4 + 0] = rv.joints[0];
			result.joints[i * 4 + 1] = rv.joints[1];
			result.joints[i * 4 + 2] = rv.joints[2];
			result.joints[i * 4 + 3] = rv.joints[3];
			result.weights[i * 4 + 0] = rv.weights[0];
			result.weights[i * 4 + 1] = rv.weights[1];
			result.weights[i * 4 + 2] = rv.weights[2];
			result.weights[i * 4 + 3] = rv.weights[3];
		}

		result.minB[0] = std::min(result.minB[0], f[0]);
		result.minB[1] = std::min(result.minB[1], f[1]);
		result.minB[2] = std::min(result.minB[2], f[2]);
		result.maxB[0] = std::max(result.maxB[0], f[0]);
		result.maxB[1] = std::max(result.maxB[1], f[1]);
		result.maxB[2] = std::max(result.maxB[2], f[2]);
	}

	return result;
}

// ============================================================================
// GLB Emitter Namespace
// ============================================================================
namespace GLB {

inline bool EmitGLB(const Compiler::IRManifest& manifest, const std::string& levelFolder,
					const std::string& outputPath) {
	std::vector<uint8_t> binBuffer;
	binBuffer.reserve(static_cast<size_t>(16 * 1024 * 1024));

	std::vector<std::string> bufferViews;
	std::vector<std::string> accessors;
	std::vector<std::string> meshesJson;
	std::vector<std::string> nodesJson;

	std::unordered_map<std::string, int> meshIdToGlbIndex;
	std::unordered_map<std::string, int> lightIdToGlbIndex;

	uint32_t accIndex = 0;
	uint32_t bViewIndex = 0;

	std::vector<std::string> images;
	std::vector<std::string> textures;
	std::vector<std::string> materialsJson;
	std::unordered_map<std::string, int> matIdToGlbIndex;

	struct PackedImage {
		std::string relativeUri;
		uint32_t bufferViewIndex;
	};
	std::vector<PackedImage> packedImages;

	auto getTextureIndex = [&](const std::string& relativeUri) -> int {
		if (relativeUri.empty()) {
			return -1;
		}

		for (size_t i = 0; i < packedImages.size(); ++i) {
			if (packedImages[i].relativeUri == relativeUri) {
				return static_cast<int>(i);
			}
		}

		std::string fullPath = levelFolder + "/" + relativeUri;
		FILE* f = std::fopen(fullPath.c_str(), "rb");
		if (f == nullptr) {
			std::println(stderr,
						 "[zcook] WARNING: Failed to open texture source file '{}': {}. Skipping.",
						 fullPath, std::strerror(errno));
			return -1;
		}

		std::fseek(f, 0, SEEK_END);
		long size = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);

		std::vector<uint8_t> imgBytes(size);
		std::fread(imgBytes.data(), 1, size, f);
		std::fclose(f);

		while (binBuffer.size() % 4 != 0) {
			binBuffer.push_back(0);
		}

		auto imgOffset = static_cast<uint32_t>(binBuffer.size());
		binBuffer.insert(binBuffer.end(), imgBytes.begin(), imgBytes.end());
		while (binBuffer.size() % 4 != 0) {
			binBuffer.push_back(0);
		}

		std::string mimeType = "image/png";
		if (relativeUri.ends_with(".jpg") || relativeUri.ends_with(".jpeg")) {
			mimeType = "image/jpeg";
		}

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {}
    }})",
										  imgOffset, size));
		uint32_t imgBViewIdx = bViewIndex++;

		int idx = static_cast<int>(packedImages.size());
		packedImages.push_back({.relativeUri = relativeUri, .bufferViewIndex = imgBViewIdx});

		textures.push_back(std::format(R"(    {{"sampler": 0, "source": {}}})", idx));
		images.push_back(
			std::format(R"(    {{"bufferView": {}, "mimeType": "{}"}})", imgBViewIdx, mimeType));

		return idx;
	};

	for (const auto& mat : manifest.materials) {
		int albedoTex = getTextureIndex(mat.albedoMap);
		int normalTex = getTextureIndex(mat.normalMap);
		int mrTex = getTextureIndex(mat.metallicRoughnessMap);
		int emissiveTex = getTextureIndex(mat.emissiveMap);

		std::string pbrStr = std::format(R"(      "baseColorFactor": [{}, {}, {}, {}],
      "metallicFactor": {},
      "roughnessFactor": {})",
										 mat.baseColor[0], mat.baseColor[1], mat.baseColor[2],
										 mat.baseColor[3], mat.metallic, mat.roughness);

		if (albedoTex != -1) {
			pbrStr += std::format(R"(,
      "baseColorTexture": {{"index": {}}})",
								  albedoTex);
		}

		std::string matStr = std::format(R"(    {{
      "name": "{}",
      "pbrMetallicRoughness": {{
  {}
      }})",
										 mat.id, pbrStr);

		// Disable backface culling in the glTF schema if requested
		if (mat.doubleSided) {
			matStr += R"(,
      "doubleSided": true)";
		}

		if (normalTex != -1) {
			matStr += std::format(R"(,
      "normalTexture": {{"index": {}}})",
								  normalTex);
		}
		if (mrTex != -1) {
			matStr += std::format(R"(,
      "metallicRoughnessTexture": {{"index": {}}})",
								  mrTex);
		}

		bool hasEmissive =
			(mat.emissiveStrength > 0.f) &&
			((emissiveTex != -1) || (mat.emissiveFactor[0] > 0.f || mat.emissiveFactor[1] > 0.f ||
									 mat.emissiveFactor[2] > 0.f));

		if (hasEmissive) {
			float ef[3] = {mat.emissiveFactor[0], mat.emissiveFactor[1], mat.emissiveFactor[2]};
			if (emissiveTex != -1 && ef[0] == 0.f && ef[1] == 0.f && ef[2] == 0.f) {
				ef[0] = 1.f;
				ef[1] = 1.f;
				ef[2] = 1.f;
			}

			float strength = mat.emissiveStrength;
			if (strength < 1.f) {
				ef[0] *= strength;
				ef[1] *= strength;
				ef[2] *= strength;
				strength = 1.f;
			}

			matStr += std::format(R"(,
      "emissiveFactor": [{}, {}, {}])",
								  ef[0], ef[1], ef[2]);

			if (emissiveTex != -1) {
				matStr += std::format(R"(,
      "emissiveTexture": {{"index": {}}})",
									  emissiveTex);
			}

			if (strength > 1.f) {
				matStr += std::format(R"(,
      "extensions": {{
        "KHR_materials_emissive_strength": {{
          "emissiveStrength": {}
        }}
      }})",
									  strength);
			}
		}

		matStr += R"(
    })";
		matIdToGlbIndex[mat.id] = static_cast<int>(materialsJson.size());
		materialsJson.push_back(matStr);
	}

	for (const auto& mesh : manifest.meshes) {
		std::string binPath = levelFolder + "/" + mesh.binFile;
		CompiledMesh compiled = CompileRawMesh(mesh, binPath);
		if (compiled.glbVertices.empty()) {
			std::println(
				stderr,
				"[zcook] WARNING: Compiled vertex buffer is empty for mesh ID '{}'. Skipping.",
				mesh.id);
			continue;
		}

		meshIdToGlbIndex[mesh.id] = static_cast<int>(meshesJson.size());

		// Append Compiled Vertices natively as standard 64-byte strided uncompressed glTF floats
		auto vboOffset = static_cast<uint32_t>(binBuffer.size());
		size_t vboBytes = compiled.glbVertices.size() * sizeof(float);
		binBuffer.insert(binBuffer.end(), reinterpret_cast<uint8_t*>(compiled.glbVertices.data()),
						 reinterpret_cast<uint8_t*>(compiled.glbVertices.data()) + vboBytes);
		while (binBuffer.size() % 4 != 0) {
			binBuffer.push_back(0);
		}

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "byteStride": 64,
      "target": 34962
    }})",
										  vboOffset, vboBytes));
		uint32_t vboBViewIdx = bViewIndex++;

		// Append Compiled Indices
		auto iboOffset = static_cast<uint32_t>(binBuffer.size());
		size_t iboBytes = compiled.indices.size() * sizeof(uint32_t);
		binBuffer.insert(binBuffer.end(), reinterpret_cast<uint8_t*>(compiled.indices.data()),
						 reinterpret_cast<uint8_t*>(compiled.indices.data()) + iboBytes);
		while (binBuffer.size() % 4 != 0) {
			binBuffer.push_back(0);
		}

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "target": 34963
    }})",
										  iboOffset, iboBytes));
		uint32_t iboBViewIdx = bViewIndex++;

		auto vertexCount = static_cast<uint32_t>(compiled.glbVertices.size() / 16);

		uint32_t posAcc = accIndex++;
		uint32_t normAcc = accIndex++;
		uint32_t tangAcc = accIndex++;
		uint32_t uvAcc = accIndex++;
		uint32_t colorAcc = accIndex++;

		accessors.push_back(std::format(R"(    {{"bufferView": {},
      "componentType": 5126,
      "count": {},
      "type": "VEC3",
      "min": [{}, {}, {}],
      "max": [{}, {}, {}]}})",
										vboBViewIdx, vertexCount, compiled.minB[0],
										compiled.minB[1], compiled.minB[2], compiled.maxB[0],
										compiled.maxB[1], compiled.maxB[2]));

		accessors.push_back(std::format(R"(    {{"bufferView": {},
      "byteOffset": 12,
      "componentType": 5126,
      "count": {},
      "type": "VEC3"}})",
										vboBViewIdx, vertexCount));
		accessors.push_back(std::format(R"(    {{"bufferView": {},
      "byteOffset": 24,
      "componentType": 5126,
      "count": {},
      "type": "VEC4"}})",
										vboBViewIdx, vertexCount));
		accessors.push_back(std::format(R"(    {{"bufferView": {},
      "byteOffset": 40,
      "componentType": 5126,
      "count": {},
      "type": "VEC2"}})",
										vboBViewIdx, vertexCount));
		accessors.push_back(std::format(R"(    {{"bufferView": {},
      "byteOffset": 48,
      "componentType": 5126,
      "count": {},
      "type": "VEC4"}})",
										vboBViewIdx, vertexCount));

		std::string primsStr;
		for (size_t p = 0; p < compiled.primitives.size(); ++p) {
			const auto& prim = compiled.primitives[p];
			uint32_t indexAcc = accIndex++;

			accessors.push_back(std::format(R"(    {{"bufferView": {},
      "byteOffset": {},
      "componentType": 5125,
      "count": {},
      "type": "SCALAR"}})",
											iboBViewIdx, prim.vertexOffset, prim.vertexCount));

			int matGlbIdx = -1;
			auto it = matIdToGlbIndex.find(prim.materialId);
			if (it != matIdToGlbIndex.end()) {
				matGlbIdx = it->second;
			}

			std::string matStr;
			if (matGlbIdx != -1) {
				matStr = std::format(R"(,
          "material": {})",
									 matGlbIdx);
			}

			primsStr += std::format(R"(        {{
          "attributes": {{
            "POSITION": {},
            "NORMAL": {},
            "TANGENT": {},
            "TEXCOORD_0": {},
            "COLOR_0": {}
          }},
          "indices": {}
          {}
        }})",
									posAcc, normAcc, tangAcc, uvAcc, colorAcc, indexAcc, matStr);
			if (p < compiled.primitives.size() - 1) {
				primsStr += ",\n";
			}
		}

		meshesJson.push_back(std::format(R"(    {{
      "name": "{}",
      "primitives": [
{}
      ]
    }})",
										 mesh.id, primsStr));
	}

	for (size_t i = 0; i < lightIdToGlbIndex.size(); ++i) {
		lightIdToGlbIndex[manifest.lights[i].id] = static_cast<int>(i);
	}

	for (const auto& node : manifest.nodes) {
		if (!node.visible && node.meshId.empty() && node.lightId.empty()) {
			continue;
		}

		std::string matrixStr = "[";
		for (int i = 0; i < 16; ++i) {
			matrixStr += std::to_string(node.worldMatrix[i]);
			if (i < 15) {
				matrixStr += ", ";
			}
		}
		matrixStr += "]";

		std::string meshStr;
		if (!node.meshId.empty()) {
			auto it = meshIdToGlbIndex.find(node.meshId);
			if (it != meshIdToGlbIndex.end()) {
				meshStr = std::format(",\n      \"mesh\": {}", it->second);
			}
		}

		std::string extStr;
		if (!node.lightId.empty()) {
			auto lit = lightIdToGlbIndex.find(node.lightId);
			if (lit != lightIdToGlbIndex.end()) {
				extStr = std::format(R"(,
      "extensions": {{
        "KHR_lights_punctual": {{
          "light": {}
        }}
      }})",
									 lit->second);
			}
		}

		nodesJson.push_back(std::format(R"(    {{
      "name": "{}",
      "matrix": {}{}{}
    }})",
										node.id, matrixStr, meshStr, extStr));
	}

	if (nodesJson.empty()) {
		for (size_t i = 0; i < manifest.meshes.size(); ++i) {
			nodesJson.push_back(std::format(R"(    {{
      "name": "Node_{}",
      "mesh": {}
    }})",
											manifest.meshes[i].id, i));
		}
	}

	std::vector<std::string> usedExts;
	if (!manifest.lights.empty()) {
		usedExts.emplace_back("\"KHR_lights_punctual\"");
	}

	bool usesEmissiveStrength = false;
	for (const auto& mat : manifest.materials) {
		bool hasEmissive = (mat.emissiveStrength > 0.f) &&
						   ((!mat.emissiveMap.empty()) ||
							(mat.emissiveFactor[0] > 0.f || mat.emissiveFactor[1] > 0.f ||
							 mat.emissiveFactor[2] > 0.f));
		if (hasEmissive && mat.emissiveStrength > 1.f) {
			usesEmissiveStrength = true;
			break;
		}
	}
	if (usesEmissiveStrength) {
		usedExts.emplace_back("\"KHR_materials_emissive_strength\"");
	}

	std::string extensionsUsed;
	for (size_t i = 0; i < usedExts.size(); ++i) {
		extensionsUsed += usedExts[i];
		if (i < usedExts.size() - 1) {
			extensionsUsed += ", ";
		}
	}
	std::string rootExtensions;

	if (!manifest.lights.empty()) {
		std::string lightsArr;
		for (size_t i = 0; i < manifest.lights.size(); ++i) {
			const auto& l = manifest.lights[i];
			lightsArr += std::format(R"(        {{
          "name": "{}",
          "type": "{}",
          "color": [{}, {}, {}],
          "intensity": {}
        }})",
									 l.id, l.type, l.color[0], l.color[1], l.color[2], l.intensity);
			if (i < manifest.lights.size() - 1) {
				lightsArr += ",\n";
			}
		}
		rootExtensions += std::format(R"(  "extensions": {{
    "KHR_lights_punctual": {{
      "lights": [
{}
      ]
    }}
  }},
)",
									  lightsArr);
	}

	std::string json;
	json.reserve(static_cast<size_t>(128 * 1024));
	json.append(R"({
  "asset": {"version": "2.0", "generator": "Zahlen GLB Emitter"},
)");

	if (!extensionsUsed.empty()) {
		json.append(std::format("  \"extensionsUsed\": [{}],\n", extensionsUsed));
	}
	if (!rootExtensions.empty()) {
		json.append(rootExtensions);
	}

	json.append("  \"bufferViews\": [\n");
	for (size_t i = 0; i < bufferViews.size(); ++i) {
		json.append(bufferViews[i]);
		json.append(i < bufferViews.size() - 1 ? ",\n" : "\n");
	}
	json.append("  ],\n");

	json.append("  \"accessors\": [\n");
	for (size_t i = 0; i < accessors.size(); ++i) {
		json.append(accessors[i]);
		json.append(i < accessors.size() - 1 ? ",\n" : "\n");
	}
	json.append("  ],\n");

	if (!materialsJson.empty()) {
		json.append("  \"materials\": [\n");
		for (size_t i = 0; i < materialsJson.size(); ++i) {
			json.append(materialsJson[i]);
			json.append(i < materialsJson.size() - 1 ? ",\n" : "\n");
		}
		json.append("  ],\n");
	}

	if (!textures.empty()) {
		json.append("  \"textures\": [\n");
		for (size_t i = 0; i < textures.size(); ++i) {
			json.append(textures[i]);
			json.append(i < textures.size() - 1 ? ",\n" : "\n");
		}
		json.append("  ],\n");
	}

	if (!images.empty()) {
		json.append("  \"images\": [\n");
		for (size_t i = 0; i < images.size(); ++i) {
			json.append(images[i]);
			json.append(i < images.size() - 1 ? ",\n" : "\n");
		}
		json.append("  ],\n");

		json.append(R"(  "samplers": [
    {"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497}
  ],
)");
	}

	if (!meshesJson.empty()) {
		json.append("  \"meshes\": [\n");
		for (size_t i = 0; i < meshesJson.size(); ++i) {
			json.append(meshesJson[i]);
			json.append(i < meshesJson.size() - 1 ? ",\n" : "\n");
		}
		json.append("  ],\n");
	}

	json.append("  \"nodes\": [\n");
	for (size_t i = 0; i < nodesJson.size(); ++i) {
		json.append(nodesJson[i]);
		json.append(i < nodesJson.size() - 1 ? ",\n" : "\n");
	}
	json.append("  ],\n");

	json.append("  \"scenes\": [\n"
				"    {\n"
				"      \"nodes\": [");
	for (size_t i = 0; i < nodesJson.size(); ++i) {
		json.append(std::to_string(i));
		if (i < nodesJson.size() - 1) {
			json.append(",");
		}
	}
	json.append("]\n"
				"    }\n"
				"  ],\n"
				"  \"scene\": 0,\n");

	json.append(std::format(R"(  "buffers": [
    {{
      "byteLength": {}
    }}
  ]
}})",
							binBuffer.size()));

	while (json.length() % 4 != 0) {
		json += ' ';
	}

	auto jsonChunkLength = static_cast<uint32_t>(json.length());
	auto binChunkLength = static_cast<uint32_t>(binBuffer.size());
	uint32_t totalFileLength = 12 + 8 + jsonChunkLength + 8 + binChunkLength;

	FILE* out = std::fopen(outputPath.c_str(), "wb");
	if (out == nullptr) {
		std::println(stderr, "[zcook] ERROR: Failed to open output GLB file '{}' for writing: {}",
					 outputPath, std::strerror(errno));
		return false;
	}

	uint32_t magic = 0x46546C67; // "glTF"
	uint32_t version = 2;
	std::fwrite(&magic, 1, 4, out);
	std::fwrite(&version, 1, 4, out);
	std::fwrite(&totalFileLength, 1, 4, out);

	uint32_t chunkTypeJson = 0x4E4F534A; // "JSON"
	std::fwrite(&jsonChunkLength, 1, 4, out);
	std::fwrite(&chunkTypeJson, 1, 4, out);
	std::fwrite(json.data(), 1, jsonChunkLength, out);

	uint32_t chunkTypeBin = 0x004E4942; // "BIN"
	std::fwrite(&binChunkLength, 1, 4, out);
	std::fwrite(&chunkTypeBin, 1, 4, out);
	std::fwrite(binBuffer.data(), 1, binChunkLength, out);

	std::fclose(out);
	return true;
}

} // namespace GLB

// ============================================================================
// CLI Subcommands
// ============================================================================

int CookMesh(int argc, char** argv) {
	std::string metaPath;
	std::string meshId;
	std::string outPath;
	std::string inPath;
	for (int i = 0; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--meta" && i + 1 < argc) {
			metaPath = argv[++i];
		} else if (arg == "--id" && i + 1 < argc) {
			meshId = argv[++i];
		} else if (arg == "-i" && i + 1 < argc) {
			inPath = argv[++i];
		} else if (arg == "-o" && i + 1 < argc) {
			outPath = argv[++i];
		}
	}

	if (metaPath.empty() || meshId.empty() || inPath.empty() || outPath.empty()) {
		std::println(stderr, "[zcook] ERROR: Missing arguments for mesh subcommand. Requirements:");
		if (metaPath.empty()) {
			std::println(stderr, "  --meta <path>  (Scene metadata JSON)");
		}
		if (meshId.empty()) {
			std::println(stderr, "  --id <string>  (Mesh Identifier)");
		}
		if (inPath.empty()) {
			std::println(stderr, "  -i <path>      (Input raw binaries)");
		}
		if (outPath.empty()) {
			std::println(stderr, "  -o <path>      (Output compiled destination file)");
		}
		return 1;
	}

	FILE* f = std::fopen(metaPath.c_str(), "rb");
	if (f == nullptr) {
		std::println(stderr, "[zcook] ERROR: Failed to open metadata scene file '{}': {}", metaPath,
					 std::strerror(errno));
		return 1;
	}
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string source(size, '\0');
	std::fread(source.data(), 1, size, f);
	std::fclose(f);

	Compiler::Lexer lexer(source);
	std::vector<Compiler::Token> tokens = lexer.Tokenize();
	Compiler::Parser parser(tokens, source);
	Compiler::IRManifest manifest = parser.Parse();

	auto it = std::ranges::find_if(manifest.meshes, [&](const auto& m) { return m.id == meshId; });
	if (it == manifest.meshes.end()) {
		std::println(stderr,
					 "[zcook] ERROR: Mesh ID '{}' was not found in parsed scene manifest '{}'",
					 meshId, metaPath);
		return 1;
	}
	const auto& mesh = *it;

	// Pass to compiled engine core to perform deduplication and tangent generation
	CompiledMesh compiled = CompileRawMesh(mesh, inPath);
	if (compiled.glbVertices.empty()) {
		std::println(stderr,
					 "[zcook] WARNING: Raw mesh compiled buffers are empty/null for path '{}'. "
					 "Defaulting to an empty mesh structure.",
					 inPath);
	}

	CookedMeshHeader meshHeader{};
	meshHeader.magic = 0x3048534D; // 'MSH0'
	meshHeader.version = 2;

	// If the compiled buffers are empty, we zero out the bounding box to prevent NaNs/Infs in
	// rendering/physics
	if (compiled.glbVertices.empty()) {
		meshHeader.boundingBoxMin[0] = 0.0f;
		meshHeader.boundingBoxMin[1] = 0.0f;
		meshHeader.boundingBoxMin[2] = 0.0f;
		meshHeader.boundingBoxMax[0] = 0.0f;
		meshHeader.boundingBoxMax[1] = 0.0f;
		meshHeader.boundingBoxMax[2] = 0.0f;
		meshHeader.vertexCount = 0;
		meshHeader.indexCount = 0;
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

	size_t vboSize = finalVerts.size() * sizeof(Vertex);
	size_t iboSize = compiled.indices.size() * sizeof(uint32_t);

	fs::create_directories(fs::path(outPath).parent_path());
	FILE* out = std::fopen(outPath.c_str(), "wb");
	if (out == nullptr) {
		std::println(stderr, "[zcook] ERROR: Failed to open output mesh file '{}' for writing: {}",
					 outPath, std::strerror(errno));
		return 1;
	}

	std::fwrite(&meshHeader, 1, sizeof(CookedMeshHeader), out);
	if (vboSize > 0) {
		std::fwrite(finalVerts.data(), 1, vboSize, out);
	}
	if (iboSize > 0) {
		std::fwrite(compiled.indices.data(), 1, iboSize, out);
	}
	std::fclose(out);

	return 0;
}

int CookTexture(int argc, char** argv) {
	std::string inPath;
	std::string outPath;
	for (int i = 0; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "-i" && i + 1 < argc) {
			inPath = argv[++i];
		} else if (arg == "-o" && i + 1 < argc) {
			outPath = argv[++i];
		}
	}

	if (inPath.empty() || outPath.empty()) {
		std::println(stderr, "[zcook] ERROR: Missing arguments for tex subcommand. Requirements:");
		if (inPath.empty()) {
			std::println(stderr, "  -i <path>      (Input raw texture/image file)");
		}
		if (outPath.empty()) {
			std::println(stderr, "  -o <path>      (Output compiled texture destination)");
		}
		return 1;
	}

	FILE* in = std::fopen(inPath.c_str(), "rb");
	if (in == nullptr) {
		std::println(stderr, "[zcook] ERROR: Failed to open input texture file '{}': {}", inPath,
					 std::strerror(errno));
		return 1;
	}
	std::fseek(in, 0, SEEK_END);
	long size = std::ftell(in);
	std::fseek(in, 0, SEEK_SET);
	std::vector<char> fileData(size);
	std::fread(fileData.data(), 1, size, in);
	std::fclose(in);

	fs::create_directories(fs::path(outPath).parent_path());
	FILE* out = std::fopen(outPath.c_str(), "wb");
	if (out == nullptr) {
		std::println(stderr,
					 "[zcook] ERROR: Failed to open output texture file '{}' for writing: {}",
					 outPath, std::strerror(errno));
		return 1;
	}
	std::fwrite(fileData.data(), 1, size, out);
	std::fclose(out);

	return 0;
}

int PackArchive(int argc, char** argv) {
	std::string outPath;
	std::string manifestPath;
	for (int i = 0; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "-o" && i + 1 < argc) {
			outPath = argv[++i];
		} else if (arg == "-i" && i + 1 < argc) {
			manifestPath = argv[++i];
		}
	}

	if (outPath.empty() || manifestPath.empty()) {
		std::println(stderr, "[zcook] ERROR: Missing arguments for pak subcommand. Requirements:");
		if (manifestPath.empty()) {
			std::println(stderr, "  -i <path>      (Input package manifest txt file)");
		}
		if (outPath.empty()) {
			std::println(stderr, "  -o <path>      (Output compiled .pak archive path)");
		}
		return 1;
	}

	// Ensure the output directory exists
	fs::create_directories(fs::path(outPath).parent_path());

	std::ifstream ifs(manifestPath);
	if (!ifs.is_open()) {
		std::println(stderr, "[zcook] ERROR: Failed to open pak manifest file '{}'", manifestPath);
		return 1;
	}

	struct ManifestEntry {
		std::string vpath;
		std::string rpath;
		std::vector<char> data;
		size_t size = 0;
		size_t alignedOffset = 0;
		int errorCode = 0;
		bool success = false;
	};

	std::vector<ManifestEntry> manifestEntries;
	std::string line;
	while (std::getline(ifs, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		auto pos = line.find('=');
		if (pos == std::string::npos) {
			continue;
		}

		std::string vpath = line.substr(0, pos);
		std::string rpath = line.substr(pos + 1);
		manifestEntries.push_back({.vpath = std::move(vpath), .rpath = std::move(rpath)});
	}
	ifs.close();

	const auto totalFiles = static_cast<uint32_t>(manifestEntries.size());

	if (manifestEntries.empty()) {
		std::println(stderr, "[zcook] WARNING: Manifest file '{}' is empty. No files to package.",
					 manifestPath);
	} else {
		std::println("[zcook] Loading {} assets in parallel...", totalFiles);

		std::atomic<uint32_t> loadedCount{0};
		ZHLN::Mutex printMutex{};

		ZHLN::TaskSystem::ParallelFor(
			totalFiles, 1,
			[&manifestEntries, &loadedCount, &printMutex, totalFiles](uint32_t start, uint32_t end,
																	  uint32_t /*chunkIdx*/) {
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
						if (size > 0) {
							size_t readBytes = std::fread(entry.data.data(), 1, size, f);
							entry.data.resize(readBytes);
							entry.size = readBytes;
						}
						entry.success = true;
						std::fclose(f);
					}

					// Update thread-safe atomic counter
					const uint32_t current =
						loadedCount.fetch_add(1, std::memory_order_relaxed) + 1;

					// Print progress updates periodically to minimize lock contention
					if (current % 10 == 0 || current == totalFiles) {
						std::lock_guard<ZHLN::Mutex> lock(printMutex);
						std::print("\r[zcook] Loading assets: {}/{} ({:.1f}%)", current, totalFiles,
								   (static_cast<float>(current) / totalFiles) * 100.0f);
						std::fflush(stdout);
					}
				}
			});
		std::println(""); // Finalize the carriage return loading line
	}

	// Calculate the exact payload write size down to the byte
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

	std::println("[zcook] Writing archive directly to '{}'...", outPath);

	FILE* out = std::fopen(outPath.c_str(), "wb");
	if (out == nullptr) {
		std::println(
			stderr,
			"[zcook] ERROR: Failed to open output archive destination file '{}' for writing: {}",
			outPath, std::strerror(errno));
		return 1;
	}

	// Allocate a 1MB buffer to prevent thousands of tiny kernel context switches
	std::vector<char> streamBuffer(static_cast<size_t>(1024 * 1024));
	std::setvbuf(out, streamBuffer.data(), _IOFBF, streamBuffer.size());

	uint64_t totalBytesWritten = 0;
	auto writeAndTrack = [&](const void* ptr, size_t size) {
		if (size == 0) {
			return;
		}
		std::fwrite(ptr, 1, size, out);
		totalBytesWritten += size;
	};

	// Step 1: Write a dummy PakHeader to reserve space at the start of the file
	PakHeader dummyHeader{};
	writeAndTrack(&dummyHeader, sizeof(PakHeader));

	std::vector<PakEntry> entries;
	uint64_t currentPayloadSize = 0; // Tracks payload offset relative to payload start

	auto lastProgressTime = std::chrono::steady_clock::now();
	auto startWriteTime = lastProgressTime;

	// Progress updates rate-limited to 100ms intervals to prevent output I/O bottlenecking
	auto updateProgress = [&](bool force) {
		auto now = std::chrono::steady_clock::now();
		auto elapsed =
			std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProgressTime).count();
		if (force || elapsed >= 100) {
			double writtenMB = static_cast<double>(totalBytesWritten) / (1024.0 * 1024.0);
			double totalMB = static_cast<double>(totalBytesToWrite) / (1024.0 * 1024.0);
			double percentage =
				totalBytesToWrite > 0
					? (static_cast<double>(totalBytesWritten) / totalBytesToWrite) * 100.0
					: 100.0;

			auto totalElapsedMS =
				std::chrono::duration_cast<std::chrono::milliseconds>(now - startWriteTime).count();
			double speedMBs = 0.0;
			if (totalElapsedMS > 0) {
				speedMBs = (writtenMB / totalElapsedMS) * 1000.0;
			}

			std::print("\r[zcook] Writing archive: {:.2f} / {:.2f} MB ({:.1f}%) | {:.1f} MB/s",
					   writtenMB, totalMB, percentage, speedMBs);
			std::fflush(stdout);
			lastProgressTime = now;
		}
	};

	for (auto& entry : manifestEntries) {
		if (!entry.success) {
			if (entry.errorCode != 0) {
				std::println(
					stderr,
					"\n[zcook] WARNING: Failed to open archive source file '{}' specified in "
					"manifest: {}. Skipping.",
					entry.rpath, std::strerror(entry.errorCode));
			}
			continue;
		}

		// Step 2: Align to 16-byte boundary
		size_t padding = (16 - (currentPayloadSize % 16)) % 16;
		if (padding > 0) {
			static const char zeroPadding[16] = {0};
			writeAndTrack(zeroPadding, padding);
			currentPayloadSize += padding;
		}

		// Step 3: Populate PakEntry metadata
		PakEntry pakEntry{};
		pakEntry.pathHash = HashAssetPath(entry.vpath);
		pakEntry.offset = sizeof(PakHeader) + currentPayloadSize;
		pakEntry.compressedSize = entry.size;
		pakEntry.uncompressedSize = entry.size;
		pakEntry.compression = 0;

		entries.push_back(pakEntry);

		// Step 4: Dump pre-loaded data block directly to the package
		if (entry.size > 0) {
			writeAndTrack(entry.data.data(), entry.size);
			currentPayloadSize += entry.size;
		}

		updateProgress(false);
	}

	// Step 5: Record final TOC Offset right after the payload data
	uint64_t tocOffset = sizeof(PakHeader) + currentPayloadSize;

	// Step 6: Write Table of Contents (entries list)
	if (!entries.empty()) {
		writeAndTrack(entries.data(), entries.size() * sizeof(PakEntry));
	}

	// Step 7: Finalize actual PakHeader metadata
	PakHeader header{};
	std::memcpy(header.magic, "ZPAK", 4);
	header.version = 1;
	header.entryCount = entries.size();
	header.tocOffset = tocOffset;

	// Step 8: Seek back to the very beginning and overwrite the dummy header
	std::fseek(out, 0, SEEK_SET);
	std::fwrite(&header, 1, sizeof(PakHeader), out);

	// Mark all bytes as written for progress tracking
	totalBytesWritten = totalBytesToWrite;

	// Force the final progress bar to 100% on the terminal before blocking
	updateProgress(true);
	std::println(""); // Newline

	// Notify the user that physical synchronization is taking place
	std::print("[zcook] Syncing memory cache to physical disk (OS flush)...");
	std::fflush(stdout);

	// This is the blocking call where the physical 5-10s write to physical blocks actually occurs
	std::fclose(out);

	// Clear the "Syncing..." line cleanly by overwriting it with spaces
	std::print("\r                                                                          \r");
	std::fflush(stdout);

	const double sizeInMB = static_cast<double>(tocOffset) / (1024.0 * 1024.0);
	std::println("[zcook] Successfully packed {}/{} assets into '{}' ({:.2f} MB)", entries.size(),
				 totalFiles, outPath, sizeInMB);

	return 0;
}

int CookGLB(int argc, char** argv) {
	std::string metaPath;
	std::string outPath;
	for (int i = 0; i < argc; ++i) {
		std::string_view arg = argv[i];
		if (arg == "--meta" && i + 1 < argc) {
			metaPath = argv[++i];
		} else if (arg == "-o" && i + 1 < argc) {
			outPath = argv[++i];
		}
	}

	if (metaPath.empty() || outPath.empty()) {
		std::println(stderr, "[zcook] ERROR: Missing arguments for glb subcommand. Requirements:");
		if (metaPath.empty()) {
			std::println(stderr, "  --meta <path>  (Metadata JSON scene layout file)");
		}
		if (outPath.empty()) {
			std::println(stderr, "  -o <path>      (Output compiled destination path .glb)");
		}
		return 1;
	}

	FILE* f = std::fopen(metaPath.c_str(), "rb");
	if (f == nullptr) {
		std::println(stderr, "[zcook] ERROR: Failed to open metadata file '{}': {}", metaPath,
					 std::strerror(errno));
		return 1;
	}
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string source(size, '\0');
	std::fread(source.data(), 1, size, f);
	std::fclose(f);

	Compiler::Lexer lexer(source);
	std::vector<Compiler::Token> tokens = lexer.Tokenize();
	Compiler::Parser parser(tokens, source);
	Compiler::IRManifest manifest = parser.Parse();

	std::string levelFolder = fs::path(metaPath).parent_path().string();
	if (!GLB::EmitGLB(manifest, levelFolder, outPath)) {
		std::println(stderr, "[zcook] ERROR: Failed to generate GLB asset package for path '{}'",
					 outPath);
		return 1;
	}
	return 0;
}

} // namespace ZHLN

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 2) {
		std::println(
			stderr,
			"[zcook] ERROR: Missing command-line action. Usage:\n"
			"  zcook <command> [options]\n\n"
			"Available Commands:\n"
			"  mesh  - Deduplicates standard uncompressed floats into static mesh arrays.\n"
			"  tex   - Copy and format static assets into standard asset structures.\n"
			"  glb   - Compiles internal scenes and layouts into standard glTF GLB containers.\n"
			"  pak   - Compact files listed in a manifest file into single custom pak indexes.");
		return 1;
	}

	std::string_view cmd = argv[1];
	if (cmd != "mesh" && cmd != "tex" && cmd != "glb" && cmd != "pak") {
		std::println(stderr,
					 "[zcook] ERROR: Unsupported action subcommand '{}'. Run with no arguments to "
					 "see usage.",
					 cmd);
		return 1;
	}

	// Initialize the Fiber and Task Systems for all active cooking paths
	ZHLN::Fiber::InitMainThread();
	ZHLN::TaskSystem::Init(0); // 0 = Auto-detect CPU cores

	int result = 0;
	if (cmd == "mesh") {
		result = ZHLN::CookMesh(argc - 2, argv + 2);
	} else if (cmd == "tex") {
		result = ZHLN::CookTexture(argc - 2, argv + 2);
	} else if (cmd == "glb") {
		result = ZHLN::CookGLB(argc - 2, argv + 2);
	} else if (cmd == "pak") {
		result = ZHLN::PackArchive(argc - 2, argv + 2);
	}

	// Graceful shutdown of workers and fibers
	ZHLN::TaskSystem::Shutdown();

	return result;
}
