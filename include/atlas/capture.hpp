#pragma once

#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>

namespace atlas {

struct CaptureResult {
    std::filesystem::path trace_path;
    unsigned long exit_code = 0;
    bool truncated = false;
};

// Starts a program that was linked with auto_runtime.cpp. The program must
// exit for a complete trace. No existing output file is overwritten.
std::expected<CaptureResult, std::string> capture_runtime(
    const std::filesystem::path& executable,
    const std::filesystem::path& output,
    std::stop_token stop = {});

} // namespace atlas
