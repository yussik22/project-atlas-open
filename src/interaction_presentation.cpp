#include "atlas/interaction_presentation.hpp"

#include <sstream>

namespace atlas {

InteractionPresentation present_interactions(
    const InteractionGraph& graph, std::optional<std::size_t> selected_file,
    bool filter_selected_file) {
    InteractionPresentation view;
    std::size_t inheritance = 0, fields = 0, calls = 0, observed = 0;
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        const auto& edge = graph.edges[i];
        switch (edge.kind) {
        case InteractionKind::inheritance: inheritance += edge.count; break;
        case InteractionKind::field_type: fields += edge.count; break;
        case InteractionKind::static_call: calls += edge.count; break;
        case InteractionKind::observed_link: observed += edge.count; break;
        }
        const auto& from = graph.nodes[edge.from];
        const auto& to = graph.nodes[edge.to];
        if (filter_selected_file && selected_file &&
            from.file_index != selected_file && to.file_index != selected_file)
            continue;
        view.rows.push_back({
            i,
            edge.kind == InteractionKind::inheritance ? "AST inherits" :
                edge.kind == InteractionKind::field_type ? "AST field" :
                edge.kind == InteractionKind::static_call ? "AST call" : "Trace link",
            from.label, to.label, edge.detail, std::to_string(edge.count),
            edge.kind == InteractionKind::observed_link
                ? "step " + std::to_string(edge.snapshot_step)
                : edge.first_line ? "line " + std::to_string(edge.first_line)
                                  : "declaration"});
    }
    view.summary = "AST  " + std::to_string(inheritance) + " inherit · " +
        std::to_string(fields) + " field · " + std::to_string(calls) + " call sites";
    if (graph.runtime_step)
        view.summary += "     Trace  " + std::to_string(observed) +
            " active links at step " + std::to_string(*graph.runtime_step);
    else view.summary += "     Trace  not loaded";
    if (graph.capture_truncated) view.summary += " · CAPTURE TRUNCATED";
    if (filter_selected_file && selected_file)
        view.summary += " · showing " + std::to_string(view.rows.size()) +
            " / " + std::to_string(graph.edges.size()) + " rows";
    return view;
}

std::string interaction_detail_text(const InteractionGraph& graph,
                                    std::size_t edge_index) {
    if (edge_index >= graph.edges.size()) return {};
    const auto& edge = graph.edges[edge_index];
    std::ostringstream out;
    if (edge.kind == InteractionKind::observed_link) {
        out << "OBSERVED IN LOADED TRACE · active at step " << edge.snapshot_step
            << "\r\n" << graph.nodes[edge.from].id << " → "
            << graph.nodes[edge.to].id << " · relation: " << edge.detail
            << "\r\nNo source/class identity is inferred. The recorder may omit allocations.";
        if (graph.capture_truncated)
            out << " The capture hit its event limit; later activity is absent.";
    } else {
        out << "STATIC AST EVIDENCE · " << interaction_kind_name(edge.kind)
            << "\r\n" << graph.nodes[edge.from].label;
        if (edge.first_line) out << ':' << edge.first_line;
        if (edge.first_column) out << ':' << edge.first_column;
        out << " → " << graph.nodes[edge.to].label << " · " << edge.detail
            << " · " << edge.count << " site(s)\r\nNot proof of execution";
        if (edge.dynamic_dispatch) out << "; dynamic dispatch may call another target";
        if (edge.indirect) out << "; indirect call target is uncertain";
        out << '.';
    }
    return out.str();
}

} // namespace atlas
