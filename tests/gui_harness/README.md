# GUI button harness

A host-only harness for `ZHLN::GUI::Context` button interaction. It links the
**real** `src/gui/GUIContext.cpp` against the **real** `extern/clay/clay.h` (the
commit the submodule pins) and drives it from a bare `ZHLN::ECS::Registry` — no
`Engine`, no `RenderContext`, no Vulkan, no window, no GPU.

Nothing here re-implements or copies the widget logic. `Context::Box`,
`Context::Button`, `Context::IsItemHovered` and `Context::IsItemActive` are the
shipped functions; `clay.h` is the shipped Clay. Only logging/assert plumbing and
the unlinked `Engine`/`Window`/`RenderContext` entry points are stubbed, and
every stub sits on a path the harness never executes (see the header comment in
`harness_stubs.cpp`).

## Build & run

```sh
tests/gui_harness/build.sh              # replay the ctest scenario, expect 0 failures
tests/gui_harness/build.sh --probe      # dump widget rects + sweep the pointer
```

Picks `clang++` if present, otherwise `zig c++` from `pip install ziglang`
(zig ships clang + libc++, which has `<format>`/`<expected>`), otherwise `g++`.
Override with `CXX=...`. The project's own toolchain needs no changes; this
build exists so the widget can be exercised on a machine with no GPU.

Output lands in `build/gui_harness/`, which `.gitignore` already covers.

## What it verifies

`main.cpp` replays the exact 8-tick scenario from
`tests/render/TestUI.cpp::button_click_interaction_and_states` — same container
config, same pointer coordinates, same assertions — and prints, per frame, the
laid-out rect of each button, what `IsItemHovered`/`IsItemActive` returned, the
click counters and how many elements Clay considered under the pointer.

Baseline result: **32 checks, 0 failures.** The button widget, the hover/active
state machine, the single-fire-on-press guarantee and the drag-into-no-click
behaviour are all correct.

## Fault injection

`--fault=<name>` breaks one engine-side thing at a time and prints the failure
signature it produces, so a ctest failure can be matched to a cause:

| `--fault=`       | what breaks                                          | signature            |
| ---------------- | ---------------------------------------------------- | -------------------- |
| `none`           | nothing                                              | 0 failures           |
| `nocallback`     | the host UI callback is never invoked                 | **14 failures**      |
| `noinput`        | `GetSingleton<InputStateComponent>()` finds nothing   | **14 failures**      |
| `stalemouse`     | mouse position never reaches `InputStateComponent`    | **14 failures**      |
| `nomousebutton`  | position arrives, `LButton` never does                | 9 failures (hover OK) |

The 14-failure set is exactly
`isHoveredA / hoverCountA>0 / wasClickedA / isActiveA / clickCountA==1 /
wasClickedB / clickCountB==1` at `TestUI.cpp:189,195,202,203,204,206,215,216,218,
226,228,253,254,255` — every "expect true"/"expect 1" fails and every
"expect false"/"expect 0" passes.

The 9-failure set is distinguishable: hover still works, only press/active/click
break. That is the signature of `SetKey(KeyCode::LButton, ...)` being dropped.

## Other knobs

- `--viewport=WxH` — layout viewport. Verified irrelevant: ButtonA lands at
  `x[20,131] y[20,64]` at 640x480, 1280x720, 1920x1080, 300x200 and even 0x0.
- `--font=fallback|realistic|nan|huge|zero` — installs a `UISettingsComponent`
  font atlas so `Context::BeginFrame` measures text with real glyph advances
  instead of the built-in 18px fallback. Changes ButtonA's fit-to-content width
  (`x[20,117]` at `realistic`) but never moves it off `(50,35)`.

## The `reflection_shim.hpp` force-include — load-bearing

`InputStateComponent::keys` is a `std::bitset<Reflect::EnumCount<KeyCode>()>`.
`EnumCount` has two spellings in `include/Zahlen/Core/Reflection.hpp`:

- with C++26 static reflection (P2996 — the project's Linux toolchain):
  `std::meta::enumerators_of(^^E).size()`, which is **72** for `KeyCode`;
- without it (clang/zig, i.e. this harness): **`return 0`**.

A `std::bitset<0>` makes every `SetKey()` hit its `key >= keys.size()` guard and
silently drop the key, so a naive harness build sees no mouse button ever go
down. `build.sh` force-includes `reflection_shim.hpp` into every TU to pin the
count at 72, and the same header `static_assert`s the real count on any toolchain
that has reflection, so the number cannot silently drift. The harness prints the
bitset width at startup so the value is visible in every run.
