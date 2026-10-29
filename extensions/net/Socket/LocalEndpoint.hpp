#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Net {

enum class IpcScope : uint8_t {
    UserSession, // Isolated to the active user session (XDG_RUNTIME_DIR / per-user Named Pipe)
    SystemWide   // Shared machine-wide (/tmp or /var/run / Global pipe)
};

struct LocalEndpointPattern {
    std::string_view serviceName;
    IpcScope         scope        = IpcScope::UserSession;
    uint32_t         maxInstances = 1;    // e.g. 10 -> tries "name-0" through "name-9"
    bool             indexed      = true; // append "-0", "-1" etc.
};

// Generates candidate addresses for the current OS in priority order
[[nodiscard]] auto EnumerateLocalEndpoints(LocalEndpointPattern pattern) -> std::vector<std::string>;

} // namespace ZHLN::Net
