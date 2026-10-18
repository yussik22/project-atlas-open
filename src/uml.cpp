#include "atlas/uml.hpp"

#include <map>
#include <algorithm>
#include <queue>
#include <set>
#include <tuple>

namespace atlas {

UmlGraph build_uml_graph(const ProjectScan& scan) {
    UmlGraph graph;
    std::map<std::string, std::size_t> by_identity;
    for (std::size_t file_index = 0; file_index < scan.files.size(); ++file_index) {
        const auto& file = scan.files[file_index];
        for (std::size_t symbol_index = 0; symbol_index < file.symbols.size(); ++symbol_index) {
            const auto& symbol = file.symbols[symbol_index];
            const auto label = symbol.qualified_name.empty()
                ? symbol.name : symbol.qualified_name;
            const auto identity = symbol.usr.empty()
                ? "qualified:" + label : symbol.usr;
            const auto [it, inserted] = by_identity.emplace(identity, graph.nodes.size());
            if (inserted) {
                graph.nodes.push_back(UmlNode{identity, label, file_index, symbol_index});
            } else {
                auto& existing = graph.nodes[it->second];
                const auto& previous = scan.files[existing.file].symbols[existing.symbol];
                if (symbol.members.size() + symbol.base_usrs.size() >
                    previous.members.size() + previous.base_usrs.size()) {
                    existing = UmlNode{identity, label, file_index, symbol_index};
                }
            }
        }
    }
    std::set<std::tuple<std::size_t, std::size_t, UmlRelation>> seen;
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        const auto& node = graph.nodes[index];
        const auto& symbol = scan.files[node.file].symbols[node.symbol];
        for (const auto& base : symbol.base_usrs) {
            const auto found = by_identity.find(base);
            if (found != by_identity.end() && found->second != index &&
                seen.emplace(found->second, index, UmlRelation::inheritance).second)
                graph.edges.push_back({found->second, index, UmlRelation::inheritance});
        }
        for (const auto& member : symbol.members) {
            const auto append_relation = [&](const std::string& identity) {
                const auto found = by_identity.find(identity);
                if (found != by_identity.end() && found->second != index &&
                    seen.emplace(index, found->second, UmlRelation::field_reference).second)
                    graph.edges.push_back({index, found->second, UmlRelation::field_reference});
            };
            if (member.related_usrs.empty()) append_relation(member.related_usr);
            else for (const auto& identity : member.related_usrs) append_relation(identity);
        }
    }
    return graph;
}

UmlGraphLayout layout_uml_graph(const UmlGraph& graph, bool include_isolated,
                                std::optional<std::size_t> selected_node) {
    UmlGraphLayout layout;
    constexpr int card_width = 190, card_height = 70;
    constexpr int column_pitch = 250, row_pitch = 106;
    constexpr int margin = 36, component_gap = 60;
    constexpr int row_width = 1020;
    const auto count = graph.nodes.size();
    std::vector<std::vector<std::size_t>> neighbors(count);
    for (const auto& edge : graph.edges) {
        if (edge.from >= count || edge.to >= count || edge.from == edge.to) continue;
        neighbors[edge.from].push_back(edge.to);
        neighbors[edge.to].push_back(edge.from);
    }
    std::vector<bool> visible(count, false), visited(count, false);
    for (std::size_t i = 0; i < count; ++i) {
        if (!neighbors[i].empty()) ++layout.connected_count;
        visible[i] = include_isolated || !neighbors[i].empty() ||
            selected_node == i || graph.edges.empty();
        if (!visible[i]) ++layout.omitted_count;
    }
    std::vector<std::vector<std::size_t>> components;
    for (std::size_t start = 0; start < count; ++start) {
        if (!visible[start] || visited[start]) continue;
        std::vector<std::size_t> component;
        std::queue<std::size_t> queue;
        queue.push(start);
        visited[start] = true;
        while (!queue.empty()) {
            const auto node = queue.front();
            queue.pop();
            component.push_back(node);
            for (const auto next : neighbors[node]) {
                if (!visited[next] && visible[next]) {
                    visited[next] = true;
                    queue.push(next);
                }
            }
        }
        if (component.size() > 1) components.push_back(std::move(component));
    }
    std::ranges::stable_sort(components, [](const auto& a, const auto& b) {
        return a.size() > b.size();
    });
    int next_y = margin, next_x = margin, row_height = 0;
    int max_right = 0;
    for (const auto& component : components) {
        std::vector<int> depth(count, -1);
        std::queue<std::size_t> queue;
        depth[component.front()] = 0;
        queue.push(component.front());
        int last_depth = 0;
        while (!queue.empty()) {
            const auto node = queue.front();
            queue.pop();
            for (const auto next : neighbors[node]) {
                if (depth[next] != -1) continue;
                depth[next] = depth[node] + 1;
                last_depth = std::max(last_depth, depth[next]);
                queue.push(next);
            }
        }
        std::vector<std::vector<std::size_t>> layers(
            static_cast<std::size_t>(last_depth + 1));
        for (const auto node : component)
            layers[static_cast<std::size_t>(depth[node])].push_back(node);
        std::size_t tallest = 0;
        for (const auto& layer : layers) tallest = std::max(tallest, layer.size());
        // A connected component must stay on one horizontal set of layers.
        // Wrapping its layers into another row creates long edges through
        // unrelated cards and makes the fitted view unreadable.
        const int band_height = static_cast<int>(tallest) * row_pitch;
        const int band_width = static_cast<int>(layers.size() - 1) *
            column_pitch + card_width;
        if (next_x > margin && next_x + band_width > row_width) {
            next_x = margin;
            next_y += row_height + component_gap;
            row_height = 0;
        }
        for (std::size_t column = 0; column < layers.size(); ++column) {
            const auto& layer = layers[column];
            const int offset = static_cast<int>(tallest - layer.size()) * row_pitch / 2;
            for (std::size_t row = 0; row < layer.size(); ++row) {
                const int x = next_x + static_cast<int>(column) * column_pitch;
                const int y = next_y + offset + static_cast<int>(row) * row_pitch;
                layout.nodes.push_back({layer[row],
                    {x, y, x + card_width, y + card_height}});
                max_right = std::max(max_right, x + card_width);
            }
        }
        next_x += band_width + component_gap;
        row_height = std::max(row_height, band_height);
    }
    if (row_height) next_y += row_height + component_gap;
    std::vector<std::size_t> isolated;
    for (std::size_t i = 0; i < count; ++i)
        if (visible[i] && neighbors[i].empty()) isolated.push_back(i);
    constexpr int isolated_columns = 4;
    for (std::size_t i = 0; i < isolated.size(); ++i) {
        const int x = margin + static_cast<int>(i % isolated_columns) * 220;
        const int y = next_y + static_cast<int>(i / isolated_columns) * 88;
        layout.nodes.push_back({isolated[i],
            {x, y, x + card_width, y + card_height}});
        max_right = std::max(max_right, x + card_width);
    }
    if (!isolated.empty())
        next_y += static_cast<int>((isolated.size() + isolated_columns - 1) /
                                   isolated_columns) * 88;
    int max_bottom = 0;
    for (const auto& placed : layout.nodes)
        max_bottom = std::max(max_bottom, placed.rect.bottom);
    layout.width = max_right + margin;
    layout.height = std::max(max_bottom + margin, 140);
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        const auto& edge = graph.edges[i];
        if (edge.from < count && edge.to < count &&
            visible[edge.from] && visible[edge.to]) layout.edges.push_back(i);
    }
    return layout;
}

} // namespace atlas
