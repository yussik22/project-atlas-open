#pragma once

#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>

namespace atlas {

struct CaptureResult {
    std::filesystem::path trace_path;
    unsigned long exit_code = 0;
    bool truncated = false;
};

// Starts a program that was linked with auto_runtime.cpp. The program must
// exit for a complete trace. No existing output file is overwritten.
// Optional test_id labels this one process run in the trace and must use
// 1-127 ASCII letters, digits, underscores, dots or hyphens.
std::expected<CaptureResult, std::string> capture_runtime(
    const std::filesystem::path& executable,
    const std::filesystem::path& output,
    std::stop_token stop = {},
    std::string_view test_id = {});

} // namespace atlas
