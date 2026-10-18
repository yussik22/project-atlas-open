# Third-party notices

The Windows binary package includes `libclang.dll` from LLVM/Clang 23.1.3.
LLVM is licensed under Apache-2.0 WITH LLVM-exception. Its full license text
is included as `third_party/llvm/LICENSE.txt` in the source tree and as
`LLVM-LICENSE.txt` beside the packaged binaries.

The MIT `LICENSE` in this repository applies to Project Atlas code, not to
LLVM/Clang or other separately licensed dependencies. The optional Node.js
MCP bridge installs its dependencies from `package-lock.json`; it does not
vendor those packages in the public source archive.
