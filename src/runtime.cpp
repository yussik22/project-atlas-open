#include "atlas/runtime.hpp"

#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace atlas {
namespace {

bool valid_id(std::string_view value) {
    return !value.empty() && value.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")
        == std::string_view::npos;
}

using LinkKey = std::tuple<std::string, std::string, std::string>;

std::string error_at(std::size_t step, std::string_view detail) {
    return "runtime step " + std::to_string(step) + ": " + std::string(detail);
}

std::expected<LinkKey, std::string> parse_link(const TraceEvent& event) {
    const auto separator = event.value.find(':');
    if (separator == std::string::npos ||
        event.value.find(':', separator + 1) != std::string::npos) {
        return std::unexpected(error_at(event.step, "link value must be relation:target"));
    }
    const auto relation = event.value.substr(0, separator);
    const auto target = event.value.substr(separator + 1);
    if (!valid_id(event.resource) || !valid_id(relation) || !valid_id(target)) {
        return std::unexpected(error_at(event.step, "invalid link identifier"));
    }
    return LinkKey{event.resource, target, relation};
}

std::string link_value(std::string_view target, std::string_view relation) {
    if (!valid_id(target) || !valid_id(relation)) {
        throw std::invalid_argument("runtime link IDs and relations must use letters, "
                                    "digits, _, . or -");
    }
    return std::string(relation) + ':' + std::string(target);
}

} // namespace

void RuntimeWriter::create_variable(std::string_view id, std::string_view label) {
    if (label.empty()) throw std::invalid_argument("runtime node label cannot be empty");
    trace_.record("var.create", id, label);
}

void RuntimeWriter::create_object(std::string_view id, std::string_view label) {
    if (label.empty()) throw std::invalid_argument("runtime node label cannot be empty");
    trace_.record("obj.create", id, label);
}

void RuntimeWriter::create_site(std::string_view id, std::string_view label) {
    if (label.empty()) throw std::invalid_argument("runtime node label cannot be empty");
    trace_.record("site.create", id, label);
}

void RuntimeWriter::add_link(std::string_view source, std::string_view target,
                             std::string_view relation) {
    trace_.record("link.add", source, link_value(target, relation));
}

void RuntimeWriter::remove_link(std::string_view source, std::string_view target,
                                std::string_view relation) {
    trace_.record("link.remove", source, link_value(target, relation));
}

void RuntimeWriter::destroy_variable(std::string_view id) {
    trace_.record("var.destroy", id, "-");
}

void RuntimeWriter::destroy_object(std::string_view id) {
    trace_.record("obj.destroy", id, "-");
}

std::expected<RuntimeSnapshot, std::string> replay_runtime(
    const Trace& trace, std::size_t through_step) {
    if (through_step > trace.size()) {
        return std::unexpected("runtime step exceeds trace length");
    }
    std::map<std::string, RuntimeNode> active;
    std::set<std::string> used_ids;
    std::set<LinkKey> links;
    bool capture_truncated = false;
    for (std::size_t i = 0; i < through_step; ++i) {
        const auto& event = trace[i];
        if (event.step != i + 1) {
            return std::unexpected(error_at(i + 1, "trace steps are not consecutive"));
        }
        const auto op = event.operation;
        if (op == "var.create" || op == "obj.create" || op == "site.create") {
            if (!valid_id(event.resource) || event.value.empty()) {
                return std::unexpected(error_at(event.step, "invalid node ID or label"));
            }
            if (!used_ids.insert(event.resource).second) {
                return std::unexpected(error_at(event.step, "node ID reused in this trace"));
            }
            const auto kind = op == "var.create" ? RuntimeNodeKind::variable
                            : op == "site.create" ? RuntimeNodeKind::site
                                                  : RuntimeNodeKind::object;
            active.emplace(event.resource, RuntimeNode{event.resource, event.value, kind});
        } else if (op == "var.destroy" || op == "obj.destroy") {
            const auto found = active.find(event.resource);
            const auto wanted = op == "var.destroy" ? RuntimeNodeKind::variable
                                                      : RuntimeNodeKind::object;
            if (event.value != "-" || found == active.end() || found->second.kind != wanted) {
                return std::unexpected(error_at(event.step, "node is not alive or has wrong kind"));
            }
            for (const auto& [source, target, relation] : links) {
                if (source == event.resource || target == event.resource) {
                    return std::unexpected(error_at(event.step,
                        "remove all links before destroying a node"));
                }
            }
            active.erase(found);
        } else if (op == "link.add" || op == "link.remove") {
            auto key = parse_link(event);
            if (!key) return std::unexpected(key.error());
            const auto& [source, target, relation] = *key;
            if (!active.contains(source) || !active.contains(target)) {
                return std::unexpected(error_at(event.step,
                    "link endpoint is not alive"));
            }
            if (op == "link.add") {
                if (!links.insert(*key).second) {
                    return std::unexpected(error_at(event.step, "duplicate link"));
                }
            } else if (links.erase(*key) == 0) {
                return std::unexpected(error_at(event.step, "link does not exist"));
            }
        } else if (op == "capture.truncated" && event.resource == "capture" &&
                   event.value == "10000") {
            // The recorder stopped at its bounded event limit. The preceding
            // graph remains a valid historical snapshot, not a final state.
            capture_truncated = true;
        } else {
            return std::unexpected(error_at(event.step, "unknown runtime operation"));
        }
    }
    RuntimeSnapshot snapshot;
    snapshot.step = through_step;
    snapshot.total_steps = trace.size();
    snapshot.capture_truncated = capture_truncated;
    for (const auto& [id, node] : active) snapshot.nodes.push_back(node);
    for (const auto& [source, target, relation] : links) {
        snapshot.links.push_back({source, target, relation});
    }
    return snapshot;
}

} // namespace atlas
