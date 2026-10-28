#include "atlas/atlas.hpp"
#include "atlas/card_archive.hpp"
#include "atlas/interactions.hpp"
#include "atlas/symbol_identity.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace atlas {
namespace {

constexpr std::string_view marker_name = ".project-atlas-generated";
constexpr std::string_view card_marker = "<!-- Project Atlas generated card -->";

std::expected<void, std::string> safe_report_path(const fs::path& path) {
    std::error_code ec;
    const auto absolute = fs::absolute(path, ec).lexically_normal();
    if (ec) return std::unexpected("cannot resolve report path: " + path.string());
    fs::path current = absolute.root_path();
    for (const auto& part : absolute.relative_path()) {
        current /= part;
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
            return std::unexpected("cannot inspect report path: " + current.string());
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return std::unexpected("report path crosses a reparse point: " + current.string());
#else
        const auto status = fs::symlink_status(current, ec);
        if (ec == std::errc::no_such_file_or_directory) { ec.clear(); continue; }
        if (ec) return std::unexpected("cannot inspect report path: " + current.string());
        if (fs::is_symlink(status))
            return std::unexpected("report path crosses a symlink: " + current.string());
#endif
    }
    return {};
}

class StagedOutput : public std::ofstream {
public:
    explicit StagedOutput(fs::path target) : target_(std::move(target)) {
        if (auto safe = safe_report_path(target_); !safe) {
            error_ = safe.error();
            setstate(std::ios::failbit);
            return;
        }
        static std::atomic<std::uint64_t> next{0};
        for (int attempt = 0; attempt < 8; ++attempt) {
            temporary_ = target_;
            temporary_ += ".atlas-" + std::to_string(++next) + ".tmp";
#ifdef _WIN32
            HANDLE reserved = CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr,
                                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (reserved == INVALID_HANDLE_VALUE) continue;
            CloseHandle(reserved);
#else
            if (fs::exists(temporary_)) continue;
#endif
            open(temporary_, std::ios::binary);
            if (is_open()) return;
            break;
        }
        error_ = "cannot create staged report file: " + target_.string();
        setstate(std::ios::failbit);
    }
    ~StagedOutput() {
        if (is_open()) close();
        if (!committed_ && !temporary_.empty()) {
            std::error_code ignored;
            fs::remove(temporary_, ignored);
        }
    }
    std::expected<void, std::string> commit() {
        if (!*this) return std::unexpected(error_.empty() ?
            "cannot complete report file: " + target_.string() : error_);
        flush();
        if (!*this) return std::unexpected("cannot flush report file: " + target_.string());
        close();
        if (!*this) return std::unexpected("cannot close report file: " + target_.string());
        if (auto safe = safe_report_path(target_); !safe) return safe;
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(target_.c_str());
        const bool replaced = attributes == INVALID_FILE_ATTRIBUTES
            ? MoveFileExW(temporary_.c_str(), target_.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE
            : ReplaceFileW(target_.c_str(), temporary_.c_str(), nullptr, 0,
                           nullptr, nullptr) != FALSE;
        if (!replaced) return std::unexpected("cannot publish report file: " + target_.string());
#else
        std::error_code ec;
        fs::rename(temporary_, target_, ec);
        if (ec) return std::unexpected("cannot publish report file: " + ec.message());
#endif
        committed_ = true;
        return {};
    }
private:
    fs::path target_, temporary_;
    std::string error_;
    bool committed_ = false;
};

std::string utf8_path(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string escape_html(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::string escape_markdown(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        if (c == '|' || c == '`' || c == '\\' || c == '[' || c == ']') {
            result += '\\';
        }
        result += c;
    }
    return result;
}

std::string hash_hex(std::uint64_t hash) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::expected<std::uint64_t, std::string> current_source_hash(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::is_symlink(path, ec) || ec) {
        return std::unexpected("source is missing or no longer a regular file: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected("cannot reopen source: " + path.string());
    std::uint64_t hash = fnv1a64("");
    char buffer[8192];
    while (input) {
        input.read(buffer, sizeof(buffer));
        for (std::streamsize i = 0; i < input.gcount(); ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }
    if (input.bad()) return std::unexpected("cannot reread source: " + path.string());
    return hash;
}

fs::path card_relative_path(const SourceFile& file) {
    fs::path path = file.relative_path;
    path += ".agent.md";
    return path;
}

fs::path summary_relative_path(const SourceFile& file) {
    fs::path path = fs::path("summaries") / file.relative_path;
    path += ".md";
    return path;
}

std::expected<void, std::string> prepare_output(
    const fs::path& root, const fs::path& output) {
    if (auto safe = safe_report_path(output); !safe) return safe;
    if (auto safe = safe_report_path(output / marker_name); !safe) return safe;
    std::error_code ec;
    const auto absolute_output = fs::weakly_canonical(output, ec);
    if (ec || absolute_output == root) {
        return std::unexpected("output must be a separate directory");
    }
    if (fs::exists(output, ec)) {
        if (!fs::is_directory(output, ec)) {
            return std::unexpected("output path is not a directory: " + output.string());
        }
        if (!fs::exists(output / marker_name, ec) && !fs::is_empty(output, ec)) {
            return std::unexpected(
                "output directory is non-empty and not owned by Project Atlas: " +
                output.string());
        }
    } else {
        fs::create_directories(output, ec);
        if (ec) {
            return std::unexpected("cannot create output directory: " + ec.message());
        }
    }
    StagedOutput marker(output / marker_name);
    if (!marker) {
        return std::unexpected("cannot write Project Atlas marker");
    }
    marker << "Generated files only. Keep human contracts outside this directory.\n";
    return marker.commit();
}

std::expected<void, std::string> write_card(
    const SourceFile& file, const fs::path& output) {
    const auto target = output / card_relative_path(file);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        return std::unexpected("cannot create report directory: " + ec.message());
    }
    StagedOutput out(target);
    if (!out) {
        return std::unexpected("cannot write source card: " + target.string());
    }
    out << card_marker << "\n\n"
        << "# " << escape_markdown(utf8_path(file.relative_path)) << "\n\n"
        << "Generated from libclang AST. Source code and tests remain authoritative.\n\n"
        << "- Source: `" << escape_markdown(utf8_path(file.relative_path)) << "`\n"
        << "- Content hash (FNV-1a 64): `" << hash_hex(file.content_hash) << "`\n"
        << "- Compile command: "
        << (file.used_compile_command ? "from compile database" : "default C/C++ flags")
        << "\n"
        << "- AST parse: **" << (file.parsed ? "completed" : "failed") << "**\n"
        << "- Clang errors: **" << file.error_diagnostic_count << "**; warnings: **"
        << file.warning_diagnostic_count << "**\n\n"
        << "## Declared types\n\n";
    if (file.symbols.empty()) {
        out << "No class or struct declaration detected.\n";
    } else {
        out << "| Kind | Qualified name | Line | Base types |\n"
            << "| --- | --- | ---: | --- |\n";
        for (const auto& symbol : file.symbols) {
            out << "| " << escape_markdown(symbol.kind) << " | "
                << escape_markdown(symbol.qualified_name.empty() ? symbol.name
                                                                 : symbol.qualified_name)
                << " | " << symbol.line << " | "
                << escape_markdown(symbol.base) << " |\n";
        }
    }
    out << "\n## Functions and methods\n\n";
    if (file.functions.empty()) {
        out << "None detected in this file.\n";
    } else {
        out << "| Kind | Qualified name | Line | Body | Signature |\n"
            << "| --- | --- | ---: | --- | --- |\n";
        for (const auto& function : file.functions) {
            out << "| " << escape_markdown(function.kind) << " | "
                << escape_markdown(function.qualified_name) << " | "
                << function.line << " | "
                << (function.has_body ? "yes" : "no") << " | "
                << escape_markdown(function.signature) << " |\n";
        }
    }
    const auto project_calls = std::ranges::count_if(file.calls,
        [](const CallSite& call) { return !call.callee_file.empty(); });
    out << "\n## Static calls to project code\n\n"
        << "Resolved from parsed function bodies; virtual dispatch and callbacks may "
           "reach additional targets.\n\n"
        << "- Project call sites: " << project_calls << "\n"
        << "- External or unresolved call sites: "
        << file.calls.size() - static_cast<std::size_t>(project_calls) << "\n\n";
    if (project_calls == 0) {
        out << "None resolved in this file.\n";
    } else {
        std::map<std::pair<std::string, std::string>,
                 std::pair<const CallSite*, std::size_t>> grouped;
        for (const auto& call : file.calls) {
            if (call.callee_file.empty()) continue;
            const auto key = std::pair{
                call.caller_usr.empty() ? call.caller_name : call.caller_usr,
                call.callee_usr.empty() ? call.callee_name : call.callee_usr};
            auto result = grouped.try_emplace(key, &call, 0);
            ++result.first->second.second;
        }
        out << "| Caller | Callee | First line | Sites | Target file |\n"
            << "| --- | --- | ---: | ---: | --- |\n";
        std::size_t shown = 0;
        for (const auto& entry : grouped) {
            if (shown++ >= 60) continue;
            const auto& group = entry.second;
            const auto& call = *group.first;
            out << "| " << escape_markdown(call.caller_name) << " | "
                << escape_markdown(call.callee_name)
                << (call.dynamic_dispatch ? " (virtual)" : "") << " | "
                << call.line << " | " << group.second << " | "
                << escape_markdown(utf8_path(call.callee_file)) << " |\n";
        }
        if (shown > 60) out << "\n" << shown - 60
                            << " additional caller/callee pairs omitted; use `agent find`.\n";
    }
    out << "\n## Includes\n\n";
    if (file.includes.empty()) {
        out << "None detected.\n";
    } else {
        for (const auto& include : file.includes) {
            out << "- `" << escape_markdown(include) << "`\n";
        }
    }
    if (!file.diagnostics.empty()) {
        out << "\n## Clang diagnostics\n\n";
        for (const auto& diagnostic : file.diagnostics) {
            out << "- " << escape_markdown(diagnostic) << "\n";
        }
    }
    return out.commit();
}

std::expected<void, std::string> write_summary(
    const SourceFile& file, const fs::path& output) {
    const auto target = output / summary_relative_path(file);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) return std::unexpected("cannot create summary directory: " + ec.message());
    StagedOutput out(target);
    if (!out) return std::unexpected("cannot write source summary: " + target.string());
    const auto detail = (output / card_relative_path(file))
        .lexically_relative(target.parent_path());
    out << card_marker << "\n\n"
        << "# " << escape_markdown(utf8_path(file.relative_path)) << " — quick context\n\n"
        << "Navigation only; verify source before editing. "
        << "[Full AST card](<" << utf8_path(detail) << ">).\n\n"
        << "- Source hash: `" << hash_hex(file.content_hash) << "`\n"
        << "- AST: " << (file.parsed ? "parsed" : "failed")
        << "; errors: " << file.error_diagnostic_count
        << "; warnings: " << file.warning_diagnostic_count << "\n"
        << "- Types: " << file.symbols.size()
        << "; functions: " << file.functions.size()
        << "; call sites: " << file.calls.size() << "\n";
    if (!file.symbols.empty()) {
        out << "\nTypes (first 4):\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(file.symbols.size(), 4); ++i) {
            const auto& symbol = file.symbols[i];
            out << "- `" << escape_markdown(symbol.qualified_name.empty()
                ? symbol.name : symbol.qualified_name) << "` : " << symbol.line << "\n";
        }
        if (file.symbols.size() > 4)
            out << "- " << file.symbols.size() - 4 << " more in the full card\n";
    }
    if (!file.functions.empty()) {
        out << "\nFunctions (first 4):\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(file.functions.size(), 4); ++i)
            out << "- `" << escape_markdown(file.functions[i].qualified_name)
                << "` : " << file.functions[i].line << "\n";
        if (file.functions.size() > 4)
            out << "- " << file.functions.size() - 4 << " more in the full card\n";
    }
    return out.commit();
}

std::expected<void, std::string> write_index(
    const ProjectScan& scan, const fs::path& output) {
    StagedOutput out(output / "index.md");
    if (!out) {
        return std::unexpected("cannot write index.md");
    }
    const auto failures = std::count_if(scan.files.begin(), scan.files.end(),
                                        [](const SourceFile& file) { return !file.parsed; });
    std::size_t errors = 0, warnings = 0;
    for (const auto& file : scan.files) {
        errors += file.error_diagnostic_count;
        warnings += file.warning_diagnostic_count;
    }
    out << "# Project Atlas\n\n"
        << "Root: `" << escape_markdown(utf8_path(scan.root)) << "`\n\n"
        << "Compilation database: "
        << (scan.compilation_database_directory
                ? "`" + escape_markdown(utf8_path(*scan.compilation_database_directory)) + "`"
                : "not supplied") << "\n\n"
        << "Detected " << scan.files.size() << " C/C++ source files; "
        << failures << " AST parse failures; " << errors << " Clang errors; "
        << warnings << " warnings. "
        << "These are navigation facts, not behavioral contracts.\n\n"
        << "See [class outline](classes.md), [static call map](calls.md), "
           "[interaction evidence](interactions.md), "
           "or [browser navigator](index.html).\n\n"
        << "## Files\n\n";
    for (const auto& file : scan.files) {
        out << "- [" << escape_markdown(utf8_path(file.relative_path)) << "](<"
            << utf8_path(summary_relative_path(file)) << ">)"
            << " ([detail](<" << utf8_path(card_relative_path(file)) << ">))";
        if (!file.parsed) {
            out << " — **AST parse failed**";
        } else if (file.error_diagnostic_count) {
            out << " — **" << file.error_diagnostic_count << " Clang error(s)**";
        } else if (!file.symbols.empty()) {
            out << " — ";
            for (std::size_t i = 0; i < std::min<std::size_t>(file.symbols.size(), 3); ++i) {
                if (i != 0) out << ", ";
                out << '`' << escape_markdown(file.symbols[i].qualified_name.empty()
                    ? file.symbols[i].name : file.symbols[i].qualified_name) << '`';
            }
            if (file.symbols.size() > 3)
                out << ", +" << file.symbols.size() - 3 << " more";
        }
        if (!file.functions.empty()) out << " — " << file.functions.size() << " functions";
        if (!file.calls.empty()) out << " — " << file.calls.size() << " call sites";
        out << '\n';
    }
    if (!scan.skipped_large_files.empty()) {
        out << "\n## Skipped files over 2 MiB\n\n";
        for (const auto& path : scan.skipped_large_files) {
            out << "- `" << escape_markdown(utf8_path(path)) << "`\n";
        }
    }
    return out.commit();
}

std::expected<void, std::string> write_classes(
    const ProjectScan& scan, const fs::path& output) {
    std::map<std::string, std::string> names;
    std::map<std::string, std::string> identities_by_id;
    std::set<std::pair<std::string, std::string>> inheritance;
    for (const auto& file : scan.files) {
        for (const auto& symbol : file.symbols) {
            const auto qualified = symbol.qualified_name.empty()
                ? symbol.name : symbol.qualified_name;
            const auto identity = symbol.usr.empty()
                ? "qualified:" + qualified : symbol.usr;
            names.emplace(identity, qualified);
            for (std::size_t i = 0; i < symbol.base_usrs.size(); ++i) {
                const auto& base_identity = symbol.base_usrs[i];
                const auto& base_label = symbol.base_qualified_names[i];
                names.emplace(base_identity, base_label);
                inheritance.emplace(base_identity, identity);
            }
        }
    }
    for (const auto& [identity, label] : names) {
        (void)label;
        const auto id = mermaid_symbol_id(identity);
        const auto [existing, inserted] = identities_by_id.emplace(id, identity);
        if (!inserted && existing->second != identity) {
            return std::unexpected("symbol ID hash collision; cannot write class diagram");
        }
    }
    StagedOutput out(output / "classes.md");
    if (!out) {
        return std::unexpected("cannot write classes.md");
    }
    out << "# Class outline\n\n"
        << "Generated from libclang declarations and base specifiers in scanned files. "
        << "Parse failures and missing build flags can make this incomplete.\n\n"
        << "```mermaid\nclassDiagram\n";
    for (const auto& [identity, label] : names) {
        out << "    class " << mermaid_symbol_id(identity) << "[\"" << label << "\"]\n";
    }
    for (const auto& [base, derived] : inheritance) {
        out << "    " << mermaid_symbol_id(base) << " <|-- "
            << mermaid_symbol_id(derived) << '\n';
    }
    out << "```\n";
    return out.commit();
}

std::expected<void, std::string> write_calls(
    const ProjectScan& scan, const fs::path& output) {
    std::map<std::pair<fs::path, fs::path>, std::size_t> links;
    std::size_t total = 0, project = 0, external = 0;
    std::size_t unresolved = 0, dynamic = 0, indirect = 0;
    for (const auto& file : scan.files) {
        for (const auto& call : file.calls) {
            ++total;
            dynamic += call.dynamic_dispatch;
            indirect += call.indirect;
            if (call.callee_usr.empty()) ++unresolved;
            else if (call.callee_file.empty()) ++external;
            if (call.callee_file.empty()) continue;
            ++project;
            if (file.relative_path != call.callee_file)
                ++links[{file.relative_path, call.callee_file}];
        }
    }
    StagedOutput out(output / "calls.md");
    if (!out) return std::unexpected("cannot write calls.md");
    out << "# Static call map\n\n"
        << "Libclang-resolved call expressions in scanned source bodies. This is "
           "not an execution trace: virtual dispatch, function pointers, callbacks, "
           "and generated code can invoke different targets.\n\n"
        << "- Call expressions: " << total << "\n"
        << "- Resolved to declarations inside this project: " << project << "\n"
        << "- Resolved to declarations outside this project: " << external << "\n"
        << "- Unresolved declaration target: " << unresolved << "\n"
        << "- Dynamic dispatch sites: " << dynamic << "\n"
        << "- Indirect calls through non-function declarations: " << indirect << "\n"
        << "- Cross-file directed links: " << links.size() << "\n\n";
    if (!links.empty()) {
        std::set<fs::path> nodes;
        for (const auto& [paths, count] : links) {
            nodes.insert(paths.first);
            nodes.insert(paths.second);
        }
        out << "```mermaid\nflowchart LR\n";
        for (const auto& path : nodes) {
            const auto label = utf8_path(path);
            out << "    " << mermaid_symbol_id("file:" + label)
                << "[\"" << label << "\"]\n";
        }
        for (const auto& [paths, count] : links) {
            out << "    " << mermaid_symbol_id("file:" + utf8_path(paths.first))
                << " -->|" << count << "| "
                << mermaid_symbol_id("file:" + utf8_path(paths.second)) << '\n';
        }
        out << "```\n\n| Source file | Target declaration file | Call sites |\n"
            << "| --- | --- | ---: |\n";
        for (const auto& [paths, count] : links)
            out << "| " << escape_markdown(utf8_path(paths.first))
                << " | " << escape_markdown(utf8_path(paths.second))
                << " | " << count << " |\n";
    } else out << "No resolved cross-file calls.\n";
    return out.commit();
}

std::expected<void, std::string> write_interactions(
    const ProjectScan& scan, const fs::path& output) {
    const auto graph = build_interaction_graph(scan);
    StagedOutput out(output / "interactions.md");
    if (!out) return std::unexpected("cannot write interactions.md");
    out << "# Interaction evidence map\n\n"
        << "Source-file view of AST inheritance, field types, and resolved call sites. "
           "These are static facts, not observed execution. Runtime IDs can be joined "
           "only when a trace is supplied to `agent interactions`; no source identity "
           "is inferred from a similarly named runtime object.\n\n"
        << "- Source files: " << scan.files.size() << "\n"
        << "- Evidence rows: " << graph.edges.size() << "\n"
        << "- Resolved calls without a mapped project file: " << graph.unmapped_calls << "\n"
        << "- Unresolved call sites: " << graph.unresolved_calls << "\n"
        << "- Dynamic-dispatch sites: " << graph.dynamic_call_sites << "\n\n";
    std::map<std::tuple<std::size_t, std::size_t, InteractionKind>, std::size_t> links;
    for (const auto& edge : graph.edges)
        links[{edge.from, edge.to, edge.kind}] += edge.count;
    if (!links.empty()) {
        out << "```mermaid\nflowchart LR\n";
        for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
            const auto& node = graph.nodes[i];
            if (node.kind != InteractionNodeKind::source_file) continue;
            out << "    n" << i << "[\"" << escape_html(node.label) << "\"]\n";
        }
        for (const auto& [key, count] : links) {
            const auto& [from, to, kind] = key;
            out << "    n" << from << " -->|" << interaction_kind_name(kind)
                << " " << count << "| n" << to << '\n';
        }
        out << "```\n\n";
    }
    out << "| Evidence | From | To | Detail | Count | First source line |\n"
        << "| --- | --- | --- | --- | ---: | ---: |\n";
    for (const auto& edge : graph.edges) {
        out << "| " << interaction_kind_name(edge.kind)
            << " | " << escape_markdown(graph.nodes[edge.from].label)
            << " | " << escape_markdown(graph.nodes[edge.to].label)
            << " | " << escape_markdown(edge.detail)
            << " | " << edge.count << " | " << edge.first_line << " |\n";
    }
    return out.commit();
}

std::expected<void, std::string> write_html(
    const ProjectScan& scan, const fs::path& output) {
    StagedOutput out(output / "index.html");
    if (!out) {
        return std::unexpected("cannot write index.html");
    }
    out << "<!doctype html>\n<html lang=\"en\"><meta charset=\"utf-8\">\n"
        << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
        << "<title>Project Atlas</title>\n"
        << "<style>body{font:16px system-ui;max-width:68rem;margin:3rem auto;padding:0 1rem;"
        << "color:#18303a;background:#f7faf9}input{font:inherit;padding:.65rem;width:min(30rem,90%);"
        << "border:1px solid #8aa;border-radius:.35rem}li{margin:.6rem 0}small{color:#52666d}"
        << "a{color:#075777}</style>\n"
        << "<h1>Project Atlas</h1><p>Generated navigation outline for "
        << escape_html(utf8_path(scan.root)) << ". Behavioral contracts are separate.</p>\n"
        << "<label for=\"query\">Find file or class</label><br>"
        << "<input id=\"query\" type=\"search\" autocomplete=\"off\">\n"
        << "<p><a href=\"index.md\">Markdown map</a> · "
        << "<a href=\"classes.md\">Class outline</a> · "
        << "<a href=\"calls.md\">Static call map</a> · "
        << "<a href=\"interactions.md\">Interaction evidence</a></p><ul id=\"files\">\n";
    for (const auto& file : scan.files) {
        const auto path = utf8_path(file.relative_path);
        std::string names;
        for (const auto& symbol : file.symbols) {
            if (!names.empty()) names += ", ";
            names += symbol.qualified_name.empty() ? symbol.name : symbol.qualified_name;
        }
        out << "<li data-search=\"" << escape_html(path + " " + names) << "\">"
            << "<a href=\"" << escape_html(utf8_path(card_relative_path(file))) << "\">"
            << escape_html(path) << "</a> <small>" << escape_html(names)
            << "</small></li>\n";
    }
    out << "</ul><script>const q=document.getElementById('query');"
        << "q.addEventListener('input',()=>{const v=q.value.toLocaleLowerCase();"
        << "for(const e of document.querySelectorAll('#files li'))"
        << "e.hidden=!e.dataset.search.toLocaleLowerCase().includes(v)});</script></html>\n";
    return out.commit();
}

} // namespace

std::expected<void, std::string> write_project_report(
    const ProjectScan& scan, const fs::path& output_directory,
    bool place_cards_next_to_source) {
    for (const auto& file : scan.files) {
        const auto path = scan.root / file.relative_path;
        const auto current = current_source_hash(path);
        if (!current) return std::unexpected(current.error());
        if (*current != file.content_hash) {
            return std::unexpected("source changed since scan; rescan before generating cards: " +
                                   path.string());
        }
    }
    if (place_cards_next_to_source) {
        for (const auto& file : scan.files) {
            const auto target = scan.root / card_relative_path(file);
            if (auto safe = safe_report_path(target); !safe) return safe;
            std::error_code ec;
            if (!fs::exists(target, ec)) continue;
            if (ec || !fs::is_regular_file(target, ec) || fs::is_symlink(target, ec)) {
                return std::unexpected("cannot replace non-regular source card: " +
                                       target.string());
            }
            std::ifstream existing(target, std::ios::binary);
            std::string first_line;
            std::getline(existing, first_line);
            if (first_line != card_marker) {
                return std::unexpected("source card is not generated by Project Atlas: " +
                                       target.string());
            }
        }
    }
    for (const auto& name : {"index.md", "classes.md", "calls.md",
                             "interactions.md", "index.html"}) {
        if (auto safe = safe_report_path(output_directory / name); !safe) return safe;
    }
    for (const auto& file : scan.files) {
        if (auto safe = safe_report_path(output_directory / card_relative_path(file)); !safe)
            return safe;
        if (auto safe = safe_report_path(output_directory / summary_relative_path(file)); !safe)
            return safe;
    }
    if (auto result = prepare_output(scan.root, output_directory); !result) {
        return result;
    }
    for (const auto& file : scan.files) {
        if (auto result = write_card(file, output_directory); !result) {
            return result;
        }
        if (auto result = write_summary(file, output_directory); !result) {
            return result;
        }
        if (place_cards_next_to_source) {
            if (auto result = write_card(file, scan.root); !result) {
                return result;
            }
        }
    }
    if (auto result = write_index(scan, output_directory); !result) return result;
    if (auto result = write_classes(scan, output_directory); !result) return result;
    if (auto result = write_calls(scan, output_directory); !result) return result;
    if (auto result = write_interactions(scan, output_directory); !result) return result;
    if (auto result = write_html(scan, output_directory); !result) return result;
    if (auto archived = archive_orphan_report_cards(scan.root, output_directory);
        !archived) return std::unexpected(archived.error());
    if (place_cards_next_to_source) {
        if (auto archived = archive_orphan_source_cards(scan.root, output_directory);
            !archived) return std::unexpected(archived.error());
    }
    return {};
}

} // namespace atlas
