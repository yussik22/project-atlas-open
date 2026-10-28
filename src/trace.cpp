#include "atlas/trace.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace atlas {
namespace {

constexpr std::string_view header_v1 = "# atlas-trace-v1";
constexpr std::string_view header_v2 = "# atlas-trace-v2";

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

TraceWriter::TraceWriter(std::ostream& output, TraceFormat format)
    : output_(output), format_(format) {
    output_ << (format_ == TraceFormat::v2 ? header_v2 : header_v1) << '\n';
    if (!output_) throw std::runtime_error("could not write trace header");
}

void TraceWriter::record(std::string_view operation, std::string_view resource,
                         std::string_view value, std::string_view source_file,
                         std::size_t source_line) {
    if (!valid_name(operation) || !valid_name(resource) || !valid_value(value)) {
        throw std::invalid_argument("trace fields must be one-line; operation and resource "
                                    "use only letters, digits, _, . or -");
    }
    if ((!source_file.empty() && (!valid_value(source_file) || source_line == 0)) ||
        (source_file.empty() && source_line != 0) ||
        (format_ == TraceFormat::v1 && !source_file.empty())) {
        throw std::invalid_argument("invalid trace source location");
    }
    output_ << next_step_ << '\t' << operation << '\t' << resource << '\t'
            << value;
    if (format_ == TraceFormat::v2)
        output_ << '\t' << source_file << '\t' << source_line;
    output_ << '\n';
    if (!output_) throw std::runtime_error("could not write trace event");
    ++next_step_;
}

std::expected<Trace, std::string> read_trace(
    const std::filesystem::path& path, TraceReadLimits limits) {
    std::error_code size_error;
    const auto file_bytes = std::filesystem::file_size(path, size_error);
    if (size_error)
        return std::unexpected("cannot inspect trace file: " + path.string());
    if (file_bytes > limits.max_bytes)
        return std::unexpected("trace exceeds byte limit: " + path.string());
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected("cannot open trace file: " + path.string());
    Trace trace;
    std::string line;
    std::size_t line_number = 0;
    std::size_t bytes_read = 0;
    const auto next_line = [&]() -> std::expected<bool, std::string> {
        line.clear();
        for (;;) {
            const int next = input.get();
            if (next == std::char_traits<char>::eof()) {
                if (input.bad()) return std::unexpected("error reading trace file: " + path.string());
                return !line.empty();
            }
            if (++bytes_read > limits.max_bytes)
                return std::unexpected("trace exceeds byte limit: " + path.string());
            if (next == '\n') return true;
            if (line.size() >= limits.max_line_bytes)
                return std::unexpected(line_error(path, line_number + 1,
                                                  "trace line exceeds limit"));
            line.push_back(static_cast<char>(next));
        }
    };
    const auto header = next_line();
    if (!header) return std::unexpected(header.error());
    if (!*header) {
        return std::unexpected(line_error(path, 1, "missing trace header"));
    }
    ++line_number;
    if (line.ends_with('\r')) line.pop_back();
    const bool v2 = line == header_v2;
    if (line != header_v1 && !v2) {
        return std::unexpected(line_error(path, 1, "expected # atlas-trace-v1 or -v2"));
    }
    for (;;) {
        const auto has_line = next_line();
        if (!has_line) return std::unexpected(has_line.error());
        if (!*has_line) break;
        ++line_number;
        if (trace.size() >= limits.max_events)
            return std::unexpected(line_error(path, line_number, "trace event limit exceeded"));
        if (line.ends_with('\r')) line.pop_back();
        const auto first = line.find('\t');
        const auto second = first == std::string::npos ? first : line.find('\t', first + 1);
        const auto third = second == std::string::npos ? second : line.find('\t', second + 1);
        const auto fourth = third == std::string::npos ? third : line.find('\t', third + 1);
        const auto fifth = fourth == std::string::npos ? fourth : line.find('\t', fourth + 1);
        if (first == std::string::npos || second == std::string::npos ||
            third == std::string::npos ||
            (v2 ? fourth == std::string::npos || fifth == std::string::npos ||
                       line.find('\t', fifth + 1) != std::string::npos
                : fourth != std::string::npos)) {
            return std::unexpected(line_error(path, line_number,
                v2 ? "expected six tab-separated trace fields"
                   : "expected step<TAB>operation<TAB>resource<TAB>value"));
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
                         v2 ? line.substr(third + 1, fourth - third - 1)
                            : line.substr(third + 1), {}, 0};
        if (v2) {
            event.source_file = line.substr(fourth + 1, fifth - fourth - 1);
            const auto source_text = std::string_view(line).substr(fifth + 1);
            const auto [source_end, source_error] = std::from_chars(
                source_text.data(), source_text.data() + source_text.size(),
                event.source_line);
            if (source_error != std::errc{} ||
                source_end != source_text.data() + source_text.size() ||
                (event.source_file.empty() != (event.source_line == 0)) ||
                !valid_value(event.source_file)) {
                return std::unexpected(line_error(path, line_number,
                    "invalid trace source location"));
            }
        }
        if (!valid_name(event.operation) || !valid_name(event.resource) ||
            !valid_value(event.value)) {
            return std::unexpected(line_error(path, line_number, "invalid trace field"));
        }
        trace.push_back(std::move(event));
    }
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
