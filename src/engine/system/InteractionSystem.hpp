#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {

class Engine;

class ZHLN_API InteractionSystem {
  public:
	InteractionSystem() = default;
	~InteractionSystem() = default;

	void Update(Engine& engine, float dt);
};

} // namespace ZHLN
