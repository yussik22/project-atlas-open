#include "atlas/atlas.hpp"
#include "atlas/observation.hpp"
#include "atlas/symbol_identity.hpp"
#include "atlas/trace.hpp"
#include "atlas/runtime.hpp"
#include "atlas/capture.hpp"
#include "atlas/debugger.hpp"
#include "atlas/uml.hpp"
#include "atlas/interactions.hpp"
#include "atlas/interaction_presentation.hpp"
#include "atlas/context.hpp"
#include "atlas/agent_tools.hpp"
#include "atlas/ui_control.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    check(static_cast<bool>(output), "cannot create test fixture");
    output << text;
}

void scan_and_report_test(const std::filesystem::path& workspace) {
    check(atlas::mermaid_symbol_id("usr:alpha") ==
              atlas::mermaid_symbol_id("usr:alpha") &&
              atlas::mermaid_symbol_id("usr:alpha") !=
              atlas::mermaid_symbol_id("usr:beta"),
          "presentation hash is not deterministic for fixture identities");
    const auto root = workspace / "source";
    write_text(root / "src" / "memory.hpp",
               "#include <vector>\n"
               "// class Fake {};\n"
               "class Resource {};\n"
               "class Buffer : public Resource {\n"
               "public:\n"
               "  const char* text = \"struct AlsoFake\";\n"
               "  int size() const { return count; }\n"
               "private:\n"
               "  int count = 7;\n"
               "  Resource* owner = nullptr;\n"
               "};\n"
               "int helper(int value) { return value + 1; }\n"
               "int use(Buffer& value) { return helper(value.size()); }\n");
    write_text(root / "build" / "generated.cpp", "class ShouldBeIgnored {};\n");
    write_text(root / "dist" / "release.cpp", "class ShouldAlsoBeIgnored {};\n");

    const auto scan = atlas::scan_project(root);
    check(scan.has_value(), "scan failed");
    check(scan->files.size() == 1, "ignored build directory was scanned");
    check(scan->files[0].parsed, "libclang did not parse fixture");
    check(scan->files[0].symbols.size() == 2, "declaration outline is incorrect");
    check(scan->files[0].symbols[1].name == "Buffer", "wrong class name");
    check(scan->files[0].symbols[1].qualified_name == "Buffer",
          "wrong qualified class name");
    check(!scan->files[0].symbols[1].usr.empty(), "missing Clang symbol identity");
    check(scan->files[0].symbols[1].base == "Resource", "wrong base class");
    check(scan->files[0].symbols[1].base_qualified_names.size() == 1 &&
              scan->files[0].symbols[1].base_qualified_names[0] == "Resource",
          "qualified base class missing");
    check(scan->files[0].symbols[1].line == 4, "wrong declaration line");
    const auto& members = scan->files[0].symbols[1].members;
    check(members.size() == 4 &&
              members[0].kind == "field" && members[0].name == "text" &&
              members[0].access == "public" &&
              members[1].kind == "method" && members[1].name == "size" &&
              members[1].access == "public" &&
              members[2].kind == "field" && members[2].name == "count" &&
              members[2].access == "private" &&
              members[3].name == "owner" &&
              members[3].related_usr == scan->files[0].symbols[0].usr,
          "class fields, methods, or access were not captured for UML");
    const auto uml = atlas::build_uml_graph(*scan);
    check(uml.nodes.size() == 2 && uml.edges.size() == 2 &&
              std::ranges::any_of(uml.edges, [](const atlas::UmlEdge& edge) {
                  return edge.relation == atlas::UmlRelation::inheritance &&
                      edge.from == 0 && edge.to == 1;
              }) &&
              std::ranges::any_of(uml.edges, [](const atlas::UmlEdge& edge) {
                  return edge.relation == atlas::UmlRelation::field_reference &&
                      edge.from == 1 && edge.to == 0;
              }),
          "project-wide UML graph lost inheritance or field reference");
    const auto layout = atlas::layout_uml_graph(uml, false);
    check(layout.nodes.size() == 2 && layout.edges.size() == 2 &&
              layout.connected_count == 2 && layout.omitted_count == 0 &&
              layout.width > 0 && layout.height > 0,
          "UML graph did not place linked classes and edges");
    atlas::UmlGraph many;
    many.nodes.resize(47);
    for (std::size_t i = 0; i < 7; ++i) many.edges.push_back({i, i + 1});
    const auto connected = atlas::layout_uml_graph(many, false);
    const auto all = atlas::layout_uml_graph(many, true);
    const auto picked = atlas::layout_uml_graph(many, false, 46);
    check(connected.nodes.size() == 8 && connected.edges.size() == 7 &&
              connected.omitted_count == 39 && all.nodes.size() == 47 &&
              picked.nodes.size() == 9 && picked.omitted_count == 38,
          "UML graph visibility lost linked or isolated classes");
    atlas::UmlGraph pairs;
    pairs.nodes.resize(10);
    for (std::size_t i = 0; i < 10; i += 2) pairs.edges.push_back({i, i + 1});
    const auto packed = atlas::layout_uml_graph(pairs, false);
    check(packed.nodes.size() == 10 && packed.width <= 1050 &&
              packed.height <= 550,
          "UML disconnected components were not packed into readable rows");
    atlas::UmlGraph chain;
    chain.nodes.resize(12);
    for (std::size_t i = 1; i < chain.nodes.size(); ++i)
        chain.edges.push_back({i - 1, i});
    const auto long_layout = atlas::layout_uml_graph(chain, false);
    check(long_layout.nodes.size() == chain.nodes.size() &&
              long_layout.edges.size() == chain.edges.size(),
          "long connected UML component lost nodes or links");
    for (std::size_t i = 1; i < long_layout.nodes.size(); ++i) {
        check(long_layout.nodes[i].rect.left > long_layout.nodes[i - 1].rect.left &&
                  long_layout.nodes[i].rect.top == long_layout.nodes[i - 1].rect.top,
              "connected UML layers wrapped into a vertical spaghetti path");
    }
    for (const auto& a : all.nodes) {
        check(a.rect.left >= 0 && a.rect.top >= 0 &&
                  a.rect.right <= all.width && a.rect.bottom <= all.height,
              "UML class escaped virtual canvas");
        for (const auto& b : all.nodes) {
            if (a.node == b.node) continue;
            check(a.rect.right <= b.rect.left || b.rect.right <= a.rect.left ||
                      a.rect.bottom <= b.rect.top || b.rect.bottom <= a.rect.top,
                  "UML class cards overlap");
        }
    }
    check(scan->files[0].includes.size() == 1 &&
              scan->files[0].includes[0] == "vector", "include not found");
    check(std::ranges::any_of(scan->files[0].functions,
              [](const atlas::FunctionSymbol& function) {
                  return function.qualified_name == "helper" &&
                      function.line == 12 && function.has_body &&
                      !function.usr.empty();
              }), "free function missing from AST navigation outline");
    const auto& calls = scan->files[0].calls;
    check(calls.size() == 2 &&
              std::ranges::any_of(calls, [](const atlas::CallSite& call) {
                  return call.caller_name == "use" && call.callee_name == "helper" &&
                      call.line == 13 && !call.callee_usr.empty() &&
                      call.callee_file == "src/memory.hpp";
              }) &&
              std::ranges::any_of(calls, [](const atlas::CallSite& call) {
                  return call.callee_name == "Buffer::size";
              }), "body scan missed direct function or method calls");
    const auto agent_map = atlas::agent_scan_json(*scan);
    check(agent_map.find("\"schema\":\"project-atlas.agent.v1\"") != std::string::npos &&
              agent_map.find("\"qualified_name\":\"Buffer\"") != std::string::npos &&
              agent_map.find("\"content_hash_fnv1a64\":\"") != std::string::npos,
          "agent map missing structured navigation fields");
    const auto agent_function = atlas::agent_scan_json(*scan, "helper");
    check(agent_function.find("\"matched_functions\":1") != std::string::npos &&
              agent_function.find("\"matched_calls\":1") != std::string::npos &&
              agent_function.find("\"qualified_name\":\"helper\"") !=
                  std::string::npos,
          "agent function or call-site search failed");

    const auto reports = workspace / "reports";
    const auto written = atlas::write_project_report(*scan, reports);
    check(written.has_value(), "report generation failed");
    check(std::filesystem::exists(reports / "index.html"), "navigator missing");
    check(std::filesystem::exists(reports / "interactions.md") &&
              read_text(reports / "interactions.md").find("static_call") !=
                  std::string::npos,
          "interaction evidence report missing");
    check(std::filesystem::exists(reports / "src" / "memory.hpp.agent.md"),
          "source card missing");
    const auto card = read_text(reports / "src" / "memory.hpp.agent.md");
    check(card.find("## Functions and methods") != std::string::npos &&
              card.find("helper") != std::string::npos &&
              card.find("## Static calls to project code") != std::string::npos &&
              card.find("| use | helper | 13 | 1 | src/memory.hpp |") !=
                  std::string::npos,
          "source card omitted function or call navigation");
    const auto summary = read_text(reports / "summaries" / "src" / "memory.hpp.md");
    check(summary.size() < card.size() &&
              summary.find("Full AST card") != std::string::npos &&
              summary.find("Source hash:") != std::string::npos &&
              read_text(reports / "index.md").find("summaries/src/memory.hpp.md") !=
                  std::string::npos,
          "quick context summary missing, stale, or not linked from index");
    const auto classes = read_text(reports / "classes.md");
    check(classes.find("[\"Resource\"]") != std::string::npos &&
              classes.find("[\"Buffer\"]") != std::string::npos &&
              classes.find(" <|-- ") != std::string::npos,
          "UML inheritance missing");

    const auto source_before = read_text(root / "src" / "memory.hpp");
    check(atlas::write_project_report(*scan, workspace / "in-place-reports", true)
              .has_value(), "in-place card generation failed");
    check(std::filesystem::exists(root / "src" / "memory.hpp.agent.md"),
          "adjacent card missing");
    check(read_text(root / "src" / "memory.hpp") == source_before,
          "source was modified by card generation");

    const auto unowned = workspace / "unowned";
    write_text(unowned / "keep.txt", "not generated\n");
    check(!atlas::write_project_report(*scan, unowned).has_value(),
          "unowned output directory was overwritten");
    check(read_text(unowned / "keep.txt") == "not generated\n",
          "unowned file was modified");
    std::filesystem::remove(reports / "index.md");
    std::filesystem::create_hard_link(unowned / "keep.txt", reports / "index.md");
    check(atlas::write_project_report(*scan, reports).has_value() &&
              read_text(unowned / "keep.txt") == "not generated\n",
          "report replacement modified an external hard-link target");
    std::filesystem::remove(reports / "index.md");
    std::error_code symlink_error;
    std::filesystem::create_symlink(unowned / "keep.txt", reports / "index.md",
                                    symlink_error);
    if (!symlink_error) {
        check(!atlas::write_project_report(*scan, reports).has_value() &&
                  read_text(unowned / "keep.txt") == "not generated\n",
              "report followed a linked output path");
        std::filesystem::remove(reports / "index.md");
    }
}

void orphan_card_archive_test(const std::filesystem::path& workspace) {
    const auto root = workspace / "orphan-cards";
    check(root.filename() == "orphan-cards" &&
              std::filesystem::weakly_canonical(root).parent_path() ==
                  std::filesystem::weakly_canonical(workspace),
          "test fixture path escaped its workspace");
    std::error_code reset_error;
    std::filesystem::remove_all(root, reset_error);
    check(!reset_error, "cannot reset orphan-card test fixture");
    const auto old_source = root / "src" / "old.cpp";
    const auto new_source = root / "src" / "new.cpp";
    const auto reports = root / "reports";
    write_text(old_source, "struct OldType {};\n");
    write_text(root / "src" / "manual.cpp.agent.md", "Human-owned notes\n");
    write_text(root / "build" / "removed.cpp.agent.md",
               "<!-- Project Atlas generated card -->\nBuild-owned fixture\n");
    auto scan = atlas::scan_project(root);
    check(scan && scan->files.size() == 1 &&
              atlas::write_project_report(*scan, reports, true).has_value(),
          "initial in-place report failed");
    const auto adjacent = root / "src" / "old.cpp.agent.md";
    const auto original = read_text(adjacent);
    check(!original.empty(), "initial adjacent card missing");

    std::filesystem::rename(old_source, new_source);
    scan = atlas::scan_project(root);
    check(scan && scan->files.size() == 1 &&
              atlas::write_project_report(*scan, reports, true).has_value(),
          "report after source rename failed");
    const auto archived = root / ".project-atlas" / "orphans" / "source" /
                          "src" / "old.cpp.agent.md";
    check(!std::filesystem::exists(adjacent) && read_text(archived) == original &&
              std::filesystem::exists(root / "src" / "new.cpp.agent.md"),
          "renamed source did not archive its old adjacent card");
    check(!std::filesystem::exists(reports / "src" / "old.cpp.agent.md") &&
              std::filesystem::exists(reports / ".orphans" / "src" /
                                      "old.cpp.agent.md") &&
              !std::filesystem::exists(reports / "summaries" / "src" / "old.cpp.md") &&
              std::filesystem::exists(reports / ".orphans" / "summaries" /
                                      "src" / "old.cpp.md"),
          "report retained stale generated card or summary");
    check(read_text(reports / "index.md").find("old.cpp") == std::string::npos &&
              read_text(reports / "index.md").find("new.cpp") != std::string::npos,
          "report index retained a deleted source");
    check(std::filesystem::exists(root / "src" / "manual.cpp.agent.md") &&
              std::filesystem::exists(root / "build" / "removed.cpp.agent.md"),
          "archive moved a user-owned or ignored-directory card");

    write_text(adjacent, original);
    check(atlas::write_project_report(*scan, reports, true).has_value() &&
              !std::filesystem::exists(adjacent) &&
              read_text(std::filesystem::path(archived.string() + ".1")) == original,
          "archive overwrote an earlier card instead of preserving both copies");

    const auto standalone_root = workspace / "report-only-orphans";
    const auto standalone_output = workspace / "report-only-output";
    check(standalone_root.filename() == "report-only-orphans" &&
              standalone_output.filename() == "report-only-output" &&
              std::filesystem::weakly_canonical(standalone_root).parent_path() ==
                  std::filesystem::weakly_canonical(workspace) &&
              std::filesystem::weakly_canonical(standalone_output).parent_path() ==
                  std::filesystem::weakly_canonical(workspace),
          "standalone test fixture escaped its workspace");
    std::filesystem::remove_all(standalone_root, reset_error);
    check(!reset_error, "cannot reset standalone source fixture");
    std::filesystem::remove_all(standalone_output, reset_error);
    check(!reset_error, "cannot reset standalone report fixture");
    write_text(standalone_root / "retired.cpp", "int retired() { return 1; }\n");
    auto standalone_scan = atlas::scan_project(standalone_root);
    check(standalone_scan &&
              atlas::write_project_report(*standalone_scan, standalone_output).has_value(),
          "standalone report generation failed");
    std::filesystem::rename(standalone_root / "retired.cpp",
                            standalone_root / "current.cpp");
    standalone_scan = atlas::scan_project(standalone_root);
    check(standalone_scan &&
              atlas::write_project_report(*standalone_scan, standalone_output).has_value(),
          "standalone report refresh failed");
    check(!std::filesystem::exists(standalone_root / ".project-atlas") &&
              std::filesystem::exists(standalone_output / ".orphans" /
                                      "retired.cpp.agent.md") &&
              std::filesystem::exists(standalone_output / ".orphans" /
                                      "summaries" / "retired.cpp.md"),
          "standalone scan changed source tree or failed to archive stale reports");
}

void interaction_evidence_test() {
    atlas::ProjectScan scan;
    scan.files.resize(2);
    scan.files[0].relative_path = "src/owner.cpp";
    scan.files[1].relative_path = "include/target.hpp";
    atlas::Symbol owner;
    owner.usr = "owner";
    owner.qualified_name = "Owner";
    owner.line = 3;
    owner.base_usrs = {"target"};
    atlas::SymbolMember field;
    field.kind = "field";
    field.name = "target";
    field.line = 5;
    field.related_usrs = {"target"};
    owner.members.push_back(field);
    scan.files[0].symbols.push_back(owner);
    atlas::Symbol target;
    target.usr = "target";
    target.qualified_name = "Target";
    scan.files[1].symbols.push_back(target);
    atlas::CallSite call;
    call.caller_usr = "owner.run";
    call.caller_name = "Owner::run";
    call.callee_usr = "target.run";
    call.callee_name = "Target::run";
    call.callee_file = "include/target.hpp";
    call.line = 12;
    call.column = 4;
    call.dynamic_dispatch = true;
    scan.files[0].calls.push_back(call);
    call.line = 15;
    scan.files[0].calls.push_back(call);
    call.callee_usr = "target.run.overload";
    call.line = 18;
    scan.files[0].calls.push_back(call);
    scan.files[0].calls.push_back(atlas::CallSite{});
    atlas::CallSite unmapped;
    unmapped.callee_usr = "library.function";
    scan.files[0].calls.push_back(unmapped);
    atlas::RuntimeSnapshot snapshot;
    snapshot.step = 3;
    snapshot.total_steps = 3;
    snapshot.capture_truncated = true;
    snapshot.nodes = {{"site.1", "exe+0x10", atlas::RuntimeNodeKind::site},
                      {"heap.1", "heap allocation", atlas::RuntimeNodeKind::object}};
    snapshot.links = {{"site.1", "heap.1", "allocates"}};
    const auto graph = atlas::build_interaction_graph(scan, snapshot);
    check(graph.nodes.size() == 4 && graph.edges.size() == 5 &&
              graph.runtime_step == 3 && graph.capture_truncated &&
              graph.unresolved_calls == 1 && graph.unmapped_calls == 1 &&
              graph.dynamic_call_sites == 3,
          "interaction graph lost evidence or unresolved-call counts");
    const auto edge_of = [&](atlas::InteractionKind kind) -> const atlas::InteractionEdge* {
        const auto found = std::ranges::find_if(graph.edges,
            [kind](const atlas::InteractionEdge& edge) { return edge.kind == kind; });
        return found == graph.edges.end() ? nullptr : &*found;
    };
    const auto* field_edge = edge_of(atlas::InteractionKind::field_type);
    const auto call_found = std::ranges::find_if(graph.edges,
        [](const atlas::InteractionEdge& edge) {
            return edge.kind == atlas::InteractionKind::static_call && edge.count == 2;
        });
    const auto* call_edge = call_found == graph.edges.end() ? nullptr : &*call_found;
    const auto* observed = edge_of(atlas::InteractionKind::observed_link);
    check(field_edge && field_edge->first_line == 5 &&
              call_edge && call_edge->count == 2 && call_edge->first_line == 12 &&
              call_edge->dynamic_dispatch && observed && observed->snapshot_step == 3 &&
              graph.nodes[observed->from].id == "runtime:site.1" &&
              graph.nodes[observed->to].id == "runtime:heap.1",
          "interaction provenance or source position was conflated");
    check(std::ranges::count_if(graph.edges, [](const atlas::InteractionEdge& edge) {
              return edge.kind == atlas::InteractionKind::static_call;
          }) == 2, "same-named overloads were merged by display text");
    const auto all_rows = atlas::present_interactions(graph, std::nullopt, false);
    const auto selected_rows = atlas::present_interactions(graph, 0, true);
    check(all_rows.rows.size() == 5 && selected_rows.rows.size() == 4 &&
              all_rows.summary.find("CAPTURE TRUNCATED") != std::string::npos &&
              selected_rows.summary.find("showing 4 / 5 rows") != std::string::npos,
          "interaction presentation lost filtering or completeness warning");
    check(atlas::interaction_detail_text(graph,
              static_cast<std::size_t>(observed - graph.edges.data()))
              .find("No source/class identity is inferred") != std::string::npos &&
              atlas::interaction_detail_text(graph,
                  static_cast<std::size_t>(call_edge - graph.edges.data()))
                  .find("dynamic dispatch") != std::string::npos,
          "interaction details lost evidence limitations");
    const auto json = atlas::agent_interactions_json(graph, 1);
    check(json.find("\"truncated\":true") != std::string::npos &&
              json.find("\"edge_count\":5") != std::string::npos &&
              json.find("\"runtime_step\":3") != std::string::npos &&
              json.find("\"capture_truncated\":true") != std::string::npos,
          "interaction JSON did not disclose truncation or runtime step");
    const atlas::Trace limited{{1, "capture.truncated", "capture", "10000"}};
    const auto replayed = atlas::replay_runtime(limited, 1);
    check(replayed && replayed->capture_truncated,
          "runtime replay hid the capture event limit");
}

void wrapped_field_graph_test(const std::filesystem::path& workspace) {
    const auto root = workspace / "wrapped-fields";
    write_text(root / "wrapped.hpp",
               "#include <map>\n"
               "#include <optional>\n"
               "#include <vector>\n"
               "struct Node {};\n"
               "struct Buffer {};\n"
               "using NodeAlias = Node;\n"
               "struct Holder {\n"
               "  std::vector<Node> nodes;\n"
               "  std::optional<Buffer*> buffer;\n"
               "  std::map<Node*, Buffer> lookup;\n"
               "  NodeAlias fixed[2];\n"
               "};\n");
    const auto scan = atlas::scan_project(root);
    check(scan && scan->files.size() == 1 && scan->files[0].parsed &&
              scan->files[0].error_diagnostic_count == 0 &&
              scan->files[0].symbols.size() == 3,
          "wrapped-field fixture did not parse cleanly");
    const auto& symbols = scan->files[0].symbols;
    const auto& members = symbols[2].members;
    check(members.size() == 4, "wrapped fields were not captured");
    for (const auto& member : members) {
        const auto& expected = member.name == "buffer" ? symbols[1].usr : symbols[0].usr;
        check(std::ranges::find(member.related_usrs, expected) !=
                  member.related_usrs.end(),
              "template or pointer wrapper hid a project class type");
    }
    check(std::ranges::find(members[2].related_usrs, symbols[1].usr) !=
              members[2].related_usrs.end(),
          "second map template argument was not linked");
    const auto graph = atlas::build_uml_graph(*scan);
    check(graph.nodes.size() == 3 && graph.edges.size() == 2 &&
              std::ranges::all_of(graph.edges, [](const atlas::UmlEdge& edge) {
                  return edge.from == 2 &&
                      edge.relation == atlas::UmlRelation::field_reference;
              }), "wrapped field dependencies were not deduplicated in UML");
    const auto json = atlas::agent_scan_json(*scan);
    check(json.find("\"related_usrs\":[") != std::string::npos &&
              json.find(symbols[0].usr) != std::string::npos,
          "agent map omitted wrapped field identities");
}

void dispatch_call_test(const std::filesystem::path& workspace) {
    const auto root = workspace / "dispatch-calls";
    write_text(root / "dispatch.cpp",
               "#include <vector>\n"
               "struct Base { virtual int compute() { return 1; } };\n"
               "int dispatch(Base& value) { return value.compute(); }\n"
               "int callback(int (*fn)(int)) { return fn(3); }\n"
               "int library(std::vector<int>& values) { return values.size(); }\n");
    const auto scan = atlas::scan_project(root);
    check(scan && scan->files.size() == 1 && scan->files[0].parsed &&
              scan->files[0].error_diagnostic_count == 0,
          "dispatch fixture did not parse cleanly");
    const auto& calls = scan->files[0].calls;
    check(std::ranges::any_of(calls, [](const atlas::CallSite& call) {
              return call.caller_name == "dispatch" &&
                  call.callee_name == "Base::compute" &&
                  call.dynamic_dispatch && !call.callee_file.empty();
          }), "virtual call was not marked as a possible dynamic dispatch");
    check(std::ranges::any_of(calls, [](const atlas::CallSite& call) {
              return call.caller_name == "callback" && call.indirect &&
                  call.callee_usr.empty() && call.callee_file.empty();
          }), "function pointer was mistaken for an exact project call target");
    check(std::ranges::any_of(calls, [](const atlas::CallSite& call) {
              return call.caller_name == "library" && !call.callee_usr.empty() &&
                  call.callee_file.empty();
          }), "external library call was omitted or misclassified");
}

void cross_file_calls_test(const std::filesystem::path& workspace) {
    const auto root = workspace / "cross-file-calls";
    write_text(root / "src" / "api.hpp",
               "inline int twice(int value) { return value * 2; }\n");
    write_text(root / "src" / "client.cpp",
               "#include \"api.hpp\"\n"
               "int answer() { return twice(20) + twice(1); }\n");
    const auto scan = atlas::scan_project(root);
    check(scan && scan->files.size() == 2, "cross-file fixture scan failed");
    const auto client = std::ranges::find_if(scan->files,
        [](const atlas::SourceFile& file) {
            return file.relative_path == "src/client.cpp";
        });
    check(client != scan->files.end() && client->calls.size() == 2 &&
              std::ranges::all_of(client->calls, [](const atlas::CallSite& call) {
                  return call.callee_file == "src/api.hpp";
              }), "repeated cross-file calls did not resolve to the project header");
    const auto source = atlas::agent_source_json(root, "src/client.cpp", 2, 1);
    check(source && source->find("\"schema\":\"project-atlas.source.v1\"") != std::string::npos &&
              source->find("\"line\":2,\"text\":\"int answer()") != std::string::npos &&
              source->find("\"truncated\":false") != std::string::npos,
          "bounded source lookup lost the exact line");
    constexpr std::string_view hash_key = "\"content_hash_fnv1a64\":\"";
    const auto hash_marker = source->find(hash_key);
    check(hash_marker != std::string::npos, "source lookup omitted content hash");
    const auto hash = source->substr(hash_marker + hash_key.size(), 16);
    check(atlas::agent_source_json(root, "src/client.cpp", 1, 2, std::string_view(hash)).has_value() &&
              !atlas::agent_source_json(root, "../outside.cpp", 1, 1).has_value() &&
              !atlas::agent_source_json(root, "src/client.cpp", 1, 81).has_value(),
          "source lookup failed hash, path or output bounds");
    const auto relations = atlas::agent_file_relations_json(*scan, "src/api.hpp");
    check(relations && relations->find("\"schema\":\"project-atlas.file-relations.v1\"") != std::string::npos &&
              relations->find("\"direction\":\"incoming\"") != std::string::npos &&
              relations->find("\"other_file\":\"src/client.cpp\"") != std::string::npos &&
              relations->find("\"count\":2") != std::string::npos &&
              !atlas::agent_file_relations_json(*scan, "missing.cpp").has_value(),
          "file relations lost direct caller evidence or accepted an unknown file");
    const auto report = workspace / "cross-file-report";
    check(atlas::write_project_report(*scan, report).has_value(),
          "cross-file report generation failed");
    const auto call_map = read_text(report / "calls.md");
    check(call_map.find("| src/client.cpp | src/api.hpp | 2 |") !=
              std::string::npos &&
              call_map.find("flowchart LR") != std::string::npos,
          "static call map lost the project interaction");
    const auto compact = atlas::agent_context_json(*scan, "answer", 400);
    check(compact.size() <= 1600 &&
              compact.find("\"schema\":\"project-atlas.context.v1\"") != std::string::npos &&
              compact.find("\"clang_warnings\":0") != std::string::npos &&
              compact.find("src/client.cpp") != std::string::npos &&
              compact.find("int answer()") != std::string::npos,
          "compact context omitted a matching source line or exceeded its byte cap");
    write_text(root / "src" / "client.cpp", "int answer() { return 0; }\n");
    check(!atlas::agent_source_json(root, "src/client.cpp", 1, 1, std::string_view(hash)).has_value(),
          "source lookup accepted a stale hash");
    const auto stale = atlas::agent_context_json(*scan, "answer", 400);
    check(stale.find("\"snippet_unavailable\":true") != std::string::npos &&
              stale.find("int answer() { return 0; }") == std::string::npos,
          "compact context served a snippet from changed source");
}

void compile_context_and_coverage_test(const std::filesystem::path& workspace) {
    const auto root = workspace / "compile-context";
    write_text(root / "forced.hpp", "#define ATLAS_FORCED 1\n");
    write_text(root / "main.cpp",
               "#if !defined(ATLAS_FORCED) || RESPONSE_VALUE != 7\n"
               "#error compilation context was lost\n"
               "#endif\n"
               "struct FromConfiguredBuild {};\n");
    write_text(root / "flags.rsp", "-DRESPONSE_VALUE=7 -include forced.hpp");
    const auto directory = root.generic_string();
    const auto source = (root / "main.cpp").generic_string();
    write_text(root / "compile_commands.json",
               "[{\"directory\":\"" + directory +
                   "\",\"file\":\"" + source +
                   "\",\"arguments\":[\"clang++\",\"@flags.rsp\",\"-c\",\"" +
                   source + "\"]}]");
    const auto complete = atlas::scan_project(root, root);
    check(complete && complete->files.size() == 2,
          "response-file build fixture scan failed");
    const auto configured = std::ranges::find_if(complete->files,
        [](const atlas::SourceFile& file) { return file.relative_path == "main.cpp"; });
    check(configured != complete->files.end() && configured->parsed &&
              configured->error_diagnostic_count == 0 &&
              configured->build_context_complete &&
              configured->compile_command_count == 1 &&
              std::ranges::find(configured->effective_compile_arguments,
                                "-D") != configured->effective_compile_arguments.end() &&
              std::ranges::find(configured->effective_compile_arguments,
                                "-include") != configured->effective_compile_arguments.end(),
          "response file, forced include, or build-context evidence was lost");

    write_text(root / "compile_commands.json",
               "[{\"directory\":\"" + directory +
                   "\",\"file\":\"" + source +
                   "\",\"arguments\":[\"clang-cl.exe\",\"/nologo\",\"-TP\","
                   "\"/EHsc\",\"/Zi\",\"/RTC1\",\"-clang:-std=c++23\","
                   "\"-MDd\",\"@flags.rsp\",\"-c\",\"" + source + "\"]}]");
    const auto clang_cl = atlas::scan_project(root, root);
    check(clang_cl.has_value(), "clang-cl build fixture scan failed");
    const auto clang_cl_file = std::ranges::find_if(clang_cl->files,
        [](const atlas::SourceFile& file) { return file.relative_path == "main.cpp"; });
    check(clang_cl_file != clang_cl->files.end() &&
              clang_cl_file->parsed && clang_cl_file->error_diagnostic_count == 0 &&
              clang_cl_file->build_context_complete &&
              std::ranges::find(clang_cl_file->effective_compile_arguments,
                                "-fexceptions") != clang_cl_file->effective_compile_arguments.end(),
          "ordinary clang-cl driver flags were mistaken for lost build context");

    write_text(root / "compile_commands.json",
               "[{\"directory\":\"" + directory +
                   "\",\"file\":\"" + source +
                   "\",\"arguments\":[\"clang++\",\"@flags.rsp\",\"-funroll-loops\",\"" +
                   source + "\"]}]");
    const auto ignored = atlas::scan_project(root, root);
    check(ignored && !atlas::scan_coverage_complete(*ignored) &&
              ignored->files.back().ignored_compile_arguments.size() == 1 &&
              ignored->files.back().ignored_compile_arguments.front() == "-funroll-loops",
          "ignored build flag was silently reported as complete coverage");

    const auto command = "{\"directory\":\"" + directory +
        "\",\"file\":\"" + source +
        "\",\"arguments\":[\"clang++\",\"@flags.rsp\",\"" + source + "\"]}";
    write_text(root / "compile_commands.json", "[" + command + "," + command + "]");
    const auto ambiguous = atlas::scan_project(root, root);
    check(ambiguous && ambiguous->files.back().compile_command_count == 2 &&
              !ambiguous->files.back().build_context_complete,
          "multiple compile configurations were silently collapsed into one");

    atlas::ProjectScan synthetic;
    synthetic.root = workspace;
    atlas::SourceFile file;
    file.relative_path = "synthetic.cpp";
    file.parsed = true;
    file.build_context_complete = true;
    for (int i = 0; i < 5; ++i) {
        atlas::Symbol symbol;
        symbol.name = "Target" + std::to_string(i);
        file.symbols.push_back(std::move(symbol));
    }
    synthetic.files.push_back(std::move(file));
    const auto context = atlas::agent_context_json(synthetic, "Target", 1000);
    check(context.find("\"truncated\":true") != std::string::npos &&
              context.find("\"symbol_matches\":5") != std::string::npos &&
              context.find("\"returned_symbols\":2") != std::string::npos,
          "nested context truncation was not disclosed");
    synthetic.skipped_large_files.push_back("oversized.cpp");
    const auto partial = atlas::agent_context_json(synthetic, "Target", 1000);
    check(partial.find("\"parse_clean\":true") != std::string::npos &&
              partial.find("\"coverage_complete\":false") != std::string::npos &&
              partial.find("\"clean\":false") != std::string::npos,
          "skipped source was presented as complete coverage");
}

void portable_lambda_paths_test(const std::filesystem::path& workspace) {
    const auto root = workspace / "portable-lambda";
    write_text(root / "lambda.cpp",
               "int answer() { auto fn = [] { return 3; }; return fn(); }\n");
    const auto scan = atlas::scan_project(root);
    check(scan && scan->files.size() == 1 && scan->files[0].parsed,
          "lambda fixture failed to scan");
    const auto& calls = scan->files[0].calls;
    check(std::ranges::any_of(calls, [](const atlas::CallSite& call) {
              return call.callee_name.find("lambda at lambda.cpp:") != std::string::npos;
          }), "lambda display name lost its portable source location");
    check(std::ranges::none_of(calls, [&](const atlas::CallSite& call) {
              return call.caller_name.find(root.string()) != std::string::npos ||
                  call.callee_name.find(root.string()) != std::string::npos;
          }), "lambda display name exposed the checkout path");
    const auto report = workspace / "portable-lambda-report";
    check(atlas::write_project_report(*scan, report).has_value(),
          "lambda report generation failed");
    const auto card = read_text(report / "lambda.cpp.agent.md");
    check(card.find("lambda at lambda.cpp:") != std::string::npos &&
              card.find("lambda at D:") == std::string::npos,
          "generated card exposed an absolute lambda path");
}

void namespaces_and_diagnostics_test(const std::filesystem::path& workspace) {
    const auto root = workspace / "namespace-source";
    const auto namespaced = root / "src" / "namespaces.cpp";
    write_text(namespaced,
               "namespace alpha { class Node {}; }\n"
               "namespace beta { class Node {}; }\n");
    write_text(root / "src" / "broken.cpp",
               "class Broken { public: void f( ; };\n");
    const auto scan = atlas::scan_project(root);
    check(scan.has_value() && scan->files.size() == 2,
          "namespace/error fixture scan failed");
    const auto namespaces = std::find_if(scan->files.begin(), scan->files.end(),
        [](const atlas::SourceFile& file) {
            return file.relative_path.filename() == "namespaces.cpp";
        });
    const auto broken = std::find_if(scan->files.begin(), scan->files.end(),
        [](const atlas::SourceFile& file) {
            return file.relative_path.filename() == "broken.cpp";
        });
    check(namespaces != scan->files.end() && namespaces->symbols.size() == 2,
          "distinct namespace classes missing");
    check(namespaces->symbols[0].qualified_name == "alpha::Node" &&
              namespaces->symbols[1].qualified_name == "beta::Node" &&
              namespaces->symbols[0].usr != namespaces->symbols[1].usr,
          "namespace classes have colliding identities");
    check(broken != scan->files.end() && broken->parsed &&
              broken->error_diagnostic_count > 0,
          "Clang syntax errors were not counted separately from AST creation");
    const auto filtered = atlas::agent_scan_json(*scan, "BETA::node");
    check(filtered.find("\"matched_files\":1") != std::string::npos &&
              filtered.find("\"qualified_name\":\"beta::Node\"") != std::string::npos &&
              filtered.find("\"qualified_name\":\"alpha::Node\"") == std::string::npos,
          "agent find failed to filter qualified symbols");
    const auto limited = atlas::agent_scan_json(*scan, std::nullopt, 1);
    check(limited.find("\"truncated\":true") != std::string::npos,
          "agent map did not mark truncated output");

    const auto reports = workspace / "namespace-reports";
    check(atlas::write_project_report(*scan, reports).has_value(),
          "namespace report failed");
    const auto diagram = read_text(reports / "classes.md");
    check(diagram.find("[\"alpha::Node\"]") != std::string::npos &&
              diagram.find("[\"beta::Node\"]") != std::string::npos,
          "namespace classes collapsed in diagram");
    const auto index = read_text(reports / "index.md");
    check(index.find("Clang errors") != std::string::npos,
          "index does not report Clang errors");

    write_text(namespaced,
               "namespace alpha { class Node {}; class Added {}; }\n"
               "namespace beta { class Node {}; }\n");
    const auto stale_output = workspace / "stale-reports";
    check(!atlas::write_project_report(*scan, stale_output, true).has_value(),
          "stale scan wrote cards after source changed");
    check(!std::filesystem::exists(stale_output),
          "stale scan created a partial output directory");
}

void expected_actual_test(const std::filesystem::path& workspace) {
    const auto expected_path = workspace / "expected.facts";
    const auto actual_path = workspace / "actual.facts";
    write_text(expected_path,
               "# Contract maintained by a human or test\n"
               "memory.buffer.bytes=67108864\n"
               "memory.buffer.owner=ModelRuntime\n"
               "memory.buffer.state=Ready\n");
    {
        std::ofstream actual(actual_path, std::ios::binary);
        atlas::ObservationWriter writer(actual);
        writer.record("memory.buffer.bytes", "134217728");
        writer.record("memory.buffer.state", "Ready");
        writer.record("memory.buffer.allocations", "2");
    }
    const auto expected = atlas::read_facts(expected_path);
    const auto actual = atlas::read_facts(actual_path);
    check(expected.has_value() && actual.has_value(), "fact parsing failed");
    const auto comparison = atlas::compare(*expected, *actual);
    check(!comparison.all_match(), "different states reported equal");
    check(comparison.entries.size() == 4, "wrong comparison cardinality");
    check(comparison.entries[1].kind == atlas::DifferenceKind::mismatch,
          "byte mismatch missing");
    check(comparison.entries[2].kind == atlas::DifferenceKind::missing_actual,
          "missing observation not found");
    check(comparison.entries[3].kind == atlas::DifferenceKind::match,
          "matching observation not found");
    const auto agent_comparison = atlas::agent_comparison_json(comparison);
    check(agent_comparison.find("\"all_match\":false") != std::string::npos &&
              agent_comparison.find("\"kind\":\"mismatch\"") != std::string::npos &&
              agent_comparison.find("\"kind\":\"missing_actual\"") != std::string::npos,
          "agent comparison omitted differences");
    check(atlas::agent_error_json("bad \"path\"\n").find("bad \\\"path\\\"\\n") !=
              std::string::npos,
          "agent error JSON did not escape control characters");

    const auto report_path = workspace / "comparison.md";
    check(atlas::write_comparison_report(comparison, report_path).has_value(),
          "comparison report failed");
    check(read_text(report_path).find("3 differences") != std::string::npos,
          "comparison report count is wrong");

    write_text(workspace / "duplicate.facts", "same=1\nsame=2\n");
    check(!atlas::read_facts(workspace / "duplicate.facts").has_value(),
          "duplicate fact silently accepted");
    const auto oversized_facts = workspace / "oversized.facts";
    write_text(oversized_facts, "a=1\n");
    std::filesystem::resize_file(oversized_facts, 16 * 1024 * 1024 + 1);
    check(!atlas::read_facts(oversized_facts).has_value(),
          "facts byte limit did not reject an oversized file");
    write_text(workspace / "long-line.facts", "a=" + std::string(70 * 1024, 'x') + "\n");
    check(!atlas::read_facts(workspace / "long-line.facts").has_value(),
          "facts line limit did not reject a giant value");
    check(atlas::compare({{"state", "Ready"}}, {{"state", "Ready"}}).all_match(),
          "identical facts reported different");
}

void memory_trace_test(const std::filesystem::path& workspace) {
    const auto expected_path = workspace / "memory_expected.trace";
    const auto actual_path = workspace / "memory_actual.trace";
    write_text(expected_path,
               "# atlas-trace-v1\n"
               "1\tallocate\tmemory.buffer\t64\n"
               "2\towner\tmemory.buffer\tModelRuntime\n"
               "3\trelease\tmemory.buffer\t0\n");
    {
        std::ofstream output(actual_path, std::ios::binary);
        atlas::TraceWriter writer(output);
        auto buffer = std::make_unique<std::byte[]>(64);
        check(buffer != nullptr, "memory test allocation failed");
        writer.record("allocate", "memory.buffer", "64");
        buffer[0] = std::byte{0x2a};
        writer.record("owner", "memory.buffer", "ModelRuntime");
        buffer.reset();
        writer.record("release", "memory.buffer", "0");
    }
    const auto expected = atlas::read_trace(expected_path);
    const auto actual = atlas::read_trace(actual_path);
    check(expected && actual && expected->size() == 3 && *expected == *actual,
          "real memory lifecycle did not match its trace contract");
    check(atlas::compare_trace(*expected, *actual).all_match(),
          "identical trace reported a divergence");

    write_text(workspace / "wrong_owner.trace",
               "# atlas-trace-v1\n"
               "1\tallocate\tmemory.buffer\t64\n"
               "2\towner\tmemory.buffer\tCache\n"
               "3\trelease\tmemory.buffer\t0\n");
    const auto wrong = atlas::read_trace(workspace / "wrong_owner.trace");
    check(wrong.has_value(), "wrong-owner fixture did not parse");
    const auto difference = atlas::compare_trace(*expected, *wrong);
    check(!difference.all_match() && difference.first_difference->step == 2 &&
              difference.first_difference->expected->value == "ModelRuntime" &&
              difference.first_difference->actual->value == "Cache",
          "first temporal divergence was not identified");
    const auto json = atlas::agent_trace_json(difference);
    check(json.find("\"first_difference\":{\"step\":2") != std::string::npos &&
              json.find("\"value\":\"Cache\"") != std::string::npos,
          "agent trace JSON omitted the first divergence");
    const auto truncated = atlas::Trace{actual->begin(), actual->begin() + 2};
    const auto missing = atlas::compare_trace(*expected, truncated);
    check(missing.first_difference->step == 3 &&
              !missing.first_difference->actual.has_value(),
          "missing final event was not identified");
    auto extended = *actual;
    extended.push_back({4, "release", "memory.buffer", "0"});
    const auto extra = atlas::compare_trace(*expected, extended);
    check(extra.first_difference->step == 4 &&
              !extra.first_difference->expected.has_value() &&
              extra.first_difference->actual.has_value(),
          "unexpected final event was not identified");

    write_text(workspace / "bad_step.trace",
               "# atlas-trace-v1\n1\tallocate\tmemory.buffer\t64\n"
               "3\trelease\tmemory.buffer\t0\n");
    check(!atlas::read_trace(workspace / "bad_step.trace").has_value(),
          "nonconsecutive trace steps were accepted");
    write_text(workspace / "bad_header.trace", "1\tallocate\tmemory.buffer\t64\n");
    check(!atlas::read_trace(workspace / "bad_header.trace").has_value(),
          "unversioned trace was accepted");
    std::string bad_control = "# atlas-trace-v1\n1\towner\tmemory.buffer\tbad";
    bad_control.push_back('\0');
    bad_control += "value\n";
    write_text(workspace / "bad_control.trace", bad_control);
    check(!atlas::read_trace(workspace / "bad_control.trace").has_value(),
          "control byte in trace value was accepted");
    std::ostringstream invalid_output;
    atlas::TraceWriter invalid_writer(invalid_output);
    bool rejected = false;
    try { invalid_writer.record("owner", "memory.buffer", "line\nbreak"); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "trace writer accepted a multiline value");

    const auto sourced_path = workspace / "sourced.trace";
    {
        std::ofstream output(sourced_path, std::ios::binary);
        atlas::TraceWriter writer(output, atlas::TraceFormat::v2);
        writer.record("site.create", "site.1", "allocation", "src/example.cpp", 42);
        writer.record("obj.create", "heap.1", "8 B");
        writer.record("link.add", "site.1", "allocates:heap.1");
    }
    const auto sourced = atlas::read_trace(sourced_path);
    check(sourced && sourced->size() == 3 &&
              (*sourced)[0].source_file == "src/example.cpp" &&
              (*sourced)[0].source_line == 42 &&
              (*sourced)[1].source_file.empty() && (*sourced)[1].source_line == 0,
          "v2 trace lost structured source location");
    const auto sourced_snapshot = atlas::replay_runtime(*sourced, 3);
    check(sourced_snapshot && sourced_snapshot->nodes.size() == 2 &&
              atlas::agent_runtime_json(*sourced_snapshot).find(
                  "\"source_file\":\"src/example.cpp\",\"source_line\":42") !=
                  std::string::npos,
          "runtime JSON lost structured source location");
    write_text(workspace / "bad_source.trace",
               "# atlas-trace-v2\n1\tsite.create\tsite.1\tallocation\t\t42\n");
    check(!atlas::read_trace(workspace / "bad_source.trace").has_value(),
          "v2 trace accepted a line number without a source file");
    write_text(workspace / "bad_source_columns.trace",
               "# atlas-trace-v2\n1\tsite.create\tsite.1\tallocation\tfile.cpp\n");
    check(!atlas::read_trace(workspace / "bad_source_columns.trace").has_value(),
          "v2 trace accepted a missing source column");
    check(!atlas::read_trace(expected_path, {16, 100, 100}).has_value(),
          "trace byte limit did not reject a file before parsing");
    check(!atlas::read_trace(expected_path, {1024, 2, 100}).has_value(),
          "trace event limit did not reject an extra event");
    check(!atlas::read_trace(expected_path, {1024, 100, 20}).has_value(),
          "trace line limit did not reject a long event");
}

void runtime_graph_test(const std::filesystem::path& workspace) {
    const auto path = workspace / "runtime.trace";
    {
        std::ofstream output(path, std::ios::binary);
        atlas::RuntimeWriter writer(output);
        auto buffer = std::make_unique<std::byte[]>(64);
        writer.create_variable("stack.buffer", "buffer");
        writer.create_object("heap.buffer", "Buffer[64]");
        writer.add_link("stack.buffer", "heap.buffer", "owns");
        writer.create_variable("stack.alias", "alias");
        writer.add_link("stack.alias", "heap.buffer", "points_to");
        writer.remove_link("stack.alias", "heap.buffer", "points_to");
        writer.destroy_variable("stack.alias");
        writer.remove_link("stack.buffer", "heap.buffer", "owns");
        buffer.reset();
        writer.destroy_object("heap.buffer");
        writer.destroy_variable("stack.buffer");
    }
    const auto trace = atlas::read_trace(path);
    check(trace && trace->size() == 10, "runtime trace was not written");
    const auto before = atlas::replay_runtime(*trace, 0);
    check(before && before->nodes.empty() && before->links.empty(),
          "runtime step zero was not empty");
    const auto shared = atlas::replay_runtime(*trace, 5);
    check(shared && shared->nodes.size() == 3 && shared->links.size() == 2,
          "runtime graph lost an alias or object");
    const auto json = atlas::agent_runtime_json(*shared);
    check(json.find("\"kind\":\"variable\"") != std::string::npos &&
              json.find("\"relation\":\"owns\"") != std::string::npos &&
              json.find("\"relation\":\"points_to\"") != std::string::npos,
          "runtime graph JSON omitted typed nodes or links");
    const auto released = atlas::replay_runtime(*trace, 9);
    check(released && released->nodes.size() == 1 && released->links.empty(),
          "destroyed object remained in runtime graph");
    const auto completed = atlas::replay_runtime(*trace, 10);
    check(completed && completed->nodes.empty() && completed->links.empty(),
          "runtime graph was not empty after cleanup");
    check(!atlas::replay_runtime(*trace, 11).has_value(),
          "runtime graph accepted a step beyond the trace");

    auto invalid = *trace;
    invalid[7] = {8, "obj.destroy", "heap.buffer", "-"};
    check(!atlas::replay_runtime(invalid, 8).has_value(),
          "runtime graph allowed destruction with live links");
    invalid = *trace;
    invalid[5] = {6, "link.add", "stack.alias", "points_to:heap.buffer"};
    check(!atlas::replay_runtime(invalid, 6).has_value(),
          "runtime graph allowed a duplicate link");
    invalid = *trace;
    invalid[9] = {10, "var.create", "stack.buffer", "reused"};
    check(!atlas::replay_runtime(invalid, 10).has_value(),
          "runtime graph allowed an ID to be reused");

    std::ostringstream site_output;
    atlas::RuntimeWriter site_writer(site_output);
    site_writer.create_site("site.1", "allocation site");
    site_writer.create_object("heap.1", "8 B");
    site_writer.add_link("site.1", "heap.1", "allocates");
    write_text(workspace / "site.trace", site_output.str());
    const auto site_trace = atlas::read_trace(workspace / "site.trace");
    check(site_trace && atlas::replay_runtime(*site_trace, 3)->links.size() == 1,
          "allocation site graph was not replayed");
    const auto site_json = atlas::agent_runtime_json(*atlas::replay_runtime(*site_trace, 3));
    check(site_json.find("\"kind\":\"site\"") != std::string::npos,
          "site kind missing from agent JSON");
}

#ifdef _WIN32
void stalled_ui_client_test() {
    const DWORD pid = GetCurrentProcessId();
    std::atomic<bool> handled{false};
    std::jthread server([&](std::stop_token stop) {
        atlas::serve_ui_commands(stop, pid, [&](const atlas::UiCommand&) {
            handled.store(true);
            atlas::UiReply reply;
            reply.ok = 1;
            return reply;
        });
    });
    const auto name = L"\\\\.\\pipe\\ProjectAtlas.UI." + std::to_wstring(pid);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(pipe != INVALID_HANDLE_VALUE, "test could not connect to UI pipe");
    atlas::UiCommand command;
    DWORD written = 0;
    check(WriteFile(pipe, &command, sizeof(command), &written, nullptr) &&
              written == sizeof(command), "test could not send UI command");
    while (!handled.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(handled.load(), "UI server did not process stalled client request");
    const auto start = std::chrono::steady_clock::now();
    server.request_stop();
    server.join();
    CloseHandle(pipe);
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(3),
          "UI server did not stop with a client holding unread reply bytes");
}

void embedded_debugger_test() {
    atlas::NativeDebugger debugger;
    std::string error;
    auto wait_for = [&](std::string_view text) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto snapshot = debugger.snapshot_json();
            if (snapshot.find(text) != std::string::npos) return snapshot;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        throw std::runtime_error("debugger did not reach expected state: " +
                                 std::string(text) + "; last=" + debugger.snapshot_json());
    };
    check(debugger.launch(ATLAS_DEBUG_DEMO_PATH, error), "debug launch failed");
    wait_for("\"reason\":\"entry\"");
    check(debugger.breakpoint(ATLAS_DEBUG_DEMO_SOURCE, 3, error),
          "debug breakpoint request failed");
    wait_for("\"verified\":true");
    check(debugger.resume(false, error), "debug continue failed");
    const auto stopped = wait_for("\"reason\":\"breakpoint\"");
    check(stopped.find("\"line\":3") != std::string::npos &&
          stopped.find("\"name\":\"counter\",\"value\":\"7\"") != std::string::npos,
          "debugger did not resolve the source line and local variable");
    check(debugger.resume(true, error), "debug step failed");
    const auto stepped = wait_for("\"reason\":\"step\"");
    check(stepped.find("\"line\":4") != std::string::npos,
          "source step did not reach the next line");
    check(debugger.resume(false, error), "debug final continue failed");
    wait_for("\"reason\":\"exit code 0\"");
}

void multithread_breakpoint_test() {
    atlas::NativeDebugger debugger;
    std::string error;
    auto wait_for = [&](std::string_view stage, auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto snapshot = debugger.snapshot_json();
            if (predicate(snapshot)) return snapshot;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto state = debugger.snapshot_json();
        std::ostringstream detail;
        const auto pid_at = state.find("\"pid\":");
        if (pid_at != std::string::npos) {
            const auto pid = static_cast<DWORD>(std::stoul(state.substr(pid_at + 6)));
            HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (threads != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry{};
                entry.dwSize = sizeof(entry);
                if (Thread32First(threads, &entry)) do {
                    if (entry.th32OwnerProcessID != pid) continue;
                    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                               FALSE, entry.th32ThreadID);
                    if (!thread) continue;
                    const auto previous = SuspendThread(thread);
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_CONTROL;
                    const bool got = GetThreadContext(thread, &context) != FALSE;
                    detail << " thread=" << entry.th32ThreadID << " suspend=" << previous
                           << " rip=" << (got ? context.Rip : 0);
                    if (previous != static_cast<DWORD>(-1)) ResumeThread(thread);
                    CloseHandle(thread);
                } while (Thread32Next(threads, &entry));
                CloseHandle(threads);
            }
        }
        throw std::runtime_error("multi-thread debugger timeout at " +
                                 std::string(stage) + ": " + state + detail.str());
    };
    auto thread_id = [](const std::string& snapshot) {
        const auto start = snapshot.find("\"thread_id\":");
        if (start == std::string::npos) return std::string{};
        const auto first = start + sizeof("\"thread_id\":") - 1;
        return snapshot.substr(first, snapshot.find(',', first) - first);
    };
    check(debugger.launch(ATLAS_DEBUG_THREADS_DEMO_PATH, error),
          "multi-thread debugger launch failed");
    wait_for("entry", [](const std::string& state) {
        return state.find("\"reason\":\"entry\"") != std::string::npos;
    });
    check(debugger.breakpoint(ATLAS_DEBUG_THREADS_DEMO_SOURCE, 6, error),
          "multi-thread breakpoint request failed");
    wait_for("breakpoint verification", [](const std::string& state) {
        return state.find("\"verified\":true") != std::string::npos;
    });
    check(debugger.resume(false, error), "multi-thread debugger continue failed");
    const auto first = wait_for("first breakpoint", [](const std::string& state) {
        return state.find("\"reason\":\"breakpoint\"") != std::string::npos;
    });
    const auto first_id = thread_id(first);
    check(!first_id.empty() && first_id != "0", "first breakpoint missed thread ID");
    check(debugger.resume(false, error), "multi-thread breakpoint rearm failed");
    wait_for("second breakpoint", [&](const std::string& state) {
        return state.find("\"reason\":\"breakpoint\"") != std::string::npos &&
               thread_id(state) != first_id;
    });
    check(debugger.resume(false, error), "second breakpoint resume failed");
    wait_for("exit", [](const std::string& state) {
        return state.find("\"reason\":\"exit code 0\"") != std::string::npos;
    });
}

void automatic_capture_test(const std::filesystem::path& workspace) {
    const auto output = workspace / "auto-runtime-generated.trace";
    const auto tagged_output = workspace / "auto-runtime-tagged.trace";
    const auto invalid_output = workspace / "auto-runtime-invalid-test.trace";
    std::filesystem::remove(output);
    std::filesystem::remove(tagged_output);
    std::filesystem::remove(invalid_output);
    std::filesystem::remove(std::filesystem::path(output.string() + ".raw"));
    std::filesystem::remove(std::filesystem::path(output.string() + ".symbolized.tmp"));
    const auto result = atlas::capture_runtime(ATLAS_AUTO_DEMO_PATH, output);
    check(result && result->exit_code == 0 && !result->truncated,
          "automatic capture failed");
    const auto trace = atlas::read_trace(output);
    check(trace && trace->size() >= 10, "automatic trace omitted allocations");
    check(trace->back().operation == "capture.complete" &&
              trace->back().resource == "capture",
          "automatic capture did not finish with a terminal record");
    const auto has_source = std::any_of(trace->begin(), trace->end(),
        [](const atlas::TraceEvent& event) {
            return event.operation == "site.create" &&
                   !event.source_file.empty() && event.source_line > 0;
        });
    check(has_source && read_text(output).starts_with("# atlas-trace-v2\n"),
          "automatic capture did not serialize PDB source locations");
    const auto active = atlas::replay_runtime(*trace, 6);
    check(active && active->links.size() == 2 && active->nodes.size() == 4,
          "automatic graph lost call sites or allocations");
    const auto finished = atlas::replay_runtime(*trace, trace->size());
    check(finished && finished->links.empty() && finished->nodes.size() == 2,
          "automatic graph retained released allocations");
    check(!atlas::capture_runtime(ATLAS_AUTO_DEMO_PATH, output).has_value(),
          "capture overwrote an existing trace");
    const auto tagged = atlas::capture_runtime(
        ATLAS_AUTO_DEMO_PATH, tagged_output, {}, "allocation_case");
    check(tagged && tagged->exit_code == 0, "tagged automatic capture failed");
    const auto tagged_trace = atlas::read_trace(tagged_output);
    check(tagged_trace && !tagged_trace->empty() &&
              tagged_trace->front().operation == "test.context" &&
              tagged_trace->front().resource == "test" &&
              tagged_trace->front().value == "allocation_case",
          "test identity was not stored in the captured trace");
    const auto tagged_snapshot = atlas::replay_runtime(*tagged_trace, tagged_trace->size());
    check(tagged_snapshot && tagged_snapshot->test_id == "allocation_case" &&
              atlas::agent_runtime_json(*tagged_snapshot).find(
                  "\"test_id\":\"allocation_case\"") != std::string::npos,
          "runtime replay or agent JSON lost test identity");
    const atlas::Trace misplaced_context{
        {1, "obj.create", "heap.1", "object"},
        {2, "test.context", "test", "allocation_case"}};
    check(!atlas::replay_runtime(misplaced_context, 2).has_value(),
          "runtime replay accepted a late test context");
    const atlas::Trace invalid_context{{1, "test.context", "test", "bad name"}};
    check(!atlas::replay_runtime(invalid_context, 1).has_value(),
          "runtime replay accepted an invalid test ID");
    check(!atlas::capture_runtime(
              ATLAS_AUTO_DEMO_PATH, invalid_output, {}, "bad test name").has_value() &&
              !std::filesystem::exists(invalid_output),
          "invalid test identity was accepted or created a trace");
    const auto crashed_output = workspace / "auto-runtime-crashed.trace";
    std::filesystem::remove(crashed_output);
    _putenv_s("ATLAS_AUTO_DEMO_CRASH", "1");
    const auto crashed = atlas::capture_runtime(ATLAS_AUTO_DEMO_PATH, crashed_output);
    _putenv_s("ATLAS_AUTO_DEMO_CRASH", "");
    check(!crashed.has_value() && !std::filesystem::exists(crashed_output),
          "crashed target published a trace without a terminal record");
    const auto concurrent_a = workspace / "auto-runtime-concurrent-a.trace";
    const auto concurrent_b = workspace / "auto-runtime-concurrent-b.trace";
    std::filesystem::remove(concurrent_a);
    std::filesystem::remove(concurrent_b);
    std::optional<std::expected<atlas::CaptureResult, std::string>> captured_a;
    std::optional<std::expected<atlas::CaptureResult, std::string>> captured_b;
    std::thread first([&] {
        captured_a = atlas::capture_runtime(ATLAS_AUTO_DEMO_PATH, concurrent_a);
    });
    std::thread second([&] {
        captured_b = atlas::capture_runtime(ATLAS_AUTO_DEMO_PATH, concurrent_b);
    });
    first.join();
    second.join();
    check(captured_a && captured_b && captured_a->has_value() &&
              captured_b->has_value() && std::filesystem::exists(concurrent_a) &&
              std::filesystem::exists(concurrent_b),
          "simultaneous captures failed DbgHelp serialization");
    const auto no_recorder = atlas::capture_runtime(
        ATLAS_CLI_PATH, workspace / "no-recorder.trace");
    check(!no_recorder.has_value(), "uninstrumented executable claimed capture");
}
#endif

} // namespace

int main() {
    try {
        const auto workspace = std::filesystem::current_path() / "atlas-test-workspace";
        scan_and_report_test(workspace);
        orphan_card_archive_test(workspace);
        interaction_evidence_test();
        wrapped_field_graph_test(workspace);
        dispatch_call_test(workspace);
        cross_file_calls_test(workspace);
        compile_context_and_coverage_test(workspace);
        portable_lambda_paths_test(workspace);
        namespaces_and_diagnostics_test(workspace);
        expected_actual_test(workspace);
        memory_trace_test(workspace);
        runtime_graph_test(workspace);
#ifdef _WIN32
        stalled_ui_client_test();
        embedded_debugger_test();
        multithread_breakpoint_test();
        automatic_capture_test(workspace);
#endif
        std::cout << "Project Atlas tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Project Atlas test failure: " << error.what() << '\n';
        return 1;
    }
}
