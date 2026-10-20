# Project Atlas

Project Atlas is a native Windows application and C++23 command-line toolkit for exploring C++ projects. It combines libclang-based source navigation, a class graph, static interaction evidence, runtime traces, and a small embedded debugger. It also exposes bounded, machine-readable context for coding agents.

Atlas is an early **Windows x64 preview**, not a complete C++ debugger or profiler. Scanning and browsing do not modify the project being scanned. Generating adjacent `.agent.md` cards, recording a run, or launching a debug target are explicit actions.

## Download and run

Download [`ProjectAtlas-0.1.0-win-x64.zip`](https://github.com/yussik22/project-atlas-open/releases/tag/v0.1.0), extract the entire archive, and run `project_atlas_gui.exe`. Keep `libclang.dll` beside the executables. The portable build does not require an LLVM installation or the Visual C++ Debug runtime. It is unsigned, so Windows may show an unknown-publisher warning.

Choose a C++ project folder and select **Scan**. If the project has a `compile_commands.json` in its root or in `build`, `build-clang`, or `build-ninja`, Atlas uses it automatically. Correct compilation flags and headers are important: a missing build context can produce incomplete AST results.

The separate source ZIP is a clean export without the original private Git history. Build and packaging details are in [PUBLIC_RELEASE.md](PUBLIC_RELEASE.md). Atlas is MIT-licensed; the bundled LLVM component has separate [third-party notices](THIRD_PARTY_NOTICES.md).

## Feedback and ideas

Found a problem? [Open a bug report](https://github.com/yussik22/project-atlas-open/issues/new/choose). Have a concrete improvement in mind? Use the feature request form in the same [Issues](https://github.com/yussik22/project-atlas-open/issues/new/choose) page. For early ideas or questions, start a conversation in [Discussions](https://github.com/yussik22/project-atlas-open/discussions). Please remove secrets and private project details before posting. See [CONTRIBUTING.md](CONTRIBUTING.md) for a short guide.

## What Atlas can do

- **Navigate code:** browse files, classes, structs, members, functions, and methods with source locations and Clang diagnostics. Open original files in your configured editor and rescan after editing.
- **Explore relationships:** inspect an interactive UML-style class graph with inheritance and field-type edges, a selected-class detail view, a minimap, pan, zoom, and a toggle for isolated classes. The Overview tab also shows cross-file static calls.
- **Compare expected and actual behavior:** compare `.facts` files or ordered `.trace` event streams and find the first mismatch.
- **Inspect a runtime map:** load a `.trace` and step through variables, heap objects, and named links such as `owns` or `points_to`. The Interactions tab puts AST evidence and recorded runtime links in one view without pretending that a matching name proves a source-to-runtime identity.
- **Debug a launched program:** on Windows x64, start a debug build, set source breakpoints, step, and inspect available stack frames and simple local values. This uses the Windows Debug API and DbgHelp; line-level information needs matching PDB files.
- **Give agents bounded context:** request JSON project maps, symbol searches, task-focused context, exact source slices, direct file relations, and interaction evidence through the CLI or optional Node.js MCP bridge.

The GUI also has a **Generate .md cards** action. It writes generated `.agent.md` files beside source files and an index under `.project-atlas/generated`. Atlas recognizes its own generated cards and does not overwrite unrelated user-authored files. Card contents are AST navigation facts, not a claim about a function's runtime behavior.

## Build from source

On Windows, install CMake 3.20+, Ninja, Visual Studio C++ build tools with a Windows SDK, and an LLVM installation containing `clang-cl`, `clang-c/Index.h`, the libclang import library, and `libclang.dll`. In **Developer PowerShell for Visual Studio**:

```powershell
cmake -S . -B build-clang -G Ninja `
  "-DLIBCLANG_ROOT=<your-LLVM-root>" `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-clang --parallel 4
ctest --test-dir build-clang --output-on-failure
```

Run `build-clang/project_atlas_gui.exe`, optionally passing a project folder as its first argument. Atlas is a native Win32 application, not a browser-based UI. The CTest suite includes a hidden GUI smoke test that scans this project and checks the facts table and runtime graph.

For development of Atlas itself, `-DATLAS_SELF_HOST_CARDS=ON` makes a build refresh Atlas-owned adjacent cards and the generated index after C++ source changes. This option is off by default so a normal build does not write beside source files.

## CLI and agent access

The following examples assume a local build in `build-clang`:

```powershell
build-clang/project_atlas.exe agent map . build-clang
build-clang/project_atlas.exe agent find . atlas::Symbol build-clang
build-clang/project_atlas.exe agent context . --task scan_project --budget 1500 --compile-db build-clang
build-clang/project_atlas.exe agent source . src/scan.cpp 1 30
build-clang/project_atlas.exe agent file-relations . src/scan.cpp build-clang
build-clang/project_atlas.exe agent interactions . --compile-db build-clang --trace examples/runtime_memory.trace --step 5
build-clang/project_atlas.exe agent compare examples/memory_expected.facts examples/memory_actual.facts
build-clang/project_atlas.exe agent trace examples/memory_expected.trace examples/memory_actual.trace
```

`map` returns up to 200 files. `find` narrows results by path, name, call, or Clang USR. The `project-atlas.agent.v1` responses include paths, source locations, diagnostics, hashes, and `clean`/`truncated` indicators. Check those indicators before treating a result as complete. A Clang USR identifies a symbol; a display name or hash alone does not.

`context` ranks files and symbol/call matches lexically for a task and returns hash-checked snippets. A missing match is **not** proof that code is unrelated. Its `--budget` accepts 400–4000 approximate tokens and bounds the response to four UTF-8 bytes per requested token; it is not an exact model tokenizer. If `truncated` is true, narrow the task or request an exact source range. `source` returns 1–80 numbered C/C++ lines without an AST scan and accepts an optional FNV-1a `expected-hash` to reject stale source. `file-relations` reports direct incoming and outgoing static evidence, not a complete impact map. Dynamic dispatch and indirect calls may be unresolved.

The optional Node.js MCP bridge exposes nine read-only analysis tools: project map, find, context, source, file relations, interactions, facts comparison, trace comparison, and runtime map. After building the C++ CLI:

```powershell
npm ci
npm run test:mcp
```

The bridge also exposes 14 `atlas_ui_*` tools for a running native window: list/status/show, load trace, set step, select tab, scan, record run, focus file or symbol, compare facts, generate cards, and UML overview/focus. Six `atlas_debug_*` tools control Atlas's embedded debugger. The local GUI control channel accepts commands only from the same Windows user. Recording runs an instrumented executable and writes a trace; debugging runs the selected debug executable. Neither is a read-only action.

The same controls are available directly from the CLI:

```powershell
build-clang/project_atlas.exe agent ui list
build-clang/project_atlas.exe agent ui load-trace examples/runtime_memory.trace
build-clang/project_atlas.exe agent ui step 5
build-clang/project_atlas.exe agent ui status
```

If multiple Atlas windows are open, get a PID from `agent ui list` and pass it as the final argument. Scan and record requests are asynchronous; poll `status` until `scanning` or `recording` becomes false. The UI response schema is `project-atlas.ui.v1`.

For a local Codex MCP setup, copy `.codex/config.example.toml` to the untracked `.codex/config.toml`. The example uses relative paths and assumes the repository root as the working directory. [AGENTS.md](AGENTS.md) describes Atlas's agent workflow.

## Export a project scan

```powershell
build-clang/project_atlas.exe scan `
  path/to/cpp-project path/to/generated-atlas path/to/cpp-project/build
```

Open `generated-atlas/index.html` for the offline export. It includes an `index.md`, `classes.md`, and per-file `.agent.md` cards. The output directory must be empty or already contain Atlas's `.project-atlas-generated` marker; Atlas refuses to overwrite an unrelated nonempty directory. The third argument is optional and points to a `compile_commands.json` directory. Atlas reads include paths, macros, and language standard from the compilation database, but does not yet translate every compiler-driver-specific flag. Exit code `2` indicates a Clang error or a translation unit that could not be parsed.

Use `scan-in-place` instead of `scan` only when you explicitly want generated cards beside source files. It refreshes Atlas-marked cards, but not an unrelated file with the same name. Keep human-written contracts separate from generated facts.

## Facts and traces

A `.facts` file contains one `key=value` entry per line; `#` starts a comment. Keys may use ASCII letters, digits, `_`, `.`, and `-`. Values are compared exactly. Expected values should come from requirements or confirmed tests, not from the scanner.

```powershell
build-clang/project_atlas.exe compare `
  examples/memory_expected.facts `
  examples/memory_actual.facts `
  build-clang/memory_comparison.md
```

The comparison exits `0` on a match, `2` on a mismatch, and `1` on a file or invocation error. C++ tests may opt in to writing observations with `atlas::ObservationWriter`.

An ordered `.trace` starts with `# atlas-trace-v1`; each subsequent line has tab-separated `step`, `operation`, `resource`, and `value` fields. Steps start at 1 and have no gaps. They represent logical order, not wall-clock time. `atlas::TraceWriter` records these events. Trace comparison is strictly positional: an inserted or missing event is itself a divergence. For the supplied example, the first mismatch is step 2 (`Cache` instead of `ModelRuntime`).

```powershell
build-clang/project_atlas.exe agent trace `
  examples/memory_expected.trace examples/memory_actual.trace
```

The JSON includes `expected_count`, `actual_count`, and `first_difference`. The command exits `0` for a full match, `2` for a difference, and `1` for invalid input. The `memory_trace_test` actually allocates and releases a buffer while recording its observations.

## Runtime map and recording

`atlas::RuntimeWriter` records typed `var.create`, `obj.create`, `link.add`, `link.remove`, `var.destroy`, and `obj.destroy` events. An ID represents one entity lifetime: reusing an ID, linking a missing node, or destroying a node while a link remains active is invalid. A reused memory address does not imply the same object lifetime. Link labels such as `owns` are supplied by the test or program; Atlas does **not** infer or prove ownership from C++ source.

```powershell
build-clang/project_atlas.exe agent runtime examples/runtime_memory.trace 5
build-clang/project_atlas_gui.exe --runtime-demo . examples/runtime_memory.trace
```

At step 5 in the example, `buffer` and `alias` both link to one object: the trace labels one edge `owns` and the other `points_to`. In the GUI, use **Load trace** on the Runtime map tab and navigate with the slider. The CLI returns the full graph; the GUI warns if it cannot fit all nodes on the canvas.

On Windows, Atlas can also record global C++ `new`/`delete` from a program relinked with `src/auto_runtime.cpp`. You do not need to edit that program's source, but Atlas cannot capture an uninstrumented executable. For CMake, set `PROJECT_ATLAS_SOURCE_DIR` to your Atlas checkout and add:

```cmake
target_sources(my_app PRIVATE "${PROJECT_ATLAS_SOURCE_DIR}/src/auto_runtime.cpp")
```

Build with matching debug symbols/PDB to map allocation sites to functions and lines. **Record run** launches the executable, creates a new trace under `.project-atlas/captures/`, and opens it. A direct CLI example is:

```powershell
build-clang/project_atlas.exe capture build-clang/project_atlas_auto_demo.exe auto-demo.trace
build-clang/project_atlas.exe agent runtime auto-demo.trace 6
```

Recorded allocation links are labeled `allocates`; they do **not** establish pointer ownership. When symbols are available, Atlas writes an enriched trace and retains the raw capture as `.trace.raw`. Existing traces are not overwritten. Capture stops at 10,000 events and marks the result truncated. It covers global C++ `new`/`delete`, not `malloc`, custom allocators, or allocations inside unrelated DLLs. It records one run for later playback; it is not live process control.

## Embedded debugger

The Debugger tab and `atlas_debug_*` agent commands run on Windows x64 through the system Debug API and DbgHelp. Use a Debug build with matching PDB files for source breakpoints and local variables. Atlas launches the debuggee itself; it does not attach to an arbitrary running process.

```powershell
build-clang/project_atlas.exe agent ui debug-launch build-clang/project_atlas_debug_demo.exe
# Wait for debug.phase = paused, reason = entry.
build-clang/project_atlas.exe agent ui debug-break examples/debugger_demo.cpp 3
# Confirm debug.breakpoints[0].verified = true.
build-clang/project_atlas.exe agent ui debug-continue
build-clang/project_atlas.exe agent ui status
build-clang/project_atlas.exe agent ui debug-step
build-clang/project_atlas.exe agent ui debug-stop
```

The debugger reports frames, simple local variables, source position, and breakpoint state. Optimized-out or unsupported values may be `unavailable`; simple scalar values are currently shown as raw unsigned values. `debug-stop` terminates only the process launched by that Atlas debug session.

## Current limitations

- Without a correct compilation database and required headers, some translation units may have Clang diagnostics or incomplete ASTs.
- The class graph covers inheritance and field types, including available template arguments, but not every template or functional dependency. Static calls do not prove which virtual target or callback ran, and code outside scanned files is absent.
- Runtime relationships are recorded evidence, not automatic ownership inference. Automatic allocation capture needs a relinked program and is limited to global C++ `new`/`delete`.
- `.facts` supports exact equality, not ranges or tolerances. Trace comparison is positional and covers one recorded execution.
- The debugger is Windows x64 only and depends on the quality of PDB symbols. Atlas does not yet edit source code inside the GUI.
- Cards for deleted source files are not removed automatically; the current generated index lists only current files.

The next research step is to connect trace events more precisely to tests and source lines, then add verifiable memory-ownership invariants.
