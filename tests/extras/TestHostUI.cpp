// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <UI/HostUI.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include "TestsFramework.hpp"
#include <memory>
#include <utility>
#include <vector>

namespace ZHLN::Test {

void test_explicit_host_ui_owns_atlas_and_renders() {
    struct UILayer {
        int*                    frames = nullptr;
        std::unique_ptr<HostUI> ui;

        void Start(COM::IEngine& engine) { ui = std::make_unique<HostUI>(engine); }
        void Tick(COM::IEngine& engine, float dt) {
            ui->BeginFrame();
            const auto& atlas = ui->GetFontAtlas();
            Expect(atlas.texture != TextureID::Invalid);
            auto gui = ui->Context().BeginFrame(ui->ConsumeInput(dt > 0.0f ? dt : 1.0f / 60.0f));
            gui.Text("Explicit HostUI extension");
            const auto data = std::move(gui).End();
            Expect(ui->Submit(data).HasValue());
            ++*frames;
            if (engine.CurrentFrame() >= 1) engine.RequestClose();
        }
        void Stop(COM::IEngine&) { ui.reset(); }
    };

    int  frames = 0;
    auto result = App()
                      .Title("ExplicitHostUIRender")
                      .Resolution(96, 64)
                      .Validation(false)
                      .Headless(true)
                      .LogLevel(LogLevel::Quiet)
                      .AddLayer(UILayer {.frames = &frames})
                      .Run();
    Expect(result.has_value());
    Expect(frames >= 2);
}

void test_explicit_host_ui_injected_input_owns_spans() {
    struct Layer {
        void Start(COM::IEngine& engine) {
            HostUI                ui(engine);
            std::vector<KeyCode>  keys {KeyCode::A, KeyCode::B};
            std::vector<uint32_t> chars {U'a', U'b'};
            ui.SubmitInput(GUI::UIInput {.pressedKeys = keys, .characterStream = chars});
            keys.assign(1, KeyCode::Z);
            chars.assign(1, U'z');
            (void)ui.ConsumeInput(0, static_cast<WindowID>(0x7fffffffu));
            const auto consumed = ui.ConsumeInput(0.01f);
            ExpectEq(consumed.pressedKeys.size(), size_t {2});
            ExpectEq(consumed.characterStream.size(), size_t {2});
            if (consumed.pressedKeys.size() == 2) Expect(consumed.pressedKeys[0] == KeyCode::A);
            if (consumed.characterStream.size() == 2) ExpectEq(consumed.characterStream[1], uint32_t {U'b'});
            const auto next = ui.ConsumeInput(0.01f);
            Expect(next.characterStream.empty() && next.pressedKeys.empty());
            engine.RequestClose();
        }
    };

    const auto result = App()
                            .Title("ExplicitHostUIInput")
                            .Resolution(96, 64)
                            .Validation(false)
                            .Headless(true)
                            .LogLevel(LogLevel::Quiet)
                            .AddLayer(Layer {})
                            .Run();
    Expect(result.has_value());
}

} // namespace ZHLN::Test
