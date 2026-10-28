#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace atlas {

// Deterministic order, not wall-clock time: step 1 is the first observed event.
struct TraceEvent {
    std::size_t step = 0;
    std::string operation;
    std::string resource;
    std::string value;
    std::string source_file;
    std::size_t source_line = 0;

    bool operator==(const TraceEvent&) const = default;
};

using Trace = std::vector<TraceEvent>;

struct TraceReadLimits {
    std::size_t max_bytes = 64 * 1024 * 1024;
    std::size_t max_events = 1'000'000;
    std::size_t max_line_bytes = 64 * 1024;
};

enum class TraceFormat { v1, v2 };

// Opt-in test/debug writer. The .trace format is tab-separated and versioned.
class TraceWriter {
public:
    explicit TraceWriter(std::ostream& output, TraceFormat format = TraceFormat::v1);
    void record(std::string_view operation, std::string_view resource,
                std::string_view value, std::string_view source_file = {},
                std::size_t source_line = 0);

private:
    std::ostream& output_;
    TraceFormat format_;
    std::size_t next_step_ = 1;
};

std::expected<Trace, std::string> read_trace(
    const std::filesystem::path& path, TraceReadLimits limits = {});

struct TraceDifference {
    std::size_t step = 0;
    std::optional<TraceEvent> expected;
    std::optional<TraceEvent> actual;
};

struct TraceComparison {
    std::size_t expected_count = 0;
    std::size_t actual_count = 0;
    std::optional<TraceDifference> first_difference;

    [[nodiscard]] bool all_match() const noexcept { return !first_difference; }
};

TraceComparison compare_trace(const Trace& expected, const Trace& actual);
[[nodiscard]] std::string agent_trace_json(const TraceComparison& comparison);

} // namespace atlas
