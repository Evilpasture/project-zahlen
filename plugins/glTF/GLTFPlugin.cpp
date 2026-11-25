// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer + COM::IAssetImporter plugin for glTF 2.0
// (.gltf / .glb). Isolates cgltf, stb_image (zahlen_image), and simdjson
// (zahlen_serialization) behind the COM plugin boundary so hosts can import
// glTF assets without linking third-party format libraries.

#include <glTF/GLTFAssets.hpp>

#include <Zahlen/COM/IAssetImporter.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <algorithm>
#include <filesystem>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using namespace ZHLN;

class GLTFPlugin final : public COM::Implements<GLTFPlugin, COM::IAppLayer, COM::IAssetImporter> {
  public:
    GLTFPlugin() noexcept = default;

    // --- COM::IAppLayer ------------------------------------------------------

    auto OnStart(COM::IEngine*) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnBeforeRender(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    void OnStop(COM::IEngine* engine) noexcept override {
        if (engine != nullptr && engine->Graphics() != nullptr) {
            UnloadTextures(engine->Graphics());
        }
    }

    // --- COM::IAssetImporter -------------------------------------------------

    auto FormatName() const noexcept -> COM::StringView override {
        return COM::StringView("gltf");
    }

    auto LoadFromBytes(
        COM::IAssetLibrary*        assets,
        COM::IGraphicsDevice*      graphics,
        COM::Span<const std::byte> bytes,
        COM::StringView            virtualPath,
        const COM::ChainInNode*    options
    ) noexcept -> COM::Result<uint64_t> override {
        if (assets == nullptr || graphics == nullptr) {
            return COM::Error::NullArgument;
        }
        if (bytes.size > 0 && bytes.data == nullptr) {
            return COM::Error::NullArgument;
        }
        if (virtualPath.size == 0 || virtualPath.data == nullptr) {
            return COM::Error::InvalidArgument;
        }

        GLTF::ImportOptions importOpts {};
        GLTF::UploadOptions uploadOpts {};
        if (const auto parsed = ParseOptions(options, importOpts, uploadOpts); !parsed.HasValue()) {
            return parsed.error;
        }

        const std::span<const uint8_t> rawBytes(
            reinterpret_cast<const uint8_t*>(bytes.data),
            bytes.size
        );
        const auto doc = GLTF::Decode(
            rawBytes,
            std::string_view(virtualPath.data, virtualPath.size),
            {},
            importOpts,
            assets
        );
        if (!doc) {
            return doc.error();
        }
        const auto uploaded = GLTF::Upload(*assets, *graphics, *doc, uploadOpts);
        if (!uploaded) {
            return uploaded.error();
        }
        TrackTextures(*uploaded);
        return uploaded->hash;
    }

    auto LoadFromPath(
        COM::IAssetLibrary*     assets,
        COM::IGraphicsDevice*   graphics,
        COM::StringView         path,
        const COM::ChainInNode* options
    ) noexcept -> COM::Result<uint64_t> override {
        if (assets == nullptr || graphics == nullptr) {
            return COM::Error::NullArgument;
        }
        if (path.size == 0 || path.data == nullptr) {
            return COM::Error::InvalidArgument;
        }

        GLTF::ImportOptions importOpts {};
        GLTF::UploadOptions uploadOpts {};
        if (const auto parsed = ParseOptions(options, importOpts, uploadOpts); !parsed.HasValue()) {
            return parsed.error;
        }

        const std::filesystem::path filePath(std::string_view(path.data, path.size));
        const auto                  doc = GLTF::Read(filePath, importOpts, assets);
        if (!doc) {
            return doc.error();
        }
        const auto uploaded = GLTF::Upload(*assets, *graphics, *doc, uploadOpts);
        if (!uploaded) {
            return uploaded.error();
        }
        TrackTextures(*uploaded);
        return uploaded->hash;
    }

    void UnloadTextures(COM::IGraphicsDevice* graphics) noexcept override {
        if (graphics != nullptr) {
            for (const TextureID tex: _ownedTextures) {
                graphics->UnloadTexture(tex);
            }
        }
        _ownedTextures.clear();
    }

  private:
    static auto ParseOptions(
        const COM::ChainInNode* options,
        GLTF::ImportOptions&    importOpts,
        GLTF::UploadOptions&    uploadOpts
    ) noexcept -> COM::Result<void> {
        return COM::DispatchInChain<COM::AssetImportOptions, GLTF::ImportOptions>(
            options,
            [&]<typename T>(const T& value) noexcept {
                if constexpr (std::is_same_v<T, COM::AssetImportOptions>) {
                    uploadOpts.buildBLAS      = value.buildBLAS;
                    uploadOpts.buildColliders = value.buildColliders;
                } else if constexpr (std::is_same_v<T, GLTF::ImportOptions>) {
                    importOpts = value;
                }
            }
        );
    }

    void TrackTextures(const GLTF::Asset& asset) noexcept {
        if (!asset.ownsResources) {
            return;
        }
        for (const TextureID tex: asset.textures) {
            if (tex != TextureID::Invalid &&
                std::find(_ownedTextures.begin(), _ownedTextures.end(), tex) == _ownedTextures.end()) {
                _ownedTextures.push_back(tex);
            }
        }
    }

    std::vector<TextureID> _ownedTextures {};
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayerRaw(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    if (config != nullptr) {
        return Error::UnsupportedExtension;
    }
    auto* instance = new (std::nothrow) GLTFPlugin();
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayerRaw>::Seal() noexcept {}
