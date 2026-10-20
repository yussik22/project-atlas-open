# Public release — Windows x64

Use the prepared `ProjectAtlas-0.11-win-x64.zip` for a portable Windows x64
copy. Extract the whole archive, keep `libclang.dll` beside both executables,
and run `project_atlas_gui.exe`. No LLVM or Visual C++ Debug runtime needs to
be installed. The CLI is `project_atlas.exe`. Atlas has a native Windows GUI;
this archive is **not** a macOS/Linux application.
The binaries are not code-signed; Windows may show an unknown-publisher
warning. Check the included `SHA256SUMS.txt` and the source before running.

The binary archive includes the Project Atlas MIT license, the LLVM license,
third-party notices, and example `.facts`/`.trace` files. The separate source
archive is a curated current-tree export with no `.git` history, build
directories, generated `.agent.md` cards, local Codex configuration, PDBs, or
`node_modules` or internal feedback logs. It includes tests and the optional
MCP bridge. For that bridge,
install Node.js, run `npm ci`, then `node tools/atlas-mcp.mjs` from the source
directory after building the CLI.

## Rebuild on another Windows machine

Install CMake, Ninja, Visual Studio C++ build tools/Windows SDK, and an LLVM
installation containing `clang-cl`, `libclang.dll`, its import library, and
`clang-c/Index.h`. Open a Developer PowerShell and provide your own LLVM root:

```powershell
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  -DATLAS_SELF_HOST_CARDS=OFF -DBUILD_TESTING=OFF `
  "-DLIBCLANG_ROOT=<your-LLVM-root>"
cmake --build build-release --parallel 4
```

`tools/make-public-release.ps1` packages an already built Release tree into
fresh archives under `dist/`. It refuses to overwrite an existing package and
checks the selected source files for common personal paths/credential markers.
The script deliberately excludes Git history; **do not change the private
repository's visibility to public** without auditing its entire history.

The release checks on the build machine cover binary imports, absence of
developer-path strings in the two Atlas executables, unit/GUI tests, and an
isolated extracted-folder smoke test. They cannot prove behavior on every
Windows installation or detect every possible secret. Review archive contents
before publishing.
