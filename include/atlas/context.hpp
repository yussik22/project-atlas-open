#pragma once

#include "atlas/atlas.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace atlas {

// Compact, evidence-linked retrieval. The budget is an approximate token target:
// output is bounded to at most four UTF-8 bytes per requested token.
[[nodiscard]] std::string agent_context_json(
    const ProjectScan& scan, std::string_view task,
    std::size_t approximate_token_budget = 1500);

} // namespace atlas
