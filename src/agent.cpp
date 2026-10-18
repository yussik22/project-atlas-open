#include "atlas/atlas.hpp"
#include "atlas/symbol_identity.hpp"
#include "atlas/trace.hpp"
#include "atlas/runtime.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace atlas {
namespace {

std::string utf8_path(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string json_string(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() + 2);
    result += '"';
    for (unsigned char c : value) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20U) {
                result += "\\u00";
                result += hex[c >> 4U];
                result += hex[c & 0x0fU];
            } else {
                result += static_cast<char>(c);
            }
        }
    }
    result += '"';
    return result;
}

std::string hex64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

unsigned char ascii_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<unsigned char>(c + ('a' - 'A'));
    return c;
}

bool contains_query(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return ascii_lower(a) == ascii_lower(b);
        }) != haystack.end();
}

void write_strings(std::ostream& out, const std::vector<std::string>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << json_string(values[i]);
    }
    out << ']';
}

void write_symbol(std::ostream& out, const Symbol& symbol) {
    out << "{\"kind\":" << json_string(symbol.kind)
        << ",\"name\":" << json_string(symbol.name)
        << ",\"qualified_name\":" << json_string(symbol.qualified_name)
        << ",\"usr\":" << json_string(symbol.usr)
        << ",\"diagram_id\":" << json_string(mermaid_symbol_id(symbol.usr))
        << ",\"line\":" << symbol.line
        << ",\"base_text\":" << json_string(symbol.base)
        << ",\"bases\":[";
    for (std::size_t i = 0; i < symbol.base_usrs.size(); ++i) {
        if (i) out << ',';
        out << "{\"usr\":" << json_string(symbol.base_usrs[i])
            << ",\"qualified_name\":";
        if (i < symbol.base_qualified_names.size()) {
            out << json_string(symbol.base_qualified_names[i]);
        } else {
            out << "null";
        }
        out << '}';
    }
    out << "],\"members\":[";
    for (std::size_t i = 0; i < symbol.members.size(); ++i) {
        if (i) out << ',';
        const auto& member = symbol.members[i];
        out << "{\"kind\":" << json_string(member.kind)
            << ",\"name\":" << json_string(member.name)
            << ",\"type\":" << json_string(member.type)
            << ",\"access\":" << json_string(member.access)
            << ",\"signature\":" << json_string(member.signature)
            << ",\"related_usr\":" << json_string(member.related_usr)
            << ",\"related_usrs\":[";
        for (std::size_t relation = 0; relation < member.related_usrs.size(); ++relation) {
            if (relation) out << ',';
            out << json_string(member.related_usrs[relation]);
        }
        out << "]}";
    }
    out << "]}";
}

void write_function(std::ostream& out, const FunctionSymbol& function) {
    out << "{\"kind\":" << json_string(function.kind)
        << ",\"qualified_name\":" << json_string(function.qualified_name)
        << ",\"usr\":" << json_string(function.usr)
        << ",\"signature\":" << json_string(function.signature)
        << ",\"line\":" << function.line
        << ",\"has_body\":" << (function.has_body ? "true" : "false")
        << '}';
}

void write_call(std::ostream& out, const CallSite& call) {
    out << "{\"caller_usr\":" << json_string(call.caller_usr)
        << ",\"caller\":" << json_string(call.caller_name)
        << ",\"callee_usr\":" << json_string(call.callee_usr)
        << ",\"callee\":" << json_string(call.callee_name)
        << ",\"callee_file\":";
    if (call.callee_file.empty()) out << "null";
    else out << json_string(utf8_path(call.callee_file));
    out << ",\"line\":" << call.line
        << ",\"column\":" << call.column
        << ",\"dynamic_dispatch\":" << (call.dynamic_dispatch ? "true" : "false")
        << ",\"indirect\":" << (call.indirect ? "true" : "false")
        << '}';
}

bool call_matches(const CallSite& call, std::string_view query) {
    return contains_query(call.caller_name, query) ||
        contains_query(call.callee_name, query) ||
        contains_query(call.caller_usr, query) ||
        contains_query(call.callee_usr, query);
}

void write_file(std::ostream& out, const ProjectScan& scan, const SourceFile& file,
                std::optional<std::string_view> query) {
    const auto relative = utf8_path(file.relative_path);
    const bool path_matches = !query || contains_query(relative, *query);
    out << "{\"path\":" << json_string(relative)
        << ",\"absolute_path\":" << json_string(utf8_path(scan.root / file.relative_path))
        << ",\"content_hash_fnv1a64\":" << json_string(hex64(file.content_hash))
        << ",\"ast_created\":" << (file.parsed ? "true" : "false")
        << ",\"compile_command_found\":" << (file.used_compile_command ? "true" : "false")
        << ",\"clang_errors\":" << file.error_diagnostic_count
        << ",\"clang_warnings\":" << file.warning_diagnostic_count
        << ",\"includes\":";
    write_strings(out, file.includes);
    out << ",\"diagnostics\":";
    write_strings(out, file.diagnostics);
    out << ",\"symbols\":[";
    bool first = true;
    for (const auto& symbol : file.symbols) {
        if (query && !path_matches &&
            !contains_query(symbol.name, *query) &&
            !contains_query(symbol.qualified_name, *query) &&
            !contains_query(symbol.usr, *query)) continue;
        if (!first) out << ',';
        first = false;
        write_symbol(out, symbol);
    }
    out << "],\"functions\":[";
    first = true;
    for (const auto& function : file.functions) {
        if (query && !path_matches &&
            !contains_query(function.qualified_name, *query) &&
            !contains_query(function.signature, *query)) continue;
        if (!first) out << ',';
        first = false;
        write_function(out, function);
    }
    out << "],\"calls\":[";
    first = true;
    for (const auto& call : file.calls) {
        if (query && !path_matches && !call_matches(call, *query)) continue;
        if (!first) out << ',';
        first = false;
        write_call(out, call);
    }
    out << "]}";
}

std::string_view kind_name(DifferenceKind kind) {
    switch (kind) {
    case DifferenceKind::match: return "match";
    case DifferenceKind::mismatch: return "mismatch";
    case DifferenceKind::missing_actual: return "missing_actual";
    case DifferenceKind::unexpected_actual: return "unexpected_actual";
    }
    return "unknown";
}

} // namespace

std::string agent_scan_json(const ProjectScan& scan,
                            const std::optional<std::string_view>& query,
                            std::size_t max_files) {
    std::size_t ast_failures = 0, errors = 0, warnings = 0;
    std::size_t matched_files = 0, matched_symbols = 0;
    std::size_t matched_functions = 0, matched_calls = 0, returned_files = 0;
    std::size_t total_calls = 0;
    for (const auto& file : scan.files) {
        ast_failures += !file.parsed;
        errors += file.error_diagnostic_count;
        warnings += file.warning_diagnostic_count;
        total_calls += file.calls.size();
    }
    std::ostringstream out;
    out << "{\"schema\":\"project-atlas.agent.v1\",\"operation\":"
        << json_string(query ? "find" : "map")
        << ",\"root\":" << json_string(utf8_path(scan.root))
        << ",\"compilation_database\":";
    if (scan.compilation_database_directory) {
        out << json_string(utf8_path(*scan.compilation_database_directory));
    } else {
        out << "null";
    }
    out << ",\"summary\":{\"file_count\":" << scan.files.size()
        << ",\"ast_failures\":" << ast_failures
        << ",\"clang_errors\":" << errors
        << ",\"clang_warnings\":" << warnings
        << ",\"call_sites\":" << total_calls
        << ",\"clean\":" << ((ast_failures == 0 && errors == 0) ? "true" : "false")
        << "},\"query\":";
    if (query) out << json_string(*query);
    else out << "null";
    out << ",\"files\":[";
    for (const auto& file : scan.files) {
        const bool path_matches = !query || contains_query(utf8_path(file.relative_path), *query);
        std::size_t symbol_matches = 0;
        std::size_t function_matches = 0;
        std::size_t call_site_matches = 0;
        if (query) {
            for (const auto& symbol : file.symbols) {
                symbol_matches += contains_query(symbol.name, *query) ||
                    contains_query(symbol.qualified_name, *query) ||
                    contains_query(symbol.usr, *query);
            }
            for (const auto& function : file.functions) {
                function_matches += contains_query(function.qualified_name, *query) ||
                    contains_query(function.signature, *query);
            }
            for (const auto& call : file.calls) call_site_matches += call_matches(call, *query);
        }
        if (!path_matches && symbol_matches == 0 && function_matches == 0 &&
            call_site_matches == 0) continue;
        ++matched_files;
        matched_symbols += path_matches ? file.symbols.size() : symbol_matches;
        matched_functions += path_matches ? file.functions.size() : function_matches;
        matched_calls += path_matches ? file.calls.size() : call_site_matches;
        if (returned_files >= max_files) continue;
        if (returned_files) out << ',';
        write_file(out, scan, file, query);
        ++returned_files;
    }
    out << "],\"matched_files\":" << matched_files
        << ",\"matched_symbols\":" << matched_symbols
        << ",\"matched_functions\":" << matched_functions
        << ",\"matched_calls\":" << matched_calls
        << ",\"returned_files\":" << returned_files
        << ",\"truncated\":" << (matched_files > returned_files ? "true" : "false")
        << ",\"skipped_large_files\":[";
    for (std::size_t i = 0; i < scan.skipped_large_files.size(); ++i) {
        if (i) out << ',';
        out << json_string(utf8_path(scan.skipped_large_files[i]));
    }
    out << "]}";
    return out.str();
}

std::string agent_comparison_json(const Comparison& comparison) {
    std::ostringstream out;
    out << "{\"schema\":\"project-atlas.agent.v1\",\"operation\":\"compare\""
        << ",\"all_match\":" << (comparison.all_match() ? "true" : "false")
        << ",\"entries\":[";
    for (std::size_t i = 0; i < comparison.entries.size(); ++i) {
        if (i) out << ',';
        const auto& entry = comparison.entries[i];
        out << "{\"key\":" << json_string(entry.key)
            << ",\"kind\":" << json_string(kind_name(entry.kind))
            << ",\"expected\":";
        if (entry.expected) out << json_string(*entry.expected);
        else out << "null";
        out << ",\"actual\":";
        if (entry.actual) out << json_string(*entry.actual);
        else out << "null";
        out << '}';
    }
    out << "]}";
    return out.str();
}

std::string agent_error_json(std::string_view message) {
    return "{\"schema\":\"project-atlas.agent.v1\",\"error\":" +
        json_string(message) + '}';
}

std::string agent_trace_json(const TraceComparison& comparison) {
    auto write_event = [](std::ostream& out, const std::optional<TraceEvent>& event) {
        if (!event) {
            out << "null";
            return;
        }
        out << "{\"step\":" << event->step
            << ",\"operation\":" << json_string(event->operation)
            << ",\"resource\":" << json_string(event->resource)
            << ",\"value\":" << json_string(event->value) << '}';
    };
    std::ostringstream out;
    out << "{\"schema\":\"project-atlas.agent.v1\",\"operation\":\"trace\""
        << ",\"all_match\":" << (comparison.all_match() ? "true" : "false")
        << ",\"expected_count\":" << comparison.expected_count
        << ",\"actual_count\":" << comparison.actual_count
        << ",\"first_difference\":";
    if (comparison.first_difference) {
        out << "{\"step\":" << comparison.first_difference->step
            << ",\"expected\":";
        write_event(out, comparison.first_difference->expected);
        out << ",\"actual\":";
        write_event(out, comparison.first_difference->actual);
        out << '}';
    } else {
        out << "null";
    }
    out << '}';
    return out.str();
}

std::string agent_runtime_json(const RuntimeSnapshot& snapshot) {
    std::ostringstream out;
    out << "{\"schema\":\"project-atlas.agent.v1\",\"operation\":\"runtime\""
        << ",\"step\":" << snapshot.step
        << ",\"total_steps\":" << snapshot.total_steps
        << ",\"capture_truncated\":" << (snapshot.capture_truncated ? "true" : "false")
        << ",\"nodes\":[";
    for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
        if (i) out << ',';
        const auto& node = snapshot.nodes[i];
        out << "{\"id\":" << json_string(node.id)
            << ",\"label\":" << json_string(node.label)
            << ",\"kind\":" << json_string(
                node.kind == RuntimeNodeKind::variable ? "variable"
                : node.kind == RuntimeNodeKind::site ? "site" : "object") << '}';
    }
    out << "],\"links\":[";
    for (std::size_t i = 0; i < snapshot.links.size(); ++i) {
        if (i) out << ',';
        const auto& link = snapshot.links[i];
        out << "{\"source\":" << json_string(link.source)
            << ",\"target\":" << json_string(link.target)
            << ",\"relation\":" << json_string(link.relation) << '}';
    }
    out << "]}";
    return out.str();
}

} // namespace atlas
