// src/zcook/Lexer.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "IR.hpp"

#include <string_view>
#include <vector>

namespace Compiler {

class Lexer {
  public:
	explicit Lexer(std::string_view source) : m_source(source) {}
	std::vector<Token> Tokenize();

  private:
	std::string_view m_source;
	size_t m_cursor = 0;
	size_t m_line = 1;

	[[nodiscard]] char Peek() const;
	char Advance();
	void SkipWhitespace();
	Token NextToken();
	Token ParseString();
	Token ParseNumber();
	Token ParseKeyword();
};

} // namespace Compiler
