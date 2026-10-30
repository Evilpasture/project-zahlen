// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: utils/zshader/SlangReflect.cpp
//
// Slang in, catalog records out; the header carries the contract. Two
// movements: CompileSlangEntry (one entry point to a linked program, with
// the cooks' codegen), ReflectSlangEntry (the linked layout and the entry
// point's usage table, to records).
//
// The compile mirrors the cooks flag for flag -- SPIR-V, the entry-point
// name, column-major matrices, the -D defines -- except it omits -O, as the
// cooks pass no -O flag either. The walk trusts Slang for every name, type,
// offset and size; the usage table trusts Slang for which (set, binding)
// slots the emitted entry point binds. No SPIR-V decoding: the compiles are
// the only description of the modules the catalog carries, and nothing
// downstream re-reads the cooked bytes to second-guess them.

#include "SlangReflect.hpp"

#include "ZShader.hpp" // Fail, ReadFile

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ZHLN::ZShader {
namespace {

template <typename... Args>
auto ReportOrFail(std::string* errorOut, std::format_string<Args...> fmt, Args&&... args) -> bool {
    if (errorOut == nullptr) {
        Fail(fmt, std::forward<Args>(args)...);
    }
    if (errorOut->empty()) {
        *errorOut = std::format(fmt, std::forward<Args>(args)...);
    }
    return false;
}

// A ParameterBlock's seat, as Slang's Vulkan layout lays it out: the
// descriptor-table slot the block itself took (its members' set) and the
// register-space offset its members' bindings are lifted by.
struct BlockSeat {
    uint32_t set         = 0;
    uint32_t bindingBase = 0;
};

// Slang's survivor oracle for one slot: the entry point's usage table names
// the (set, binding) pairs its emitted code binds. A query Slang refuses is
// a broken program, not an unused binding; failing beats guessing.
auto SlotIsUsed(slang::IMetadata& metadata, uint32_t set, uint32_t binding, std::string_view file,
                std::string_view entry, std::string* errorOut, bool& ok) -> bool {
    bool used = false;
    if (metadata.isParameterLocationUsed(
            SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT, static_cast<SlangUInt>(set),
            static_cast<SlangUInt>(binding), used)
        != SLANG_OK) {
        ok = ReportOrFail(errorOut, "'{}' entry '{}': Slang cannot answer slot ({}, {})", file, entry, set, binding);
        return false;
    }
    return used;
}

// Slang hands diagnostics back as one blob; empty is the only clean bill.
auto DiagText(slang::IBlob* blob) -> std::string {
    if (blob == nullptr) {
        return {};
    }
    return std::string(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
}

auto CheckDiagnostics(slang::IBlob* blob, std::string_view source, std::string_view entry, std::string* errorOut)
    -> bool {
    const std::string text = DiagText(blob);
    if (!text.empty()) {
        return ReportOrFail(
            errorOut, "'{}' entry '{}': Slang reported diagnostics; the catalog compiles warning-free:\n{}", source,
            entry, text
        );
    }
    return true;
}

auto SourceOf(const SlangCompileArgs& args) -> std::string_view {
    return !args.file.empty() ? args.file : args.module;
}

// The module name Slang files the compilation under: the stem, so a
// diagnostic reads 'lighting' and __FILE__ still carries the full path.
auto StemOf(std::string_view path) -> std::string_view {
    const size_t slash = path.find_last_of("/\\");
    const std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot             = base.find_last_of('.');
    return dot == std::string_view::npos ? base : base.substr(0, dot);
}

// One binding, on the slot Slang seats it at, kept only when the usage table
// says the emitted entry point binds there. A block member's slot is the
// block's seat with its own index lifted by the block's offset; a global
// keeps the pair Slang reported. An empty answer means "unused"; the name
// existed in the declaration model and the cook will not carry it.
auto BindingOf(
    slang::VariableLayoutReflection* var, slang::TypeReflection* type, std::string name,
    slang::TypeReflection::Kind kind, const BlockSeat* block, slang::IMetadata* metadata, std::string_view file,
    std::string_view entry, std::string* errorOut, bool& ok
) -> std::optional<SlangBinding> {
    if (type == nullptr) {
        ok = ReportOrFail(errorOut, "'{}' entry '{}': Slang reports '{}' without a type", file, entry, name);
        return {};
    }
    SlangBinding binding {
        .name    = std::move(name),
        .set     = block != nullptr ? block->set : var->getBindingSpace(),
        .binding = block != nullptr ? var->getBindingIndex() + block->bindingBase : var->getBindingIndex(),
        .kind    = kind,
        .shape   = type->getResourceShape(),
        .access  = type->getResourceAccess(),
    };
    const bool used = SlotIsUsed(*metadata, binding.set, binding.binding, file, entry, errorOut, ok);
    if (!ok || !used) {
        return {};
    }
    return binding;
}

auto WalkPushBlock(
    slang::TypeLayoutReflection* element, std::string name, SlangEntryInfo& info, std::string_view file,
    std::string_view entry, std::string* errorOut
) -> bool {
    if (element == nullptr) {
        return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports a push block without an element type", file, entry);
    }
    SlangPushBlock block;
    block.name = std::move(name);
    for (const unsigned i : std::views::iota(0U, element->getFieldCount())) {
        slang::VariableLayoutReflection* member = element->getFieldByIndex(i);
        slang::TypeLayoutReflection*     layout = member->getTypeLayout();
        if (layout == nullptr) {
            return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports a push member without a layout", file, entry);
        }
        const char*    raw    = member->getName();
        const uint32_t offset = static_cast<uint32_t>(member->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM));
        const uint32_t size   = static_cast<uint32_t>(layout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
        block.members.push_back(SlangPushMember{.name = raw != nullptr ? raw : "", .offset = offset, .size = size});
        block.extent = std::max(block.extent, offset + size);
    }
    info.pushes.push_back(std::move(block));
    return true;
}

// One global -- or, recursing, one ParameterBlock member -- into the
// records, when its slot survives Slang's own usage filter. Only
// resource-kind declarations bind; scalars, vectors and matrices are
// specialization constants and plain uniforms, never descriptors. The prefix
// carries the dotted path (scene.frame); `block` carries the seat a member's
// slot composes from, and is null at the top.
auto WalkGlobal(
    slang::VariableLayoutReflection* var, std::string_view prefix, SlangEntryInfo& info, const BlockSeat* block,
    slang::IMetadata* metadata, std::string_view file, std::string_view entry, std::string* errorOut
) -> bool {
    using enum slang::TypeReflection::Kind;
    const char* raw = var->getName();
    if (raw == nullptr || *raw == '\0') {
        return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports an unnamed global", file, entry);
    }
    std::string            name = prefix.empty() ? std::string(raw) : std::format("{}.{}", prefix, raw);
    slang::TypeReflection* type = var->getType();
    if (type == nullptr) {
        return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports '{}' without a type", file, entry, name);
    }
    bool ok = true;
    switch (type->getKind()) {
    case Resource:
    case SamplerState:
    case TextureBuffer:
    case ShaderStorageBuffer:
        if (auto binding = BindingOf(var, type, std::move(name), type->getKind(), block, metadata, file, entry, errorOut, ok)) {
            info.bindings.push_back(std::move(*binding));
        }
        return ok;
    case ConstantBuffer: {
        slang::TypeLayoutReflection* layout = var->getTypeLayout();
        if (layout == nullptr) {
            return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports '{}' without a layout", file, entry, name);
        }
        if (layout->getSize(SLANG_PARAMETER_CATEGORY_PUSH_CONSTANT_BUFFER) != 0) {
            return WalkPushBlock(
                layout->getElementVarLayout() != nullptr ? layout->getElementVarLayout()->getTypeLayout() : nullptr,
                std::move(name), info, file, entry, errorOut
            );
        }
        if (auto binding = BindingOf(var, type, std::move(name), type->getKind(), block, metadata, file, entry, errorOut, ok)) {
            info.bindings.push_back(std::move(*binding));
        }
        return ok;
    }
    case ParameterBlock: {
        slang::TypeLayoutReflection* layout = var->getTypeLayout();
        slang::TypeLayoutReflection* element =
            layout != nullptr && layout->getElementVarLayout() != nullptr ? layout->getElementVarLayout()->getTypeLayout()
                                                                          : nullptr;
        if (element == nullptr) {
            return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports '{}' without an element type", file, entry, name);
        }
        // f1 composition: the members' set is the slot the block took in
        // the global descriptor table, and their bindings sit above the
        // block's register-space offset. Slang's per-variable pair alone
        // would report block-relative slots that collide with the globals'.
        const BlockSeat seat {
            .set         = var->getBindingIndex(),
            .bindingBase = static_cast<uint32_t>(
                var->getOffset(static_cast<slang::ParameterCategory>(SLANG_PARAMETER_CATEGORY_REGISTER_SPACE))
            ),
        };
        for (const unsigned i : std::views::iota(0U, element->getFieldCount())) {
            if (!WalkGlobal(element->getFieldByIndex(i), name, info, &seat, metadata, file, entry, errorOut)) {
                return false;
            }
        }
        return true;
    }
    case Array: {
        slang::TypeReflection* element = type->getElementType();
        if (element == nullptr) {
            return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports '{}' without an element type", file, entry, name);
        }
        switch (element->getKind()) {
        case Resource:
        case SamplerState:
        case TextureBuffer:
        case ShaderStorageBuffer:
        case ConstantBuffer:
            if (auto binding =
                    BindingOf(var, element, std::move(name), element->getKind(), block, metadata, file, entry, errorOut, ok)) {
                info.bindings.push_back(std::move(*binding));
            }
            return ok;
        default:
            return true; // Arrays of values are uniforms, not descriptors.
        }
    }
    case Scalar:
    case Vector:
    case Matrix:
        return true; // Specialization constants and uniforms; never bindings.
    case Struct:
        return ReportOrFail(errorOut, "'{}' entry '{}': global '{}' is a bare struct, which the catalog cannot place", file, entry, name);
    default:
        return ReportOrFail(
            errorOut, "'{}' entry '{}': global '{}' has Slang kind {}, which the catalog cannot place", file, entry, name,
            static_cast<int>(type->getKind())
        );
    }
}

auto ReflectSlangEntryImpl(
    slang::IComponentType* linked, std::string_view file, std::string_view entry, SlangEntryInfo& info,
    std::string* errorOut
) -> bool {
    using enum slang::TypeReflection::Kind;
    if (linked == nullptr) {
        return ReportOrFail(errorOut, "'{}' entry '{}': Slang linked nothing", file, entry);
    }
    slang::ProgramLayout* program = linked->getLayout();
    if (program == nullptr) {
        return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports no program layout", file, entry);
    }
    if (program->getEntryPointCount() != 1) {
        return ReportOrFail(
            errorOut, "'{}' entry '{}': Slang links {} entry points; the catalog compiles one entry per module", file,
            entry, program->getEntryPointCount()
        );
    }
    slang::EntryPointReflection* point     = program->getEntryPointByIndex(0);
    const char*                  pointName = point->getName();
    if (pointName == nullptr || *pointName == '\0') {
        return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports an unnamed entry point", file, entry);
    }
    info            = {};
    info.entryPoint = pointName;
    info.stage      = point->getStage();
    if (info.stage == SLANG_STAGE_COMPUTE) {
        SlangUInt workgroup[3] = {0, 0, 0};
        point->getComputeThreadGroupSize(3, workgroup);
        if (workgroup[0] == 0 || workgroup[1] == 0 || workgroup[2] == 0) {
            return ReportOrFail(
                errorOut, "'{}' entry '{}': Slang reports no threadgroup size for a compute entry point", file, entry
            );
        }
        info.localSize = {
            static_cast<uint32_t>(workgroup[0]),
            static_cast<uint32_t>(workgroup[1]),
            static_cast<uint32_t>(workgroup[2]),
        };
    }
    for (const unsigned i : std::views::iota(0U, point->getParameterCount())) {
        slang::VariableLayoutReflection* param = point->getParameterByIndex(i);
        slang::TypeReflection*           type  = param->getType();
        if (type == nullptr) {
            return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports an entry parameter without a type", file, entry);
        }
        switch (type->getKind()) {
        case Struct:
        case Scalar:
        case Vector:
        case Matrix:
        case Array:
            break; // Varyings and payloads; never bindings.
        case MeshOutput:
        case OutputStream:
            break; // Stage outputs (out vertices/indices, GS streams); addressed, never bound.
        default: {
            const char* paramName = param->getName();
            return ReportOrFail(
                errorOut, "'{}' entry '{}': entry parameter '{}' has Slang kind {}, which the catalog does not walk",
                file, entry, paramName != nullptr ? paramName : "(unnamed)", static_cast<int>(type->getKind())
            );
        }
        }
    }
    Slang::ComPtr<slang::IMetadata> metadata;
    Slang::ComPtr<slang::IBlob>     metadataDiagnostics;
    if (linked->getEntryPointMetadata(0, 0, metadata.writeRef(), metadataDiagnostics.writeRef()) != SLANG_OK ||
        metadata == nullptr) {
        return ReportOrFail(
            errorOut,
            "'{}' entry '{}': Slang reports no entry-point metadata; without the usage table the catalog cannot name survivors:\n{}",
            file, entry, DiagText(metadataDiagnostics.get())
        );
    }
    slang::VariableLayoutReflection* globals = program->getGlobalParamsVarLayout();
    if (globals == nullptr) {
        return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports no globals", file, entry);
    }
    slang::TypeLayoutReflection* globalType = globals->getTypeLayout();
    if (globalType == nullptr) {
        return ReportOrFail(errorOut, "'{}' entry '{}': Slang reports no global layout", file, entry);
    }
    const std::string prefix;
    for (const unsigned i : std::views::iota(0U, globalType->getFieldCount())) {
        if (!WalkGlobal(globalType->getFieldByIndex(i), prefix, info, nullptr, metadata.get(), file, entry, errorOut)) {
            return false;
        }
    }
    return true;
}

} // namespace

auto ParseSlangStage(std::string_view file, std::string_view entry, std::string_view stage) -> SlangStage {
    if (stage == "vertex") {
        return SLANG_STAGE_VERTEX;
    }
    if (stage == "fragment") {
        return SLANG_STAGE_FRAGMENT;
    }
    if (stage == "compute") {
        return SLANG_STAGE_COMPUTE;
    }
    if (stage == "amplification") {
        return SLANG_STAGE_AMPLIFICATION;
    }
    if (stage == "mesh") {
        return SLANG_STAGE_MESH;
    }
    Fail(
        "'{}' entry '{}': unknown stage '{}'; want vertex, fragment, compute, amplification or mesh", file, entry,
        stage
    );
}

auto CompileSlangEntry(const SlangCompileArgs& args) -> SlangProgram {
    const std::string_view source = SourceOf(args);
    if (args.file.empty() == args.module.empty()) {
        Fail("cannot compile entry '{}': pass a source file or a module name, not {}", args.entry,
             args.file.empty() ? "neither" : "both");
    }
    Slang::ComPtr<slang::IGlobalSession> global;
    if (slang::createGlobalSession(global.writeRef()) != SLANG_OK) {
        Fail("'{}' entry '{}': cannot create the Slang global session", source, args.entry);
    }

    std::vector<const char*> searchDirs;
    searchDirs.reserve(args.searchPaths.size());
    for (const auto& dir : args.searchPaths) {
        searchDirs.push_back(dir.c_str());
    }
    std::vector<slang::PreprocessorMacroDesc> macros;
    macros.reserve(args.defines.size());
    for (const auto& [name, value] : args.defines) {
        macros.push_back(slang::PreprocessorMacroDesc{.name = name.c_str(), .value = value.c_str()});
    }
    // Appended in this order, deliberately: Slang echoes the options into the
    // emitted module's recorded command line verbatim, so the gpu-types mode
    // keeps its historical order (debug, optimization, entry-point name) and
    // the catalog's two-option order is its prefix.
    slang::CompilerOptionEntry entries[3] = {};
    uint32_t                  entryCount  = 0;
    entries[entryCount++]                 = slang::CompilerOptionEntry{
                        .name = slang::CompilerOptionName::DebugInformation, .value = {.intValue0 = args.debugInfoLevel}};
    if (args.optimizationLevel.has_value()) {
        entries[entryCount++] =
            slang::CompilerOptionEntry{.name = slang::CompilerOptionName::Optimization,
                                       .value = {.intValue0 = *args.optimizationLevel}};
    }
    entries[entryCount++] = slang::CompilerOptionEntry{
        .name = slang::CompilerOptionName::VulkanUseEntryPointName, .value = {.intValue0 = 1}};
    slang::TargetDesc target = {};
    target.format            = SLANG_SPIRV;
    // No profile: the cooks pass no -profile either, so the default both
    // sides agree on is the only parity there is.
    slang::SessionDesc sessionDesc                 = {};
    sessionDesc.targets                         = &target;
    sessionDesc.targetCount                     = 1;
    sessionDesc.compilerOptionEntries           = entries;
    sessionDesc.compilerOptionEntryCount        = entryCount;
    sessionDesc.searchPaths                     = searchDirs.empty() ? nullptr : searchDirs.data();
    sessionDesc.searchPathCount                 = static_cast<SlangInt>(searchDirs.size());
    sessionDesc.preprocessorMacros              = macros.empty() ? nullptr : macros.data();
    sessionDesc.preprocessorMacroCount          = static_cast<SlangInt>(macros.size());
    sessionDesc.defaultMatrixLayoutMode         = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;

    Slang::ComPtr<slang::ISession> session;
    Slang::ComPtr<slang::IBlob>    diagnostics;
    const auto Check = [&](SlangResult result, std::string_view step) {
        if (result != SLANG_OK) {
            Fail("'{}' entry '{}': cannot {}:\n{}", source, args.entry, step, DiagText(diagnostics.get()));
        }
        CheckDiagnostics(diagnostics.get(), source, args.entry, nullptr);
    };
    Check(global->createSession(sessionDesc, session.writeRef()), "create the Slang session");

    // The strings below outlive the whole compile: session loads consume them
    // synchronously, but nothing says the module doesn't keep a view.
    const std::string entryName(args.entry);
    // Borrowed: the session owns (and caches by name) every module it loads,
    // so the compile holds the raw pointer and never releases it.
    slang::IModule* module = nullptr;
    if (!args.file.empty()) {
        const std::vector<uint8_t> bytes = ReadFile(std::string(args.file));
        if (bytes.empty()) {
            Fail("'{}' entry '{}': the source file is empty", source, args.entry);
        }
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (text.find('\0') != std::string::npos) {
            Fail("'{}' entry '{}': the source holds a NUL byte; refusing to compile a truncated file", source,
                 args.entry);
        }
        const std::string moduleName(StemOf(args.file));
        const std::string filePath(args.file);
        module = session->loadModuleFromSourceString(
            moduleName.c_str(), filePath.c_str(), text.c_str(), diagnostics.writeRef());
        Check(module ? SLANG_OK : SLANG_FAIL, "load the Slang source");
    } else {
        const std::string moduleName(args.module);
        module = session->loadModule(moduleName.c_str(), diagnostics.writeRef());
        Check(module ? SLANG_OK : SLANG_FAIL, "load the Slang module");
    }
    Slang::ComPtr<slang::IEntryPoint> found;
    Check(
        module->findAndCheckEntryPoint(entryName.c_str(), args.stage, found.writeRef(), diagnostics.writeRef()),
        "use this entry point at this stage"
    );
    std::array<slang::IComponentType*, 2> components = {module, found.get()};
    Slang::ComPtr<slang::IComponentType> composite;
    Check(
        session->createCompositeComponentType(
            components.data(), static_cast<SlangInt>(components.size()), composite.writeRef(),
            diagnostics.writeRef()
        ),
        "compose the Slang program"
    );
    Slang::ComPtr<slang::IComponentType> linked;
    Check(composite->link(linked.writeRef(), diagnostics.writeRef()), "link the Slang program");
    SlangProgram program;
    program.global    = std::move(global);
    program.session   = std::move(session);
    program.entry     = std::move(found);
    program.composite = std::move(composite);
    program.linked    = std::move(linked);
    return program;
}

auto ReflectSlangEntry(slang::IComponentType* linked, std::string_view file, std::string_view entry)
    -> SlangEntryInfo {
    SlangEntryInfo info;
    ReflectSlangEntryImpl(linked, file, entry, info, nullptr);
    return info;
}

auto ReflectCatalogModule(const SlangCompileArgs& args) -> SlangEntryInfo {
    const SlangProgram     program = CompileSlangEntry(args);
    const std::string_view source  = SourceOf(args);
    return ReflectSlangEntry(program.linked.get(), source, args.entry);
}

auto ReflectCatalogBatch(std::span<const SlangBatchRequest> requests, std::span<const std::string> searchPaths)
    -> std::vector<SlangEntryInfo> {
    std::vector<SlangEntryInfo> results(requests.size());
    if (requests.empty()) {
        return results;
    }

    struct SourceGroup {
        std::string_view                                     file {};
        std::span<const std::pair<std::string, std::string>> defines {};
        std::vector<size_t>                                  indices {};
    };

    std::vector<SourceGroup>                   groups;
    std::map<std::string, std::string, std::less<>> sourceTexts;

    for (size_t idx = 0; idx < requests.size(); ++idx) {
        const SlangBatchRequest& req = requests[idx];
        if (!sourceTexts.contains(req.file)) {
            const std::vector<uint8_t> bytes = ReadFile(req.file);
            if (bytes.empty()) {
                Fail("'{}' entry '{}': the source file is empty", req.file, req.entry);
            }
            std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (text.find('\0') != std::string::npos) {
                Fail("'{}' entry '{}': the source holds a NUL byte; refusing to compile a truncated file", req.file, req.entry);
            }
            sourceTexts.emplace(std::string(req.file), std::move(text));
        }

        auto existing = std::ranges::find_if(groups, [&](const SourceGroup& group) {
            return group.file == req.file && std::ranges::equal(group.defines, req.defines);
        });
        if (existing != groups.end()) {
            existing->indices.push_back(idx);
        } else {
            groups.push_back(SourceGroup {.file = req.file, .defines = req.defines, .indices = {idx}});
        }
    }

    std::vector<const char*> searchDirs;
    searchDirs.reserve(searchPaths.size());
    for (const auto& dir : searchPaths) {
        searchDirs.push_back(dir.c_str());
    }

    const unsigned hwThreads   = std::thread::hardware_concurrency();
    const size_t   maxWorkers  = std::clamp<size_t>(hwThreads == 0U ? 1U : hwThreads, 1, 8);
    const size_t   workerCount = std::min<size_t>(groups.size(), maxWorkers);

    // Create one private Slang global session per worker on the main thread so
    // workers share zero Slang state while avoiding concurrent cold-start init.
    std::vector<Slang::ComPtr<slang::IGlobalSession>> globals(workerCount);
    for (size_t w = 0; w < workerCount; ++w) {
        if (slang::createGlobalSession(globals[w].writeRef()) != SLANG_OK) {
            Fail("cannot create Slang global session for worker {}", w);
        }
    }

    std::atomic<size_t> nextGroup {0};
    std::atomic<bool>   hasError {false};
    std::mutex          errorMutex;
    size_t              earliestFailedIdx = requests.size();
    std::string         earliestError;

    const auto recordFailure = [&](size_t reqIdx, std::string message) {
        std::scoped_lock lock(errorMutex);
        if (reqIdx < earliestFailedIdx) {
            earliestFailedIdx = reqIdx;
            earliestError     = std::move(message);
        }
        hasError.store(true, std::memory_order_release);
    };

    const auto runWorker = [&](size_t workerIdx) {
        slang::IGlobalSession* global = globals[workerIdx].get();
        while (!hasError.load(std::memory_order_acquire)) {
            const size_t groupIdx = nextGroup.fetch_add(1, std::memory_order_relaxed);
            if (groupIdx >= groups.size()) {
                break;
            }
            const SourceGroup& group      = groups[groupIdx];
            const size_t       firstReq   = group.indices.front();
            const std::string& sourceText = sourceTexts.find(group.file)->second;

            std::vector<slang::PreprocessorMacroDesc> macros;
            macros.reserve(group.defines.size());
            for (const auto& [name, value] : group.defines) {
                macros.push_back(slang::PreprocessorMacroDesc {.name = name.c_str(), .value = value.c_str()});
            }

            slang::CompilerOptionEntry entries[2] = {
                slang::CompilerOptionEntry {
                    .name = slang::CompilerOptionName::DebugInformation,
                    .value = {.intValue0 = SLANG_DEBUG_INFO_LEVEL_NONE},
                },
                slang::CompilerOptionEntry {
                    .name = slang::CompilerOptionName::VulkanUseEntryPointName,
                    .value = {.intValue0 = 1},
                },
            };
            slang::TargetDesc target = {};
            target.format            = SLANG_SPIRV;

            slang::SessionDesc sessionDesc         = {};
            sessionDesc.targets                    = &target;
            sessionDesc.targetCount                = 1;
            sessionDesc.compilerOptionEntries      = entries;
            sessionDesc.compilerOptionEntryCount   = 2;
            sessionDesc.searchPaths                = searchDirs.empty() ? nullptr : searchDirs.data();
            sessionDesc.searchPathCount            = static_cast<SlangInt>(searchDirs.size());
            sessionDesc.preprocessorMacros         = macros.empty() ? nullptr : macros.data();
            sessionDesc.preprocessorMacroCount     = static_cast<SlangInt>(macros.size());
            sessionDesc.defaultMatrixLayoutMode    = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;

            Slang::ComPtr<slang::ISession> session;
            Slang::ComPtr<slang::IBlob>    diagnostics;
            std::string                    err;
            const auto checkStep = [&](SlangResult res, std::string_view entry, std::string_view step) -> bool {
                if (res != SLANG_OK) {
                    err = std::format("'{}' entry '{}': cannot {}:\n{}", group.file, entry, step, DiagText(diagnostics.get()));
                    return false;
                }
                return CheckDiagnostics(diagnostics.get(), group.file, entry, &err);
            };

            if (!checkStep(global->createSession(sessionDesc, session.writeRef()), requests[firstReq].entry, "create the Slang session")) {
                recordFailure(firstReq, std::move(err));
                return;
            }

            const std::string moduleName(StemOf(group.file));
            const std::string filePath(group.file);
            slang::IModule*   slangModule = session->loadModuleFromSourceString(
                moduleName.c_str(), filePath.c_str(), sourceText.c_str(), diagnostics.writeRef()
            );
            if (!checkStep(slangModule != nullptr ? SLANG_OK : SLANG_FAIL, requests[firstReq].entry, "load the Slang source")) {
                recordFailure(firstReq, std::move(err));
                return;
            }

            for (const size_t reqIdx : group.indices) {
                if (hasError.load(std::memory_order_acquire)) {
                    return;
                }
                const SlangBatchRequest& req = requests[reqIdx];
                const std::string        entryName(req.entry);
                diagnostics.setNull();

                Slang::ComPtr<slang::IEntryPoint> found;
                if (!checkStep(
                        slangModule->findAndCheckEntryPoint(entryName.c_str(), req.stage, found.writeRef(), diagnostics.writeRef()),
                        req.entry, "use this entry point at this stage"
                    )) {
                    recordFailure(reqIdx, std::move(err));
                    return;
                }

                diagnostics.setNull();
                std::array<slang::IComponentType*, 2> components = {slangModule, found.get()};
                Slang::ComPtr<slang::IComponentType>  composite;
                if (!checkStep(
                        session->createCompositeComponentType(
                            components.data(), static_cast<SlangInt>(components.size()), composite.writeRef(), diagnostics.writeRef()
                        ),
                        req.entry, "compose the Slang program"
                    )) {
                    recordFailure(reqIdx, std::move(err));
                    return;
                }

                diagnostics.setNull();
                Slang::ComPtr<slang::IComponentType> linked;
                if (!checkStep(composite->link(linked.writeRef(), diagnostics.writeRef()), req.entry, "link the Slang program")) {
                    recordFailure(reqIdx, std::move(err));
                    return;
                }

                if (!ReflectSlangEntryImpl(linked.get(), req.file, req.entry, results[reqIdx], &err)) {
                    recordFailure(reqIdx, std::move(err));
                    return;
                }
            }
        }
    };

    if (workerCount == 1) {
        runWorker(0);
    } else {
        std::vector<std::jthread> workers;
        workers.reserve(workerCount);
        for (size_t w = 0; w < workerCount; ++w) {
            workers.emplace_back(runWorker, w);
        }
    }

    if (!earliestError.empty()) {
        Fail("{}", earliestError);
    }
    return results;
}

} // namespace ZHLN::ZShader
