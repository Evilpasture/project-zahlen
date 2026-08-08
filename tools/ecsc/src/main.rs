// tools/ecsc/src/main.rs
// Copyright (C) 2026 Evilpasture | SPDX-License-Identifier: GPL-3.0-or-later

mod ast;
mod codegen;
mod lsp;
mod parser;

use clap::{Parser, Subcommand};
use codegen::CXXModuleGenerator;
use parser::{Lexer, Parser as ECSParser};
use std::fs;

#[derive(Parser)]
#[command(
    name = "ecsc",
    version = "0.1.0",
    about = "Zahlen Engine ECS DSL Compiler"
)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Compile .ecs DSL to C++20/26 Module (.cppm & .cpp)
    Compile {
        #[arg(short, long)]
        input: String,
        #[arg(long = "output-cppm")]
        output_cppm: String,
        #[arg(long = "output-cpp")]
        output_cpp: String,
        #[arg(short, long, default_value = "Gameplay")]
        module_name: String,
    },
    /// Launch the Language Server Protocol (LSP) over stdio
    Lsp,
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();

    match cli.command {
        Commands::Compile {
            input,
            output_cppm,
            output_cpp,
            module_name,
        } => {
            let code = fs::read_to_string(&input)?;
            let mut lexer = Lexer::new(&code);
            let tokens = lexer.tokenize()?;

            let mut parser = ECSParser::new(tokens);
            let ast = parser.parse()?;

            let generator = CXXModuleGenerator::new(&ast, &module_name);

            if let Some(parent) = std::path::Path::new(&output_cppm).parent() {
                fs::create_dir_all(parent)?;
            }
            if let Some(parent) = std::path::Path::new(&output_cpp).parent() {
                fs::create_dir_all(parent)?;
            }

            fs::write(&output_cppm, generator.generate_cppm())?;
            fs::write(&output_cpp, generator.generate_cpp())?;

            println!(
                "[ecsc] Successfully compiled {} -> C++20/26 Module ({}, {})",
                input, output_cppm, output_cpp
            );
        }
        Commands::Lsp => {
            lsp::run_lsp_server().await;
        }
    }

    Ok(())
}
