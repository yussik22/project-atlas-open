#include "atlas/atlas.hpp"

#include <fstream>
#include <set>
#include <string_view>
#include <system_error>

namespace atlas {
namespace {

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r");
    return value.substr(first, last - first + 1);
}

bool valid_key(std::string_view key) {
    return !key.empty() &&
           key.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")
               == std::string_view::npos;
}

std::string markdown_cell(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char c : value) {
        if (c == '|' || c == '\\' || c == '`') result += '\\';
        result += c;
    }
    return result;
}

std::string_view kind_name(DifferenceKind kind) {
    switch (kind) {
    case DifferenceKind::match: return "match";
    case DifferenceKind::mismatch: return "mismatch";
    case DifferenceKind::missing_actual: return "missing actual";
    case DifferenceKind::unexpected_actual: return "unexpected actual";
    }
    return "unknown";
}

} // namespace

std::expected<Facts, std::string> read_facts(const fs::path& path) {
    constexpr std::uintmax_t max_facts_bytes = 16 * 1024 * 1024;
    constexpr std::size_t max_fact_line_bytes = 64 * 1024;
    constexpr std::size_t max_facts = 100'000;
    std::error_code size_error;
    const auto file_bytes = fs::file_size(path, size_error);
    if (size_error)
        return std::unexpected("cannot inspect facts file: " + path.string());
    if (file_bytes > max_facts_bytes)
        return std::unexpected("facts file exceeds byte limit: " + path.string());
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected("cannot open facts file: " + path.string());
    }
    Facts facts;
    std::string line;
    std::size_t line_number = 0;
    std::size_t bytes_read = 0;
    const auto next_line = [&]() -> std::expected<bool, std::string> {
        line.clear();
        for (;;) {
            const int next = input.get();
            if (next == std::char_traits<char>::eof()) {
                if (input.bad()) return std::unexpected("error reading facts file: " + path.string());
                return !line.empty();
            }
            if (++bytes_read > max_facts_bytes)
                return std::unexpected("facts file exceeds byte limit: " + path.string());
            if (next == '\n') return true;
            if (line.size() >= max_fact_line_bytes)
                return std::unexpected(path.string() + ":" +
                    std::to_string(line_number + 1) + ": fact line exceeds limit");
            line.push_back(static_cast<char>(next));
        }
    };
    for (;;) {
        const auto has_line = next_line();
        if (!has_line) return std::unexpected(has_line.error());
        if (!*has_line) break;
        ++line_number;
        if (facts.size() >= max_facts)
            return std::unexpected("facts entry limit exceeded: " + path.string());
        const auto text = trim(line);
        if (text.empty() || text.front() == '#') continue;
        const auto equals = text.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected(path.string() + ":" + std::to_string(line_number) +
                                   ": expected key=value");
        }
        const auto key = trim(text.substr(0, equals));
        const auto value = trim(text.substr(equals + 1));
        if (!valid_key(key)) {
            return std::unexpected(path.string() + ":" + std::to_string(line_number) +
                                   ": invalid fact key");
        }
        if (!facts.emplace(std::string(key), std::string(value)).second) {
            return std::unexpected(path.string() + ":" + std::to_string(line_number) +
                                   ": duplicate fact key: " + std::string(key));
        }
    }
    return facts;
}

bool Comparison::all_match() const noexcept {
    for (const auto& entry : entries) {
        if (entry.kind != DifferenceKind::match) return false;
    }
    return true;
}

Comparison compare(const Facts& expected, const Facts& actual) {
    Comparison result;
    std::set<std::string> keys;
    for (const auto& [key, _] : expected) keys.insert(key);
    for (const auto& [key, _] : actual) keys.insert(key);
    result.entries.reserve(keys.size());
    for (const auto& key : keys) {
        Difference entry;
        entry.key = key;
        if (const auto it = expected.find(key); it != expected.end()) {
            entry.expected = it->second;
        }
        if (const auto it = actual.find(key); it != actual.end()) {
            entry.actual = it->second;
        }
        if (!entry.expected) {
            entry.kind = DifferenceKind::unexpected_actual;
        } else if (!entry.actual) {
            entry.kind = DifferenceKind::missing_actual;
        } else if (*entry.expected != *entry.actual) {
            entry.kind = DifferenceKind::mismatch;
        }
        result.entries.push_back(std::move(entry));
    }
    return result;
}

std::expected<void, std::string> write_comparison_report(
    const Comparison& comparison, const fs::path& output_file) {
    std::error_code ec;
    if (!output_file.parent_path().empty()) {
        fs::create_directories(output_file.parent_path(), ec);
        if (ec) {
            return std::unexpected("cannot create comparison report directory: " +
                                   ec.message());
        }
    }
    std::ofstream out(output_file, std::ios::binary);
    if (!out) {
        return std::unexpected("cannot write comparison report: " + output_file.string());
    }
    std::size_t mismatches = 0;
    for (const auto& entry : comparison.entries) {
        if (entry.kind != DifferenceKind::match) ++mismatches;
    }
    out << "# Expected vs actual\n\n"
        << "Compared " << comparison.entries.size() << " facts; "
        << mismatches << " differences.\n\n"
        << "| Key | Expected | Actual | Status |\n"
        << "| --- | --- | --- | --- |\n";
    for (const auto& entry : comparison.entries) {
        out << "| " << markdown_cell(entry.key)
            << " | " << (entry.expected ? markdown_cell(*entry.expected) : "—")
            << " | " << (entry.actual ? markdown_cell(*entry.actual) : "—")
            << " | " << kind_name(entry.kind) << " |\n";
    }
    return out ? std::expected<void, std::string>{}
               : std::unexpected("cannot complete comparison report: " +
                                 output_file.string());
}

} // namespace atlas
