// src/zcook/Parser.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "IR.hpp"

#include <string_view>
#include <vector>

namespace Compiler {

class Parser {
  public:
	explicit Parser(const std::vector<Token>& tokens, std::string_view source)
		: m_tokens(tokens), m_source(source) {}
	IRManifest Parse();

  private:
	const std::vector<Token>& m_tokens;
	size_t m_cursor = 0;
	std::string_view m_source;

	[[nodiscard]] Token Peek() const;
	[[nodiscard]] bool Peek(TokenType type) const;
	Token Advance();
	bool Match(TokenType type);
	Token Expect(TokenType type);

	void ParseSceneInfo(IRManifest& manifest);
	void ParseMeshes(IRManifest& manifest);
	bool matchMeshField(IRMesh& mesh);
	void ParseBuffers(IRMesh& mesh);
	void ParseBufferBound(IRBuffer& buf);
	void ParsePrimitives(IRMesh& mesh);
	void ParseNodes(IRManifest& manifest);
	void ParseTransform(IRNode& node);
	void ParseRefs(IRNode& node);
	void ParseLights(IRManifest& manifest);
	void ParseMaterials(IRManifest& manifest);
	void ParsePBR(IRMaterial& mat);
	void ParseMaps(IRMaterial& mat);
	void SkipValue();
	void ParseAnimations(IRManifest& manifest);
	void ParseAnimChannels(IRAnimation& anim);
	void ParseAnimSamplers(IRAnimation& anim);
	void ParseSkins(IRManifest& manifest);
};

} // namespace Compiler
