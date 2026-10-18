#include "atlas/trace.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace atlas {
namespace {

constexpr std::string_view header = "# atlas-trace-v1";

bool valid_name(std::string_view name) {
    return !name.empty() && name.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")
        == std::string_view::npos;
}

bool valid_value(std::string_view value) {
    for (unsigned char byte : value) {
        if (byte < 0x20U || byte == 0x7fU) return false;
    }
    return true;
}

std::string line_error(const std::filesystem::path& path, std::size_t line,
                       std::string_view reason) {
    return path.string() + ':' + std::to_string(line) + ": " + std::string(reason);
}

} // namespace

TraceWriter::TraceWriter(std::ostream& output) : output_(output) {
    output_ << header << '\n';
    if (!output_) throw std::runtime_error("could not write trace header");
}

void TraceWriter::record(std::string_view operation, std::string_view resource,
                         std::string_view value) {
    if (!valid_name(operation) || !valid_name(resource) || !valid_value(value)) {
        throw std::invalid_argument("trace fields must be one-line; operation and resource "
                                    "use only letters, digits, _, . or -");
    }
    output_ << next_step_ << '\t' << operation << '\t' << resource << '\t'
            << value << '\n';
    if (!output_) throw std::runtime_error("could not write trace event");
    ++next_step_;
}

std::expected<Trace, std::string> read_trace(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected("cannot open trace file: " + path.string());
    Trace trace;
    std::string line;
    std::size_t line_number = 0;
    if (!std::getline(input, line)) {
        return std::unexpected(line_error(path, 1, "missing trace header"));
    }
    ++line_number;
    if (line.ends_with('\r')) line.pop_back();
    if (line != header) {
        return std::unexpected(line_error(path, 1, "expected # atlas-trace-v1"));
    }
    while (std::getline(input, line)) {
        ++line_number;
        if (line.ends_with('\r')) line.pop_back();
        const auto first = line.find('\t');
        const auto second = first == std::string::npos ? first : line.find('\t', first + 1);
        const auto third = second == std::string::npos ? second : line.find('\t', second + 1);
        if (first == std::string::npos || second == std::string::npos ||
            third == std::string::npos || line.find('\t', third + 1) != std::string::npos) {
            return std::unexpected(line_error(path, line_number,
                "expected step<TAB>operation<TAB>resource<TAB>value"));
        }
        std::size_t step = 0;
        const auto [end, error] = std::from_chars(line.data(), line.data() + first, step);
        if (error != std::errc{} || end != line.data() + first ||
            step != trace.size() + 1) {
            return std::unexpected(line_error(path, line_number,
                "trace steps must be consecutive starting at 1"));
        }
        TraceEvent event{step, line.substr(first + 1, second - first - 1),
                         line.substr(second + 1, third - second - 1),
                         line.substr(third + 1)};
        if (!valid_name(event.operation) || !valid_name(event.resource) ||
            !valid_value(event.value)) {
            return std::unexpected(line_error(path, line_number, "invalid trace field"));
        }
        trace.push_back(std::move(event));
    }
    if (input.bad()) return std::unexpected("error reading trace file: " + path.string());
    return trace;
}

TraceComparison compare_trace(const Trace& expected, const Trace& actual) {
    TraceComparison result{expected.size(), actual.size(), std::nullopt};
    const auto count = std::max(expected.size(), actual.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (i < expected.size() && i < actual.size() && expected[i] == actual[i]) continue;
        TraceDifference difference;
        difference.step = i + 1;
        if (i < expected.size()) difference.expected = expected[i];
        if (i < actual.size()) difference.actual = actual[i];
        result.first_difference = std::move(difference);
        break;
    }
    return result;
}

} // namespace atlas
