#include "atlas/context.hpp"
#include "atlas/symbol_identity.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <numeric>
#include <optional>
#include <sstream>
#include <vector>

namespace atlas {
namespace {

std::string path_text(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string quote(std::string_view value) {
    std::string out = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += digits[c >> 4];
                out += digits[c & 15];
            } else out += static_cast<char>(c);
        }
    }
    out += '"';
    return out;
}

std::string ascii_lower(std::string_view value) {
    std::string lower(value);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return lower;
}

std::vector<std::string> task_terms(std::string_view task) {
    std::vector<std::string> terms;
    std::string current;
    const auto finish = [&] {
        if (current.size() >= 3 &&
            std::ranges::find(terms, current) == terms.end())
            terms.push_back(current);
        current.clear();
    };
    for (unsigned char c : task) {
        if (std::isalnum(c) || c == '_' || c == '/' || c == '.' || c >= 0x80) {
            current += static_cast<char>(c);
        } else finish();
    }
    finish();
    return terms;
}

bool contains_any(std::string_view value, const std::vector<std::string>& terms) {
    const auto lower = ascii_lower(value);
    return std::ranges::any_of(terms, [&](const std::string& term) {
        return lower.find(term) != std::string::npos;
    });
}

struct Match {
    std::size_t file = 0;
    int score = 0;
    std::vector<std::size_t> symbols;
    std::vector<std::size_t> functions;
    std::vector<std::size_t> calls;
    bool path = false;
};

std::optional<std::string> verified_source_line(
    const ProjectScan& scan, const SourceFile& file, std::size_t line) {
    if (!line || line > 100000) return std::nullopt;
    std::ifstream input(scan.root / file.relative_path, std::ios::binary);
    if (!input) return std::nullopt;
    const std::string bytes(std::istreambuf_iterator<char>{input}, {});
    if (fnv1a64(bytes) != file.content_hash) return std::nullopt;
    std::size_t begin = 0;
    for (std::size_t current = 1; current < line; ++current) {
        const auto next = bytes.find('\n', begin);
        if (next == std::string::npos) return std::nullopt;
        begin = next + 1;
    }
    const auto end = bytes.find('\n', begin);
    std::string result = bytes.substr(begin, end == std::string::npos ?
        std::string::npos : end - begin);
    if (!result.empty() && result.back() == '\r') result.pop_back();
    if (result.size() > 180) result = result.substr(0, 177) + "...";
    return result;
}

std::string file_json(const ProjectScan& scan, const Match& match) {
    const auto& file = scan.files[match.file];
    std::ostringstream out;
    out << "{\"path\":" << quote(path_text(file.relative_path))
        << ",\"score\":" << match.score
        << ",\"hash\":" << quote([&] {
            std::ostringstream hash;
            hash << std::hex << std::setw(16) << std::setfill('0') << file.content_hash;
            return hash.str();
        }())
        << ",\"parsed\":" << (file.parsed ? "true" : "false")
        << ",\"errors\":" << file.error_diagnostic_count
        << ",\"warnings\":" << file.warning_diagnostic_count
        << ",\"symbols\":[";
    for (std::size_t i = 0; i < std::min<std::size_t>(match.symbols.size(), 2); ++i) {
        const auto& symbol = file.symbols[match.symbols[i]];
        if (i) out << ',';
        out << "{\"name\":" << quote(symbol.qualified_name.empty() ?
            symbol.name : symbol.qualified_name) << ",\"line\":" << symbol.line << '}';
    }
    out << "],\"functions\":[";
    for (std::size_t i = 0; i < std::min<std::size_t>(match.functions.size(), 3); ++i) {
        const auto& function = file.functions[match.functions[i]];
        if (i) out << ',';
        out << "{\"name\":" << quote(function.qualified_name)
            << ",\"line\":" << function.line << '}';
    }
    out << "],\"calls\":[";
    for (std::size_t i = 0; i < std::min<std::size_t>(match.calls.size(), 2); ++i) {
        const auto& call = file.calls[match.calls[i]];
        if (i) out << ',';
        out << "{\"caller\":" << quote(call.caller_name)
            << ",\"callee\":" << quote(call.callee_name)
            << ",\"line\":" << call.line << '}';
    }
    out << ']';
    std::size_t source_line = 0;
    if (!match.functions.empty()) source_line = file.functions[match.functions[0]].line;
    else if (!match.symbols.empty()) source_line = file.symbols[match.symbols[0]].line;
    else if (!match.calls.empty()) source_line = file.calls[match.calls[0]].line;
    if (source_line) {
        if (const auto snippet = verified_source_line(scan, file, source_line))
            out << ",\"snippet\":{\"line\":" << source_line
                << ",\"text\":" << quote(*snippet) << '}';
        else out << ",\"snippet_unavailable\":true";
    }
    out << '}';
    return out.str();
}

} // namespace

std::string agent_context_json(const ProjectScan& scan, std::string_view task,
                               std::size_t approximate_token_budget) {
    const auto terms = task_terms(task);
    std::vector<Match> matches;
    std::size_t errors = 0, warnings = 0, failures = 0;
    for (std::size_t i = 0; i < scan.files.size(); ++i) {
        const auto& file = scan.files[i];
        errors += file.error_diagnostic_count;
        warnings += file.warning_diagnostic_count;
        failures += !file.parsed;
        Match match;
        match.file = i;
        if (terms.empty()) continue;
        match.path = contains_any(path_text(file.relative_path), terms);
        if (match.path) match.score += 12;
        for (std::size_t j = 0; j < file.symbols.size(); ++j) {
            const auto& symbol = file.symbols[j];
            if (contains_any(symbol.qualified_name.empty() ? symbol.name :
                             symbol.qualified_name, terms)) match.symbols.push_back(j);
        }
        for (std::size_t j = 0; j < file.functions.size(); ++j)
            if (contains_any(file.functions[j].qualified_name, terms))
                match.functions.push_back(j);
        for (std::size_t j = 0; j < file.calls.size(); ++j)
            if (contains_any(file.calls[j].caller_name, terms) ||
                contains_any(file.calls[j].callee_name, terms))
                match.calls.push_back(j);
        if (!match.symbols.empty()) match.score += 8;
        if (!match.functions.empty()) match.score += 6;
        if (!match.calls.empty()) match.score += 3;
        if (match.score) matches.push_back(std::move(match));
    }
    std::ranges::sort(matches, [&](const Match& a, const Match& b) {
        if (a.score != b.score) return a.score > b.score;
        return path_text(scan.files[a.file].relative_path) <
               path_text(scan.files[b.file].relative_path);
    });
    const auto byte_cap = approximate_token_budget * 4;
    std::ostringstream prefix;
    prefix << "{\"schema\":\"project-atlas.context.v1\",\"operation\":\"context\""
           << ",\"task\":" << quote(task)
           << ",\"approx_token_budget\":" << approximate_token_budget
           << ",\"max_utf8_bytes\":" << byte_cap
           << ",\"clean\":" << (errors == 0 && failures == 0 ? "true" : "false")
           << ",\"ast_failures\":" << failures
           << ",\"clang_errors\":" << errors
           << ",\"clang_warnings\":" << warnings
           << ",\"search_terms\":" << terms.size()
           << ",\"matched_files\":" << matches.size() << ",\"files\":[";
    std::string output = prefix.str();
    std::size_t returned = 0;
    constexpr std::size_t tail_reserve = 220;
    for (const auto& match : matches) {
        auto item = file_json(scan, match);
        if (output.size() + item.size() + tail_reserve + (returned ? 1 : 0) > byte_cap)
            break;
        if (returned) output += ',';
        output += item;
        ++returned;
    }
    output += "],\"returned_files\":" + std::to_string(returned) +
        ",\"truncated\":" + (returned < matches.size() ? "true" : "false") +
        ",\"ranking\":\"lexical; verify source before editing\""
        ",\"budget_note\":\"UTF-8 byte cap; actual model tokens may differ\"}";
    return output;
}

} // namespace atlas
