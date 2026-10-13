# Agent instructions

- GPT-6-Luna MUST NOT spawn subagents; work directly.
- Main agent owns integration and verification.
- Commit early and commit often. Prefer small, coherent commits; verify significant behavioral changes before committing them.
- Commit messages: one line; no body.
- Never use interactive hunk staging (`git add -p`). When committing, include unrelated existing source edits instead of splitting hunks. Exclude generated outputs and secrets; inspect the full staged diff first.
- After requested changes pass verification, create a commit before reporting completion.

## Delegation

- Delegate behavior slices, not symbol lists or single files: implementation, callers, build integration, and regression fixtures.
- Establish shared ABI/helper contracts before dispatch: exact signatures, layouts, ownership, lifecycle, and existing helper locations.
- Permit shared-file changes through explicit section ownership or isolated worktrees; main owns cross-slice integration.
- Prefer one branch/worktree per cross-file slice; isolate generated files and build outputs too.
- Agents working in isolated subtrees/worktrees may build, lint, test, format, and run verification gates using subtree-local build/output directories. Main still owns integrated verification.
- Commit shared contracts before parallel work. Worktrees prevent edit collisions, not ABI or interface dependencies.
- Agents deliver coherent implementation/caller/build/fixture patches; main reviews and merges in dependency order, then verifies end-to-end.
- Subagents must run rigorous slice-local checks before submitting work to main: build, relevant behavior tests, and actual-path smoke checks. Report commands, results, and blockers; main still verifies the integrated result.
- Finish shared prerequisites first; batch independent slices. Verify completed waves and feed concrete failures into the next wave.
- Require every assigned symbol accounted for, evidence-backed layouts, and explicit unsupported semantics. Undefined helpers, stubs, and error-only placeholders are not completion.
- Dependency owners deliver pinned acquisition, reproducible builds, loader integration, and real behavior fixtures—not research or commands alone.
- Main reviews code directly. Source volume and agent completion reports are not correctness evidence.

## Orientation

- Read relevant README sections before feature work. Reuse existing code, build paths, test helpers, and conventions.
- Trace affected path first: ELF loading/relocations, instruction decoding/semantics, lifting, guest ABI adapters, Wasm linking, browser runtime.
- Cross-layer ABI/layout changes: update producers, consumers, offsets, and size assertions together. Rebuild affected artifacts; never mix incompatible versions.
- Preserve guest calling conventions, pointer ownership, TLS, register state, and host graphics state. Native host pointers are not guest addresses.

## Build variant target names

- Native i386 targets MUST use `<name>-i386`.
- Direct source-to-Wasm targets MUST use `<name>-wasm`.
- ELF conversion targets from i386 to Wasm MUST use `<name>-i386-to-wasm`.
- Keep these suffixes distinct; update all callers and documentation when renaming.

## Binary analysis

- User observations are ground truth. Name exact unknown and required evidence before tracing.
- Read existing audited code/comments and available source implementations first. Source explains semantics; the installed binary proves ABI, offsets, and exact behavior.
- Portal references: Valve's `source-sdk-2013`; `SonicEraZoR/Portal-Base` for Portal-specific behavioral leads. Neither proves retail ABI or offsets.
- Trace Portal errors in online Source SDK/reference source first: exact message, emitting function, failure condition, and callers. Cite repository/path; reference source does not prove retail behavior.
- Clone reference repositories locally when repeated searches or cross-file tracing are needed. Reuse existing clones; use official upstream when available, record URL and checked-out revision, and keep reference clones outside this repository. Never add reference source as a project dependency or modify it without need.
- Never guess offsets, transplant offsets between builds, or treat PIC/string matches as proof. Validate binary identity and relevant instructions, call targets, or vtable slots.
- Use Ghidra for retail ABI/offset analysis. If unavailable, download official release into `.tools/`. Reuse saved analyzed projects; keep projects outside dot-prefixed directories.
- Preserve analysis and verified symbol names. Label only evidence-backed discoveries; distinguish candidates from proven facts.
- Batch independent queries per module. Read narrow excerpts; assembly only for specific operands, indirect calls, patch targets, or unreliable decompilation.
- GPT-6-Luna MUST consult authoritative online documentation for instruction behavior; prefer vendor ISA manuals, and cite the specific source used in analysis.
- Empty search: change evidence source/query. Bad recovery: repair reusable analysis tooling, not recurring throwaway scripts.
- Extend reusable helpers for repeated queries. Keep compact verified facts near affected code: binary/hash, module-relative address, signature/layout, ownership/order, evidence.
- Stop tracing when required behavior is proven. Implement and exercise affected path; no unrelated binary inventory.

## Testing and verification

- Conversion terminology is enforced. The overall process is **conversion**, with
  **lifting** (x86 to LLVM bitcode), **compiling** (LLVM bitcode to Wasm object
  files), and **linking** (Wasm object files to one final binary). Use these names
  consistently for stage identifiers, progress/log output, documentation, and tests;
  do not use aliases.
- The generic word "test" refers to an i386 program lifted to Wasm and checked for correct behavior in Wasm; use the native-versus-Wasm `run_i386.py` path.
- Use this repository's CMake/CTest and existing native-versus-Wasm fixtures; do not introduce another test framework without need.
- Prefer vendored tests for regression coverage; do not add project-owned regression tests when a vendored test exercises the behavior.
- Custom tests MUST NOT be written for libc functions; vendor the unchanged glibc test instead.
- Place vendored tests in `test/vendored/<vendor>/`.
- Vendored test sources MUST remain unchanged from their pinned vendor copies. If a test depends on a facility unavailable in the browser (for example, `cmp` via `system()`), exclude it from browser registration rather than adapting the vendored source.
- For each libc import, vendor the glibc test(s) that exercise that function under `test/vendored/glibc/<upstream-subdir>/`, unchanged; record the upstream revision and source path.
- Register the test with CMake/CTest and `run_i386.py` when it directly checks the imported function, is deterministic, and its required facilities are supported by the runner. Otherwise keep the source unchanged and document why it cannot be registered; do not weaken or rewrite it.
- Run the native baseline, then iterate on the import implementation until the native-versus-Wasm vendored test passes. Cover relevant results, errors, and boundary behavior; a successful lift or link is not a passing test.
- Permanent tests protect consumer-visible behavior, boundaries, invariants, transitions, and failure cases. No source-text/wiring assertions or mock echoes.
- Keep helper source in separate language-appropriate files; reuse shared build/test utilities. No embedded source strings for multi-language fixtures.
- Exercise actual changed path after build. For instruction/ABI changes, compare native and lifted execution where practical.
- Graphics checks must distinguish orientation, depth, alpha, and ordering; uniform frames cannot prove these properties.
- Separate build, isolated-test, browser/render, and actual gameplay evidence. Linking or reaching an error path does not establish Portal compatibility.
- Report unobserved behavior and remaining blockers explicitly. Restart/reload after rebuilding affected components.
- Answers explaining slow conversion MUST report bitcode and Wasm sizes.

## Documentation

- Markdown: extremely terse; minimum words, meaning intact. No filler.
- README: user-facing setup, build, launch, controls, troubleshooting. Keep contributions minimal.
- Internals: code comments, tests, AGENTS.md. Tests document behavior; avoid duplicating assertions in README.
- If documentation becomes outdated, erase the outdated information instead of updating it.

## Environment safety

- If a required toolchain, sysroot, or development library is unavailable, acquire a pinned distribution under `~/.local/opt/` instead of requiring host-wide package installation. Record its source and version; never overwrite an existing user toolchain.
- Steam: non-root user; writable runtime directories. Never store credentials in tracked files or chat.
- Use `~/emsdk-6` (Emscripten 6.0.12, LLVM 24) for compiling and linking Wasm from LLVM 23 bitcode. Source `~/emsdk-6/emsdk_env.sh`; its `emcc` and `em++` are under `~/emsdk-6/upstream/emscripten`.
- Do not use `~/emsdk` (Emscripten 4.0.9, LLVM 21) with LLVM 23 bitcode; its reader rejects LLVM 23 attributes.
- Diagnose sandbox/display/GPU failures before changing permissions. No blanket `xhost +`, privileged containers, or Docker-socket shortcuts.
- Host-wide security-policy changes require explicit approval; document risk and restoration. Prefer narrow, scoped permissions.
