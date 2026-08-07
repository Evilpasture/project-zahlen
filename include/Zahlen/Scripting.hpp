// include/Zahlen/Scripting.hpp
#pragma once

#include <memory>
#include <string_view>

namespace ZHLN {

class Engine; // Forward declaration
class IScriptRuntime;

class ScriptRunner {
  public:
    ScriptRunner();
    ~ScriptRunner();

    ScriptRunner(const ScriptRunner&)            = delete;
    ScriptRunner& operator=(const ScriptRunner&) = delete;

    void RunFile(std::string_view path);
    void CallUpdate(Engine* engine, float dt);
    void ExecuteString(std::string_view code);
    void ReloadFile(std::string_view path);

    [[nodiscard]] IScriptRuntime* GetRuntime() const noexcept {
        return _runtime.get();
    }

  private:
    std::unique_ptr<IScriptRuntime> _runtime;
};

} // namespace ZHLN
