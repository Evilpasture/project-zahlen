#include "engine/detail/Loop.hpp"
#include "engine/detail/Prefetch.hpp"
#include "engine/detail/Span.hpp"

#include <print>
#include <span>

using namespace ZHLN;

auto main(int argc, char* argv[]) -> int {
	auto args = std::span(argv, argc);

	std::print("Hello! You passed {} arguments.\n", argc);

	for (size_t i = 1; i < args.size(); ++i) {
		std::println("Ignoring first argument.");
		std::print("Argument [{}]: {}\n", i, args[i]);
	}

	int values[] = {1, 2, 3, 4, 5, 6, 7, 8};
	RestrictSpan<int> span(values);

	// Prefetch the first element to demonstrate the Prefetch header.
	Prefetch<AccessType::Read, CacheLevel::L1>(span.data());

	int sum = 0;
	UnrollLoop<4>(span.size(), [&](size_t index) { sum += span[index]; });

	std::print("Computed sum of {} values = {}\n", span.size(), sum);

	constexpr auto testRepeat = [] {
		int counter = 0;
		Repeat<3>([&] { ++counter; });
		return counter;
	}();

	std::print("Repeat test count = {}\n", testRepeat);

	return (sum == 36 && testRepeat == 3) ? 0 : 1;
}