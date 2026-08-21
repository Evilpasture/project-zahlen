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
    Recursively strips pointer (*), lvalue-ref (&), and rvalue-ref (&&) layers
    to obtain the underlying canonical class/struct/enum type.
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
    for prefix in ["enum class ", "enum ", "struct ", "class "]:
        if spelling.startswith(prefix):
            spelling = spelling[len(prefix) :]
    return spelling.strip()


def is_target_type(clean_name):
    """Filters for engine types to avoid generating invalid specializations for std/internal types."""
    if not clean_name:
        return False
    # Exclude VkResult to prevent precompiled header template instantiation conflicts
    if clean_name.startswith("ZHLN::") or clean_name.startswith("ZHLN_"):
        return True
    return False


def get_struct_fields(type_obj):
    """
    Recursively extracts all PUBLIC field names from a type declaration and its
    base classes using Clang's Type and Cursor hierarchy.
    """
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
# 2. GLOBAL AST REFLECTION COLLECTOR (HANDLES TEMPLATES & HEADERS)
# ==============================================================================


def collect_enums(cursor, enums_dict, visited=None):
    """Scans the Translation Unit for all target enum definitions to generate specializations."""
    if visited is None:
        visited = set()
    if cursor.hash in visited:
        return
    visited.add(cursor.hash)

    try:
        kind = cursor.kind
    except ValueError:
        return

    if kind == CursorKind.ENUM_DECL and cursor.is_definition():
        raw_type = cursor.type.get_canonical().spelling
        clean_name = clean_type_spelling(raw_type)
        if is_target_type(clean_name):
            constants = []
            seen_values = set()
            for child in cursor.get_children():
                if child.kind == CursorKind.ENUM_CONSTANT_DECL:
                    val = child.enum_value
                    if val not in seen_values:
                        seen_values.add(val)
                        constants.append(child.spelling)
            if constants and clean_name not in enums_dict:
                enums_dict[clean_name] = (cursor.spelling, constants)

    for child in cursor.get_children():
        collect_enums(child, enums_dict, visited)


def collect_structs(cursor, structs_dict, visited=None):
    """Scans the Translation Unit for target struct/class definitions for TypeName<T>."""
    if visited is None:
        visited = set()
    if cursor.hash in visited:
        return
    visited.add(cursor.hash)

    try:
        kind = cursor.kind
    except ValueError:
        return

    if (
        kind in (CursorKind.STRUCT_DECL, CursorKind.CLASS_DECL)
        and cursor.is_definition()
    ):
        raw_type = cursor.type.get_canonical().spelling
        clean_name = clean_type_spelling(raw_type)
        if is_target_type(clean_name):
            if clean_name not in structs_dict:
                structs_dict[clean_name] = cursor.spelling

    for child in cursor.get_children():
        collect_structs(child, structs_dict, visited)


def generate_reflection_specializations(enums_dict, structs_dict):
    """
    Generates explicit C++ template specializations.
    Splits them into:
      - external_specs: Prepended at offset 0 (with required headers)
      - internal_specs: Inserted at insert_pos (after main includes)
    """
    external_lines = []
    internal_lines = []

    # Inject VkResult at the very beginning of the file to prevent PCH pre-instantiation conflicts
    if "VkResult" in enums_dict:
        external_lines.append("#include <vulkan/vulkan.h>")
        external_lines.append("namespace ZHLN::Reflect {")

        _, constants = enums_dict["VkResult"]
        external_lines.append("template <>")
        external_lines.append(
            "constexpr std::string_view EnumToString<VkResult>(VkResult __val) {"
        )
        external_lines.append("    switch(__val) {")
        for const_name in constants:
            external_lines.append(
                f'        case VkResult::{const_name}: return "{const_name}";'
            )
        external_lines.append('        default: return "Unknown";')
        external_lines.append("    }")
        external_lines.append("}")

        external_lines.append("template <>")
        external_lines.append("consteval std::string_view TypeName<VkResult>() {")
        external_lines.append('    return "VkResult";')
        external_lines.append("}")
        external_lines.append("} // namespace ZHLN::Reflect\n")

    # Process internal engine types (must reside after main includes)
    internal_lines.append(
        "\n\n// --- AUTOMATICALLY GENERATED ZHLN REFLECTION SPECIALIZATIONS ---"
    )
    internal_lines.append("namespace ZHLN::Reflect {")

    has_internal = False
    for enum_full_name, (short_name, constants) in enums_dict.items():
        if enum_full_name == "VkResult":
            continue
        has_internal = True
        internal_lines.append("template <>")
        internal_lines.append(
            f"constexpr std::string_view EnumToString<{enum_full_name}>({enum_full_name} __val) {{"
        )
        internal_lines.append("    switch(__val) {")
        for const_name in constants:
            internal_lines.append(
                f'        case {enum_full_name}::{const_name}: return "{const_name}";'
            )
        internal_lines.append('        default: return "Unknown";')
        internal_lines.append("    }")
        internal_lines.append("}")

        internal_lines.append("template <>")
        internal_lines.append(
            f"consteval std::string_view TypeName<{enum_full_name}>() {{"
        )
        internal_lines.append(f'    return "{short_name}";')
        internal_lines.append("}")

    for struct_full_name, short_name in structs_dict.items():
        has_internal = True
        internal_lines.append("template <>")
        internal_lines.append(
            f"consteval std::string_view TypeName<{struct_full_name}>() {{"
        )
        internal_lines.append(f'    return "{short_name}";')
        internal_lines.append("}")

    internal_lines.append("} // namespace ZHLN::Reflect\n")

    external_specs = "\n".join(external_lines) if external_lines else ""
    internal_specs = "\n".join(internal_lines) if has_internal else ""

    return external_specs, internal_specs


def find_first_declaration_offset(tu, input_abspath, file_size) -> int:
    """Finds the character offset of the first actual C++ declaration in the main file using pure AST."""
    first_offset = [file_size]

    def traverse(node):
        if (
            node.location.file
            and os.path.abspath(node.location.file.name) == input_abspath
        ):
            offset = node.extent.start.offset
            if offset < first_offset[0]:
                first_offset[0] = offset
            return  # Found root of this branch, do not recurse deeper

        for child in node.get_children():
            traverse(child)

    traverse(tu.cursor)
    return first_offset[0] if first_offset[0] < file_size else 0


# ==============================================================================
# 3. AST TRANSFORMERS (INLINE CALL EXPR REWRITING)
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
        return None  # Let the generated template overload handle it

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
# 4. AST TRAVERSAL AND REWRITING ENGINE
# ==============================================================================

DISPATCH_TABLE = {
    "ForEachField": transform_ForEachField,
    "ForEachFieldWithName": transform_ForEachFieldWithName,
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
    """Tries parsing TU with fallback handling, preserving C++23 minimum for std::expected."""
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
# 5. ENTRY POINT
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

    # 1. Collect and perform inline AST call replacements
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

    # 2. Collect all target ZHLN enums and structs in the translation unit
    enums_dict = {}
    structs_dict = {}
    collect_enums(tu.cursor, enums_dict)
    collect_structs(tu.cursor, structs_dict)

    external_specs, internal_specs = generate_reflection_specializations(
        enums_dict, structs_dict
    )

    # 3. Locate the first C++ declaration in the main file using pure AST.
    insert_pos = find_first_declaration_offset(tu, input_abspath, len(modified_code))

    # Insert internal engine types after the includes
    if internal_specs:
        modified_code = (
            modified_code[:insert_pos]
            + "\n"
            + internal_specs
            + modified_code[insert_pos:]
        )

    # Prepend external types (like VkResult) at the very top of the file (offset 0)
    if external_specs:
        modified_code = external_specs + "\n" + modified_code

    os_dir = os.path.dirname(args.output)
    if os_dir:
        os.makedirs(os_dir, exist_ok=True)

    with open(args.output, "w", encoding="utf-8") as f:
        f.write("// --- GENERATED BY ZHLN REFLECTION AST TRANSPILER ---\n")
        f.write(f"// Original Source: {args.input}\n\n")
        f.write(modified_code)


if __name__ == "__main__":
    main()
