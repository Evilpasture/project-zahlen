// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Types.hpp>
#include <detail/String.hpp>
#include <memory>

namespace ZHLN {

class InputContext;

class ZHLN_API Window {
  public:
	Window(const String32& title, uint32_t width, uint32_t height, InputContext* input);
	~Window();

	Window(const Window&) = delete;
	Window& operator=(const Window&) = delete;

	[[nodiscard]] bool IsRunning() const;
	void ProcessEvents();
	void Focus();

	[[nodiscard]] Extent2D GetSize() const;

	struct Impl;
	[[nodiscard]] Impl* GetImpl() const { return _impl.get(); }

	// Returns the underlying LLGL::Window* as a void* to keep headers clean
	[[nodiscard]] void* GetNativeHandle() const;

	void Close();

  private:
	std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
