// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer + COM::IAssetImporter plugin for Source
// BSP maps. Isolates BSP lump reading, VMT/VTF/MDL decoding, and zahlen_image
// behind the COM plugin boundary.

#include <BSP/BSPAssets.hpp>
#include <BSP/BSPPluginOptions.hpp>

#include <Zahlen/COM/IAssetImporter.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <algorithm>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using namespace ZHLN;

class BSPPlugin final : public COM::Implements<BSPPlugin, COM::IAppLayer, COM::IAssetImporter> {
  public:
    BSPPlugin() noexcept = default;

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
        return COM::StringView("bsp");
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

        BSP::AssetOptions assetOpts {};
        if (const auto parsed = ParseOptions(options, assetOpts); !parsed.HasValue()) {
            return parsed.error;
        }

        auto prepared = BSP::DecodeAssets(
            std::span<const std::byte>(bytes.data, bytes.size),
            BSP::SourceVFS(*assets, assetOpts.import.virtualPrefix),
            std::string_view(virtualPath.data, virtualPath.size),
            assetOpts.import
        );
        if (!prepared) {
            return prepared.error();
        }
        return UploadPrepared(*assets, *graphics, *prepared, assetOpts);
    }

    auto LoadFromVirtualPath(
        COM::IAssetLibrary*     assets,
        COM::IGraphicsDevice*   graphics,
        COM::StringView         virtualPath,
        const COM::ChainInNode* options
    ) noexcept -> COM::Result<uint64_t> override {
        if (assets == nullptr || graphics == nullptr) {
            return COM::Error::NullArgument;
        }
        if (virtualPath.size == 0 || virtualPath.data == nullptr) {
            return COM::Error::InvalidArgument;
        }

        BSP::AssetOptions assetOpts {};
        if (const auto parsed = ParseOptions(options, assetOpts); !parsed.HasValue()) {
            return parsed.error;
        }

        auto prepared = BSP::ReadAssets(
            BSP::SourceVFS(*assets, assetOpts.import.virtualPrefix),
            std::string_view(virtualPath.data, virtualPath.size),
            assetOpts.import
        );
        if (!prepared) {
            return prepared.error();
        }
        return UploadPrepared(*assets, *graphics, *prepared, assetOpts);
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
    static auto ParseOptions(const COM::ChainInNode* options, BSP::AssetOptions& out) noexcept -> COM::Result<void> {
        return COM::DispatchInChain<COM::AssetImportOptions, BSP::PluginImportOptions>(
            options,
            [&]<typename T>(const T& value) noexcept {
                if constexpr (std::is_same_v<T, COM::AssetImportOptions>) {
                    out.buildBLAS             = value.buildBLAS;
                    out.import.buildColliders = value.buildColliders;
                } else if constexpr (std::is_same_v<T, BSP::PluginImportOptions>) {
                    out.import.convertCoordinates   = value.convertCoordinates;
                    out.import.unitScale            = value.unitScale;
                    out.import.includeDisplacements = value.includeDisplacements;
                    out.import.importSkybox         = value.importSkybox;
                    out.import.gatherLights         = value.gatherLights;
                    out.import.buildColliders       = value.buildColliders;
                    if (value.virtualPrefix.data != nullptr && value.virtualPrefix.size > 0) {
                        out.import.virtualPrefix.assign(value.virtualPrefix.data, value.virtualPrefix.size);
                    }
                }
            }
        );
    }

    auto UploadPrepared(
        COM::IAssetLibrary&       assets,
        COM::IGraphicsDevice&     graphics,
        const BSP::PreparedAssets& prepared,
        const BSP::AssetOptions&  assetOpts
    ) noexcept -> COM::Result<uint64_t> {
        if (prepared.prefabs.empty()) {
            return COM::Error::InvalidArgument;
        }
        uint64_t primaryHash = 0;
        for (size_t i = 0; i < prepared.prefabs.size(); ++i) {
            const auto uploaded = BSP::UploadAsset(assets, graphics, prepared.source, prepared.prefabs[i], assetOpts);
            if (!uploaded) {
                return uploaded.error();
            }
            if (i == 0) {
                primaryHash = uploaded->hash;
            }
            for (const TextureID tex: uploaded->textures) {
                if (tex != TextureID::Invalid &&
                    std::find(_ownedTextures.begin(), _ownedTextures.end(), tex) == _ownedTextures.end()) {
                    _ownedTextures.push_back(tex);
                }
            }
        }
        return primaryHash;
    }

    std::vector<TextureID> _ownedTextures {};
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayerRaw(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    if (config != nullptr) {
        return Error::UnsupportedExtension;
    }
    auto* instance = new (std::nothrow) BSPPlugin();
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayerRaw>::Seal() noexcept {}
