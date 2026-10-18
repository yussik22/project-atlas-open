# Project Atlas agent guide

This repository is a C++23/libclang project navigator. Use the Atlas machine-readable
CLI before broad source searches when locating symbols or understanding the AST.

For a task-specific overview, start with `agent context` and its `clean` and
`truncated` flags. Its ranking is lexical, its token budget approximate, and a
missing result is not proof that code is unrelated. Then read
`.project-atlas/generated/index.md` and the linked `summaries/<file>.md` for a
relevant file. Expand to its full `*.agent.md` card and source only as needed.
The card's content hash must match the source; the source and tests are
authoritative. The `agent find`
command also searches functions, methods, and static call sites, not only types.
Read `.project-atlas/generated/calls.md` for cross-file static interactions;
read `.project-atlas/generated/interactions.md` for a combined AST evidence map;
inspect `files[].calls` in JSON for exact source locations and the
`dynamic_dispatch`/`indirect` limitations. A static edge is not a runtime fact.

Useful commands:

- `build-fixed/project_atlas.exe agent find . <query> build-fixed`
- `build-fixed/project_atlas.exe agent context . --task <text> --budget 1500 --compile-db build-fixed`
- `build-fixed/project_atlas.exe agent source . <relative-C++-file> <start-line> <count> [expected-hash]`
- `build-fixed/project_atlas.exe agent file-relations . <relative-C++-file> build-fixed`
- `build-fixed/project_atlas.exe agent map . build-fixed`
- `build-fixed/project_atlas.exe agent interactions . --compile-db build-fixed [--trace <trace>] [--step <n>]`
- `build-fixed/project_atlas.exe agent compare <expected.facts> <actual.facts>`
- `build-fixed/project_atlas.exe agent trace <expected.trace> <actual.trace>`
- `build-fixed/project_atlas.exe agent runtime <runtime.trace> [step]`
- `build-fixed/project_atlas.exe capture <instrumented.exe> <new-output.trace>`
- `build-fixed/project_atlas.exe agent ui list`
- `build-fixed/project_atlas.exe agent ui status [pid]`
- `build-fixed/project_atlas.exe agent ui show [pid]`
- `build-fixed/project_atlas.exe agent ui uml-overview|uml-focus [pid]`
- `build-fixed/project_atlas.exe agent ui load-trace <trace> [pid]`
- `build-fixed/project_atlas.exe agent ui step <number> [pid]`
- `build-fixed/project_atlas.exe agent ui tab <0..6> [pid]`
- `build-fixed/project_atlas.exe agent ui scan <project-root> [pid]`
- `build-fixed/project_atlas.exe agent ui record <instrumented.exe> [pid]`
- `build-fixed/project_atlas.exe agent ui focus-file <path> [pid]`
- `build-fixed/project_atlas.exe agent ui focus-symbol <USR-or-qualified-name> [pid]`
- `build-fixed/project_atlas.exe agent ui compare-facts <expected.facts> <actual.facts> [pid]`
- `build-fixed/project_atlas.exe agent ui generate-cards [pid]`
- `build-fixed/project_atlas.exe agent ui debug-launch <debug-build.exe> [pid]`
- `build-fixed/project_atlas.exe agent ui debug-break <source-file> <line> [pid]`
- `build-fixed/project_atlas.exe agent ui debug-continue|debug-step|debug-stop [pid]`

The JSON schema is `project-atlas.agent.v1`. Check `clean`, `truncated`, and Clang
diagnostics before trusting a map as complete. A symbol's Clang USR is identity;
the display name or hash alone is not. Read the source and tests before editing.
`agent interactions` has its own `project-atlas.interactions.v1` schema. Its
AST and trace edges have different evidence kinds and runtime IDs do not imply
a source-class match; check `capture_truncated` before treating a trace as complete.
`agent context` has schema `project-atlas.context.v1` and caps response bytes at
four times the requested approximate token budget. It does not use an exact
model tokenizer. Snippets are returned only when the current source hash still
matches the scan; if `truncated`, narrow the task or use `agent find`/source.
Use `agent source` after context/find for an exact, bounded line range, passing
the returned FNV-1a hash to reject changed source. It is fast because it does
not scan the AST. Use `agent file-relations` to inspect direct incoming and
outgoing AST evidence for one file before editing. It does scan the project;
its edges are not a complete impact or runtime dependency map.

The nine analysis `atlas_*` MCP tools are read-only. Fourteen `atlas_ui_*` tools
control a running native Atlas window through a local, same-user IPC channel:
list/status/show, load trace, set step, select tab, scan, record run, focus file or
symbol, compare facts, and explicitly generate cards. `record_run`
executes the selected instrumented executable and writes a trace; request it
only within the user's intended project. Scan/record are asynchronous: poll
`atlas_ui_status` until `scanning`/`recording` becomes false. If multiple GUI
instances exist, select one by PID from `atlas_ui_list`. UI status reports
live state and selected file/symbol, not a cached CLI approximation.
`atlas_ui_generate_cards` writes beside source files; invoke it only when asked.
The six `atlas_debug_*` MCP tools use Atlas's embedded x64 Windows debugger;
they do not launch LLDB. Launching executes the chosen project executable and
stopping terminates that exact debuggee. Use a debug build with matching PDB.
Wait for the initial paused state, set a source breakpoint, verify it in
`atlas_debug_status`, then continue. The status contains frames and simple
local variables; unsupported/optimized-out values are reported unavailable.

Preserve user-owned `.agent.md` cards and `.facts`. The user has explicitly
authorized automatic regeneration of Atlas's own generated cards after code
changes. This checkout enables `ATLAS_SELF_HOST_CARDS`: a normal build refreshes
the adjacent cards and `.project-atlas/generated` if C++ sources changed.
On a new checkout, enable this with
`cmake -S . -B build-fixed -DATLAS_SELF_HOST_CARDS=ON` before building.
Verify the card stamp/content hash after the build; never edit generated cards
by hand. Do not generate in-place cards for other projects without authorization.
After code changes, build and run CTest; after MCP changes, run
`npm run test:mcp`. Do not commit or push unless the user requests it.

Self-hosting loop: use `agent find` to locate the affected Atlas symbol, make a
small change, build/CTest, then scan Atlas itself in the GUI and verify
`agent ui status` (zero Clang diagnostics and expected UML counts). Record
observed UI failures separately from inferred causes; a clean AST scan does
not establish that rendering or debugger behavior is correct.
