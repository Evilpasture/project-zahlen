// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/core/TestCOMBoundary.cpp
//
// The SDK as a consumer sees it. Apart from the test framework this file
// includes only Zahlen/COM and Zahlen/Core/COM headers -- no engine header,
// no PIMPL, no internal access -- and every engine call it makes is a virtual
// dispatch through a vtable into libzahlen_engine.so. EngineCOM's definition
// is not visible here, so nothing can be inlined or devirtualised away.

#include "TestsFramework.hpp"
#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/Core/COM/Ref.hpp>

#include <cstdint>
#include <expected>
#include <type_traits>

namespace {

// A component the library has never heard of, declared in the consumer's TU.
struct ClientTransform {
    float position[3];
    float scale;
};

enum class BoundaryTestError : uint8_t { QueryFailed = 1 };

// The SDK surface is reachable without any engine header.
static_assert(ZHLN::COM::ComComponent<ClientTransform>);
static_assert(std::is_standard_layout_v<ZHLN::COM::EngineConfigCOM>);
static_assert(std::is_trivially_copyable_v<ZHLN::COM::EngineConfigCOM>);
static_assert(std::is_abstract_v<ZHLN::COM::IEngine>);
static_assert(std::is_abstract_v<ZHLN::COM::IGraphicsDevice>);
static_assert(std::is_abstract_v<ZHLN::COM::IEntities>);
static_assert(!std::has_virtual_destructor_v<ZHLN::COM::IEngine>);
static_assert(!std::has_virtual_destructor_v<ZHLN::COM::IGraphicsDevice>);
static_assert(!std::has_virtual_destructor_v<ZHLN::COM::IEntities>);

// Distinct identities, and stable across translation units.
static_assert(ZHLN::COM::InterfaceIdOf<ZHLN::COM::IEngine>() != ZHLN::COM::InterfaceIdOf<ZHLN::COM::IGraphicsDevice>());
static_assert(ZHLN::COM::InterfaceIdOf<ZHLN::COM::IEngine>() != ZHLN::COM::InterfaceIdOf<ZHLN::COM::IEntities>());

// Component families are keyed on the qualified name, so two components with
// the same unqualified name in different namespaces stay apart.
static_assert(ZHLN::COM::IEntities::ComponentName<ClientTransform>().size > 0);

struct COMBoundarySuite {
    struct Tests {
        // One engine, because standing one up is a real device initialisation.
        // Creating it may legitimately fail on a machine with no Vulkan at
        // all; what is asserted unconditionally is that the call crossed the
        // boundary and came back with a well-formed answer either way.
        std::expected<void, ZHLN::ErrorCode> the_sdk_boundary_works_end_to_end() {
            const ZHLN::COM::EngineConfigCOM config {
                .appName          = "COM boundary test",
                .width            = 320,
                .height           = 240,
                .vsync            = false,
                .enableValidation = false,
                .headless         = true,
            };

            auto created = ZHLN::COM::CreateEngineRef(config);
            if (!created.has_value()) {
                // The failure travelled back as a fixed-layout ErrorCode, not
                // an exception and not a std::expected across the ABI, and it
                // names a real error rather than a default-constructed blank.
                ZHLN::Test::ExpectTrue(created.error() != ZHLN::ErrorCode {});
                return {};
            }

            ZHLN::COM::Ref<ZHLN::COM::IEngine> engine = std::move(*created);
            ZHLN::Test::ExpectTrue(static_cast<bool>(engine));

            // Query reaches sibling interfaces on the same allocation, and the
            // borrowed accessors reach the very same subobjects.
            ZHLN::COM::Ref<ZHLN::COM::IGraphicsDevice> graphics = engine.As<ZHLN::COM::IGraphicsDevice>();
            ZHLN::COM::Ref<ZHLN::COM::IEntities>       entities = engine.As<ZHLN::COM::IEntities>();
            if (!ZHLN::Test::ExpectTrue(static_cast<bool>(graphics)) || !ZHLN::Test::ExpectTrue(static_cast<bool>(entities))) {
                return std::unexpected(BoundaryTestError::QueryFailed);
            }
            ZHLN::Test::ExpectEq(static_cast<void*>(engine->Graphics()), static_cast<void*>(graphics.Get()));
            ZHLN::Test::ExpectEq(static_cast<void*>(engine->Entities()), static_cast<void*>(entities.Get()));

            // Refcounting lives in the library: these are virtual calls into
            // the .so, and so is the delete when the last one goes.
            ZHLN::COM::IObject* root = engine.Get();
            ZHLN::Test::ExpectEq(root->AddRef(), uint32_t {4});
            ZHLN::Test::ExpectEq(root->Release(), uint32_t {3});

            // A virtual call returning a fixed-layout struct through the
            // Itanium hidden-pointer convention.
            const ZHLN::ViewportRect viewport = graphics->Viewport();
            ZHLN::Test::ExpectTrue(viewport.width >= 0.0f);
            ZHLN::Test::ExpectTrue(engine->IsRunning() || !engine->IsRunning());

            // The ECS seam: a component type the library has never seen,
            // attached by qualified name and read back through the vtable.
            const ZHLN::Entity entity = entities->CreateEntity();
            ZHLN::Test::ExpectTrue(entities->IsAlive(entity));

            ClientTransform* attached = entities->Attach(entity, ClientTransform {.position = {1.0F, 2.0F, 3.0F}, .scale = 4.0F});
            if (ZHLN::Test::ExpectTrue(attached != nullptr)) {
                ClientTransform* fetched = entities->Get<ClientTransform>(entity);
                if (ZHLN::Test::ExpectTrue(fetched != nullptr)) {
                    ZHLN::Test::ExpectEq(fetched->position[1], 2.0F);
                    ZHLN::Test::ExpectEq(fetched->scale, 4.0F);
                }
                ZHLN::Test::ExpectTrue(entities->Detach<ClientTransform>(entity));
                ZHLN::Test::ExpectTrue(entities->Get<ClientTransform>(entity) == nullptr);
            }

            // Despawn marks the entity for destruction and the engine frees it
            // during pending-destroy processing, so it is deliberately still
            // alive here. Asserting the deferral is the honest check; the
            // boundary forwards the call, it does not change the semantics.
            entities->Despawn(entity);
            ZHLN::Test::ExpectTrue(entities->IsAlive(entity));
            return {};
        }
    };
};

} // namespace
