#pragma once

#include "atlas/atlas.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace atlas {

// Fast, bounded, read-only source lookup. The optional FNV-1a hash makes a
// previous AST/context reference fail closed when the file has changed.
[[nodiscard]] std::expected<std::string, std::string> agent_source_json(
    const fs::path& root, const fs::path& relative_path,
    std::size_t start_line, std::size_t line_count,
    const std::optional<std::string_view>& expected_hash = std::nullopt);

// Direct, provenance-labelled AST relationships for one scanned file.
// These are candidate dependencies, not a complete runtime impact analysis.
[[nodiscard]] std::expected<std::string, std::string> agent_file_relations_json(
    const ProjectScan& scan, const fs::path& relative_path,
    std::size_t max_edges = 80);

} // namespace atlas
