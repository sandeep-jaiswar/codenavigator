# CodeNavigator

CodeNavigator is a local-first C++23 code-intelligence CLI for AI agents. It builds a deterministic evidence graph so agents can navigate symbols, relationships, call paths, and impact sets without repeatedly scanning a repository with shell search tools.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The current implementation has no third-party build or runtime dependencies.

## Quick Start

```bash
# Index the current workspace.
./build/codenavigator .

# Ask structured questions.
./build/codenavigator query workspace-map
./build/codenavigator query locate QueryEngine
./build/codenavigator query inspect QueryEngine
./build/codenavigator query relations QueryEngine
./build/codenavigator query impact Json --depth 4
./build/codenavigator query context QueryEngine --budget 800

# Keep the index updated with the portable polling watcher.
./build/codenavigator . --watch

# Expose the seven tools over MCP stdio.
./build/codenavigator mcp --workspace .
```

Indexes are stored outside the repository under the platform user-cache directory. Normal indexing never changes project files.

## Implemented Vertical Slice

- Stable 128-bit IDs and deterministic index generations.
- Structural extraction for C, C++, Objective-C, C#, Python, Java, Kotlin, Scala, JavaScript, TypeScript, Go, Rust, Ruby, PHP, Swift, Dart, Lua, Zig, Bash, Elixir, and Haskell.
- File, symbol, containment, import, and call graph construction.
- Explicit `exact`, `structural`, and `ambiguous` evidence precision.
- Atomic binary index publication and cross-process loading.
- Symbol/path/signature lookup, inspection, relations, bounded traces, and reverse impact analysis.
- Fixed-order, token-budgeted context capsules labeled as untrusted repository evidence.
- MCP tools: `workspace_map`, `locate`, `inspect`, `relations`, `trace`, `impact`, and `context`.
- Portable polling watcher, diagnostics, status output, and an MCP registration template.

## Architecture

The executable is split into three stable layers:

1. `Indexer` discovers files and emits normalized nodes and evidence edges.
2. `EvidenceGraph` owns deterministic storage, indexes, and graph traversal.
3. `QueryEngine` serves the same versioned JSON model to the CLI and MCP.

Parser and persistence concerns are intentionally isolated. Production providers can replace the built-in structural extractor without changing queries or the public schema.

## Production Roadmap

The repository now provides a functional foundation, not the entire 10M LOC production target. The next implementation phases are:

1. Replace regex structural extraction with bundled Tree-sitter grammars.
2. Add SCIP importers plus Clang, TypeScript, Go, Rust, Java, and Kotlin precise providers.
3. Replace full rebuilds with file fingerprints, incremental extraction packets, and delta segments.
4. Move hot graph data into checksummed memory-mapped columnar/CSR segments.
5. Add SQLite WAL metadata, crash recovery, compaction, and snapshot leases.
6. Replace polling with `inotify`, FSEvents, and `ReadDirectoryChangesW`.
7. Add Git working-tree/diff evidence, SCCs, dominators, cycles, test linkage, and package layers.
8. Add native installers, client-specific global MCP registration, benchmarks, fuzzing, and signed releases.

`codenavigator doctor` reports which production providers are currently available.
