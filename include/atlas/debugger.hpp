#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace atlas {

// One debuggee per Atlas window. The debugger owns its process and keeps all
// Windows debug events and DbgHelp calls on one worker thread.
class NativeDebugger {
public:
    NativeDebugger();
    ~NativeDebugger();
    NativeDebugger(const NativeDebugger&) = delete;
    NativeDebugger& operator=(const NativeDebugger&) = delete;

    bool launch(const std::filesystem::path& executable, std::string& error);
    bool breakpoint(const std::filesystem::path& source, std::uint32_t line,
                    std::string& error);
    bool resume(bool single_step, std::string& error);
    bool stop(std::string& error);
    [[nodiscard]] std::string snapshot_json() const;
    [[nodiscard]] std::string snapshot_text() const;
    [[nodiscard]] std::string summary() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace atlas
