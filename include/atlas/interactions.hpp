#pragma once

#include "atlas/atlas.hpp"
#include "atlas/runtime.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace atlas {

enum class InteractionNodeKind { source_file, runtime_variable, runtime_object, runtime_site };
enum class InteractionKind { inheritance, field_type, static_call, observed_link };

struct InteractionNode {
    std::string id;
    std::string label;
    InteractionNodeKind kind = InteractionNodeKind::source_file;
    std::optional<std::size_t> file_index;
};

struct InteractionEdge {
    std::size_t from = 0;
    std::size_t to = 0;
    InteractionKind kind = InteractionKind::static_call;
    std::string detail; // field name, call pair, or observed relation
    std::size_t count = 1;
    std::size_t first_line = 0;
    std::size_t first_column = 0;
    std::size_t snapshot_step = 0;
    bool dynamic_dispatch = false;
    bool indirect = false;
};

struct InteractionGraph {
    std::vector<InteractionNode> nodes;
    std::vector<InteractionEdge> edges;
    std::size_t unmapped_calls = 0; // resolved declaration without a scanned-project file
    std::size_t unresolved_calls = 0;
    std::size_t dynamic_call_sites = 0;
    std::size_t indirect_call_sites = 0;
    std::optional<std::size_t> runtime_step;
    bool capture_truncated = false;
};

// Combines evidence without inventing a static-to-runtime identity. Runtime
// nodes stay in their own namespace until a source mapping can be proven.
[[nodiscard]] InteractionGraph build_interaction_graph(
    const ProjectScan& scan,
    const std::optional<RuntimeSnapshot>& snapshot = std::nullopt);

[[nodiscard]] std::string interaction_kind_name(InteractionKind kind);
[[nodiscard]] std::string interaction_node_kind_name(InteractionNodeKind kind);
[[nodiscard]] std::string agent_interactions_json(
    const InteractionGraph& graph, std::size_t max_edges = 2000);

} // namespace atlas
