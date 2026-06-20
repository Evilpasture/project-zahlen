// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/zcook.cpp
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Types.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace ZHLN;

// locale-safe checks to prevent glibc lookup table crashes
inline bool IsDigit(char c) noexcept {
	return std::isdigit(static_cast<unsigned char>(c));
}

inline bool IsAlpha(char c) noexcept {
	return std::isalpha(static_cast<unsigned char>(c));
}

// Decodes standard JSON Unicode escape sequences (\uXXXX) and control characters to UTF-8
std::string DecodeJSONString(std::string_view raw) {
	std::string result;
	result.reserve(raw.size());
	for (size_t i = 0; i < raw.size(); ++i) {
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
					i += 5; // skip uXXXX
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
			i++; // skip escape char
		} else {
			result.push_back(raw[i]);
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
			if (tok.type == TokenType::EndOfFile)
				break;
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
		if (m_cursor >= m_source.length())
			return '\0';
		char c = m_source[m_cursor++];
		if (c == '\n')
			m_line++;
		return c;
	}

	void SkipWhitespace() {
		while (true) {
			char c = Peek();
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
				Advance();
			} else if (c == '/' && m_cursor + 1 < m_source.length() &&
					   m_source[m_cursor + 1] == '/') {
				while (Peek() != '\n' && Peek() != '\0')
					Advance();
			} else {
				break;
			}
		}
	}

	Token NextToken() {
		SkipWhitespace();
		size_t startLine = m_line;
		char c = Peek();

		if (c == '\0')
			return Token{.type = TokenType::EndOfFile, .value = {}, .line = startLine};

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

		if (IsDigit(c) || c == '-')
			return ParseNumber();
		if (IsAlpha(c))
			return ParseKeyword();

		Advance();
		return Token{
			.type = TokenType::Null, .value = m_source.substr(m_cursor - 1, 1), .line = startLine};
	}

	Token ParseString() {
		size_t startLine = m_line;
		Advance();
		size_t startIdx = m_cursor;
		while (Peek() != '"' && Peek() != '\0') {
			if (Peek() == '\\')
				Advance();
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
		if (Peek() == '-')
			Advance();
		while (IsDigit(Peek()))
			Advance();
		if (Peek() == '.') {
			Advance();
			while (IsDigit(Peek()))
				Advance();
		}
		return Token{.type = TokenType::Number,
					 .value = m_source.substr(startIdx, m_cursor - startIdx),
					 .line = startLine};
	}

	Token ParseKeyword() {
		size_t startLine = m_line;
		size_t startIdx = m_cursor;
		while (IsAlpha(Peek()))
			Advance();
		std::string_view word = m_source.substr(startIdx, m_cursor - startIdx);
		if (word == "true")
			return Token{.type = TokenType::True, .value = word, .line = startLine};
		if (word == "false")
			return Token{.type = TokenType::False, .value = word, .line = startLine};
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
	uint32_t indexOffset;
	uint32_t indexCount;
};
struct IRMesh {
	std::string id, layout, binFile;
	IRBuffer vertexBuffer, indexBuffer, jointsBuffer, weightsBuffer;
	std::vector<IRPrimitive> primitives;
};
struct IRNode {
	std::string id, meshId;
	float matrix[16];
	bool visible = true;
};
struct IRMaterial {
	std::string id, albedoMap, normalMap, metallicRoughnessMap;
	float baseColor[4] = {1.f, 1.f, 1.f, 1.f};
	float metallic = 0.f, roughness = 0.5f;
};
struct IRManifest {
	std::string levelName;
	std::vector<IRMesh> meshes;
	std::vector<IRNode> nodes;
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
			if (key.value == "scene_info")
				ParseSceneInfo(manifest);
			else if (key.value == "meshes")
				ParseMeshes(manifest);
			else if (key.value == "nodes")
				ParseNodes(manifest);
			else if (key.value == "materials")
				ParseMaterials(manifest);
			else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
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
			if (key.value == "name")
				manifest.levelName = DecodeJSONString(Expect(TokenType::String).value);
			else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
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
			if (!Peek(TokenType::EndArray))
				Expect(TokenType::Comma);
		}
	}

	bool matchMeshField(IRMesh& mesh) {
		if (Match(TokenType::EndObject)) {
			return true;
		}
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
		else
			SkipValue();
		return false;
	}

	void ParseBuffers(IRMesh& mesh) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "bin_file")
				mesh.binFile = DecodeJSONString(Expect(TokenType::String).value);
			else if (key.value == "vertex_buffer")
				ParseBufferBound(mesh.vertexBuffer);
			else if (key.value == "index_buffer")
				ParseBufferBound(mesh.indexBuffer);
			else if (key.value == "joints")
				ParseBufferBound(mesh.jointsBuffer);
			else if (key.value == "weights")
				ParseBufferBound(mesh.weightsBuffer);
			else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
	}

	void ParseBufferBound(IRBuffer& buf) {
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
					if (val.type == TokenType::String)
						prim.materialId = DecodeJSONString(val.value);
				} else if (key.value == "index_offset")
					prim.indexOffset = std::stoul(std::string(Expect(TokenType::Number).value));
				else if (key.value == "index_count")
					prim.indexCount = std::stoul(std::string(Expect(TokenType::Number).value));
				else
					SkipValue();
				if (!Peek(TokenType::EndObject))
					Expect(TokenType::Comma);
			}
			mesh.primitives.push_back(prim);
			if (!Peek(TokenType::EndArray))
				Expect(TokenType::Comma);
		}
	}

	void ParseNodes(IRManifest& manifest) {
		Expect(TokenType::BeginArray);
		while (!Match(TokenType::EndArray)) {
			IRNode node;
			std::memset(node.matrix, 0, sizeof(node.matrix));
			node.matrix[0] = node.matrix[5] = node.matrix[10] = node.matrix[15] = 1.0f;

			Expect(TokenType::BeginObject);
			while (!Match(TokenType::EndObject)) {
				Token key = Expect(TokenType::String);
				Expect(TokenType::Colon);
				if (key.value == "id")
					node.id = DecodeJSONString(Expect(TokenType::String).value);
				else if (key.value == "visible")
					node.visible = (Advance().type == TokenType::True);
				else if (key.value == "transform")
					ParseTransform(node.matrix);
				else if (key.value == "refs")
					ParseRefs(node.meshId);
				else
					SkipValue();
				if (!Peek(TokenType::EndObject))
					Expect(TokenType::Comma);
			}
			manifest.nodes.push_back(node);
			if (!Peek(TokenType::EndArray))
				Expect(TokenType::Comma);
		}
	}

	void ParseTransform(float* outMatrix) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "local") {
				Expect(TokenType::BeginArray);
				for (int i = 0; i < 16; ++i) {
					outMatrix[i] = std::stof(std::string(Expect(TokenType::Number).value));
					if (i < 15)
						Expect(TokenType::Comma);
				}
				Expect(TokenType::EndArray);
			} else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
	}

	void ParseRefs(std::string& outMeshId) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "mesh_id") {
				Token val = Advance();
				if (val.type == TokenType::String)
					outMeshId = DecodeJSONString(val.value);
			} else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
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
				if (key.value == "id")
					mat.id = DecodeJSONString(Expect(TokenType::String).value);
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

	void ParsePBR(IRMaterial& mat) {
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
			else
				SkipValue();
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
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
			if (key.value == "albedo" && !fileStr.empty())
				mat.albedoMap = fileStr;
			else if (key.value == "normal" && !fileStr.empty())
				mat.normalMap = fileStr;
			else if (key.value == "metallic_roughness" && !fileStr.empty())
				mat.metallicRoughnessMap = fileStr;
			if (!Peek(TokenType::EndObject))
				Expect(TokenType::Comma);
		}
	}

	void SkipValue() {
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
};
} // namespace Compiler

// ============================================================================
// CLI Subcommands
// ============================================================================

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
		return 1;
	}

	FILE* f = std::fopen(metaPath.c_str(), "rb");
	if (!f)
		return 1;
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

	auto it = std::find_if(manifest.meshes.begin(), manifest.meshes.end(),
						   [&](const auto& m) { return m.id == meshId; });
	if (it == manifest.meshes.end()) {
		std::println(stderr, "[zcook Error] Mesh ID '{}' not declared in {}", meshId, metaPath);
		return 1;
	}
	const auto& mesh = *it;

	FILE* bf = std::fopen(inPath.c_str(), "rb");
	if (!bf)
		return 1;
	std::fseek(bf, 0, SEEK_END);
	long binSize = std::ftell(bf);
	std::fseek(bf, 0, SEEK_SET);
	std::vector<char> rawBinBytes(binSize);
	std::fread(rawBinBytes.data(), 1, binSize, bf);
	std::fclose(bf);

	uint32_t vertexCount = mesh.vertexBuffer.byteLength / 64;

	if (mesh.vertexBuffer.byteOffset + mesh.vertexBuffer.byteLength >
		static_cast<uint32_t>(binSize)) {
		std::println(stderr,
					 "[zcook Error] Vertex buffer offset ({}) exceeds binary file size ({})",
					 mesh.vertexBuffer.byteOffset + mesh.vertexBuffer.byteLength, binSize);
		return 1;
	}

	const auto* floatVBO =
		reinterpret_cast<const float*>(rawBinBytes.data() + mesh.vertexBuffer.byteOffset);

	uint32_t indexCount = 0;
	std::vector<uint32_t> compiledIndices;

	if (mesh.indexBuffer.byteLength > 0) {
		if (mesh.indexBuffer.byteOffset + mesh.indexBuffer.byteLength >
			static_cast<uint32_t>(binSize)) {
			std::println(stderr, "[zcook Error] Index buffer offset exceeds binary file size!");
			return 1;
		}

		const auto* rawIndexData =
			reinterpret_cast<const uint8_t*>(rawBinBytes.data() + mesh.indexBuffer.byteOffset);
		bool is16Bit = (vertexCount <= 65535);

		if (is16Bit) {
			indexCount = mesh.indexBuffer.byteLength / 2;
			compiledIndices.resize(indexCount);
			const auto* indices16 = reinterpret_cast<const uint16_t*>(rawIndexData);
			for (uint32_t i = 0; i < indexCount; ++i) {
				compiledIndices[i] = static_cast<uint32_t>(indices16[i]);
			}
		} else {
			indexCount = mesh.indexBuffer.byteLength / 4;
			compiledIndices.resize(indexCount);
			const auto* indices32 = reinterpret_cast<const uint32_t*>(rawIndexData);
			std::memcpy(compiledIndices.data(), indices32, indexCount * sizeof(uint32_t));
		}
	}

	bool hasSkin = mesh.layout.contains("J4W4");
	const uint16_t* joints = nullptr;
	if (hasSkin && mesh.jointsBuffer.byteLength > 0) {
		if (mesh.jointsBuffer.byteOffset + mesh.jointsBuffer.byteLength >
			static_cast<uint32_t>(binSize)) {
			std::println(stderr, "[zcook Error] Joints buffer offset exceeds binary file size!");
			return 1;
		}
		joints =
			reinterpret_cast<const uint16_t*>(rawBinBytes.data() + mesh.jointsBuffer.byteOffset);
	}

	const float* weights = nullptr;
	if (hasSkin && mesh.weightsBuffer.byteLength > 0) {
		if (mesh.weightsBuffer.byteOffset + mesh.weightsBuffer.byteLength >
			static_cast<uint32_t>(binSize)) {
			std::println(stderr, "[zcook Error] Weights buffer offset exceeds binary file size!");
			return 1;
		}
		weights =
			reinterpret_cast<const float*>(rawBinBytes.data() + mesh.weightsBuffer.byteOffset);
	}

	std::vector<Vertex> compiledVertices(vertexCount);
	for (uint32_t i = 0; i < vertexCount; ++i) {
		Vertex& dest = compiledVertices[i];
		std::memset(&dest, 0, sizeof(Vertex));

		dest.position[0] = floatVBO[i * 16 + 0];
		dest.position[1] = floatVBO[i * 16 + 1];
		dest.position[2] = floatVBO[i * 16 + 2];

		dest.normal =
			Math::PackNormal(floatVBO[i * 16 + 3], floatVBO[i * 16 + 4], floatVBO[i * 16 + 5]);
		dest.tangent = Math::PackNormal(floatVBO[i * 16 + 6], floatVBO[i * 16 + 7],
										floatVBO[i * 16 + 8], floatVBO[i * 16 + 9]);
		dest.uv = Math::PackUV(floatVBO[i * 16 + 10], floatVBO[i * 16 + 11]);
		dest.color = Math::PackColor(floatVBO[i * 16 + 12], floatVBO[i * 16 + 13],
									 floatVBO[i * 16 + 14], floatVBO[i * 16 + 15]);

		if (hasSkin && joints && weights) {
			dest.joints[0] = joints[i * 4 + 0];
			dest.joints[1] = joints[i * 4 + 1];
			dest.joints[2] = joints[i * 4 + 2];
			dest.joints[3] = joints[i * 4 + 3];

			dest.weights[0] = weights[i * 4 + 0];
			dest.weights[1] = weights[i * 4 + 1];
			dest.weights[2] = weights[i * 4 + 2];
			dest.weights[3] = weights[i * 4 + 3];
		}
	}

	std::vector<Vertex> unrolledVertices;
	unrolledVertices.reserve(indexCount);
	for (uint32_t idx = 0; idx < indexCount; ++idx) {
		uint32_t originalIdx = compiledIndices[idx];
		if (originalIdx >= vertexCount) [[unlikely]] {
			std::println(stderr,
						 "[zcook Error] Index {} points to invalid vertex {} (Max count={})", idx,
						 originalIdx, vertexCount);
			return 1;
		}
		unrolledVertices.push_back(compiledVertices[originalIdx]);
	}

	float minB[3] = {1e30f, 1e30f, 1e30f};
	float maxB[3] = {-1e30f, -1e30f, -1e30f};
	for (const auto& v : unrolledVertices) {
		minB[0] = std::min(minB[0], v.position[0]);
		minB[1] = std::min(minB[1], v.position[1]);
		minB[2] = std::min(minB[2], v.position[2]);
		maxB[0] = std::max(maxB[0], v.position[0]);
		maxB[1] = std::max(maxB[1], v.position[1]);
		maxB[2] = std::max(maxB[2], v.position[2]);
	}

	CookedMeshHeader meshHeader{};
	meshHeader.magic = 0x3048534D; // 'MSH0'
	meshHeader.version = 2;
	meshHeader.boundingBoxMin[0] = minB[0];
	meshHeader.boundingBoxMin[1] = minB[1];
	meshHeader.boundingBoxMin[2] = minB[2];
	meshHeader.boundingBoxMax[0] = maxB[0];
	meshHeader.boundingBoxMax[1] = maxB[1];
	meshHeader.boundingBoxMax[2] = maxB[2];
	meshHeader.vertexCount = vertexCount;
	meshHeader.indexCount = indexCount;

	size_t vboSize = compiledVertices.size() * sizeof(Vertex);
	size_t iboSize = indexCount * sizeof(uint32_t);

	fs::create_directories(fs::path(outPath).parent_path());
	FILE* out = std::fopen(outPath.c_str(), "wb");
	if (!out)
		return 1;

	std::fwrite(&meshHeader, 1, sizeof(CookedMeshHeader), out);
	std::fwrite(compiledVertices.data(), 1, vboSize, out);
	std::fwrite(compiledIndices.data(), 1, iboSize, out);
	std::fclose(out);

	std::println("Compiled Mesh: {}", outPath);
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

	if (inPath.empty() || outPath.empty()) {
		std::println(stderr, "Usage: zcook tex -i <input.png> -o <out.ztex>");
		return 1;
	}

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
	std::fwrite(fileData.data(), 1, size, out);
	std::fclose(out);

	std::println("Compiled Texture: {}", outPath);
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

	if (outPath.empty() || manifestPath.empty()) {
		std::println(stderr, "Usage: zcook pak -i <manifest.txt> -o <out.pak>");
		return 1;
	}

	std::vector<PakEntry> entries;
	std::vector<char> payloadData;

	auto appendToPak = [&](std::string_view virtualPath, const void* data, size_t size) {
		size_t padding = (16 - (payloadData.size() % 16)) % 16;
		payloadData.insert(payloadData.end(), padding, 0);

		PakEntry entry{};
		entry.pathHash = HashAssetPath(virtualPath);
		entry.offset = sizeof(PakHeader) + payloadData.size();
		entry.compressedSize = size;
		entry.uncompressedSize = size;
		entry.compression = 0; // Zero-copy mapping

		entries.push_back(entry);
		const char* bytes = reinterpret_cast<const char*>(data);
		payloadData.insert(payloadData.end(), bytes, bytes + size);
	};

	std::ifstream ifs(manifestPath);
	std::string line;
	while (std::getline(ifs, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		auto pos = line.find('=');
		if (pos == std::string::npos)
			continue;

		std::string vpath = line.substr(0, pos);
		std::string rpath = line.substr(pos + 1);

		FILE* f = std::fopen(rpath.c_str(), "rb");
		if (!f) {
			std::println(stderr, "Warning: Could not open {}", rpath);
			continue;
		}
		std::fseek(f, 0, SEEK_END);
		long size = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		std::vector<char> fileData(size);
		std::fread(fileData.data(), 1, size, f);
		std::fclose(f);

		appendToPak(vpath, fileData.data(), size);
	}

	PakHeader header{};
	std::memcpy(header.magic, "ZPAK", 4);
	header.version = 1;
	header.entryCount = entries.size();
	header.tocOffset = sizeof(PakHeader) + payloadData.size();

	fs::create_directories(fs::path(outPath).parent_path());
	FILE* out = std::fopen(outPath.c_str(), "wb");
	if (!out)
		return 1;

	std::fwrite(&header, 1, sizeof(PakHeader), out);
	std::fwrite(payloadData.data(), 1, payloadData.size(), out);
	std::fwrite(entries.data(), 1, entries.size() * sizeof(PakEntry), out);
	std::fclose(out);

	std::println("Packed {} files into {}", entries.size(), outPath);
	return 0;
}

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	if (argc < 2) {
		std::println(stderr, "Zahlen Engine Asset Compiler (ZCook)");
		std::println(stderr, "Usage: zcook <command> [args]");
		std::println(stderr, "Commands:");
		std::println(stderr, "  mesh   Compile a single mesh");
		std::println(stderr, "  tex    Compile a single texture");
		std::println(stderr, "  pak    Pack assets into an archive");
		return 1;
	}

	std::string_view cmd = argv[1];
	if (cmd == "mesh")
		return CookMesh(argc - 2, argv + 2);
	if (cmd == "tex")
		return CookTexture(argc - 2, argv + 2);
	if (cmd == "pak")
		return PackArchive(argc - 2, argv + 2);

	std::println(stderr, "Unknown command: {}", cmd);
	return 1;
}
