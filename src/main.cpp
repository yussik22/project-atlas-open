#include "atlas/atlas.hpp"
#include "atlas/trace.hpp"
#include "atlas/runtime.hpp"
#include "atlas/capture.hpp"
#include "atlas/ui_control.hpp"
#include "atlas/interactions.hpp"
#include "atlas/context.hpp"
#include "atlas/agent_tools.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <iostream>
#include <limits>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

void usage() {
    std::cerr << "Usage:\n"
              << "  project_atlas scan <project-root> <output-dir> [compile-db-dir]\n"
              << "  project_atlas scan-in-place <project-root> <output-dir> [compile-db-dir]\n"
              << "  project_atlas compare <expected.facts> <actual.facts> <report.md>\n"
              << "  project_atlas agent map <project-root> [compile-db-dir]\n"
              << "  project_atlas agent find <project-root> <query> [compile-db-dir]\n"
              << "  project_atlas agent context <project-root> --task <text> [--budget <400..4000>] [--compile-db <dir>]\n"
              << "  project_atlas agent source <project-root> <relative-file> <start-line> <count> [expected-hash]\n"
              << "  project_atlas agent file-relations <project-root> <relative-file> [compile-db-dir]\n"
              << "  project_atlas agent interactions <project-root> [--compile-db <dir>] [--trace <file>] [--step <n>]\n"
              << "  project_atlas agent compare <expected.facts> <actual.facts>\n"
              << "  project_atlas agent trace <expected.trace> <actual.trace>\n"
              << "  project_atlas agent runtime <runtime.trace> [step]\n"
              << "  project_atlas agent ui list|status|show [pid]\n"
              << "  project_atlas agent ui uml-overview|uml-focus [pid]\n"
              << "  project_atlas agent ui load-trace|scan|record <path> [pid]\n"
              << "  project_atlas agent ui step|tab <number> [pid] (tab 0..6)\n"
              << "  project_atlas agent ui focus-file <path> [pid]\n"
              << "  project_atlas agent ui focus-symbol <USR-or-qualified-name> [pid]\n"
              << "  project_atlas agent ui compare-facts <expected.facts> <actual.facts> [pid]\n"
              << "  project_atlas agent ui generate-cards [pid]\n"
              << "  project_atlas agent ui debug-launch <executable.exe> [pid]\n"
              << "  project_atlas agent ui debug-break <source-file> <line> [pid]\n"
              << "  project_atlas agent ui debug-continue|debug-step|debug-stop [pid]\n"
              << "  project_atlas capture <instrumented-exe> <new-output.trace>\n";
}

std::optional<std::uint64_t> unsigned_number(std::wstring_view value) {
    if (value.empty()) return std::nullopt;
    std::uint64_t result = 0;
    for (wchar_t c : value) {
        if (c < L'0' || c > L'9' ||
            result > (std::numeric_limits<std::uint64_t>::max() -
                      static_cast<std::uint64_t>(c - L'0')) / 10)
            return std::nullopt;
        result = result * 10 + static_cast<std::uint64_t>(c - L'0');
    }
    return result;
}

int agent_ui(int argc) {
#ifdef _WIN32
    int wide_count = 0;
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    if (!raw || wide_count != argc) {
        if (raw) LocalFree(raw);
        std::cout << atlas::agent_error_json("cannot parse Unicode UI arguments") << '\n';
        return 1;
    }
    std::vector<std::wstring> args;
    for (int i = 0; i < argc; ++i) args.emplace_back(raw[i]);
    LocalFree(raw);
    const std::wstring_view action = args[3];
    if (action == L"list" && argc == 4) {
        std::cout << atlas::ui_instances_json(atlas::running_ui_instances()) << '\n';
        return 0;
    }
    atlas::UiAction operation = atlas::UiAction::status;
    int argument_count = 0;
    bool numeric_argument = false;
    bool text_argument = false;
    bool second_numeric = false;
    if (action == L"status") operation = atlas::UiAction::status;
    else if (action == L"show") operation = atlas::UiAction::show_window;
    else if (action == L"uml-overview") operation = atlas::UiAction::uml_overview;
    else if (action == L"uml-focus") operation = atlas::UiAction::uml_focus;
    else if (action == L"load-trace") {
        operation = atlas::UiAction::load_trace; argument_count = 1;
    } else if (action == L"scan") {
        operation = atlas::UiAction::scan_project; argument_count = 1;
    } else if (action == L"record") {
        operation = atlas::UiAction::record_run; argument_count = 1;
    } else if (action == L"step") {
        operation = atlas::UiAction::set_step; argument_count = 1; numeric_argument = true;
    } else if (action == L"tab") {
        operation = atlas::UiAction::select_tab; argument_count = 1; numeric_argument = true;
    } else if (action == L"focus-file") {
        operation = atlas::UiAction::focus_file; argument_count = 1;
    } else if (action == L"focus-symbol") {
        operation = atlas::UiAction::focus_symbol; argument_count = 1; text_argument = true;
    } else if (action == L"compare-facts") {
        operation = atlas::UiAction::compare_facts; argument_count = 2;
    } else if (action == L"generate-cards") {
        operation = atlas::UiAction::generate_cards;
    } else if (action == L"debug-launch") {
        operation = atlas::UiAction::debug_launch; argument_count = 1;
    } else if (action == L"debug-break") {
        operation = atlas::UiAction::debug_breakpoint;
        argument_count = 2; second_numeric = true;
    } else if (action == L"debug-continue") {
        operation = atlas::UiAction::debug_continue;
    } else if (action == L"debug-step") {
        operation = atlas::UiAction::debug_step;
    } else if (action == L"debug-stop") {
        operation = atlas::UiAction::debug_stop;
    } else {
        std::cout << atlas::agent_error_json("unknown UI command") << '\n';
        return 1;
    }
    const int first_extra = 4 + argument_count;
    if (argc != first_extra && argc != first_extra + 1) {
        std::cout << atlas::agent_error_json("invalid UI command arguments") << '\n';
        return 1;
    }
    std::optional<std::uint32_t> pid;
    if (argc == first_extra + 1) {
        const auto parsed = unsigned_number(args[first_extra]);
        if (!parsed || *parsed == 0 || *parsed > UINT32_MAX) {
            std::cout << atlas::agent_error_json("invalid Atlas GUI PID") << '\n';
            return 1;
        }
        pid = static_cast<std::uint32_t>(*parsed);
    }
    std::uint64_t value = 0;
    std::wstring_view path;
    std::wstring absolute_path;
    std::wstring second_path;
    if (argument_count) {
        if (numeric_argument) {
            const auto parsed = unsigned_number(args[4]);
            if (!parsed) {
                std::cout << atlas::agent_error_json("invalid UI number") << '\n';
                return 1;
            }
            value = *parsed;
        } else if (text_argument) {
            path = args[4];
        } else {
            absolute_path = std::filesystem::absolute(std::filesystem::path(args[4])).wstring();
            path = absolute_path;
        }
    }
    if (argument_count == 2) {
        if (second_numeric) {
            const auto parsed = unsigned_number(args[5]);
            if (!parsed || *parsed == 0 || *parsed > UINT32_MAX) {
                std::cout << atlas::agent_error_json("invalid source line") << '\n';
                return 1;
            }
            value = *parsed;
        } else second_path = std::filesystem::absolute(std::filesystem::path(args[5])).wstring();
    }
    auto response = atlas::send_ui_command(operation, path, value, second_path, pid);
    if (!response) {
        std::cout << atlas::agent_error_json(response.error()) << '\n';
        return 1;
    }
    std::cout << atlas::ui_reply_json(*response) << '\n';
    return response->ok ? 0 : 2;
#else
    (void)argc;
    std::cout << atlas::agent_error_json("GUI control currently requires Windows") << '\n';
    return 1;
#endif
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string_view(argv[1]) == "capture") {
            auto result = atlas::capture_runtime(argv[2], argv[3]);
            if (!result) {
                std::cerr << "capture: " << result.error() << '\n';
                return 1;
            }
            std::cout << "Captured " << result->trace_path << " (target exit "
                      << result->exit_code << (result->truncated ? ", truncated" : "")
                      << ")\n";
            return result->exit_code == 0 ? 0 : 2;
        }
        if (argc >= 2 && std::string_view(argv[1]) == "agent") {
            const auto command = argc >= 3 ? std::string_view(argv[2]) : std::string_view{};
            if (command == "ui" && argc >= 4) return agent_ui(argc);
            if (command == "source" && (argc == 7 || argc == 8)) {
                std::size_t start = 0, count = 0;
                const std::string_view start_text(argv[5]), count_text(argv[6]);
                const auto [start_end, start_error] = std::from_chars(
                    start_text.data(), start_text.data() + start_text.size(), start);
                const auto [count_end, count_error] = std::from_chars(
                    count_text.data(), count_text.data() + count_text.size(), count);
                if (start_error != std::errc{} || start_end != start_text.data() + start_text.size() ||
                    count_error != std::errc{} || count_end != count_text.data() + count_text.size()) {
                    std::cout << atlas::agent_error_json("invalid source line range") << '\n';
                    return 1;
                }
                const auto result = atlas::agent_source_json(argv[3], argv[4], start, count,
                    argc == 8 ? std::optional<std::string_view>(argv[7]) : std::nullopt);
                if (!result) {
                    std::cout << atlas::agent_error_json(result.error()) << '\n';
                    return 1;
                }
                std::cout << *result << '\n';
                return 0;
            }
            if (command == "file-relations" && (argc == 5 || argc == 6)) {
                auto scan = atlas::scan_project(argv[3], argc == 6
                    ? std::optional<atlas::fs::path>(argv[5]) : std::nullopt);
                if (!scan) {
                    std::cout << atlas::agent_error_json(scan.error()) << '\n';
                    return 1;
                }
                const auto result = atlas::agent_file_relations_json(*scan, argv[4]);
                if (!result) {
                    std::cout << atlas::agent_error_json(result.error()) << '\n';
                    return 1;
                }
                std::cout << *result << '\n';
                const auto unhealthy = std::any_of(scan->files.begin(), scan->files.end(),
                    [](const atlas::SourceFile& file) {
                        return !file.parsed || file.error_diagnostic_count != 0;
                    });
                return unhealthy ? 2 : 0;
            }
            if ((command == "map" && (argc == 4 || argc == 5)) ||
                (command == "find" && (argc == 5 || argc == 6))) {
                const bool find = command == "find";
                const int database_argument = find ? 5 : 4;
                auto scan = atlas::scan_project(
                    argv[3], argc > database_argument
                        ? std::optional<atlas::fs::path>(argv[database_argument])
                        : std::nullopt);
                if (!scan) {
                    std::cout << atlas::agent_error_json(scan.error()) << '\n';
                    return 1;
                }
                std::cout << atlas::agent_scan_json(
                    *scan, find ? std::optional<std::string_view>(argv[4])
                                : std::nullopt) << '\n';
                const auto unhealthy = std::any_of(scan->files.begin(), scan->files.end(),
                    [](const atlas::SourceFile& file) {
                        return !file.parsed || file.error_diagnostic_count != 0;
                    });
                return unhealthy ? 2 : 0;
            }
            if (command == "context" && argc >= 6) {
                std::optional<std::string_view> task;
                std::optional<atlas::fs::path> database;
                std::size_t budget = 1500;
                bool budget_seen = false;
                for (int i = 4; i < argc; i += 2) {
                    if (i + 1 >= argc) {
                        std::cout << atlas::agent_error_json("missing context option value") << '\n';
                        return 1;
                    }
                    const std::string_view option(argv[i]);
                    if (option == "--task" && !task) task = argv[i + 1];
                    else if (option == "--compile-db" && !database) database = argv[i + 1];
                    else if (option == "--budget" && !budget_seen) {
                        budget_seen = true;
                        const std::string_view argument(argv[i + 1]);
                        const auto [end, error] = std::from_chars(
                            argument.data(), argument.data() + argument.size(), budget);
                        if (error != std::errc{} ||
                            end != argument.data() + argument.size() ||
                            budget < 400 || budget > 4000) {
                            std::cout << atlas::agent_error_json("context budget must be 400..4000") << '\n';
                            return 1;
                        }
                    } else {
                        std::cout << atlas::agent_error_json("invalid context option") << '\n';
                        return 1;
                    }
                }
                if (!task || task->empty() || task->size() > 200) {
                    std::cout << atlas::agent_error_json("context task must be 1..200 bytes") << '\n';
                    return 1;
                }
                auto scan = atlas::scan_project(argv[3], database);
                if (!scan) {
                    std::cout << atlas::agent_error_json(scan.error()) << '\n';
                    return 1;
                }
                std::cout << atlas::agent_context_json(*scan, *task, budget) << '\n';
                const auto unhealthy = std::any_of(scan->files.begin(), scan->files.end(),
                    [](const atlas::SourceFile& file) {
                        return !file.parsed || file.error_diagnostic_count != 0;
                    });
                return unhealthy ? 2 : 0;
            }
            if (command == "interactions" && argc >= 4) {
                std::optional<atlas::fs::path> database, trace_path;
                std::optional<std::size_t> step;
                for (int i = 4; i < argc; i += 2) {
                    if (i + 1 >= argc) {
                        std::cout << atlas::agent_error_json("missing interactions option value") << '\n';
                        return 1;
                    }
                    const std::string_view option(argv[i]);
                    if (option == "--compile-db" && !database) database = argv[i + 1];
                    else if (option == "--trace" && !trace_path) trace_path = argv[i + 1];
                    else if (option == "--step" && !step) {
                        std::size_t parsed = 0;
                        const std::string_view argument(argv[i + 1]);
                        const auto [end, error] = std::from_chars(
                            argument.data(), argument.data() + argument.size(), parsed);
                        if (error != std::errc{} || end != argument.data() + argument.size()) {
                            std::cout << atlas::agent_error_json("invalid interaction step") << '\n';
                            return 1;
                        }
                        step = parsed;
                    } else {
                        std::cout << atlas::agent_error_json("invalid interactions option") << '\n';
                        return 1;
                    }
                }
                if (step && !trace_path) {
                    std::cout << atlas::agent_error_json("--step requires --trace") << '\n';
                    return 1;
                }
                auto scan = atlas::scan_project(argv[3], database);
                if (!scan) {
                    std::cout << atlas::agent_error_json(scan.error()) << '\n';
                    return 1;
                }
                std::optional<atlas::RuntimeSnapshot> snapshot;
                if (trace_path) {
                    auto trace = atlas::read_trace(*trace_path);
                    if (!trace) {
                        std::cout << atlas::agent_error_json(trace.error()) << '\n';
                        return 1;
                    }
                    auto replayed = atlas::replay_runtime(*trace, step.value_or(trace->size()));
                    if (!replayed) {
                        std::cout << atlas::agent_error_json(replayed.error()) << '\n';
                        return 1;
                    }
                    snapshot = std::move(*replayed);
                }
                std::cout << atlas::agent_interactions_json(
                    atlas::build_interaction_graph(*scan, snapshot)) << '\n';
                const auto unhealthy = std::any_of(scan->files.begin(), scan->files.end(),
                    [](const atlas::SourceFile& file) {
                        return !file.parsed || file.error_diagnostic_count != 0;
                    });
                return unhealthy ? 2 : 0;
            }
            if (command == "compare" && argc == 5) {
                auto expected = atlas::read_facts(argv[3]);
                if (!expected) {
                    std::cout << atlas::agent_error_json(expected.error()) << '\n';
                    return 1;
                }
                auto actual = atlas::read_facts(argv[4]);
                if (!actual) {
                    std::cout << atlas::agent_error_json(actual.error()) << '\n';
                    return 1;
                }
                const auto result = atlas::compare(*expected, *actual);
                std::cout << atlas::agent_comparison_json(result) << '\n';
                return result.all_match() ? 0 : 2;
            }
            if (command == "trace" && argc == 5) {
                auto expected = atlas::read_trace(argv[3]);
                if (!expected) {
                    std::cout << atlas::agent_error_json(expected.error()) << '\n';
                    return 1;
                }
                auto actual = atlas::read_trace(argv[4]);
                if (!actual) {
                    std::cout << atlas::agent_error_json(actual.error()) << '\n';
                    return 1;
                }
                const auto result = atlas::compare_trace(*expected, *actual);
                std::cout << atlas::agent_trace_json(result) << '\n';
                return result.all_match() ? 0 : 2;
            }
            if (command == "runtime" && (argc == 4 || argc == 5)) {
                auto trace = atlas::read_trace(argv[3]);
                if (!trace) {
                    std::cout << atlas::agent_error_json(trace.error()) << '\n';
                    return 1;
                }
                std::size_t step = trace->size();
                if (argc == 5) {
                    const std::string_view argument(argv[4]);
                    const auto [end, error] = std::from_chars(
                        argument.data(), argument.data() + argument.size(), step);
                    if (error != std::errc{} || end != argument.data() + argument.size()) {
                        std::cout << atlas::agent_error_json("invalid runtime step") << '\n';
                        return 1;
                    }
                }
                auto snapshot = atlas::replay_runtime(*trace, step);
                if (!snapshot) {
                    std::cout << atlas::agent_error_json(snapshot.error()) << '\n';
                    return 1;
                }
                std::cout << atlas::agent_runtime_json(*snapshot) << '\n';
                return 0;
            }
            std::cout << atlas::agent_error_json("invalid agent command or arguments") << '\n';
            return 1;
        }
        if ((argc == 4 || argc == 5) &&
            (std::string_view(argv[1]) == "scan" ||
             std::string_view(argv[1]) == "scan-in-place")) {
            const bool in_place = std::string_view(argv[1]) == "scan-in-place";
            auto scan = atlas::scan_project(
                argv[2], argc == 5 ? std::optional<atlas::fs::path>(argv[4])
                                    : std::nullopt);
            if (!scan) {
                std::cerr << "scan: " << scan.error() << '\n';
                return 1;
            }
            if (auto report = atlas::write_project_report(*scan, argv[3], in_place);
                !report) {
                std::cerr << "scan: " << report.error() << '\n';
                return 1;
            }
            const auto failures = std::count_if(
                scan->files.begin(), scan->files.end(),
                [](const atlas::SourceFile& file) { return !file.parsed; });
            std::size_t errors = 0, warnings = 0;
            for (const auto& file : scan->files) {
                errors += file.error_diagnostic_count;
                warnings += file.warning_diagnostic_count;
            }
            std::cout << "Mapped " << scan->files.size() << " source files ("
                      << failures << " AST parse failures, " << errors << " Clang errors, "
                      << warnings << " warnings) into " << argv[3] << '\n';
            return failures == 0 && errors == 0 ? 0 : 2;
        }
        if (argc == 5 && std::string_view(argv[1]) == "compare") {
            auto expected = atlas::read_facts(argv[2]);
            if (!expected) {
                std::cerr << "compare: " << expected.error() << '\n';
                return 1;
            }
            auto actual = atlas::read_facts(argv[3]);
            if (!actual) {
                std::cerr << "compare: " << actual.error() << '\n';
                return 1;
            }
            const auto comparison = atlas::compare(*expected, *actual);
            if (auto report = atlas::write_comparison_report(comparison, argv[4]); !report) {
                std::cerr << "compare: " << report.error() << '\n';
                return 1;
            }
            std::cout << "Compared " << comparison.entries.size() << " facts; "
                      << (comparison.all_match() ? "all match" : "differences found") << '\n';
            return comparison.all_match() ? 0 : 2;
        }
        usage();
        return 1;
    } catch (const std::exception& error) {
        if (argc >= 2 && std::string_view(argv[1]) == "agent") {
            std::cout << atlas::agent_error_json(error.what()) << '\n';
        } else {
            std::cerr << "project_atlas: " << error.what() << '\n';
        }
        return 1;
    }
}
