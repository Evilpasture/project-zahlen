#include "PipelineBuilder.hpp"
#include <print>

namespace ZHLN::Vk {

void ReportPipelineBuilderError(PipelineBuilderResult result) noexcept {
	switch (result) {
		case PipelineBuilderResult::Succeeded:
			// No error to report
			break;
		case PipelineBuilderResult::MissingShaders:
			std::println(stderr, "[PipelineBuilder] Missing shader stages.");
			break;
		case PipelineBuilderResult::MissingLayout:
			std::println(stderr, "[PipelineBuilder] Missing pipeline layout.");
			break;
	}
}

void ReportComputePipelineBuilderError(PipelineBuilderResult result) noexcept {
	switch (result) {
		case PipelineBuilderResult::Succeeded:
			// No error to report
			break;
		case PipelineBuilderResult::MissingShaders:
			std::println(stderr, "[ComputePipelineBuilder] Missing or invalid shader code.");
			break;
		case PipelineBuilderResult::MissingLayout:
			std::println(stderr, "[ComputePipelineBuilder] Missing pipeline layout.");
			break;
	}
}

} // namespace ZHLN::Vk
