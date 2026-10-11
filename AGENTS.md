# Agent instructions

- GPT-6-Luna MUST NOT spawn subagents; work directly.
- Main agent owns integration and verification.
- Commit early and commit often. Prefer small, coherent commits; verify significant behavioral changes before committing them.
- Commit messages: one line; no body. Keep unrelated user changes out of commits.

## Delegation

- Delegate behavior slices, not symbol lists or single files: implementation, callers, build integration, and regression fixtures.
- Establish shared ABI/helper contracts before dispatch: exact signatures, layouts, ownership, lifecycle, and existing helper locations.
- Permit shared-file changes through explicit section ownership or isolated worktrees; main owns cross-slice integration.
- Prefer one branch/worktree per cross-file slice; isolate generated files and build outputs too.
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

## Binary analysis

- User observations are ground truth. Name exact unknown and required evidence before tracing.
- Read existing audited code/comments and available source implementations first. Source explains semantics; the installed binary proves ABI, offsets, and exact behavior.
- Portal references: Valve's `source-sdk-2013`; `SonicEraZoR/Portal-Base` for Portal-specific behavioral leads. Neither proves retail ABI or offsets.
- Never guess offsets, transplant offsets between builds, or treat PIC/string matches as proof. Validate binary identity and relevant instructions, call targets, or vtable slots.
- Use Ghidra for retail ABI/offset analysis. If unavailable, download official release into `.tools/`. Reuse saved analyzed projects; keep projects outside dot-prefixed directories.
- Preserve analysis and verified symbol names. Label only evidence-backed discoveries; distinguish candidates from proven facts.
- Batch independent queries per module. Read narrow excerpts; assembly only for specific operands, indirect calls, patch targets, or unreliable decompilation.
- Empty search: change evidence source/query. Bad recovery: repair reusable analysis tooling, not recurring throwaway scripts.
- Extend reusable helpers for repeated queries. Keep compact verified facts near affected code: binary/hash, module-relative address, signature/layout, ownership/order, evidence.
- Stop tracing when required behavior is proven. Implement and exercise affected path; no unrelated binary inventory.

## Testing and verification

- The generic word "test" refers to an i386 program lifted to Wasm and checked for correct behavior in Wasm; use the native-versus-Wasm `run_i386.py` path.
- Use this repository's CMake/CTest and existing native-versus-Wasm fixtures; do not introduce another test framework without need.
- Prefer finding and reusing existing repository or upstream behavior/conformance tests over writing new ones. Search first; add tests only for uncovered behavior.
- Place vendored tests in `test/vendored/<vendor>/`.
- Permanent tests protect consumer-visible behavior, boundaries, invariants, transitions, and failure cases. No source-text/wiring assertions or mock echoes.
- Keep helper source in separate language-appropriate files; reuse shared build/test utilities. No embedded source strings for multi-language fixtures.
- Exercise actual changed path after build. For instruction/ABI changes, compare native and lifted execution where practical.
- Graphics checks must distinguish orientation, depth, alpha, and ordering; uniform frames cannot prove these properties.
- Separate build, isolated-test, browser/render, and actual gameplay evidence. Linking or reaching an error path does not establish Portal compatibility.
- Report unobserved behavior and remaining blockers explicitly. Restart/reload after rebuilding affected components.

## Documentation

- Markdown: extremely terse; minimum words, meaning intact. No filler.
- README: user-facing setup, build, launch, controls, troubleshooting. Keep contributions minimal.
- Internals: code comments, tests, AGENTS.md. Tests document behavior; avoid duplicating assertions in README.
- If documentation becomes outdated, erase the outdated information instead of updating it.

## Environment safety

- Steam: non-root user; writable runtime directories. Never store credentials in tracked files or chat.
- Emscripten SDK is installed at `~/emsdk`; source `~/emsdk/emsdk_env.sh` to add its tools to `PATH`.
- Diagnose sandbox/display/GPU failures before changing permissions. No blanket `xhost +`, privileged containers, or Docker-socket shortcuts.
- Host-wide security-policy changes require explicit approval; document risk and restoration. Prefer narrow, scoped permissions.
