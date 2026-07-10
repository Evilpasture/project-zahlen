#pragma once

namespace ZHLN::Vk {

class RayTracingContext {
  public:
	RayTracingContext() = default;

	[[nodiscard]] bool Init(VkDevice device) noexcept;
	[[nodiscard]] bool Valid() const noexcept { return _raw.device != VK_NULL_HANDLE; }

	void GetBlasSizes(const ZHLN_BlasGeometryDesc& desc, uint32_t primCount,
					  ZHLN_AccelerationStructureSizes& outSizes) const noexcept;
	void GetTlasSizes(uint32_t instanceCount,
					  ZHLN_AccelerationStructureSizes& outSizes) const noexcept;

	[[nodiscard]] VkAccelerationStructureKHR
	CreateAS(VkBuffer buffer, VkDeviceSize size, ZHLN_AccelerationStructureType type) const noexcept;
	void DestroyAS(VkAccelerationStructureKHR as) const noexcept;
	[[nodiscard]] VkDeviceAddress GetASAddress(VkAccelerationStructureKHR as) const noexcept;

	void CmdBuildBlas(VkCommandBuffer cmd, const ZHLN_BlasGeometryDesc& desc,
					  VkAccelerationStructureKHR dst, VkDeviceAddress scratch,
					  uint32_t primCount) const noexcept;
	void CmdBuildTlas(VkCommandBuffer cmd, const ZHLN_TlasGeometryDesc& desc,
					  VkAccelerationStructureKHR dst, VkDeviceAddress scratch,
					  uint32_t instanceCount) const noexcept;

  private:
	ZHLN_RayTracingContext _raw{};
};
} // namespace ZHLN::Vk
