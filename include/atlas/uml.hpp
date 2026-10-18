#pragma once

#include "atlas/atlas.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace atlas {

enum class UmlRelation { inheritance, field_reference };

struct UmlNode {
    std::string identity;
    std::string label;
    std::size_t file = 0;
    std::size_t symbol = 0;
};

struct UmlEdge {
    std::size_t from = 0;
    std::size_t to = 0;
    UmlRelation relation = UmlRelation::inheritance;
};

struct UmlGraph {
    std::vector<UmlNode> nodes;
    std::vector<UmlEdge> edges;
};

struct UmlRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct UmlPlacedNode {
    std::size_t node = 0;
    UmlRect rect;
};

struct UmlGraphLayout {
    int width = 0;
    int height = 0;
    std::size_t connected_count = 0;
    std::size_t omitted_count = 0;
    std::vector<UmlPlacedNode> nodes;
    std::vector<std::size_t> edges;
};

// An intra-project class graph. Unresolved external types remain in focused views.
[[nodiscard]] UmlGraph build_uml_graph(const ProjectScan& scan);

// Deterministic, scrollable graph geometry. Connected components occupy separate
// lanes; isolated classes can be revealed without shrinking the default view.
[[nodiscard]] UmlGraphLayout layout_uml_graph(
    const UmlGraph& graph, bool include_isolated,
    std::optional<std::size_t> selected_node = std::nullopt);

} // namespace atlas
