// src/zcook/Lexer.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Lexer.hpp"

#include <cctype>

namespace {
inline bool IsDigit(char c) noexcept {
	return std::isdigit(static_cast<unsigned char>(c)) != 0;
}
inline bool IsAlpha(char c) noexcept {
	return std::isalpha(static_cast<unsigned char>(c)) != 0;
}
} // namespace

namespace Compiler {

const char* GetTokenTypeName(TokenType type) {
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

std::vector<Token> Lexer::Tokenize() {
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

char Lexer::Peek() const {
	return m_cursor >= m_source.length() ? '\0' : m_source[m_cursor];
}

char Lexer::Advance() {
	if (m_cursor >= m_source.length())
		return '\0';
	char c = m_source[m_cursor++];
	if (c == '\n')
		m_line++;
	return c;
}

void Lexer::SkipWhitespace() {
	while (true) {
		char c = Peek();
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			Advance();
		} else if (c == '/' && m_cursor + 1 < m_source.length() && m_source[m_cursor + 1] == '/') {
			while (Peek() != '\n' && Peek() != '\0')
				Advance();
		} else {
			break;
		}
	}
}

Token Lexer::NextToken() {
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

	if (IsDigit(c) || c == '-')
		return ParseNumber();
	if (IsAlpha(c))
		return ParseKeyword();

	Advance();
	return Token{
		.type = TokenType::Null, .value = m_source.substr(m_cursor - 1, 1), .line = startLine};
}

Token Lexer::ParseString() {
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

Token Lexer::ParseNumber() {
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
	if (Peek() == 'e' || Peek() == 'E') {
		Advance();
		if (Peek() == '+' || Peek() == '-')
			Advance();
		while (IsDigit(Peek()))
			Advance();
	}
	return Token{.type = TokenType::Number,
				 .value = m_source.substr(startIdx, m_cursor - startIdx),
				 .line = startLine};
}

Token Lexer::ParseKeyword() {
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

} // namespace Compiler
