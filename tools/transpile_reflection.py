#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import os
import sys
import subprocess
import clang.cindex
from clang.cindex import CursorKind, TypeKind, AccessSpecifier

# ==============================================================================
# 1. AST HELPERS & TYPE QUERY ENGINE (ZERO REGEX)
# ==============================================================================


def get_node_text(node, source_code):
    """Extracts exact source code corresponding to an AST node's extent."""
    return source_code[node.extent.start.offset : node.extent.end.offset]


def get_called_function_name(call_node):
    """Extracts the function name from a CALL_EXPR using pure AST references."""
    if call_node.referenced:
        return call_node.referenced.spelling
    for child in call_node.get_children():
        try:
            child_kind = child.kind
        except ValueError:
            continue
        if child_kind == CursorKind.DECL_REF_EXPR:
            return child.spelling
    return call_node.spelling


def unwrap_type(type_obj):
    """
    Recursively strips pointer (*), lvalue-ref (&), and rvalue-ref (&&) layers.
    Returns: (unwrapped_canonical_type, is_pointer_type)
    """
    t = type_obj.get_canonical()
    is_pointer = False

    while t.kind in (
        TypeKind.POINTER,
        TypeKind.LVALUEREFERENCE,
        TypeKind.RVALUEREFERENCE,
    ):
        if t.kind == TypeKind.POINTER:
            is_pointer = True
        t = t.get_pointee().get_canonical()

    return t, is_pointer


def clean_type_spelling(spelling):
    """Strips C++ type keywords to yield clean fully-qualified C++ type names."""
    if not spelling:
        return ""
    for prefix in ["enum class ", "enum ", "struct ", "class ", "const ", "volatile "]:
        if spelling.startswith(prefix):
            spelling = spelling[len(prefix) :]
    return spelling.strip()


def get_template_argument_type(node):
    """Finds the first explicit template argument type for a template function call."""
    for child in node.get_children():
        if child.kind == CursorKind.TYPE_REF:
            return child.type
        for sub in child.get_children():
            if sub.kind == CursorKind.TYPE_REF:
                return sub.type
            for sub2 in sub.get_children():
                if sub2.kind == CursorKind.TYPE_REF:
                    return sub2.type
    return None


def get_nested_types(type_obj):
    """Extracts all nested struct/class type names within a container struct."""
    unwrapped_type, _ = unwrap_type(type_obj)
    decl_cursor = unwrapped_type.get_declaration()
    nested = []

    for child in decl_cursor.get_children():
        if (
            child.kind
            in (
                CursorKind.STRUCT_DECL,
                CursorKind.CLASS_DECL,
            )
            and child.is_definition()
        ):
            raw_type = child.type.get_canonical().spelling
            clean_name = clean_type_spelling(raw_type)
            if clean_name and "<" not in clean_name:
                nested.append(clean_name)

    return nested


def get_struct_fields(type_obj):
    """Recursively extracts all PUBLIC field names from a type declaration and base classes."""
    unwrapped_type, _ = unwrap_type(type_obj)
    decl_cursor = unwrapped_type.get_declaration()
    fields = []

    for child in decl_cursor.get_children():
        if child.kind == CursorKind.CXX_BASE_SPECIFIER:
            if child.access_specifier not in (
                AccessSpecifier.PRIVATE,
                AccessSpecifier.PROTECTED,
            ):
                fields.extend(get_struct_fields(child.type))
        elif child.kind == CursorKind.FIELD_DECL:
            if child.access_specifier not in (
                AccessSpecifier.PRIVATE,
                AccessSpecifier.PROTECTED,
            ):
                fields.append(child.spelling)

    return fields


def get_enum_constants(type_obj):
    """Extracts all unique enumerator names for an Enum type using Clang's AST."""
    unwrapped_type, _ = unwrap_type(type_obj)
    decl_cursor = unwrapped_type.get_declaration()
    constants = []
    seen_values = set()

    for child in decl_cursor.get_children():
        if child.kind == CursorKind.ENUM_CONSTANT_DECL:
            val = child.enum_value
            if val not in seen_values:
                seen_values.add(val)
                constants.append((child.spelling, decl_cursor.spelling))

    return constants


# ==============================================================================
# 2. AST TRANSFORMERS (INLINE CALL EXPR REWRITING)
# ==============================================================================


def transform_ForEachField(node, source_code):
    """Transforms ForEachField(t, f) into unrolled calls with pointer dereferencing."""
    args = list(node.get_arguments())
    if len(args) < 2:
        return None

    obj_text = get_node_text(args[0], source_code)
    func_text = get_node_text(args[1], source_code)

    _, is_pointer = unwrap_type(args[0].type)
    fields = get_struct_fields(args[0].type)

    if is_pointer:
        code = [
            "[&]() {",
            f"    if (auto* __ptr = {obj_text}) {{",
            "        auto&& __obj = *__ptr;",
            f"        auto&& __fn = {func_text};",
        ]
        for field in fields:
            code.append(f"        __fn(__obj.{field});")
        code.append("    }")
        code.append("}()")
    else:
        code = [
            "[&]() {",
            f"    auto&& __obj = {obj_text};",
            f"    auto&& __fn = {func_text};",
        ]
        for field in fields:
            code.append(f"    __fn(__obj.{field});")
        code.append("}()")

    return "\n".join(code)


def transform_ForEachFieldWithName(node, source_code):
    """Transforms ForEachFieldWithName(t, f) into unrolled (name, value) calls."""
    args = list(node.get_arguments())
    if len(args) < 2:
        return None

    obj_text = get_node_text(args[0], source_code)
    func_text = get_node_text(args[1], source_code)

    _, is_pointer = unwrap_type(args[0].type)
    fields = get_struct_fields(args[0].type)

    indent = "        " if is_pointer else "    "
    code = ["[&]() {"]

    if is_pointer:
        code.append(f"    if (auto* __ptr = {obj_text}) {{")
        code.append("        auto&& __obj = *__ptr;")
        code.append(f"        auto&& __fn = {func_text};")
    else:
        code.append(f"    auto&& __obj = {obj_text};")
        code.append(f"    auto&& __fn = {func_text};")

    for field in fields:
        code.append(f'{indent}__fn(std::string_view("{field}"), __obj.{field});')

    if is_pointer:
        code.append("    }")
    code.append("}()")

    return "\n".join(code)


def transform_ForEachFieldInfo(node, source_code):
    """Transforms ForEachFieldInfo<T>(fn) into unrolled calls with field names and offsetof."""
    args = list(node.get_arguments())
    if len(args) < 1:
        return None

    func_text = get_node_text(args[0], source_code)
    target_type = get_template_argument_type(node)
    if target_type is None:
        return None

    unwrapped_type, _ = unwrap_type(target_type)
    type_name = clean_type_spelling(unwrapped_type.get_canonical().spelling)
    fields = get_struct_fields(target_type)

    code = ["[&]() {", f"    auto&& __fn = {func_text};"]
    for field in fields:
        code.append(
            f'    __fn.template operator()<decltype({type_name}::{field})>(std::string_view("{field}"), offsetof({type_name}, {field}));'
        )
    code.append("}()")

    return "\n".join(code)


def transform_ForEachNestedType(node, source_code):
    """Transforms ForEachNestedType<Container>(fn) into unrolled calls for each nested struct."""
    args = list(node.get_arguments())
    if len(args) < 1:
        return None

    func_text = get_node_text(args[0], source_code)
    target_type = get_template_argument_type(node)
    if target_type is None:
        return None

    nested_types = get_nested_types(target_type)
    if not nested_types:
        return None

    code = ["[&]() {", f"    auto&& __fn = {func_text};"]
    for t in nested_types:
        code.append(f"    __fn.template operator()<{t}>();")
    code.append("}()")

    return "\n".join(code)


def transform_TieFields(node, source_code):
    """Transforms TieFields(t) into std::tie(t.f1, t.f2, ...)."""
    args = list(node.get_arguments())
    if len(args) < 1:
        return None

    obj_text = get_node_text(args[0], source_code)
    _, is_pointer = unwrap_type(args[0].type)
    fields = get_struct_fields(args[0].type)

    if not fields:
        return "std::tie()"

    deref = "->" if is_pointer else "."
    field_accesses = [f"{obj_text}{deref}{f}" for f in fields]
    return f"std::tie({', '.join(field_accesses)})"


def transform_EnumToString(node, source_code):
    """Transforms EnumToString(enum_val) into an AST-generated switch statement."""
    args = list(node.get_arguments())
    if len(args) < 1:
        return None

    val_text = get_node_text(args[0], source_code)
    constants = get_enum_constants(args[0].type)

    if not constants:
        return None

    code = ["[&](auto __val) -> std::string_view {", "    switch(__val) {"]
    for const_name, enum_type_name in constants:
        type_prefix = f"{enum_type_name}::" if enum_type_name else ""
        code.append(f'        case {type_prefix}{const_name}: return "{const_name}";')
    code.append('        default: return "Unknown";')
    code.append("    }")
    code.append(f"}}({val_text})")

    return "\n".join(code)


def transform_VisitFieldByName(node, source_code):
    """Transforms VisitFieldByName(t, name, f) into an if-else dispatch chain."""
    args = list(node.get_arguments())
    if len(args) < 3:
        return None

    obj_text = get_node_text(args[0], source_code)
    name_text = get_node_text(args[1], source_code)
    func_text = get_node_text(args[2], source_code)

    _, is_pointer = unwrap_type(args[0].type)
    fields = get_struct_fields(args[0].type)

    indent = "        " if is_pointer else "    "
    code = ["[&]() -> bool {"]

    if is_pointer:
        code.append(f"    if (auto* __ptr = {obj_text}) {{")
        code.append("        auto&& __obj = *__ptr;")
        code.append(f"        std::string_view __target = {name_text};")
        code.append(f"        auto&& __fn = {func_text};")
    else:
        code.append(f"    auto&& __obj = {obj_text};")
        code.append(f"    std::string_view __target = {name_text};")
        code.append(f"    auto&& __fn = {func_text};")

    for field in fields:
        code.append(
            f'{indent}if (__target == "{field}") {{ __fn(__obj.{field}); return true; }}'
        )

    if is_pointer:
        code.append("    }")
    code.append("    return false;")
    code.append("}()")

    return "\n".join(code)


# ==============================================================================
# 3. AST TRAVERSAL ENGINE
# ==============================================================================

DISPATCH_TABLE = {
    "ForEachField": transform_ForEachField,
    "ForEachFieldWithName": transform_ForEachFieldWithName,
    "ForEachFieldInfo": transform_ForEachFieldInfo,
    "ForEachNestedType": transform_ForEachNestedType,
    "TieFields": transform_TieFields,
    "EnumToString": transform_EnumToString,
    "VisitFieldByName": transform_VisitFieldByName,
}


def walk_ast_and_collect_edits(node, source_code, edits):
    """Recursively traverses Clang AST nodes looking for ZHLN::Reflect calls."""
    try:
        node_kind = node.kind
    except ValueError:
        node_kind = None

    if node_kind == CursorKind.CALL_EXPR:
        func_name = get_called_function_name(node)

        if func_name in DISPATCH_TABLE:
            transformer = DISPATCH_TABLE[func_name]
            replacement_code = transformer(node, source_code)

            if replacement_code:
                edits.append(
                    {
                        "start": node.extent.start.offset,
                        "end": node.extent.end.offset,
                        "replacement": replacement_code,
                    }
                )
                return

    for child in node.get_children():
        walk_ast_and_collect_edits(child, source_code, edits)


def sanitize_flags(raw_args, cmd_dir, input_abspath):
    """Clean compile flags retrieved from compile_commands.json."""
    flags = []
    i = 0
    while i < len(raw_args):
        arg = raw_args[i]

        if arg in ("-c", "--", "-Winvalid-pch"):
            i += 1
            continue

        if arg in ("-o", "-MF", "-MT", "-MQ"):
            i += 2
            continue
        if arg.startswith(("-o", "-MF", "-MT", "-MQ")):
            i += 1
            continue

        if arg in ("-MMD", "-MP", "-MG", "-MD"):
            i += 1
            continue

        if arg == "-include-pch":
            i += 2
            continue
        if arg.startswith("-include-pch"):
            i += 1
            continue
        if arg == "-Xclang":
            if i + 1 < len(raw_args):
                next_arg = raw_args[i + 1]
                if next_arg in ("-include-pch", "-include"):
                    if i + 3 < len(raw_args) and raw_args[i + 2] == "-Xclang":
                        i += 4
                        continue
                    else:
                        i += 2
                        continue
                elif next_arg.endswith((".pch", ".gch")):
                    i += 2
                    continue
            i += 1
            continue

        if arg.endswith((".pch", ".gch")):
            i += 1
            continue

        if arg in ("-freflection", "-fmodules-ts"):
            i += 1
            continue

        try:
            if os.path.abspath(os.path.join(cmd_dir or "", arg)) == input_abspath:
                i += 1
                continue
        except Exception:
            pass

        flags.append(arg)
        i += 1

    if sys.platform == "darwin":
        has_isysroot = any(f == "-isysroot" or f.startswith("-isysroot") for f in flags)
        if not has_isysroot:
            try:
                sdk_path = subprocess.check_output(
                    ["xcrun", "--show-sdk-path"], text=True
                ).strip()
                if sdk_path and os.path.exists(sdk_path):
                    flags.extend(["-isysroot", sdk_path])
            except Exception:
                pass

    return flags


def parse_translation_unit(index, input_abspath, flags, cmd_dir):
    """Parses TU with fallback handling, preserving C++23 minimum for std::expected."""
    orig_cwd = os.getcwd()
    if cmd_dir and os.path.exists(cmd_dir):
        os.chdir(cmd_dir)

    try:
        try:
            return index.parse(input_abspath, args=flags)
        except clang.cindex.TranslationUnitLoadError:
            pass

        fallback_flags_23 = []
        for f in flags:
            if f.startswith(("-std=c++26", "-std=c++2c")):
                fallback_flags_23.append("-std=c++23")
            else:
                fallback_flags_23.append(f)

        try:
            return index.parse(input_abspath, args=fallback_flags_23)
        except clang.cindex.TranslationUnitLoadError:
            pass

        fallback_flags_2b = [
            "-std=c++2b"
            if f.startswith(("-std=c++26", "-std=c++2c", "-std=c++23"))
            else f
            for f in flags
        ]

        try:
            return index.parse(input_abspath, args=fallback_flags_2b)
        except clang.cindex.TranslationUnitLoadError:
            pass

        clean_fallback = []
        i = 0
        while i < len(fallback_flags_23):
            f = fallback_flags_23[i]
            if f in ("-include", "-include-pch") and i + 1 < len(fallback_flags_23):
                target_hdr = fallback_flags_23[i + 1]
                if "cmake_pch" in target_hdr or target_hdr.endswith(
                    (".pch", ".gch", ".hpp", ".h")
                ):
                    i += 2
                    continue
            clean_fallback.append(f)
            i += 1

        return index.parse(input_abspath, args=clean_fallback)

    finally:
        os.chdir(orig_cwd)


# ==============================================================================
# 4. ENTRY POINT
# ==============================================================================


def main():
    parser = argparse.ArgumentParser(description="ZHLN Reflection AST Transpiler")
    parser.add_argument("-i", "--input", required=True, help="Input C++ source file")
    parser.add_argument("-o", "--output", required=True, help="Output C++ source file")
    parser.add_argument(
        "-c", "--compdb", required=True, help="Path to compile_commands.json directory"
    )
    args = parser.parse_args()

    index = clang.cindex.Index.create()
    compdb = clang.cindex.CompilationDatabase.fromDirectory(args.compdb)

    input_abspath = os.path.abspath(args.input)

    compile_cmds = compdb.getCompileCommands(args.input)
    if not compile_cmds:
        compile_cmds = compdb.getCompileCommands(input_abspath)
    if not compile_cmds:
        try:
            rel_path = os.path.relpath(args.input, args.compdb)
            compile_cmds = compdb.getCompileCommands(rel_path)
        except ValueError:
            pass

    flags = []
    cmd_dir = None

    if compile_cmds:
        cmd = compile_cmds[0]
        cmd_dir = cmd.directory
        raw_args = list(cmd.arguments)[1:]
        flags = sanitize_flags(raw_args, cmd_dir, input_abspath)

    tu = parse_translation_unit(index, input_abspath, flags, cmd_dir)

    with open(args.input, "r", encoding="utf-8") as f:
        source_code = f.read()

    # 1. Collect and perform inline AST call replacements (end-to-start)
    edits = []
    walk_ast_and_collect_edits(tu.cursor, source_code, edits)
    edits.sort(key=lambda x: x["start"], reverse=True)

    modified_code = source_code
    for edit in edits:
        modified_code = (
            modified_code[: edit["start"]]
            + edit["replacement"]
            + modified_code[edit["end"] :]
        )

    os_dir = os.path.dirname(args.output)
    if os_dir:
        os.makedirs(os_dir, exist_ok=True)

    with open(args.output, "w", encoding="utf-8") as f:
        f.write("// --- GENERATED BY ZHLN REFLECTION AST TRANSPILER ---\n")
        f.write(f"// Original Source: {args.input}\n\n")
        f.write(modified_code)


if __name__ == "__main__":
    main()
