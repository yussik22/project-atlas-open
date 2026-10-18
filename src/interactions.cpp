#include "atlas/interactions.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <tuple>

namespace atlas {
namespace {

std::string utf8_path(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string json_string(std::string_view value) {
    std::ostringstream out;
    out << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out << '\\' << static_cast<char>(c); }
        else if (c < 0x20) {
            out << "\\u00" << hex[c >> 4] << hex[c & 15];
        } else out << static_cast<char>(c);
    }
    out << '"';
    return out.str();
}

using EdgeKey = std::tuple<std::size_t, std::size_t, InteractionKind, std::string>;

} // namespace

std::string interaction_kind_name(InteractionKind kind) {
    switch (kind) {
    case InteractionKind::inheritance: return "inheritance";
    case InteractionKind::field_type: return "field_type";
    case InteractionKind::static_call: return "static_call";
    case InteractionKind::observed_link: return "observed_link";
    }
    return "unknown";
}

std::string interaction_node_kind_name(InteractionNodeKind kind) {
    switch (kind) {
    case InteractionNodeKind::source_file: return "source_file";
    case InteractionNodeKind::runtime_variable: return "runtime_variable";
    case InteractionNodeKind::runtime_object: return "runtime_object";
    case InteractionNodeKind::runtime_site: return "runtime_site";
    }
    return "unknown";
}

InteractionGraph build_interaction_graph(
    const ProjectScan& scan, const std::optional<RuntimeSnapshot>& snapshot) {
    InteractionGraph graph;
    std::map<fs::path, std::size_t> files;
    for (std::size_t i = 0; i < scan.files.size(); ++i) {
        const auto& path = scan.files[i].relative_path;
        files.emplace(path, graph.nodes.size());
        graph.nodes.push_back({"file:" + utf8_path(path), utf8_path(path),
                               InteractionNodeKind::source_file, i});
    }
    const auto file_node = [&](const fs::path& path) {
        if (const auto it = files.find(path); it != files.end()) return it->second;
        const auto index = graph.nodes.size();
        files.emplace(path, index);
        graph.nodes.push_back({"file:" + utf8_path(path), utf8_path(path),
                               InteractionNodeKind::source_file, std::nullopt});
        return index;
    };
    // A USR is the identity. Duplicate declarations keep the first scanned
    // owner; this is a source-file map, not a claim about the unique definition.
    std::map<std::string, fs::path> type_file;
    for (const auto& file : scan.files)
        for (const auto& symbol : file.symbols) {
            const auto label = symbol.qualified_name.empty()
                ? symbol.name : symbol.qualified_name;
            const auto identity = symbol.usr.empty()
                ? "qualified:" + label : symbol.usr;
            type_file.try_emplace(identity, file.relative_path);
        }

    std::map<EdgeKey, std::size_t> grouped;
    const auto append = [&](std::size_t from, std::size_t to, InteractionKind kind,
                            std::string detail, std::string identity,
                            std::size_t line, std::size_t column,
                            std::size_t step, bool dynamic, bool indirect) {
        auto [it, inserted] = grouped.try_emplace(
            EdgeKey{from, to, kind, std::move(identity)}, graph.edges.size());
        if (inserted) {
            graph.edges.push_back({from, to, kind, std::move(detail), 1,
                                   line, column, step, dynamic, indirect});
        } else {
            auto& edge = graph.edges[it->second];
            ++edge.count;
            edge.dynamic_dispatch |= dynamic;
            edge.indirect |= indirect;
            if (line && (!edge.first_line || line < edge.first_line)) {
                edge.first_line = line;
                edge.first_column = column;
            }
        }
    };
    for (const auto& file : scan.files) {
        const auto from = file_node(file.relative_path);
        for (const auto& symbol : file.symbols) {
            const auto owner = symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
            const auto owner_key = symbol.usr.empty()
                ? owner + "@" + std::to_string(symbol.line) : symbol.usr;
            for (const auto& base : symbol.base_usrs) {
                if (const auto found = type_file.find(base); found != type_file.end())
                    append(from, file_node(found->second), InteractionKind::inheritance,
                           owner, owner_key + "->" + base,
                           symbol.line, 0, 0, false, false);
            }
            for (const auto& member : symbol.members) {
                if (member.kind != "field" && member.kind != "static_field") continue;
                const auto add_type = [&](const std::string& identity) {
                    if (const auto found = type_file.find(identity); found != type_file.end())
                        append(from, file_node(found->second), InteractionKind::field_type,
                               owner + "::" + member.name,
                               owner_key + "::" + member.name + "->" + identity,
                               member.line, 0, 0, false, false);
                };
                if (member.related_usrs.empty()) add_type(member.related_usr);
                else for (const auto& identity : member.related_usrs) add_type(identity);
            }
        }
        for (const auto& call : file.calls) {
            graph.dynamic_call_sites += call.dynamic_dispatch;
            graph.indirect_call_sites += call.indirect;
            if (call.callee_file.empty()) {
                if (call.callee_usr.empty()) ++graph.unresolved_calls;
                else ++graph.unmapped_calls;
                continue;
            }
            const auto display = call.caller_name + " -> " + call.callee_name;
            const auto identity = call.caller_usr.empty() || call.callee_usr.empty()
                ? display + "@" + std::to_string(call.line) + ":" +
                    std::to_string(call.column)
                : call.caller_usr + "->" + call.callee_usr;
            append(from, file_node(call.callee_file), InteractionKind::static_call,
                   display, identity,
                   call.line, call.column, 0,
                   call.dynamic_dispatch, call.indirect);
        }
    }
    if (snapshot) {
        graph.runtime_step = snapshot->step;
        graph.capture_truncated = snapshot->capture_truncated;
        std::map<std::string, std::size_t> runtime_nodes;
        for (const auto& node : snapshot->nodes) {
            const auto kind = node.kind == RuntimeNodeKind::variable
                ? InteractionNodeKind::runtime_variable
                : node.kind == RuntimeNodeKind::site
                    ? InteractionNodeKind::runtime_site
                    : InteractionNodeKind::runtime_object;
            runtime_nodes.emplace(node.id, graph.nodes.size());
            graph.nodes.push_back({"runtime:" + node.id, node.label, kind, std::nullopt});
        }
        for (const auto& link : snapshot->links) {
            const auto from = runtime_nodes.find(link.source);
            const auto to = runtime_nodes.find(link.target);
            if (from != runtime_nodes.end() && to != runtime_nodes.end())
                append(from->second, to->second, InteractionKind::observed_link,
                       link.relation, link.relation,
                       0, 0, snapshot->step, false, false);
        }
    }
    std::ranges::sort(graph.edges, [](const InteractionEdge& a, const InteractionEdge& b) {
        return std::tie(a.kind, a.from, a.to, a.detail,
                        a.first_line, a.first_column) <
               std::tie(b.kind, b.from, b.to, b.detail,
                        b.first_line, b.first_column);
    });
    return graph;
}

std::string agent_interactions_json(const InteractionGraph& graph, std::size_t max_edges) {
    const auto returned = std::min(graph.edges.size(), max_edges);
    std::ostringstream out;
    out << "{\"schema\":\"project-atlas.interactions.v1\",\"operation\":\"interactions\""
        << ",\"node_count\":" << graph.nodes.size()
        << ",\"edge_count\":" << graph.edges.size()
        << ",\"returned_edges\":" << returned
        << ",\"truncated\":" << (returned < graph.edges.size() ? "true" : "false")
        << ",\"unmapped_calls\":" << graph.unmapped_calls
        << ",\"unresolved_calls\":" << graph.unresolved_calls
        << ",\"dynamic_call_sites\":" << graph.dynamic_call_sites
        << ",\"indirect_call_sites\":" << graph.indirect_call_sites
        << ",\"runtime_step\":";
    if (graph.runtime_step) out << *graph.runtime_step;
    else out << "null";
    out << ",\"capture_truncated\":" << (graph.capture_truncated ? "true" : "false");
    out << ",\"limitations\":\"AST edges are not observed executions; runtime IDs are not joined to source without a proven mapping; capture may omit allocations\""
        << ",\"nodes\":[";
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        if (i) out << ',';
        const auto& node = graph.nodes[i];
        out << "{\"id\":" << json_string(node.id)
            << ",\"label\":" << json_string(node.label)
            << ",\"kind\":" << json_string(interaction_node_kind_name(node.kind))
            << ",\"file_index\":";
        if (node.file_index) out << *node.file_index;
        else out << "null";
        out << '}';
    }
    out << "],\"edges\":[";
    for (std::size_t i = 0; i < returned; ++i) {
        if (i) out << ',';
        const auto& edge = graph.edges[i];
        out << "{\"from\":" << edge.from << ",\"to\":" << edge.to
            << ",\"kind\":" << json_string(interaction_kind_name(edge.kind))
            << ",\"detail\":" << json_string(edge.detail)
            << ",\"count\":" << edge.count
            << ",\"first_line\":" << edge.first_line
            << ",\"first_column\":" << edge.first_column
            << ",\"snapshot_step\":" << edge.snapshot_step
            << ",\"dynamic_dispatch\":" << (edge.dynamic_dispatch ? "true" : "false")
            << ",\"indirect\":" << (edge.indirect ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}

} // namespace atlas
