#pragma once

#include "atlas/interactions.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace atlas {

// Text and filtering for the interaction view; independent of native widgets.
struct InteractionRow {
    std::size_t edge_index = 0;
    std::string kind;
    std::string from;
    std::string to;
    std::string detail;
    std::string count;
    std::string location;
};

struct InteractionPresentation {
    std::vector<InteractionRow> rows;
    std::string summary;
};

[[nodiscard]] InteractionPresentation present_interactions(
    const InteractionGraph& graph, std::optional<std::size_t> selected_file,
    bool filter_selected_file);
[[nodiscard]] std::string interaction_detail_text(
    const InteractionGraph& graph, std::size_t edge_index);

} // namespace atlas
