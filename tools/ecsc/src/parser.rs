// tools/ecsc/src/parser.rs
// Copyright (C) 2026 Evilpasture | SPDX-License-Identifier: GPL-3.0-or-later

#![allow(
    clippy::upper_case_acronyms,
    clippy::collapsible_match,
    clippy::collapsible_if
)]

use crate::ast::*;
use std::fmt;

#[derive(Debug, Clone)]
pub struct ParseError {
    pub line: usize,
    pub col: usize,
    pub message: String,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Line {}:{}: {}", self.line, self.col, self.message)
    }
}

impl std::error::Error for ParseError {}

#[derive(Debug, Clone, PartialEq)]
pub enum TokenKind {
    Ident(String),
    Symbol(char),
    Operator(String),
    Number(String),
    StringLit(String),
    Annotation(String),
    Eof,
}

#[derive(Debug, Clone)]
pub struct Token {
    pub kind: TokenKind,
    pub line: usize,
    pub col: usize,
}

pub struct Lexer {
    chars: Vec<char>,
    pos: usize,
    line: usize,
    col: usize,
}

impl Lexer {
    pub fn new(input: &str) -> Self {
        Self {
            chars: input.chars().collect(),
            pos: 0,
            line: 1,
            col: 1,
        }
    }

    fn peek(&self) -> Option<char> {
        self.chars.get(self.pos).copied()
    }

    fn advance(&mut self) -> Option<char> {
        if let Some(ch) = self.peek() {
            self.pos += 1;
            if ch == '\n' {
                self.line += 1;
                self.col = 1;
            } else {
                self.col += 1;
            }
            Some(ch)
        } else {
            None
        }
    }

    pub fn tokenize(&mut self) -> Result<Vec<Token>, ParseError> {
        let mut tokens = Vec::new();

        while let Some(ch) = self.peek() {
            if ch.is_whitespace() {
                self.advance();
                continue;
            }

            // Comments
            if ch == '/' {
                self.advance();
                if let Some('/') = self.peek() {
                    while let Some(c) = self.peek() {
                        self.advance();
                        if c == '\n' {
                            break;
                        }
                    }
                    continue;
                } else if let Some('*') = self.peek() {
                    self.advance();
                    while let Some(c) = self.advance() {
                        if c == '*' && self.peek() == Some('/') {
                            self.advance();
                            break;
                        }
                    }
                    continue;
                }
            }

            let line = self.line;
            let col = self.col;

            if ch == '@' {
                self.advance();
                let mut name = String::new();
                while let Some(c) = self.peek() {
                    if c.is_alphanumeric() || c == '_' {
                        name.push(c);
                        self.advance();
                    } else {
                        break;
                    }
                }
                tokens.push(Token {
                    kind: TokenKind::Annotation(name),
                    line,
                    col,
                });
            } else if ch.is_alphabetic() || ch == '_' {
                let mut ident = String::new();
                while let Some(c) = self.peek() {
                    if c.is_alphanumeric() || c == '_' {
                        ident.push(c);
                        self.advance();
                    } else {
                        break;
                    }
                }
                tokens.push(Token {
                    kind: TokenKind::Ident(ident),
                    line,
                    col,
                });
            // Simplified using .is_some_and()
            } else if ch.is_numeric()
                || (ch == '-' && self.chars.get(self.pos + 1).is_some_and(|c| c.is_numeric()))
            {
                let mut num = String::new();
                num.push(self.advance().unwrap());
                while let Some(c) = self.peek() {
                    if c.is_numeric() || c == '.' || c == 'f' {
                        num.push(c);
                        self.advance();
                    } else {
                        break;
                    }
                }
                tokens.push(Token {
                    kind: TokenKind::Number(num),
                    line,
                    col,
                });
            } else if ch == '"' {
                self.advance();
                let mut str_val = String::new();
                while let Some(c) = self.peek() {
                    self.advance();
                    if c == '"' {
                        break;
                    }
                    str_val.push(c);
                }
                tokens.push(Token {
                    kind: TokenKind::StringLit(str_val),
                    line,
                    col,
                });
            } else if "{}().;,".contains(ch) {
                self.advance();
                tokens.push(Token {
                    kind: TokenKind::Symbol(ch),
                    line,
                    col,
                });
            } else if "+-*/=<>!".contains(ch) {
                let mut op = String::new();
                op.push(self.advance().unwrap());
                if let Some(next) = self.peek() {
                    if next == '=' {
                        op.push(self.advance().unwrap());
                    }
                }
                tokens.push(Token {
                    kind: TokenKind::Operator(op),
                    line,
                    col,
                });
            } else {
                return Err(ParseError {
                    line,
                    col,
                    message: format!("Unexpected character: '{}'", ch),
                });
            }
        }

        tokens.push(Token {
            kind: TokenKind::Eof,
            line: self.line,
            col: self.col,
        });
        Ok(tokens)
    }
}

pub struct Parser {
    tokens: Vec<Token>,
    cursor: usize,
    pub ast: AST,
}

impl Parser {
    pub fn new(tokens: Vec<Token>) -> Self {
        Self {
            tokens,
            cursor: 0,
            ast: AST::default(),
        }
    }

    fn peek(&self) -> &Token {
        self.tokens
            .get(self.cursor)
            .unwrap_or(&self.tokens[self.tokens.len() - 1])
    }

    fn advance(&mut self) -> &Token {
        let idx = self.cursor;
        if self.cursor < self.tokens.len() - 1 {
            self.cursor += 1;
        }
        &self.tokens[idx]
    }

    fn expect_ident(&mut self) -> Result<(String, usize), ParseError> {
        let tok = self.advance().clone();
        if let TokenKind::Ident(name) = tok.kind {
            Ok((name, tok.line))
        } else {
            Err(ParseError {
                line: tok.line,
                col: tok.col,
                message: format!("Expected identifier, found {:?}", tok.kind),
            })
        }
    }

    fn expect_symbol(&mut self, sym: char) -> Result<(), ParseError> {
        let tok = self.advance().clone();
        if tok.kind == TokenKind::Symbol(sym) {
            Ok(())
        } else {
            Err(ParseError {
                line: tok.line,
                col: tok.col,
                message: format!("Expected '{}', found {:?}", sym, tok.kind),
            })
        }
    }

    pub fn parse(&mut self) -> Result<AST, ParseError> {
        while self.peek().kind != TokenKind::Eof {
            let tok = self.peek().clone();
            if let TokenKind::Ident(ref kw) = tok.kind {
                match kw.as_str() {
                    "component" | "property" => {
                        self.advance();
                        let comp = self.parse_component()?;
                        self.ast.components.push(comp);
                    }
                    "entity" => {
                        self.advance();
                        let ent = self.parse_entity()?;
                        self.ast.entities.push(ent);
                    }
                    "system" => {
                        self.advance();
                        let sys = self.parse_system()?;
                        self.ast.systems.push(sys);
                    }
                    _ => {
                        self.advance();
                    }
                }
            } else {
                self.advance();
            }
        }
        Ok(self.ast.clone())
    }

    fn parse_annotations(&mut self) -> Result<Annotation, ParseError> {
        let mut ann = Annotation::default();
        while let TokenKind::Annotation(ref name) = self.peek().kind.clone() {
            let ann_name = name.clone();
            self.advance();
            self.expect_symbol('(')?;

            let mut args = Vec::new();
            while self.peek().kind != TokenKind::Symbol(')') {
                let tok = self.advance().clone();
                match tok.kind {
                    TokenKind::Ident(s) | TokenKind::Number(s) | TokenKind::StringLit(s) => {
                        args.push(s)
                    }
                    _ => {}
                }
                if self.peek().kind == TokenKind::Symbol(',') {
                    self.advance();
                }
            }
            self.expect_symbol(')')?;

            match ann_name.as_str() {
                "range" => {
                    if args.len() >= 2 {
                        ann.min_val = Some(args[0].clone());
                        ann.max_val = Some(args[1].clone());
                    }
                    if args.len() >= 3 {
                        ann.step = Some(args[2].clone());
                    }
                }
                "label" => {
                    if !args.is_empty() {
                        ann.label = Some(args[0].clone());
                    }
                }
                "tooltip" => {
                    if !args.is_empty() {
                        ann.tooltip = Some(args[0].clone());
                    }
                }
                _ => {}
            }
        }
        Ok(ann)
    }

    fn parse_component(&mut self) -> Result<ComponentDef, ParseError> {
        let (name, line) = self.expect_ident()?;
        self.expect_symbol('{')?;

        let mut fields = Vec::new();
        while self.peek().kind != TokenKind::Symbol('}') {
            let ann = self.parse_annotations()?;
            let (f_type, f_line) = self.expect_ident()?;
            let (f_name, _) = self.expect_ident()?;

            let mut default_val = None;
            if let TokenKind::Operator(ref op) = self.peek().kind {
                if op == "=" {
                    self.advance();
                    let tok = self.advance().clone();
                    default_val = match tok.kind {
                        TokenKind::Number(v) | TokenKind::Ident(v) | TokenKind::StringLit(v) => {
                            Some(v)
                        }
                        _ => None,
                    };
                }
            }

            if self.peek().kind == TokenKind::Symbol(';') {
                self.advance();
            }

            fields.push(FieldDef {
                type_name: f_type,
                name: f_name,
                default_value: default_val,
                annotation: ann,
                line: f_line,
            });
        }
        self.expect_symbol('}')?;
        Ok(ComponentDef { name, fields, line })
    }

    fn parse_entity(&mut self) -> Result<EntityDef, ParseError> {
        let (name, line) = self.expect_ident()?;
        self.expect_symbol('{')?;

        let mut components = Vec::new();
        let mut uses = Vec::new();
        let mut behaviors = Vec::new();

        while self.peek().kind != TokenKind::Symbol('}') {
            let tok = self.peek().clone();
            if let TokenKind::Ident(ref kw) = tok.kind {
                match kw.as_str() {
                    "property" | "component" => {
                        self.advance();
                        let comp = self.parse_component()?;
                        components.push(comp.clone());
                        self.ast.components.push(comp);
                    }
                    "use" => {
                        self.advance();
                        let (use_name, _) = self.expect_ident()?;
                        uses.push(use_name);
                        if self.peek().kind == TokenKind::Symbol(';') {
                            self.advance();
                        }
                    }
                    "behavior" => {
                        self.advance();
                        let beh = self.parse_behavior()?;
                        behaviors.push(beh);
                    }
                    _ => {
                        self.advance();
                    }
                }
            } else {
                self.advance();
            }
        }
        self.expect_symbol('}')?;
        Ok(EntityDef {
            name,
            components,
            uses,
            behaviors,
            line,
        })
    }

    fn parse_behavior(&mut self) -> Result<BehaviorDef, ParseError> {
        let (name, line) = self.expect_ident()?;
        self.expect_symbol('(')?;

        let mut params = Vec::new();
        while self.peek().kind != TokenKind::Symbol(')') {
            let (p_type, _) = self.expect_ident()?;
            let (p_name, _) = self.expect_ident()?;
            params.push(BehaviorParam {
                type_name: p_type,
                name: p_name,
            });
            if self.peek().kind == TokenKind::Symbol(',') {
                self.advance();
            }
        }
        self.expect_symbol(')')?;
        self.expect_symbol('{')?;

        let body = self.extract_block_body()?;
        Ok(BehaviorDef {
            name,
            params,
            body,
            line,
        })
    }

    fn parse_system(&mut self) -> Result<SystemDef, ParseError> {
        let (name, line) = self.expect_ident()?;
        self.expect_symbol('{')?;

        let mut reads = Vec::new();
        let mut writes = Vec::new();
        let mut execute_body = String::new();
        let mut param_name = "dt".to_string();

        while self.peek().kind != TokenKind::Symbol('}') {
            let tok = self.peek().clone();
            if let TokenKind::Ident(ref kw) = tok.kind {
                match kw.as_str() {
                    "read" => {
                        self.advance();
                        let (r, _) = self.expect_ident()?;
                        reads.push(r);
                        if self.peek().kind == TokenKind::Symbol(';') {
                            self.advance();
                        }
                    }
                    "write" => {
                        self.advance();
                        let (w, _) = self.expect_ident()?;
                        writes.push(w);
                        if self.peek().kind == TokenKind::Symbol(';') {
                            self.advance();
                        }
                    }
                    "execute" => {
                        self.advance();
                        self.expect_symbol('(')?;
                        let (p, _) = self.expect_ident()?;
                        param_name = p;
                        self.expect_symbol(')')?;
                        self.expect_symbol('{')?;
                        execute_body = self.extract_block_body()?;
                    }
                    _ => {
                        self.advance();
                    }
                }
            } else {
                self.advance();
            }
        }
        self.expect_symbol('}')?;
        Ok(SystemDef {
            name,
            reads,
            writes,
            execute_param: param_name,
            execute_body,
            line,
        })
    }

    fn extract_block_body(&mut self) -> Result<String, ParseError> {
        let mut depth = 1;
        let mut body = String::new();

        while depth > 0 && self.peek().kind != TokenKind::Eof {
            let tok = self.advance().clone();
            match tok.kind {
                TokenKind::Symbol('{') => {
                    depth += 1;
                    body.push('{');
                }
                TokenKind::Symbol('}') => {
                    depth -= 1;
                    if depth > 0 {
                        body.push('}');
                    }
                }
                TokenKind::Ident(s)
                | TokenKind::Number(s)
                | TokenKind::StringLit(s)
                | TokenKind::Operator(s) => {
                    body.push_str(&s);
                    body.push(' ');
                }
                TokenKind::Symbol(s) => {
                    body.push(s);
                    if s == ';' || s == ',' {
                        body.push(' ');
                    }
                }
                _ => {}
            }
        }
        Ok(body.trim().to_string())
    }
}
