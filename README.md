# Project Atlas

Project Atlas is a native Windows C++23 navigator for C++ projects. It combines
libclang-based source analysis, a class graph, runtime traces, an embedded
debugger, and compact evidence for coding agents. It does not edit the project
being scanned. Atlas is still experimental: a clean AST scan is not proof that
all program behavior has been observed.

**Languages:** English (this file) · [Українська](README.uk.md)

The [Windows x64 release](https://github.com/yussik22/project-atlas-open/releases)
contains a portable GUI, CLI, and `libclang.dll`. Source code is MIT-licensed;
the bundled LLVM component has its own license. See [public release notes](PUBLIC_RELEASE.md),
[license](LICENSE), and [third-party notices](THIRD_PARTY_NOTICES.md). The
release binaries are not code-signed. The private development repository's Git
history must not be made public without a separate audit; public releases use a
curated source snapshot.

## Build and run

To build on Windows, install CMake 3.20+, Ninja, Visual Studio C++ build
tools/Windows SDK, and LLVM/Clang with `clang-c/Index.h`, the libclang import
library, and `libclang.dll`. Use a Developer PowerShell:

```powershell
cmake -S . -B build-clang -G Ninja `
  "-DLIBCLANG_ROOT=<your-LLVM-root>" `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-clang
ctest --test-dir build-clang --output-on-failure
```

Run `build-clang/project_atlas_gui.exe`, optionally passing a project directory
as the first argument. Choose a C++ project and press **Scan**. Atlas shows a
file/class tree, source preview, Clang diagnostics, static interactions, a
class graph, expected-versus-actual facts, a runtime map, and a debugger. Use
**Open in editor** to edit source in your configured Windows application, then
scan again. Atlas itself does not provide in-app source editing.

When available, the GUI uses `compile_commands.json` from the project root or
common build directories, including `build`, `build-clang`, and `build-ninja`.
Without the right compile database or headers, the AST may be incomplete.

The **UML** tab has a navigable class graph with inheritance and field-type
links, a minimap, zoom/pan controls, and a detailed selected-class view. It
does not yet infer every template or functional dependency. The **Overview**
and **Interactions** tabs show static call sites and their evidence. Virtual
dispatch, callbacks, external code, and indirect calls may not have a known
runtime target.

## Agent-facing CLI

Atlas exposes bounded, versioned JSON so an agent can inspect relevant
evidence before reading the whole repository:

```powershell
build-clang/project_atlas.exe agent map . build-clang
build-clang/project_atlas.exe agent find . atlas::Symbol build-clang
build-clang/project_atlas.exe agent context . --task scan_project --budget 1500 --compile-db build-clang
build-clang/project_atlas.exe agent source . src/scan.cpp 1 30
build-clang/project_atlas.exe agent file-relations . src/scan.cpp build-clang
build-clang/project_atlas.exe agent interactions . --compile-db build-clang --trace examples/runtime_memory.trace --step 5
build-clang/project_atlas.exe agent compare examples/memory_expected.facts examples/memory_actual.facts
```

`map` returns at most 200 files. `find` searches paths, symbols, functions,
methods, and recorded static call sites. `context` ranks evidence lexically,
not semantically, and caps its UTF-8 response at four bytes per requested
approximate token. Its `truncated` flag also covers nested symbol/function/call
limits, not only omitted files. A missing match is not proof
that no dependency exists. `source` returns an exact line range with an FNV-1a
content hash; pass that hash back as `expected-hash` to reject stale reads.
`file-relations` reports direct AST evidence for a file, not a complete impact
analysis. `parse_clean` means all visited files parsed without Clang errors;
`coverage_complete` (also exposed as `clean`) additionally requires no skipped
large files or silently dropped build flags. Inspect `ignored_compile_arguments`
and `build_context_complete` per file when coverage is false. Check `truncated`,
AST failures, and Clang diagnostics before
relying on any result. Static call edges are not observed runtime calls.
Trace and facts readers enforce byte, line, and entry limits while reading;
the default trace cap is 64 MiB and the facts cap is 16 MiB. The GUI applies
a tighter 8 MiB / 10,000-event trace cap.

The `agent` JSON schema is `project-atlas.agent.v1`; the combined interaction
schema is `project-atlas.interactions.v1`. Call-site records include source
locations, target USRs when known, and `dynamic_dispatch`/`indirect` flags.
Trace node IDs are not automatically mapped to source classes.

### MCP bridge and native-window control

The optional Node.js MCP bridge launches the C++ CLI. Build the CLI first,
then run:

```powershell
npm ci
npm run test:mcp
```

`atlas_connection_status` reports the actual installation root, workspace
root, CLI, and compilation database. Nine read-only analysis tools cover
project maps, search, context, source, file relations, interactions, facts,
traces, and runtime maps. The bridge defaults to this Atlas checkout. To use
Atlas with another C++ project, set `ATLAS_WORKSPACE_ROOT` to that project's
directory; the Atlas installation remains separate. `ATLAS_CLI` and
`ATLAS_COMPILE_DB` are optional overrides. Paths for project files, traces,
and executables are validated against the selected workspace.

Fourteen `atlas_ui_*` tools control a **running native Atlas window** over
local, same-user IPC. They include list/status/show, trace loading and step
selection, tab selection, scan, capture, file/symbol focus, fact comparison,
card generation, and UML overview/focus. Scan and capture are asynchronous;
poll `atlas_ui_status` for completion. Capture runs the selected executable
and writes a new trace. Card generation writes adjacent `.agent.md` files.
Before either write operation, the bridge verifies that the GUI is showing
the configured workspace. If multiple windows are open, select one by PID
from `atlas_ui_list`.

Equivalent CLI commands work without MCP:

```powershell
build-clang/project_atlas.exe agent ui list
build-clang/project_atlas.exe agent ui show
build-clang/project_atlas.exe agent ui load-trace examples/runtime_memory.trace
build-clang/project_atlas.exe agent ui step 5
build-clang/project_atlas.exe agent ui focus-file include/atlas/atlas.hpp
build-clang/project_atlas.exe agent ui focus-symbol atlas::Symbol
build-clang/project_atlas.exe agent ui uml-overview
build-clang/project_atlas.exe agent ui status
```

Copy `.codex/config.example.toml` to your local `.codex/config.toml` for a
Codex setup rooted in this repository. For another workspace, point its
configuration at this installation's `tools/atlas-mcp.mjs` and set, for
example, `ATLAS_WORKSPACE_ROOT = "<absolute-cpp-project-path>"`. The local
configuration is ignored by Git; a new Codex task may be needed to load a
changed MCP configuration. [AGENTS.md](AGENTS.md) contains the working guide
for agents in this repository.

### Embedded debugger

The **Debugger** tab and six `atlas_debug_*` agent commands use the Windows
x64 Debug API and DbgHelp. They launch a debuggee; they do not attach to an
arbitrary running process. Build the target with PDB symbols for source lines
and locals. No LLDB, Python runtime, or separate debug server is required.

```powershell
build-clang/project_atlas.exe agent ui debug-launch build-clang/project_atlas_debug_demo.exe
# Wait for debug.phase=paused and reason=entry.
build-clang/project_atlas.exe agent ui debug-break examples/debugger_demo.cpp 3
# Wait for debug.breakpoints[0].verified=true.
build-clang/project_atlas.exe agent ui debug-continue
build-clang/project_atlas.exe agent ui status
build-clang/project_atlas.exe agent ui debug-step
build-clang/project_atlas.exe agent ui debug-stop
```

Status includes the current source line, stack, simple local variables, and
breakpoints. Scalar values can currently be raw unsigned values; complex or
optimized-out variables may be unavailable. `debug-stop` terminates only the
debuggee started by that Atlas session. Atlas temporarily suspends other
debuggee threads while re-arming a software breakpoint; a capture session and
the embedded debugger cannot run concurrently in the same Atlas window.

## Generated cards and project scans

```powershell
build-clang/project_atlas.exe scan `
  path/to/cpp-project path/to/generated-atlas path/to/cpp-project/build
```

The output contains `index.html`, Markdown indexes, class summaries, and
`*.agent.md` source cards. The output directory must be empty or already have
the `.project-atlas-generated` marker; Atlas will not overwrite an unrelated
non-empty directory. Generated cards are AST facts, not human-authored
contracts. The optional third argument is a directory containing
`compile_commands.json`; Atlas uses its includes, macros, and language
standard, expands bounded response files, and passes forced includes and
target settings when supported. Unsupported compiler-driver flags and multiple
build configurations are reported as incomplete build context rather than
silently counted as full coverage.
Exit code `2` means at least one file had a Clang error or failed AST parsing.

Use `scan-in-place` instead of `scan` to put generated cards beside source
files. Atlas refreshes only cards carrying its own marker and preserves
unrelated files with the same name. When a source file disappears or moves,
its Atlas card moves to `.project-atlas/orphans/source` for recovery. Stale
report cards and summaries move to the report directory's `.orphans`.

For Atlas self-development, `-DATLAS_SELF_HOST_CARDS=ON` makes a normal build
refresh Atlas's own cards after C++ source changes. It is **off by default**
for ordinary builds so building the tool does not modify the checkout:

```powershell
cmake -S . -B build-fixed -DATLAS_SELF_HOST_CARDS=ON
cmake --build build-fixed
```

## Expected versus actual behavior

`.facts` files contain one `key=value` per line; `#` starts a comment.
Values use exact equality. Expected values must come from requirements or
validated tests, not from the scanner:

```powershell
build-clang/project_atlas.exe compare `
  examples/memory_expected.facts `
  examples/memory_actual.facts `
  build-clang/memory_comparison.md
```

Exit code `0` means equal, `2` means differences, and `1` means an input or
execution error. C++ tests may opt into `atlas::ObservationWriter` to record
observations; it is not intended for a hot loop.

`atlas::TraceWriter` records ordered events in `.trace` files. The default
format is still `# atlas-trace-v1`, with tab-separated `step`, `operation`,
`resource`, and `value` fields. Symbolized automatic captures use
`# atlas-trace-v2`: two additional fields, `source_file` and `source_line`,
store a PDB-resolved location when available (otherwise an empty path and
`0`). Both versions can be replayed. Steps are logical order, not wall-clock
timestamps. Comparison is positional and reports the first difference,
including missing or extra events:

```powershell
build-clang/project_atlas.exe agent trace `
  examples/memory_expected.trace examples/memory_actual.trace
```

The sample differs at step 2: `Cache` is recorded as owner instead of
`ModelRuntime`. Exit codes are `0` for a match, `2` for a difference, and `1`
for malformed input or a file error.

## Runtime map

### Automatic `new` / `delete` capture

On Windows, link `src/auto_runtime.cpp` into the target executable. This does
not require edits to the target's source, but it **does require relinking**;
Atlas cannot capture an arbitrary uninstrumented executable. For CMake:

```cmake
target_sources(my_app PRIVATE "${PROJECT_ATLAS_SOURCE_DIR}/src/auto_runtime.cpp")
```

Set `PROJECT_ATLAS_SOURCE_DIR` to your Atlas checkout. Build with PDB symbols
to improve allocation-site function/line labels. In the GUI's **Runtime map**
tab, select the executable and press **Record run**. Atlas launches it, writes
a new trace under `.project-atlas/captures/`, and opens the timeline. The CLI
equivalent is:

```powershell
build-clang/project_atlas.exe capture build-clang/project_atlas_auto_demo.exe auto-demo.trace --test allocation_case
build-clang/project_atlas.exe agent runtime auto-demo.trace 6
```

The trace contains heap objects, sizes, call sites, and `allocates` links.
This is **not proof of pointer ownership**. When symbols are available,
Atlas annotates site labels, writes structured source locations to the v2
trace, and keeps the original as `.trace.raw`. In the GUI, click a blue call
site in the runtime map to open its source line, provided that file belongs
to the scanned project. Without PDB line information, the call site is not
clickable. Serialized paths may reveal local directory names; inspect traces
before sharing them. Atlas writes to a unique temporary file and publishes it
only after a terminal `capture.complete` record is present; an interrupted run
leaves its incomplete temporary trace for inspection, without replacing the
requested output. Atlas refuses to overwrite an existing trace and marks
capture as truncated at its 10,000-event limit. The recorder sees global C++
`new`/`delete`, not `malloc`, custom allocators, or allocations in unrelated
DLLs. It records one run for later replay; it is not a live profiler.
The optional `--test` ID tags the whole captured run with a first
`test.context` event; replay and `agent runtime` expose it as `test_id`.
IDs use 1–127 ASCII letters, digits, `_`, `.` or `-`. The GUI's optional
test-ID field applies the same validation when recording and displays the ID
when loading a tagged trace.

### Explicit semantic trace

`atlas::RuntimeWriter` records `var.create`, `obj.create`, `link.add`,
`link.remove`, `var.destroy`, and `obj.destroy` into the same trace format.
An ID identifies one lifetime: reusing it, linking a nonexistent node, or
destroying a linked node is a replay error. Link names such as `owns` and
`points_to` are supplied by the test or application; Atlas does not infer
them from C++ semantics.

```powershell
build-clang/project_atlas.exe agent runtime examples/runtime_memory.trace 5
build-clang/project_atlas_gui.exe --runtime-demo . examples/runtime_memory.trace
```

At step 5 of that sample, `buffer` owns an object while `alias` points to it.
The GUI lets you load the trace and navigate its steps; the CLI returns the
full graph. The GUI warns if more nodes exist than it can display.

## Current limits and next step

- AST results depend on the correct compilation database and headers.
- The class and call graphs are static evidence, not a complete runtime graph.
- Automatic capture covers only relinked global C++ `new`/`delete` on Windows.
- Atlas does not infer pointer ownership or root causes automatically.
- Facts use exact equality; range/tolerance comparisons are not implemented.
- Trace replay compares one run positionally, not across reordered events.
- Orphaned cards are recoverable, but not restored automatically.

Next, add testable memory-ownership invariants. Structured source navigation
only covers PDB-resolved sites in files present in the scanned project.

The local [C++23 decision notes](knowledge/cpp23/README.md) explain design
choices, including why a class's identity is not its hash.
