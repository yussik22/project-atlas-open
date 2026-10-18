#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atlas {

namespace fs = std::filesystem;

struct SymbolMember {
    std::string kind; // field, static_field, method, constructor, destructor
    std::string name;
    std::string type;
    std::string access; // public, protected, private
    std::string signature;
    std::size_t line = 0;
    std::string related_usr; // first resolved field type, for existing consumers
    std::vector<std::string> related_usrs; // all resolved field types, including template arguments
};

struct Symbol {
    std::string kind;
    std::string name;
    std::string base;
    std::size_t line = 0;
    std::string qualified_name;
    std::string usr;
    std::vector<std::string> base_qualified_names;
    std::vector<std::string> base_usrs;
    std::vector<SymbolMember> members;
};

struct FunctionSymbol {
    std::string kind; // function, function_template, method, constructor, destructor
    std::string qualified_name;
    std::string usr;
    std::string signature;
    std::size_t line = 0;
    bool has_body = false;
};

struct CallSite {
    std::string caller_usr;
    std::string caller_name;
    std::string callee_usr; // empty when libclang cannot resolve a declaration
    std::string callee_name;
    fs::path callee_file; // project-relative when target resolves inside the project
    std::size_t line = 0;
    std::size_t column = 0;
    bool dynamic_dispatch = false;
    bool indirect = false; // function pointer or other non-function declaration
};

struct SourceFile {
    fs::path relative_path;
    std::uint64_t content_hash = 0;
    bool parsed = false;
    bool used_compile_command = false;
    std::size_t error_diagnostic_count = 0;
    std::size_t warning_diagnostic_count = 0;
    std::vector<std::string> includes;
    std::vector<Symbol> symbols;
    std::vector<FunctionSymbol> functions;
    std::vector<CallSite> calls;
    std::vector<std::string> diagnostics;
};

struct ProjectScan {
    fs::path root;
    std::optional<fs::path> compilation_database_directory;
    std::vector<SourceFile> files;
    std::vector<fs::path> skipped_large_files;
};

// AST outline from libclang. An optional compilation database directory improves
// accuracy for projects with non-default includes, defines, and language flags.
std::expected<ProjectScan, std::string> scan_project(
    const fs::path& root,
    const std::optional<fs::path>& compilation_database_directory = std::nullopt);

// Generated reports are written only to an empty directory or a directory
// previously marked as Project Atlas generated output.
std::expected<void, std::string> write_project_report(
    const ProjectScan& scan, const fs::path& output_directory,
    bool place_cards_next_to_source = false);

using Facts = std::map<std::string, std::string>;

std::expected<Facts, std::string> read_facts(const fs::path& path);

enum class DifferenceKind { match, mismatch, missing_actual, unexpected_actual };

struct Difference {
    std::string key;
    std::optional<std::string> expected;
    std::optional<std::string> actual;
    DifferenceKind kind = DifferenceKind::match;
};

struct Comparison {
    std::vector<Difference> entries;

    [[nodiscard]] bool all_match() const noexcept;
};

Comparison compare(const Facts& expected, const Facts& actual);
std::expected<void, std::string> write_comparison_report(
    const Comparison& comparison, const fs::path& output_file);

// Read-only, versioned JSON surface for agents and protocol adapters.
// A query is an ASCII-case-insensitive substring; non-ASCII bytes match exactly.
[[nodiscard]] std::string agent_scan_json(
    const ProjectScan& scan,
    const std::optional<std::string_view>& query = std::nullopt,
    std::size_t max_files = 200);
[[nodiscard]] std::string agent_comparison_json(const Comparison& comparison);
[[nodiscard]] std::string agent_error_json(std::string_view message);

} // namespace atlas
