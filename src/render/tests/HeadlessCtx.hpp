#pragma once
#include "RenderCore.hpp"

// Returns a fully managed, RAII Context ready for headless testing.
ZHLN::Vk::Context MakeHeadlessCtx();