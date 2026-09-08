// resources/shaders/hang_gpu.slang
//
// VK_KHR_shader_abort: OpAbortKHR ceases this invocation and loses the
// device in finite time. The format string is retrieved after device-lost
// via vkGetDeviceFaultDebugInfoKHR (VkDeviceFaultShaderAbortMessageInfoKHR).

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    abort("Zahlen hang_gpu: OpAbortKHR tid=%u", tid.x);
}
