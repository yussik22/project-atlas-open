#pragma once

#include "atlas/trace.hpp"

#include <expected>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace atlas {

enum class RuntimeNodeKind { variable, object, site };

struct RuntimeNode {
    std::string id;
    std::string label;
    RuntimeNodeKind kind;
};

struct RuntimeLink {
    std::string source;
    std::string target;
    std::string relation;
};

struct RuntimeSnapshot {
    std::size_t step = 0;
    std::size_t total_steps = 0;
    bool capture_truncated = false;
    std::vector<RuntimeNode> nodes;
    std::vector<RuntimeLink> links;
};

// Explicit instrumentation: IDs identify one lifetime, not a recycled address.
class RuntimeWriter {
public:
    explicit RuntimeWriter(std::ostream& output) : trace_(output) {}
    void create_variable(std::string_view id, std::string_view label);
    void create_object(std::string_view id, std::string_view label);
    void create_site(std::string_view id, std::string_view label);
    void add_link(std::string_view source, std::string_view target,
                  std::string_view relation);
    void remove_link(std::string_view source, std::string_view target,
                     std::string_view relation);
    void destroy_variable(std::string_view id);
    void destroy_object(std::string_view id);

private:
    TraceWriter trace_;
};

// Replays through step 0..trace.size(); rejects impossible graph transitions.
std::expected<RuntimeSnapshot, std::string> replay_runtime(
    const Trace& trace, std::size_t through_step);
[[nodiscard]] std::string agent_runtime_json(const RuntimeSnapshot& snapshot);

} // namespace atlas
