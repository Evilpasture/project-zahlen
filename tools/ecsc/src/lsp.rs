// tools/ecsc/src/lsp.rs
// Copyright (C) 2026 Evilpasture | SPDX-License-Identifier: GPL-3.0-or-later

#![allow(clippy::collapsible_if)]

use crate::parser::{Lexer, Parser};
use dashmap::DashMap;
use tower_lsp::jsonrpc::Result;
use tower_lsp::lsp_types::*;
use tower_lsp::{Client, LanguageServer, LspService, Server};

#[derive(Debug)]
pub struct Backend {
    client: Client,
    document_map: DashMap<String, String>,
}

impl Backend {
    pub fn new(client: Client) -> Self {
        Self {
            client,
            document_map: DashMap::new(),
        }
    }

    async fn validate_document(&self, uri: Url, text: String) {
        let mut lexer = Lexer::new(&text);
        let mut diagnostics = Vec::new();

        match lexer.tokenize() {
            Ok(tokens) => {
                let mut parser = Parser::new(tokens);
                if let Err(err) = parser.parse() {
                    let line = (err.line.saturating_sub(1)) as u32;
                    let col = (err.col.saturating_sub(1)) as u32;
                    diagnostics.push(Diagnostic {
                        range: Range {
                            start: Position {
                                line,
                                character: col,
                            },
                            end: Position {
                                line,
                                character: col + 10,
                            },
                        },
                        severity: Some(DiagnosticSeverity::ERROR),
                        code: None,
                        code_description: None,
                        source: Some("ecsc".into()),
                        message: err.message,
                        related_information: None,
                        tags: None,
                        data: None,
                    });
                }
            }
            Err(err) => {
                let line = (err.line.saturating_sub(1)) as u32;
                let col = (err.col.saturating_sub(1)) as u32;
                diagnostics.push(Diagnostic {
                    range: Range {
                        start: Position {
                            line,
                            character: col,
                        },
                        end: Position {
                            line,
                            character: col + 10,
                        },
                    },
                    severity: Some(DiagnosticSeverity::ERROR),
                    code: None,
                    code_description: None,
                    source: Some("ecsc".into()),
                    message: err.message,
                    related_information: None,
                    tags: None,
                    data: None,
                });
            }
        }

        self.client
            .publish_diagnostics(uri, diagnostics, None)
            .await;
    }
}

#[tower_lsp::async_trait]
impl LanguageServer for Backend {
    async fn initialize(&self, _: InitializeParams) -> Result<InitializeResult> {
        Ok(InitializeResult {
            capabilities: ServerCapabilities {
                text_document_sync: Some(TextDocumentSyncCapability::Kind(
                    TextDocumentSyncKind::FULL,
                )),
                completion_provider: Some(CompletionOptions {
                    trigger_characters: Some(vec![".".into(), " ".into(), "(".into(), "{".into()]),
                    ..Default::default()
                }),
                hover_provider: Some(HoverProviderCapability::Simple(true)),
                definition_provider: Some(OneOf::Left(true)),
                ..Default::default()
            },
            ..Default::default()
        })
    }

    async fn shutdown(&self) -> Result<()> {
        Ok(())
    }

    async fn did_open(&self, params: DidOpenTextDocumentParams) {
        self.document_map.insert(
            params.text_document.uri.to_string(),
            params.text_document.text.clone(),
        );
        self.validate_document(params.text_document.uri, params.text_document.text)
            .await;
    }

    async fn did_change(&self, params: DidChangeTextDocumentParams) {
        if let Some(change) = params.content_changes.into_iter().next() {
            self.document_map
                .insert(params.text_document.uri.to_string(), change.text.clone());
            self.validate_document(params.text_document.uri, change.text)
                .await;
        }
    }

    async fn completion(&self, params: CompletionParams) -> Result<Option<CompletionResponse>> {
        let mut items = Vec::new();

        let keywords = [
            "component",
            "property",
            "entity",
            "behavior",
            "system",
            "use",
            "read",
            "write",
            "execute",
        ];
        for kw in keywords {
            items.push(CompletionItem {
                label: kw.into(),
                kind: Some(CompletionItemKind::KEYWORD),
                detail: Some("DSL Keyword".into()),
                ..Default::default()
            });
        }

        let types = ["float", "int", "bool", "double", "Vec3", "String64"];
        for t in types {
            items.push(CompletionItem {
                label: t.into(),
                kind: Some(CompletionItemKind::TYPE_PARAMETER),
                detail: Some("Type".into()),
                ..Default::default()
            });
        }

        if let Some(text) = self
            .document_map
            .get(params.text_document_position.text_document.uri.as_str())
        {
            let mut lexer = Lexer::new(&text);
            if let Ok(tokens) = lexer.tokenize() {
                let mut parser = Parser::new(tokens);
                if let Ok(ast) = parser.parse() {
                    for comp in ast.components {
                        items.push(CompletionItem {
                            label: comp.name.clone(),
                            kind: Some(CompletionItemKind::STRUCT),
                            detail: Some("Component".into()),
                            ..Default::default()
                        });
                        for f in comp.fields {
                            items.push(CompletionItem {
                                label: f.name,
                                kind: Some(CompletionItemKind::FIELD),
                                detail: Some(format!("Field ({})", f.type_name)),
                                ..Default::default()
                            });
                        }
                    }
                }
            }
        }

        Ok(Some(CompletionResponse::Array(items)))
    }

    async fn hover(&self, params: HoverParams) -> Result<Option<Hover>> {
        let uri = params.text_document_position_params.text_document.uri;
        let pos = params.text_document_position_params.position;

        if let Some(text) = self.document_map.get(uri.as_str()) {
            let lines: Vec<&str> = text.lines().collect();
            if (pos.line as usize) < lines.len() {
                let line = lines[pos.line as usize];
                let col = pos.character as usize;

                let start = line[..col.min(line.len())]
                    .rfind(|c: char| !c.is_alphanumeric() && c != '_')
                    .map_or(0, |i| i + 1);
                let end = line[col.min(line.len())..]
                    .find(|c: char| !c.is_alphanumeric() && c != '_')
                    .map_or(line.len(), |i| col + i);
                let word = &line[start..end];

                if !word.is_empty() {
                    let mut lexer = Lexer::new(&text);
                    if let Ok(tokens) = lexer.tokenize() {
                        let mut parser = Parser::new(tokens);
                        if let Ok(ast) = parser.parse() {
                            // FIXED: Checks comp.name == word inside the loop
                            for comp in &ast.components {
                                if comp.name == word {
                                    let fields_str = comp
                                        .fields
                                        .iter()
                                        .map(|f| format!("  {} {}", f.type_name, f.name))
                                        .collect::<Vec<_>>()
                                        .join("\n");
                                    return Ok(Some(Hover {
                                        contents: HoverContents::Scalar(MarkedString::String(
                                            format!(
                                                "**Component `{}`**\n```dsl\nstruct {} {{\n{}\n}}\n```",
                                                comp.name, comp.name, fields_str
                                            ),
                                        )),
                                        range: None,
                                    }));
                                }
                            }
                            for ent in &ast.entities {
                                if ent.name == word {
                                    let comps = ent
                                        .components
                                        .iter()
                                        .map(|c| c.name.clone())
                                        .chain(ent.uses.clone())
                                        .collect::<Vec<_>>()
                                        .join(", ");
                                    return Ok(Some(Hover {
                                        contents: HoverContents::Scalar(MarkedString::String(
                                            format!(
                                                "**Entity Blueprint `{}`**\n\nComponents: `{}`",
                                                ent.name, comps
                                            ),
                                        )),
                                        range: None,
                                    }));
                                }
                            }
                        }
                    }
                }
            }
        }
        Ok(None)
    }

    async fn goto_definition(
        &self,
        params: GotoDefinitionParams,
    ) -> Result<Option<GotoDefinitionResponse>> {
        let uri = params.text_document_position_params.text_document.uri;
        let line_num = params.text_document_position_params.position.line as usize;

        if let Some(text) = self.document_map.get(uri.as_str()) {
            let lines: Vec<&str> = text.lines().collect();
            if line_num < lines.len() {
                let current_line = lines[line_num];
                for (i, l) in lines.iter().enumerate() {
                    if (l.contains("component") || l.contains("entity") || l.contains("system"))
                        && current_line.split_whitespace().any(|w| l.contains(w))
                    {
                        return Ok(Some(GotoDefinitionResponse::Scalar(Location {
                            uri,
                            range: Range {
                                start: Position {
                                    line: i as u32,
                                    character: 0,
                                },
                                end: Position {
                                    line: i as u32,
                                    character: l.len() as u32,
                                },
                            },
                        })));
                    }
                }
            }
        }
        Ok(None)
    }
}

pub async fn run_lsp_server() {
    let stdin = tokio::io::stdin();
    let stdout = tokio::io::stdout();

    // FIXED: Replaced closure with direct constructor Backend::new
    let (service, socket) = LspService::new(Backend::new);
    Server::new(stdin, stdout, socket).serve(service).await;
}
