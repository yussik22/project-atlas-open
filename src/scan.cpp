#include "atlas/atlas.hpp"
#include "atlas/symbol_identity.hpp"

#include <clang-c/CXCompilationDatabase.h>
#include <clang-c/Index.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>

namespace atlas {
namespace {

constexpr std::uintmax_t max_source_bytes = 2U * 1024U * 1024U;

struct IndexHandle {
    CXIndex value = clang_createIndex(0, 0);
    IndexHandle() = default;
    ~IndexHandle() { if (value) clang_disposeIndex(value); }
    IndexHandle(const IndexHandle&) = delete;
    IndexHandle& operator=(const IndexHandle&) = delete;
};

struct TranslationUnitHandle {
    CXTranslationUnit value = nullptr;
    TranslationUnitHandle() = default;
    ~TranslationUnitHandle() { if (value) clang_disposeTranslationUnit(value); }
    TranslationUnitHandle(const TranslationUnitHandle&) = delete;
    TranslationUnitHandle& operator=(const TranslationUnitHandle&) = delete;
};

struct DatabaseHandle {
    CXCompilationDatabase value = nullptr;
    DatabaseHandle() = default;
    ~DatabaseHandle() { if (value) clang_CompilationDatabase_dispose(value); }
    DatabaseHandle(const DatabaseHandle&) = delete;
    DatabaseHandle& operator=(const DatabaseHandle&) = delete;
};

struct CommandsHandle {
    CXCompileCommands value = nullptr;
    CommandsHandle() = default;
    ~CommandsHandle() { if (value) clang_CompileCommands_dispose(value); }
    CommandsHandle(const CommandsHandle&) = delete;
    CommandsHandle& operator=(const CommandsHandle&) = delete;
};

std::string cx_text(CXString text) {
    const char* value = clang_getCString(text);
    std::string result = value ? value : "";
    clang_disposeString(text);
    return result;
}

std::string utf8_path(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

fs::path from_utf8(std::string_view value) {
    return fs::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

// Clang includes absolute build-machine paths in anonymous lambda display
// names. Keep the stable USR untouched, but never persist those paths in
// human-readable maps or adjacent agent cards.
std::string portable_clang_name(std::string text, const SourceFile& file) {
    constexpr std::string_view marker = "(lambda at ";
    std::size_t search = 0;
    while (true) {
        const auto start = text.find(marker, search);
        if (start == std::string::npos) break;
        const auto path_begin = start + marker.size();
        const auto end = text.find(')', path_begin);
        if (end == std::string::npos) {
            text.replace(path_begin, std::string::npos, "<location>");
            break;
        }
        const auto last_colon = text.rfind(':', end);
        const auto line_colon = last_colon == std::string::npos ?
            std::string::npos : text.rfind(':', last_colon - 1);
        if (line_colon == std::string::npos || line_colon < path_begin) {
            text.replace(path_begin, end - path_begin, "<location>");
            search = path_begin + sizeof("<location>") - 1;
            continue;
        }
        const auto path = from_utf8(std::string_view(text).substr(
            path_begin, line_colon - path_begin));
        const auto normalized = utf8_path(path.lexically_normal());
        const auto relative = utf8_path(file.relative_path);
        std::string display = normalized == relative ||
            normalized.ends_with("/" + relative) ? relative : utf8_path(path.filename());
        if (display.empty()) display = "<location>";
        text.replace(path_begin, line_colon - path_begin, display);
        search = path_begin + display.size();
    }
    return text;
}

bool is_source(const fs::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::set<std::string> extensions{
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ipp"};
    return extensions.contains(extension);
}

bool ignored_directory(const fs::path& path) {
    const auto name = path.filename().string();
    static const std::set<std::string> names{
        ".git", ".venv", ".project-atlas", "__pycache__", "build", "dist",
        "node_modules", ".vendor", ".vs", "atlas-test-workspace"};
    return names.contains(name) || name.starts_with("build-") ||
           name.starts_with("cmake-build-");
}

unsigned location_line(CXCursor cursor) {
    unsigned line = 0;
    clang_getFileLocation(clang_getCursorLocation(cursor), nullptr, &line, nullptr, nullptr);
    return line;
}

unsigned location_column(CXCursor cursor) {
    unsigned column = 0;
    clang_getFileLocation(clang_getCursorLocation(cursor), nullptr, nullptr,
                          &column, nullptr);
    return column;
}

std::string qualified_name(CXCursor cursor, const SourceFile& file) {
    std::vector<std::string> parts;
    for (CXCursor current = cursor; !clang_Cursor_isNull(current);
         current = clang_getCursorSemanticParent(current)) {
        const auto kind = clang_getCursorKind(current);
        if (kind == CXCursor_TranslationUnit) break;
        if (kind != CXCursor_Namespace && kind != CXCursor_ClassDecl &&
            kind != CXCursor_StructDecl && kind != CXCursor_ClassTemplate &&
            kind != CXCursor_ClassTemplatePartialSpecialization &&
            kind != CXCursor_UnionDecl) continue;
        auto part = portable_clang_name(cx_text(clang_getCursorSpelling(current)), file);
        if (part.empty() && kind == CXCursor_Namespace) {
            part = "(anonymous@" + utf8_path(file.relative_path) + ")";
        }
        if (!part.empty()) parts.push_back(std::move(part));
    }
    std::string result;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!result.empty()) result += "::";
        result += *it;
    }
    return result;
}

std::string cursor_identity(CXCursor cursor, const SourceFile& file) {
    auto usr = cx_text(clang_getCursorUSR(cursor));
    if (!usr.empty()) return usr;
    return "qualified:" + qualified_name(cursor, file);
}

std::string callable_name(CXCursor cursor, const SourceFile& file) {
    auto name = portable_clang_name(cx_text(clang_getCursorSpelling(cursor)), file);
    if (name.empty()) name = portable_clang_name(
        cx_text(clang_getCursorDisplayName(cursor)), file);
    auto scope = qualified_name(cursor, file);
    if (!scope.empty()) scope += "::";
    return scope + name;
}

bool callable_declaration(CXCursorKind kind) {
    return kind == CXCursor_FunctionDecl || kind == CXCursor_FunctionTemplate ||
        kind == CXCursor_CXXMethod || kind == CXCursor_Constructor ||
        kind == CXCursor_Destructor || kind == CXCursor_ConversionFunction;
}

using DeclarationFileCache = std::unordered_map<CXFile, fs::path>;

fs::path project_declaration_file(CXCursor declaration, const fs::path& root,
                                  DeclarationFileCache& cache) {
    CXFile clang_file = nullptr;
    clang_getFileLocation(clang_getCursorLocation(declaration), &clang_file,
                          nullptr, nullptr, nullptr);
    if (!clang_file) return {};
    if (const auto found = cache.find(clang_file); found != cache.end())
        return found->second;
    const auto name = cx_text(clang_getFileName(clang_file));
    fs::path relative;
    if (!name.empty()) {
        std::error_code error;
        const auto absolute = fs::weakly_canonical(from_utf8(name), error);
        if (!error) {
            auto candidate = absolute.lexically_relative(root);
            if (!candidate.empty() && !candidate.is_absolute() &&
                *candidate.begin() != fs::path(".."))
                relative = std::move(candidate);
        }
    }
    cache.emplace(clang_file, relative);
    return relative;
}

std::string member_access(CXCursor cursor, CXCursor parent) {
    switch (clang_getCXXAccessSpecifier(cursor)) {
    case CX_CXXPublic: return "public";
    case CX_CXXProtected: return "protected";
    case CX_CXXPrivate: return "private";
    default:
        return clang_getCursorKind(parent) == CXCursor_ClassDecl ? "private" : "public";
    }
}

void collect_field_types(CXType type, const SourceFile& file,
                         std::vector<std::string>& identities, int depth = 0) {
    if (type.kind == CXType_Invalid || depth >= 10) return;
    const auto declaration = clang_getTypeDeclaration(type);
    const auto kind = clang_getCursorKind(declaration);
    if (kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl ||
        kind == CXCursor_ClassTemplate ||
        kind == CXCursor_ClassTemplatePartialSpecialization) {
        const auto identity = cursor_identity(declaration, file);
        if (std::ranges::find(identities, identity) == identities.end())
            identities.push_back(identity);
    }
    const auto arguments = clang_Type_getNumTemplateArguments(type);
    for (int index = 0; index < arguments; ++index)
        collect_field_types(clang_Type_getTemplateArgumentAsType(type,
            static_cast<unsigned>(index)), file, identities, depth + 1);
    if (type.kind == CXType_Pointer || type.kind == CXType_LValueReference ||
        type.kind == CXType_RValueReference)
        collect_field_types(clang_getPointeeType(type), file, identities, depth + 1);
    else if (type.kind == CXType_ConstantArray || type.kind == CXType_IncompleteArray ||
             type.kind == CXType_VariableArray || type.kind == CXType_DependentSizedArray)
        collect_field_types(clang_getArrayElementType(type), file, identities, depth + 1);
    const auto canonical = clang_getCanonicalType(type);
    if (!clang_equalTypes(type, canonical))
        collect_field_types(canonical, file, identities, depth + 1);
}

std::optional<SymbolMember> class_member(CXCursor cursor, CXCursor parent,
                                         const SourceFile& file) {
    const auto parent_kind = clang_getCursorKind(parent);
    if (parent_kind != CXCursor_ClassDecl && parent_kind != CXCursor_StructDecl &&
        parent_kind != CXCursor_ClassTemplate &&
        parent_kind != CXCursor_ClassTemplatePartialSpecialization) return std::nullopt;
    const auto kind = clang_getCursorKind(cursor);
    SymbolMember member;
    if (kind == CXCursor_FieldDecl) member.kind = "field";
    else if (kind == CXCursor_VarDecl) member.kind = "static_field";
    else if (kind == CXCursor_CXXMethod) member.kind = "method";
    else if (kind == CXCursor_Constructor) member.kind = "constructor";
    else if (kind == CXCursor_Destructor) member.kind = "destructor";
    else return std::nullopt;
    member.name = cx_text(clang_getCursorSpelling(cursor));
    if (member.name.empty()) return std::nullopt;
    member.line = location_line(cursor);
    member.access = member_access(cursor, parent);
    if (member.kind == "field" || member.kind == "static_field") {
        CXType type = clang_getCursorType(cursor);
        member.type = portable_clang_name(cx_text(clang_getTypeSpelling(type)), file);
        member.signature = member.name + ": " + member.type;
        collect_field_types(type, file, member.related_usrs);
        if (!member.related_usrs.empty()) member.related_usr = member.related_usrs.front();
    } else {
        member.signature = portable_clang_name(
            cx_text(clang_getCursorDisplayName(cursor)), file);
        if (member.signature.empty()) member.signature = member.name + "()";
        if (member.kind == "method") {
            member.type = cx_text(clang_getTypeSpelling(
                clang_getResultType(clang_getCursorType(cursor))));
            if (!member.type.empty()) member.signature += ": " + member.type;
        }
    }
    return member;
}

struct VisitContext {
    SourceFile* file = nullptr;
    const fs::path* root = nullptr;
    CXCursor caller{};
    DeclarationFileCache* declaration_files = nullptr;
};

CXChildVisitResult visit_cursor(CXCursor cursor, CXCursor parent, CXClientData data) {
    if (!clang_Location_isFromMainFile(clang_getCursorLocation(cursor))) {
        return CXChildVisit_Continue;
    }
    const auto& context = *static_cast<VisitContext*>(data);
    auto& file = *context.file;
    const auto kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_InclusionDirective) {
        const auto include = cx_text(clang_getCursorSpelling(cursor));
        if (!include.empty()) file.includes.push_back(include);
        return CXChildVisit_Continue;
    }
    const bool function = kind == CXCursor_FunctionDecl ||
        kind == CXCursor_FunctionTemplate || kind == CXCursor_CXXMethod ||
        kind == CXCursor_Constructor || kind == CXCursor_Destructor;
    if (function) {
        const auto name = cx_text(clang_getCursorSpelling(cursor));
        if (!name.empty()) {
            FunctionSymbol entry;
            entry.kind = kind == CXCursor_FunctionTemplate ? "function_template" :
                kind == CXCursor_CXXMethod ? "method" :
                kind == CXCursor_Constructor ? "constructor" :
                kind == CXCursor_Destructor ? "destructor" : "function";
            entry.qualified_name = qualified_name(cursor, file);
            if (!entry.qualified_name.empty()) entry.qualified_name += "::";
            entry.qualified_name += name;
            entry.usr = cursor_identity(clang_getCanonicalCursor(cursor), file);
            entry.signature = portable_clang_name(
                cx_text(clang_getCursorDisplayName(cursor)), file);
            if (entry.signature.empty()) entry.signature = name + "()";
            if (kind != CXCursor_Constructor && kind != CXCursor_Destructor) {
                const auto result = cx_text(clang_getTypeSpelling(
                    clang_getResultType(clang_getCursorType(cursor))));
                if (!result.empty()) entry.signature += " → " + result;
            }
            entry.line = location_line(cursor);
            entry.has_body = clang_isCursorDefinition(cursor) != 0;
            const auto existing = std::find_if(file.functions.begin(), file.functions.end(),
                [&](const FunctionSymbol& previous) {
                    return previous.usr == entry.usr &&
                        previous.signature == entry.signature;
                });
            if (existing == file.functions.end()) file.functions.push_back(std::move(entry));
            else if ((entry.has_body && !existing->has_body) ||
                     (entry.has_body == existing->has_body && entry.line > existing->line))
                *existing = std::move(entry);
        }
    }
    if (kind == CXCursor_CallExpr) {
        CallSite call;
        call.caller_usr = clang_Cursor_isNull(context.caller) ? "" :
            cursor_identity(clang_getCanonicalCursor(context.caller), file);
        call.caller_name = clang_Cursor_isNull(context.caller) ?
            "<initializer>" : callable_name(context.caller, file);
        const auto target = clang_getCursorReferenced(cursor);
        if (!clang_Cursor_isNull(target) &&
            callable_declaration(clang_getCursorKind(target))) {
            call.callee_usr = cursor_identity(clang_getCanonicalCursor(target), file);
            call.callee_name = callable_name(target, file);
            call.callee_file = project_declaration_file(
                target, *context.root, *context.declaration_files);
        } else {
            call.indirect = !clang_Cursor_isNull(target);
            call.callee_name = clang_Cursor_isNull(target)
                ? cx_text(clang_getCursorSpelling(cursor))
                : callable_name(target, file);
            if (call.callee_name.empty()) call.callee_name = "<unresolved call>";
        }
        call.line = location_line(cursor);
        call.column = location_column(cursor);
        call.dynamic_dispatch = clang_Cursor_isDynamicCall(cursor) != 0;
        file.calls.push_back(std::move(call));
    }
    if (kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl ||
        kind == CXCursor_ClassTemplate ||
        kind == CXCursor_ClassTemplatePartialSpecialization) {
        const auto name = cx_text(clang_getCursorSpelling(cursor));
        if (!name.empty()) {
            const auto line = location_line(cursor);
            const auto qualified = qualified_name(cursor, file);
            const auto duplicate = std::find_if(file.symbols.begin(), file.symbols.end(),
                [&](const Symbol& symbol) {
                    return symbol.qualified_name == qualified && symbol.line == line;
                });
            if (duplicate == file.symbols.end()) {
                Symbol symbol;
                symbol.kind = kind == CXCursor_StructDecl ? "struct" : "class";
                symbol.name = name;
                symbol.line = line;
                symbol.qualified_name = qualified;
                symbol.usr = cursor_identity(cursor, file);
                file.symbols.push_back(std::move(symbol));
            }
        }
    } else if (kind == CXCursor_CXXBaseSpecifier) {
        const auto parent_qualified = qualified_name(parent, file);
        const auto parent_line = location_line(parent);
        const auto base = cx_text(clang_getTypeSpelling(clang_getCursorType(cursor)));
        const auto declaration = clang_getTypeDeclaration(clang_getCursorType(cursor));
        const auto base_qualified = clang_Cursor_isNull(declaration)
            ? base : qualified_name(declaration, file);
        const auto base_usr = clang_Cursor_isNull(declaration)
            ? "qualified:" + base_qualified : cursor_identity(declaration, file);
        for (auto it = file.symbols.rbegin(); it != file.symbols.rend(); ++it) {
            if (it->qualified_name == parent_qualified && it->line == parent_line) {
                if (!it->base.empty()) it->base += ", ";
                it->base += base;
                it->base_qualified_names.push_back(base_qualified);
                it->base_usrs.push_back(base_usr);
                break;
            }
        }
    } else if (auto member = class_member(cursor, parent, file)) {
        const auto parent_qualified = qualified_name(parent, file);
        const auto parent_line = location_line(parent);
        for (auto it = file.symbols.rbegin(); it != file.symbols.rend(); ++it) {
            if (it->qualified_name == parent_qualified && it->line == parent_line) {
                it->members.push_back(std::move(*member));
                break;
            }
        }
    }
    VisitContext child = context;
    if (function && clang_isCursorDefinition(cursor)) child.caller = cursor;
    clang_visitChildren(cursor, visit_cursor, &child);
    return CXChildVisit_Continue;
}

void collect_diagnostics(CXTranslationUnit unit, SourceFile& file) {
    const unsigned count = clang_getNumDiagnostics(unit);
    unsigned omitted = 0;
    for (unsigned i = 0; i < count; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnostic(unit, i);
        const auto severity = clang_getDiagnosticSeverity(diagnostic);
        if (severity >= CXDiagnostic_Error) ++file.error_diagnostic_count;
        else if (severity == CXDiagnostic_Warning) ++file.warning_diagnostic_count;
        if (severity >= CXDiagnostic_Warning) {
            if (file.diagnostics.size() < 10) {
                file.diagnostics.push_back(cx_text(clang_formatDiagnostic(
                    diagnostic, clang_defaultDiagnosticDisplayOptions())));
            } else ++omitted;
        }
        clang_disposeDiagnostic(diagnostic);
    }
    if (omitted) {
        file.diagnostics.push_back("Additional diagnostics omitted: " +
                                   std::to_string(omitted));
    }
}

void parse_ast(const fs::path& path, const fs::path& root, CXIndex index,
               CXCompilationDatabase database, SourceFile& file) {
    std::vector<std::string> arguments;
    fs::path working_directory;
    const auto path_string = utf8_path(path);
    const auto extension = path.extension().string();
    const bool header = extension == ".h" || extension == ".hh" ||
                        extension == ".hpp" || extension == ".hxx" ||
                        extension == ".ipp";
    arguments = extension == ".c"
                    ? std::vector<std::string>{"-x", "c", "-std=c17"}
                    : std::vector<std::string>{
                          "-x", header ? "c++-header" : "c++", "-std=c++23"};
    CommandsHandle commands;
    if (database) {
        commands.value = clang_CompilationDatabase_getCompileCommands(
            database, path_string.c_str());
    }
    if (commands.value && clang_CompileCommands_getSize(commands.value) > 0) {
        file.used_compile_command = true;
        const auto command = clang_CompileCommands_getCommand(commands.value, 0);
        working_directory = from_utf8(cx_text(clang_CompileCommand_getDirectory(command)));
        const auto count = clang_CompileCommand_getNumArgs(command);
        arguments.reserve(count + 3);
        for (unsigned i = 1; i < count; ++i) {
            const auto arg = cx_text(clang_CompileCommand_getArg(command, i));
            auto take_value = [&](std::string_view output_flag,
                                  std::string_view option, bool path_argument) {
                auto append = [&](std::string value) {
                    if (path_argument && !working_directory.empty()) {
                        auto path = from_utf8(value);
                        if (path.is_relative()) value = utf8_path(
                            (working_directory / path).lexically_normal());
                    }
                    arguments.emplace_back(output_flag);
                    arguments.push_back(std::move(value));
                };
                if (arg == option && i + 1 < count) {
                    append(cx_text(clang_CompileCommand_getArg(command, ++i)));
                    return true;
                }
                if (arg.starts_with(option) && arg.size() > option.size()) {
                    append(std::string(arg.substr(option.size())));
                    return true;
                }
                return false;
            };
            if (take_value("-I", "/I", true) || take_value("-I", "-I", true) ||
                take_value("-D", "/D", false) || take_value("-D", "-D", false) ||
                take_value("-U", "/U", false) || take_value("-U", "-U", false) ||
                take_value("-include", "/FI", true) ||
                take_value("-isystem", "-isystem", true)) {
                continue;
            }
            if (arg.starts_with("-std=") || arg.starts_with("/std:")) {
                if (arg == "/std:c++latest" || arg == "/std:c++23preview") {
                    arguments.emplace_back("-std=c++23");
                } else if (arg.starts_with("/std:")) {
                    arguments.emplace_back("-std=" + arg.substr(5));
                } else {
                    arguments.push_back(arg);
                }
            }
        }
    }
    std::vector<const char*> raw_arguments;
    raw_arguments.reserve(arguments.size());
    for (const auto& argument : arguments) raw_arguments.push_back(argument.c_str());

    TranslationUnitHandle unit;
    const auto error = clang_parseTranslationUnit2(
        index, path_string.c_str(), raw_arguments.data(),
        static_cast<int>(raw_arguments.size()), nullptr, 0,
        CXTranslationUnit_DetailedPreprocessingRecord,
        &unit.value);
    if (error != CXError_Success || !unit.value) {
        file.diagnostics.push_back("libclang could not parse this file (error " +
                                   std::to_string(static_cast<int>(error)) + ")");
        return;
    }
    file.parsed = true;
    DeclarationFileCache declaration_files;
    VisitContext context{&file, &root, clang_getNullCursor(), &declaration_files};
    clang_visitChildren(clang_getTranslationUnitCursor(unit.value), visit_cursor,
                        &context);
    collect_diagnostics(unit.value, file);
    std::sort(file.includes.begin(), file.includes.end());
    file.includes.erase(std::unique(file.includes.begin(), file.includes.end()),
                        file.includes.end());
    std::ranges::sort(file.functions, {}, &FunctionSymbol::line);
    const auto call_key = [](const CallSite& call) {
        return std::tie(call.line, call.column, call.caller_usr,
                        call.callee_usr, call.callee_name);
    };
    std::ranges::sort(file.calls, [&](const CallSite& a, const CallSite& b) {
        return call_key(a) < call_key(b);
    });
    file.calls.erase(std::unique(file.calls.begin(), file.calls.end(),
        [&](const CallSite& a, const CallSite& b) {
            return call_key(a) == call_key(b);
        }), file.calls.end());
}

std::expected<SourceFile, std::string> read_source(
    const fs::path& path, const fs::path& root, std::uintmax_t size,
    CXIndex index, CXCompilationDatabase database) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected("cannot open source: " + path.string());
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return std::unexpected("cannot read source: " + path.string());
    }
    SourceFile file;
    file.relative_path = path.lexically_relative(root);
    file.content_hash = fnv1a64(bytes);
    parse_ast(path, root, index, database, file);
    return file;
}

} // namespace

std::expected<ProjectScan, std::string> scan_project(
    const fs::path& root_path,
    const std::optional<fs::path>& compilation_database_directory) {
    std::error_code ec;
    const auto root = fs::weakly_canonical(root_path, ec);
    if (ec || !fs::is_directory(root)) {
        return std::unexpected("project root is not a readable directory: " +
                               root_path.string());
    }
    IndexHandle index;
    if (!index.value) {
        return std::unexpected("libclang could not create an index");
    }
    DatabaseHandle database;
    if (compilation_database_directory) {
        CXCompilationDatabase_Error database_error = CXCompilationDatabase_NoError;
        const auto path = utf8_path(*compilation_database_directory);
        database.value = clang_CompilationDatabase_fromDirectory(
            path.c_str(), &database_error);
        if (!database.value || database_error != CXCompilationDatabase_NoError) {
            return std::unexpected("cannot load compile_commands.json from " +
                                   compilation_database_directory->string());
        }
    }
    ProjectScan result;
    result.root = root;
    result.compilation_database_directory = compilation_database_directory;
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return std::unexpected("cannot list project root: " + ec.message());
    }
    const fs::recursive_directory_iterator end;
    while (it != end) {
        const auto entry = *it;
        const auto path = entry.path();
        if (entry.is_directory(ec)) {
            if (ignored_directory(path) || entry.is_symlink(ec)) {
                it.disable_recursion_pending();
            }
        } else if (!entry.is_symlink(ec) && entry.is_regular_file(ec) && is_source(path)) {
            const auto size = entry.file_size(ec);
            if (ec) {
                return std::unexpected("cannot stat source: " + path.string());
            }
            if (size > max_source_bytes) {
                result.skipped_large_files.push_back(path.lexically_relative(root));
            } else {
                auto file = read_source(path, root, size, index.value, database.value);
                if (!file) {
                    return std::unexpected(file.error());
                }
                result.files.push_back(std::move(*file));
            }
        }
        ec.clear();
        it.increment(ec);
        if (ec) {
            return std::unexpected("cannot continue project scan: " + ec.message());
        }
    }
    std::sort(result.files.begin(), result.files.end(),
              [](const SourceFile& a, const SourceFile& b) {
                  return a.relative_path.generic_string() < b.relative_path.generic_string();
              });
    return result;
}

} // namespace atlas
