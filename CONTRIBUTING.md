# Contributing to Project Atlas

Thanks for your interest in Atlas. This is an early Windows x64 preview, so reproducible feedback is especially useful.

- [Report a bug](https://github.com/yussik22/project-atlas-open/issues/new/choose) with the Atlas version, reproduction steps, expected behavior, and actual behavior.
- [Propose a specific improvement](https://github.com/yussik22/project-atlas-open/issues/new/choose) using the feature request form.
- Use [Discussions](https://github.com/yussik22/project-atlas-open/discussions) for early ideas, questions, and design conversations.

Search existing issues and discussions before opening a new one. Please do not post credentials, private project files, or unredacted local paths in public issues. A minimal standalone example is more helpful than a large private project.

For code changes, keep a change focused, explain its expected behavior, and run the relevant checks described in [README.md](README.md). On Windows, the core checks are `cmake --build build-clang` and `ctest --test-dir build-clang --output-on-failure`; MCP changes also need `npm run test:mcp`. A pull request should say what changed, why, and which checks passed.
