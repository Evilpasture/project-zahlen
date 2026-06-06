// src/cooker_main.cpp
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Types.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdio> // For C FILE* APIs
#include <cstring>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace ZHLN;

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
		if (m_cursor >= m_source.length()) {
			return '\0';
		}
		return m_source[m_cursor];
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

		if (isdigit(c) || c == '-') {
			return ParseNumber();
		}
		if (isalpha(c)) {
			return ParseKeyword();
		}

		Advance();
		return Token{
			.type = TokenType::Null, .value = m_source.substr(m_cursor - 1, 1), .line = startLine};
	}

	Token ParseString() {
		size_t startLine = m_line;
		Advance(); // skip opening quote
		size_t startIdx = m_cursor;
		while (Peek() != '"' && Peek() != '\0') {
			if (Peek() == '\\') {
				Advance();
			}
			Advance();
		}
		size_t len = m_cursor - startIdx;
		Advance(); // skip closing quote
		return Token{
			.type = TokenType::String, .value = m_source.substr(startIdx, len), .line = startLine};
	}

	Token ParseNumber() {
		size_t startLine = m_line;
		size_t startIdx = m_cursor;
		if (Peek() == '-') {
			Advance();
		}
		while (isdigit(Peek())) {
			Advance();
		}
		if (Peek() == '.') {
			Advance();
			while (isdigit(Peek())) {
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
		while (isalpha(Peek())) {
			Advance();
		}
		std::string_view word = m_source.substr(startIdx, m_cursor - startIdx);
		if (word == "true") {
			return Token{.type = TokenType::True, .value = word, .line = startLine};
		}
		if (word == "false") {
			return Token{.type = TokenType::False, .value = word, .line = startLine};
		}
		if (word == "null") {
			return Token{.type = TokenType::Null, .value = word, .line = startLine};
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
	uint32_t indexOffset;
	uint32_t indexCount;
};

struct IRMesh {
	std::string id;
	std::string layout;
	std::string binFile;
	IRBuffer vertexBuffer;
	IRBuffer indexBuffer;
	IRBuffer jointsBuffer;
	IRBuffer weightsBuffer;
	std::vector<IRPrimitive> primitives;
};

struct IRNode {
	std::string id;
	std::string meshId;
	float matrix[16];
	bool visible = true;
};

struct IRMaterial {
	std::string id;
	std::string albedoMap;
	std::string normalMap;
	std::string metallicRoughnessMap;
	float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float metallic = 0.0f;
	float roughness = 0.5f;
};

struct IRManifest {
	std::string levelName;
	std::vector<IRMesh> meshes;
	std::vector<IRNode> nodes;
	std::vector<IRMaterial> materials;
};

class Parser {
  public:
	explicit Parser(const std::vector<Token>& tokens) : m_tokens(tokens) {}

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

	[[nodiscard]] Token Peek() const {
		if (m_cursor >= m_tokens.size()) {
			return Token{.type = TokenType::EndOfFile, .value = {}};
		}
		return m_tokens[m_cursor];
	}

	[[nodiscard]] bool Peek(TokenType type) const { return Peek().type == type; }

	Token Advance() {
		if (m_cursor >= m_tokens.size()) {
			return Token{.type = TokenType::EndOfFile, .value = {}};
		}
		return m_tokens[m_cursor++];
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
			throw std::runtime_error("Parser error on line " + std::to_string(tok.line) +
									 ": Expected token type " +
									 std::to_string(static_cast<int>(type)) + " but found '" +
									 std::string(tok.value) + "'");
		}
		return tok;
	}

	void ParseSceneInfo(IRManifest& manifest) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "name") {
				manifest.levelName = std::string(Expect(TokenType::String).value);
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
			mesh.id = std::string(Expect(TokenType::String).value);
		} else if (key.value == "layout") {
			mesh.layout = std::string(Expect(TokenType::String).value);
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
				mesh.binFile = std::string(Expect(TokenType::String).value);
			} else if (key.value == "vertex_buffer") {
				ParseBufferBound(mesh.vertexBuffer);
			} else if (key.value == "index_buffer") {
				ParseBufferBound(mesh.indexBuffer);
			} else if (key.value == "joints") {
				ParseBufferBound(mesh.jointsBuffer);
			} else if (key.value == "weights") {
				ParseBufferBound(mesh.weightsBuffer);
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
						prim.materialId = std::string(val.value);
					}
				} else if (key.value == "index_offset") {
					prim.indexOffset = std::stoul(std::string(Expect(TokenType::Number).value));
				} else if (key.value == "index_count") {
					prim.indexCount = std::stoul(std::string(Expect(TokenType::Number).value));
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
			std::memset(node.matrix, 0, sizeof(node.matrix));
			node.matrix[0] = node.matrix[5] = node.matrix[10] = node.matrix[15] =
				1.0f; // Default Identity

			Expect(TokenType::BeginObject);
			while (!Match(TokenType::EndObject)) {
				Token key = Expect(TokenType::String);
				Expect(TokenType::Colon);
				if (key.value == "id") {
					node.id = std::string(Expect(TokenType::String).value);
				} else if (key.value == "visible") {
					Token t = Advance();
					node.visible = (t.type == TokenType::True);
				} else if (key.value == "transform") {
					ParseTransform(node.matrix);
				} else if (key.value == "refs") {
					ParseRefs(node.meshId);
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

	void ParseTransform(float* outMatrix) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "local") {
				Expect(TokenType::BeginArray);
				for (int i = 0; i < 16; ++i) {
					outMatrix[i] = std::stof(std::string(Expect(TokenType::Number).value));
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

	void ParseRefs(std::string& outMeshId) {
		Expect(TokenType::BeginObject);
		while (!Match(TokenType::EndObject)) {
			Token key = Expect(TokenType::String);
			Expect(TokenType::Colon);
			if (key.value == "mesh_id") {
				Token val = Advance();
				if (val.type == TokenType::String) {
					outMeshId = std::string(val.value);
				}
			} else {
				SkipValue();
			}
			if (!Peek(TokenType::EndObject)) {
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
					mat.id = std::string(Expect(TokenType::String).value);
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
			std::string fileStr = "";
			if (val.type == TokenType::String) {
				fileStr = std::string(val.value);
			}
			if (key.value == "albedo" && !fileStr.empty()) {
				mat.albedoMap = fileStr;
			} else if (key.value == "normal" && !fileStr.empty()) {
				mat.normalMap = fileStr;
			} else if (key.value == "metallic_roughness" && !fileStr.empty()) {
				mat.metallicRoughnessMap = fileStr;
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
// Phase 3: High-Performance GLB Emitter (C-API Buffered I/O)
// ============================================================================
namespace GLB {

inline void EmitGLB(const Compiler::IRManifest& manifest, const std::string& levelFolder,
					const std::string& outputPath) {
	std::vector<uint8_t> binBuffer;
	binBuffer.reserve(16 * 1024 * 1024); // 16MB pre-allocated to avoid re-allocation spikes

	std::vector<std::string> bufferViews;
	std::vector<std::string> accessors;
	std::vector<std::string> meshesJson;
	std::vector<std::string> nodesJson;

	// Track the actual index assigned to the mesh in the output GLB
	std::unordered_map<std::string, int> meshIdToGlbIndex;

	uint32_t accIndex = 0;
	uint32_t bViewIndex = 0;

	// Gather PBR materials and bindless textures
	std::vector<std::string> images;
	std::vector<std::string> textures;
	std::vector<std::string> materialsJson;
	std::unordered_map<std::string, int> matIdToGlbIndex;

	struct PackedImage {
		std::string relativeUri;
		uint32_t bufferViewIndex;
	};
	std::vector<PackedImage> packedImages;

	// FIX: Pack raw texture bytes directly into GLB binary chunk as fully self-contained
	// bufferViews
	auto getTextureIndex = [&](const std::string& relativeUri) -> int {
		if (relativeUri.empty())
			return -1;

		for (size_t i = 0; i < packedImages.size(); ++i) {
			if (packedImages[i].relativeUri == relativeUri) {
				return static_cast<int>(i);
			}
		}

		std::string fullPath = levelFolder + "/" + relativeUri;
		FILE* f = std::fopen(fullPath.c_str(), "rb");
		if (f == nullptr) {
			std::println(stderr, "  [Warning] Failed to find texture for GLB packing: {}",
						 fullPath);
			return -1;
		}

		std::fseek(f, 0, SEEK_END);
		long size = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);

		std::vector<uint8_t> imgBytes(size);
		std::fread(imgBytes.data(), 1, size, f);
		std::fclose(f);

		// Align binBuffer to 4 bytes before inserting image bytes
		while (binBuffer.size() % 4 != 0) {
			binBuffer.push_back(0);
		}

		auto imgOffset = static_cast<uint32_t>(binBuffer.size());
		binBuffer.insert(binBuffer.end(), imgBytes.begin(), imgBytes.end());
		while (binBuffer.size() % 4 != 0) {
			binBuffer.push_back(0); // align after
		}

		std::string mimeType = "image/png";
		if (relativeUri.ends_with(".jpg") || relativeUri.ends_with(".jpeg")) {
			mimeType = "image/jpeg";
		}

		// Register a new bufferView for this image
		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {}
    }})",
										  imgOffset, size));
		uint32_t imgBViewIdx = bViewIndex++;

		int idx = static_cast<int>(packedImages.size());
		packedImages.push_back({.relativeUri = relativeUri, .bufferViewIndex = imgBViewIdx});

		// Register a texture pointing to this image source
		textures.push_back(std::format(R"(    {{"sampler": 0, "source": {}}})", idx));

		// Register image JSON using bufferView instead of loose file URIs
		images.push_back(
			std::format(R"(    {{"bufferView": {}, "mimeType": "{}"}})", imgBViewIdx, mimeType));

		return idx;
	};

	for (const auto& mat : manifest.materials) {
		int albedoTex = getTextureIndex(mat.albedoMap);
		int normalTex = getTextureIndex(mat.normalMap);
		int mrTex = getTextureIndex(mat.metallicRoughnessMap);

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

		matStr += R"(
    })";
		matIdToGlbIndex[mat.id] = static_cast<int>(materialsJson.size());
		materialsJson.push_back(matStr);
	}

	for (const auto& mesh : manifest.meshes) {
		std::string binPath = levelFolder + "/" + mesh.binFile;

		// Use high-speed raw C FILE* instead of fstream
		FILE* bf = std::fopen(binPath.c_str(), "rb");
		if (bf == nullptr) {
			continue;
		}

		std::fseek(bf, 0, SEEK_END);
		long binSize = std::ftell(bf);
		std::fseek(bf, 0, SEEK_SET);

		std::vector<uint8_t> rawBin(binSize);
		std::fread(rawBin.data(), 1, binSize, bf);
		std::fclose(bf);

		meshIdToGlbIndex[mesh.id] = static_cast<int>(meshesJson.size());

		// 1. Pack VBO slice (Now 64 bytes instead of 48)
		auto vboOffset = static_cast<uint32_t>(binBuffer.size());
		binBuffer.insert(binBuffer.end(), rawBin.begin() + mesh.vertexBuffer.byteOffset,
						 rawBin.begin() + mesh.vertexBuffer.byteOffset +
							 mesh.vertexBuffer.byteLength);
		while (binBuffer.size() % 4 != 0) {
			binBuffer.push_back(0); // 4-byte padding
		}

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "byteStride": 64,
      "target": 34962
    }})",
										  vboOffset, mesh.vertexBuffer.byteLength));
		uint32_t vboBViewIdx = bViewIndex++;

		// 2. Pack IBO slice
		auto iboOffset = static_cast<uint32_t>(binBuffer.size());
		binBuffer.insert(binBuffer.end(), rawBin.begin() + mesh.indexBuffer.byteOffset,
						 rawBin.begin() + mesh.indexBuffer.byteOffset +
							 mesh.indexBuffer.byteLength);
		while (binBuffer.size() % 4 != 0) {
			binBuffer.push_back(0);
		}

		bufferViews.push_back(std::format(R"(    {{
      "buffer": 0,
      "byteOffset": {},
      "byteLength": {},
      "target": 34963
    }})",
										  iboOffset, mesh.indexBuffer.byteLength));
		uint32_t iboBViewIdx = bViewIndex++;

		// Accurately deduce loop-indexed attribute counts using 64-byte intermediate stride
		uint32_t vertexCount = mesh.vertexBuffer.byteLength / 64;

		// Generate glTF 2.0 Accessor declarations
		uint32_t posAcc = accIndex++;
		uint32_t normAcc = accIndex++;
		uint32_t tangAcc = accIndex++;
		uint32_t uvAcc = accIndex++;
		uint32_t colorAcc = accIndex++; // Added color accessor index

		// Calculate spatial boundaries (min/max) using 16-float (64B) indexing
		float minB[3] = {1e30f, 1e30f, 1e30f};
		float maxB[3] = {-1e30f, -1e30f, -1e30f};
		const float* floatVBO =
			reinterpret_cast<const float*>(rawBin.data() + mesh.vertexBuffer.byteOffset);
		for (uint32_t i = 0; i < vertexCount; ++i) {
			float x = floatVBO[i * 16 + 0];
			float y = floatVBO[i * 16 + 1];
			float z = floatVBO[i * 16 + 2];
			minB[0] = std::min(minB[0], x);
			minB[1] = std::min(minB[1], y);
			minB[2] = std::min(minB[2], z);
			maxB[0] = std::max(maxB[0], x);
			maxB[1] = std::max(maxB[1], y);
			maxB[2] = std::max(maxB[2], z);
		}

		accessors.push_back(std::format(R"(    {{"bufferView": {},
      "componentType": 5126,
      "count": {},
      "type": "VEC3",
      "min": [{}, {}, {}],
      "max": [{}, {}, {}]}})",
										vboBViewIdx, vertexCount, minB[0], minB[1], minB[2],
										maxB[0], maxB[1], maxB[2]));

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
										vboBViewIdx, vertexCount)); // COLOR_0

		std::string primsStr = "";
		for (size_t p = 0; p < mesh.primitives.size(); ++p) {
			const auto& prim = mesh.primitives[p];
			uint32_t indexAcc = accIndex++;

			accessors.push_back(std::format(R"(    {{"bufferView": {},
      "byteOffset": {},
      "componentType": 5125,
      "count": {},
      "type": "SCALAR"}})",
											iboBViewIdx, prim.indexOffset, prim.indexCount));

			int matGlbIdx = -1;
			auto it = matIdToGlbIndex.find(prim.materialId);
			if (it != matIdToGlbIndex.end()) {
				matGlbIdx = it->second;
			}

			std::string matStr = "";
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
			if (p < mesh.primitives.size() - 1) {
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

	if (meshesJson.empty()) {
		return;
	}

	// Generate glTF Node mappings using the correctly mapped GLB indices
	for (const auto& node : manifest.nodes) {
		if (!node.visible) {
			continue;
		}

		int meshIndex = -1;
		auto it = meshIdToGlbIndex.find(node.meshId);
		if (it != meshIdToGlbIndex.end()) {
			meshIndex = it->second;
		}

		std::string matrixStr = "[";
		for (int i = 0; i < 16; ++i) {
			matrixStr += std::to_string(node.matrix[i]);
			if (i < 15)
				matrixStr += ", ";
		}
		matrixStr += "]";

		std::string meshStr = "";
		if (meshIndex != -1) {
			meshStr = std::format(",\n      \"mesh\": {}", meshIndex);
		}

		nodesJson.push_back(std::format(R"(    {{
      "name": "{}",
      "matrix": {}{}
    }})",
										node.id, matrixStr, meshStr));
	}

	// Backward Compatibility Fallback
	if (nodesJson.empty()) {
		for (size_t i = 0; i < manifest.meshes.size(); ++i) {
			nodesJson.push_back(std::format(R"(    {{
      "name": "Node_{}",
      "mesh": {}
    }})",
											manifest.meshes[i].id, i));
		}
	}

	// High-Performance string allocation (zero stringstream overhead)
	std::string json;
	json.reserve(static_cast<size_t>(128 * 1024)); // 128KB
	json.append(R"({
  "asset": {"version": "2.0", "generator": "Zahlen GLB Emitter"},
)");

	// BufferViews Section
	json.append("  \"bufferViews\": [\n");
	for (size_t i = 0; i < bufferViews.size(); ++i) {
		json.append(bufferViews[i]);
		json.append(i < bufferViews.size() - 1 ? ",\n" : "\n");
	}
	json.append("  ],\n");

	// Accessors Section
	json.append("  \"accessors\": [\n");
	for (size_t i = 0; i < accessors.size(); ++i) {
		json.append(accessors[i]);
		json.append(i < accessors.size() - 1 ? ",\n" : "\n");
	}
	json.append("  ],\n");

	// Materials Section
	if (!materialsJson.empty()) {
		json.append("  \"materials\": [\n");
		for (size_t i = 0; i < materialsJson.size(); ++i) {
			json.append(materialsJson[i]);
			json.append(i < materialsJson.size() - 1 ? ",\n" : "\n");
		}
		json.append("  ],\n");
	}

	// Textures Section
	if (!textures.empty()) {
		json.append("  \"textures\": [\n");
		for (size_t i = 0; i < textures.size(); ++i) {
			json.append(textures[i]);
			json.append(i < textures.size() - 1 ? ",\n" : "\n");
		}
		json.append("  ],\n");
	}

	// Images Section
	if (!images.empty()) {
		json.append("  \"images\": [\n");
		for (size_t i = 0; i < images.size(); ++i) {
			json.append(images[i]);
			json.append(i < images.size() - 1 ? ",\n" : "\n");
		}
		json.append("  ],\n");

		// Samplers Section (Required if we reference samplers)
		json.append(R"(  "samplers": [
    {"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497}
  ],
)");
	}

	// Meshes Section
	json.append("  \"meshes\": [\n");
	for (size_t i = 0; i < meshesJson.size(); ++i) {
		json.append(meshesJson[i]);
		json.append(i < meshesJson.size() - 1 ? ",\n" : "\n");
	}
	json.append("  ],\n");

	// Nodes Section
	json.append("  \"nodes\": [\n");
	for (size_t i = 0; i < nodesJson.size(); ++i) {
		json.append(nodesJson[i]);
		json.append(i < nodesJson.size() - 1 ? ",\n" : "\n");
	}
	json.append("  ],\n");

	// Scene Hierarchies
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

	// Buffers Section
	json.append(std::format(R"(  "buffers": [
    {{
      "byteLength": {}
    }}
  ]
}})",
							binBuffer.size()));

	// Pad JSON string with spaces to maintain strict 4-byte GLB chunk bounds
	while (json.length() % 4 != 0) {
		json += ' ';
	}

	auto jsonChunkLength = static_cast<uint32_t>(json.length());
	auto binChunkLength = static_cast<uint32_t>(binBuffer.size());
	uint32_t totalFileLength = 12 + 8 + jsonChunkLength + 8 + binChunkLength;

	// Pure C-API File write (No streams)
	FILE* out = std::fopen(outputPath.c_str(), "wb");
	if (out == nullptr) {
		std::println(stderr, "  [Error] Failed to open GLB output: {}", outputPath);
		return;
	}

	// 12-byte GLB Container Header
	// FIX: Use 0x46546C67 (lowercase 'l') to match standard glTF specification
	uint32_t magic = 0x46546C67; // "glTF"
	uint32_t version = 2;
	std::fwrite(&magic, 1, 4, out);
	std::fwrite(&version, 1, 4, out);
	std::fwrite(&totalFileLength, 1, 4, out);

	// JSON Chunk (Chunk 0)
	uint32_t chunkTypeJson = 0x4E4F534A; // "JSON"
	std::fwrite(&jsonChunkLength, 1, 4, out);
	std::fwrite(&chunkTypeJson, 1, 4, out);
	std::fwrite(json.data(), 1, jsonChunkLength, out);

	// BIN Chunk (Chunk 1)
	uint32_t chunkTypeBin = 0x004E4942; // "BIN"
	std::fwrite(&binChunkLength, 1, 4, out);
	std::fwrite(&chunkTypeBin, 1, 4, out);
	std::fwrite(binBuffer.data(), 1, binChunkLength, out);

	std::fclose(out);
	std::println("  - Compiled Validated GLB: {} ({} bytes)",
				 fs::path(outputPath).filename().string(), totalFileLength);
}

} // namespace GLB

int main(int argc, char** argv) {
	std::println("=========================================================");
	std::println("Zahlen Engine Asset Compiler (C++23 Native Pipeline)");
	std::println("=========================================================");

	if (argc < 3) {
		std::println(stderr, "Usage: zahlen_cooker <input_dir> <output.pak> [output_debug_dir]");
		return 1;
	}

	std::string inputDir = argv[1];
	std::string outputFile = argv[2];

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
		entry.compression = 0; // Zero-copy native asset mapping

		entries.push_back(entry);

		const char* bytes = reinterpret_cast<const char*>(data);
		payloadData.insert(payloadData.end(), bytes, bytes + size);
	};

	// 1. Scan the raw assets folder for legacy support & loose textures
	for (const auto& entry : fs::recursive_directory_iterator(inputDir)) {
		if (entry.is_directory()) {
			continue;
		}

		std::string filepath = entry.path().string();
		std::string ext = entry.path().extension().string();

		std::string virtualPath = filepath.substr(inputDir.length());
		if (virtualPath[0] == '/' || virtualPath[0] == '\\') {
			virtualPath = virtualPath.substr(1);
		}
		std::replace(virtualPath.begin(), virtualPath.end(), '\\', '/');

		// Copy raw image files straight into the PAK using C-style buffered reading
		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
			FILE* f = std::fopen(filepath.c_str(), "rb");
			if (f == nullptr) {
				continue;
			}

			std::fseek(f, 0, SEEK_END);
			long size = std::ftell(f);
			std::fseek(f, 0, SEEK_SET);
			std::vector<char> fileData(size);
			std::fread(fileData.data(), 1, size, f);
			std::fclose(f);

			appendToPak(virtualPath, fileData.data(), size);
			std::println("  - Packed Raw Texture: {} ({} bytes)", virtualPath, size);
		}
	}

	// 2. Discover and compile intermediate Blender exports
	std::string intermediateRoot = "resources/intermediate";
	if (fs::exists(intermediateRoot)) {
		for (const auto& levelEntry : fs::directory_iterator(intermediateRoot)) {
			if (!levelEntry.is_directory()) {
				continue;
			}

			std::string levelName = levelEntry.path().filename().string();
			std::string metadataPath = levelEntry.path().string() + "/metadata.json";

			if (!fs::exists(metadataPath)) {
				continue;
			}

			std::println("Compiling Intermediate Level: {}...", levelName);

			// Read the manifest file securely using C FILE*
			FILE* f = std::fopen(metadataPath.c_str(), "rb");
			if (f == nullptr) {
				continue;
			}
			std::fseek(f, 0, SEEK_END);
			long size = std::ftell(f);
			std::fseek(f, 0, SEEK_SET);
			std::string source(size, '\0');
			std::fread(source.data(), 1, size, f);
			std::fclose(f);

			// Run compiler front-end (Lexer & Parser)
			Compiler::Lexer lexer(source);
			std::vector<Compiler::Token> tokens = lexer.Tokenize();
			Compiler::Parser parser(tokens);
			Compiler::IRManifest manifest = parser.Parse();

			// 3. Compile geometries referenced in the level
			for (const auto& mesh : manifest.meshes) {
				std::string binPath = levelEntry.path().string() + "/" + mesh.binFile;
				FILE* bf = std::fopen(binPath.c_str(), "rb");
				if (bf == nullptr) {
					std::println(stderr, "  [Error] Failed to read geometry binary: {}", binPath);
					continue;
				}

				std::fseek(bf, 0, SEEK_END);
				long binSize = std::ftell(bf);
				std::fseek(bf, 0, SEEK_SET);
				std::vector<char> rawBinBytes(binSize);
				std::fread(rawBinBytes.data(), 1, binSize, bf);
				std::fclose(bf);

				// Extract VBO boundaries using 64-byte intermediate stride
				uint32_t vertexCount = mesh.vertexBuffer.byteLength / 64;
				const auto* floatVBO = reinterpret_cast<const float*>(rawBinBytes.data() +
																	  mesh.vertexBuffer.byteOffset);

				// Extract IBO boundaries (Detect and convert 16-bit or 32-bit indices)
				uint32_t indexCount = 0;
				std::vector<uint32_t> compiledIndices;

				if (mesh.indexBuffer.byteLength > 0) {
					const auto* rawIndexData = reinterpret_cast<const uint8_t*>(
						rawBinBytes.data() + mesh.indexBuffer.byteOffset);

					// Heuristic: standard tools use 16-bit (2 bytes per index) if the vertex count
					// fits in a uint16_t
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
						std::memcpy(compiledIndices.data(), indices32,
									indexCount * sizeof(uint32_t));
					}
				}

				// Extract skinning weights if present in compiled mesh layouts
				bool hasSkin = mesh.layout.contains("J4W4");
				const uint16_t* joints =
					hasSkin ? reinterpret_cast<const uint16_t*>(rawBinBytes.data() +
																mesh.jointsBuffer.byteOffset)
							: nullptr;
				const float* weights = hasSkin
										   ? reinterpret_cast<const float*>(
												 rawBinBytes.data() + mesh.weightsBuffer.byteOffset)
										   : nullptr;

				// Transpile vertices from unoptimized intermediate float32 blocks (64B) into
				// GPU-native Vertex (64B)
				std::vector<Vertex> compiledVertices(vertexCount);
				for (uint32_t i = 0; i < vertexCount; ++i) {
					Vertex& dest = compiledVertices[i];
					std::memset(&dest, 0, sizeof(Vertex));

					// Position (Float3) - index 0..2
					dest.position[0] = floatVBO[i * 16 + 0];
					dest.position[1] = floatVBO[i * 16 + 1];
					dest.position[2] = floatVBO[i * 16 + 2];

					// Normal (Pack float3 -> 10-10-10-2) - index 3..5
					dest.normal = Math::PackNormal(floatVBO[i * 16 + 3], floatVBO[i * 16 + 4],
												   floatVBO[i * 16 + 5]);

					// Tangent (Pack float4 -> 10-10-10-2) - index 6..9
					dest.tangent = Math::PackNormal(floatVBO[i * 16 + 6], floatVBO[i * 16 + 7],
													floatVBO[i * 16 + 8], floatVBO[i * 16 + 9]);

					// UV (Pack float2 -> half2) - index 10..11
					dest.uv = Math::PackUV(floatVBO[i * 16 + 10], floatVBO[i * 16 + 11]);

					// Color (Pack float4 -> RGBA8) - index 12..15
					dest.color = Math::PackColor(floatVBO[i * 16 + 12], floatVBO[i * 16 + 13],
												 floatVBO[i * 16 + 14], floatVBO[i * 16 + 15]);

					// Joint bindings
					if (hasSkin && (joints != nullptr) && (weights != nullptr)) {
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

				// Unroll indices on-the-fly to support the engine's unindexed renderer
				std::vector<Vertex> unrolledVertices;
				unrolledVertices.reserve(indexCount);
				for (uint32_t idx = 0; idx < indexCount; ++idx) {
					uint32_t originalIdx = compiledIndices[idx]; // Using safe 32-bit index list
					unrolledVertices.push_back(compiledVertices[originalIdx]);
				}

				// Compute bounding box coordinates
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

				// Set up the final Engine MSH0 payload using index arrays
				CookedMeshHeader meshHeader{};
				meshHeader.magic = 0x3048534D; // 'MSH0'
				meshHeader.version = 2;		   // Version 2 supports native index buffers
				meshHeader.boundingBoxMin[0] = minB[0];
				meshHeader.boundingBoxMin[1] = minB[1];
				meshHeader.boundingBoxMin[2] = minB[2];
				meshHeader.boundingBoxMax[0] = maxB[0];
				meshHeader.boundingBoxMax[1] = maxB[1];
				meshHeader.boundingBoxMax[2] = maxB[2];
				meshHeader.vertexCount = vertexCount; // Raw split-VBO vertex count
				meshHeader.indexCount = indexCount;	  // Raw IBO index count

				// Compile final payload structure: Header + VBO + IBO
				size_t vboSize = compiledVertices.size() * sizeof(Vertex);
				size_t iboSize = indexCount * sizeof(uint32_t);
				size_t cookedSize = sizeof(CookedMeshHeader) + vboSize + iboSize;

				std::vector<char> cookedData(cookedSize);
				std::memcpy(cookedData.data(), &meshHeader, sizeof(CookedMeshHeader));
				std::memcpy(cookedData.data() + sizeof(CookedMeshHeader), compiledVertices.data(),
							vboSize);
				std::memcpy(cookedData.data() + sizeof(CookedMeshHeader) + vboSize,
							compiledIndices.data(),
							iboSize); // Writes safe 32-bit index list
									  //  Append .zmesh reference to PAK
				std::string meshVirtualPath = mesh.id + ".zmesh";
				appendToPak(meshVirtualPath, cookedData.data(), cookedSize);
				std::println("  - Compiled Indexed Mesh: {} ({} vertices, {} indices)",
							 meshVirtualPath, vertexCount, indexCount);
			}

			// 4. Extract and copy level texture folders to PAK
			std::string texturesPath = levelEntry.path().string() + "/textures";
			if (fs::exists(texturesPath)) {
				for (const auto& texEntry : fs::directory_iterator(texturesPath)) {
					if (texEntry.is_directory()) {
						continue;
					}

					std::string tPath = texEntry.path().string();
					FILE* tFile = std::fopen(tPath.c_str(), "rb");
					if (tFile == nullptr) {
						continue;
					}

					std::fseek(tFile, 0, SEEK_END);
					long size = std::ftell(tFile);
					std::fseek(tFile, 0, SEEK_SET);
					std::vector<char> tData(size);
					std::fread(tData.data(), 1, size, tFile);
					std::fclose(tFile);

					std::string tVirtual = "textures/" + texEntry.path().filename().string();
					appendToPak(tVirtual, tData.data(), size);
					std::println("  - Packed Level Texture: {} ({} bytes)", tVirtual, size);
				}
			}

			// 5. Emit fully validated glTF 2.0 GLB for testing/debugging in Babylon.js [1.1]
			if (argc >= 4) {
				std::string outGlbFolder = argv[3];
				std::string glbPath = outGlbFolder + "/" + levelName + ".glb";
				GLB::EmitGLB(manifest, levelEntry.path().string(), glbPath);
			}
		}
	}

	// 6. Build and serialize the final Pak file structure using C-style buffered I/O [1.1]
	PakHeader header{};
	std::memcpy(header.magic, "ZPAK", 4);
	header.version = 1;
	header.entryCount = entries.size();
	header.tocOffset = sizeof(PakHeader) + payloadData.size();

	// Check directory path bounds
	std::filesystem::path outPath(outputFile);
	if (outPath.has_parent_path()) {
		fs::create_directories(outPath.parent_path());
	}

	FILE* out = std::fopen(outputFile.c_str(), "wb");
	if (out == nullptr) {
		std::println(stderr, "Compiler Error: Failed to open output write target: {}", outputFile);
		return 1;
	}

	std::fwrite(&header, 1, sizeof(PakHeader), out);
	std::fwrite(payloadData.data(), 1, payloadData.size(), out);
	std::fwrite(entries.data(), 1, entries.size() * sizeof(PakEntry), out);
	std::fclose(out);

	std::println("\n=========================================================");
	std::println("Successfully cooked {} assets into {}", entries.size(), outputFile);
	std::println("=========================================================");

	return 0;
}
