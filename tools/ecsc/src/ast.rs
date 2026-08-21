// tools/ecsc/src/ast.rs
// Copyright (C) 2026 Evilpasture | SPDX-License-Identifier: GPL-3.0-or-later

#![allow(dead_code, clippy::upper_case_acronyms)]

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Annotation {
    pub label: Option<String>,
    pub tooltip: Option<String>,
    pub min_val: Option<String>,
    pub max_val: Option<String>,
    pub step: Option<String>,
}

#[derive(Debug, Clone)]
pub struct FieldDef {
    pub type_name: String,
    pub name: String,
    pub default_value: Option<String>,
    pub annotation: Annotation,
    pub line: usize,
}

#[derive(Debug, Clone)]
pub struct ComponentDef {
    pub name: String,
    pub fields: Vec<FieldDef>,
    pub line: usize,
}

#[derive(Debug, Clone)]
pub struct BehaviorParam {
    pub type_name: String,
    pub name: String,
}

#[derive(Debug, Clone)]
pub struct BehaviorDef {
    pub name: String,
    pub params: Vec<BehaviorParam>,
    pub body: String,
    pub line: usize,
}

#[derive(Debug, Clone)]
pub struct EntityDef {
    pub name: String,
    pub components: Vec<ComponentDef>,
    pub uses: Vec<String>,
    pub behaviors: Vec<BehaviorDef>,
    pub line: usize,
}

#[derive(Debug, Clone)]
pub struct SystemDef {
    pub name: String,
    pub reads: Vec<String>,
    pub writes: Vec<String>,
    pub execute_param: String,
    pub execute_body: String,
    pub line: usize,
}

#[derive(Debug, Clone, Default)]
pub struct Ast {
    pub components: Vec<ComponentDef>,
    pub entities: Vec<EntityDef>,
    pub systems: Vec<SystemDef>,
}

pub type AST = Ast;
