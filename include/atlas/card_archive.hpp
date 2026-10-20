#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace atlas {

// Move adjacent Project Atlas cards whose source file is gone into a recoverable
// project-local archive. User-owned cards and cards for existing sources stay put.
std::expected<std::size_t, std::string> archive_orphan_source_cards(
    const std::filesystem::path& project_root,
    const std::filesystem::path& report_output);

// Keep a generated report directory free of cards/summaries for deleted files
// without changing the project tree when scan-in-place was not requested.
std::expected<std::size_t, std::string> archive_orphan_report_cards(
    const std::filesystem::path& project_root,
    const std::filesystem::path& report_output);

} // namespace atlas
