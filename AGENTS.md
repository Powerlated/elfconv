# Agent instructions

- GPT-6-Luna MUST NOT spawn subagents; work directly.
- Main agent owns integration and verification.
- Commit early and commit often. Prefer small, coherent commits; verify significant behavioral changes before committing them.
- Commit messages: one line; no body. Keep unrelated user changes out of commits.
- After requested changes pass verification, create a focused commit before reporting completion. Stage only files or hunks belonging to the current task and inspect the staged diff first. Never include unrelated user changes, generated outputs, or secrets. If task changes cannot be safely isolated, do not commit; explain the blocker.

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

## MUSTFIX: Portal conversion blocker

- MUST fix function-boundary recovery and indirect-tail dispatch before treating Portal conversion as supported. More chunks or `-O0` are not fixes.
- `client.so` SHA-256: `00b6e45eb84a93f0974c23d3e2656aa9e32e9ada64443f651899a78cde694aae`. Guest entry `0x2bf98c0` = ELF `0x6798c0`; `.eh_frame` range `[0x6798c0, 0x6798e2)` (34 bytes). Direct tail jumps reach helpers at ELF `0x679860` and `0x679800`; Ghidra follows 210 bytes, ending one path in virtual dispatch via `jmp eax`.
- Partial `.symtab` selects `LoadStaticSymbolsBFD` instead of unwind recovery (`src/lifter/Binary/Loader.cpp`). Missing entries become `_ecv_rest_fun`; `AddRestDisasmFunc` bounds them by the next known entry. `TraceLifter.cpp` exhaustively lifts that assumed range after encountering an indirect jump and lists every lifted instruction block as an indirect destination.
- Preserved `_ecv_rest_fun_____2bf98c0`: 37,423 LLVM blocks, 306,956 instructions, 37,420 indirect destinations; 33 direct lifted call sites. Only 15 instruction blocks lie inside the 34-byte native entry. This is not evidence of a giant native function.
- Required: merge symbol/unwind boundaries, preserve extracted unwind sizes, and distinguish inter-function tail dispatch from local indirect branches without discarding valid guest targets. Rebuild/reconvert; verify recovered boundaries, native-versus-Wasm behavior, and actual compilation timing.
- Reproduction: `unit-49367809dea17a587bf5e69d.chunk-28.bc`, 3,342,624 bytes; `-O1` Wasm object, 2,805,948 bytes. Compiler passes: ~428 s; CFG Stackify 164 s, CodeGen Prepare 128 s, Register Coloring 69 s. `-O0`: ~79 s.
- Evidence: `/tmp/elfconv-compile-investigation/` (preserved cache, ELF, inspection helpers/reports); saved Ghidra program `build/portal-ghidra/PortalAudit:/client.so`. Temporary evidence may disappear; binary hash and module-relative addresses identify the reproduction.

## Testing and verification

- Conversion terminology is enforced. The overall process is **conversion**, with
  **lifting** (x86 to LLVM bitcode), **compiling** (LLVM bitcode to Wasm object
  files), and **linking** (Wasm object files to one final binary). Use these names
  consistently for stage identifiers, progress/log output, documentation, and tests;
  do not use aliases.
- The generic word "test" refers to an i386 program lifted to Wasm and checked for correct behavior in Wasm; use the native-versus-Wasm `run_i386.py` path.
- Use this repository's CMake/CTest and existing native-versus-Wasm fixtures; do not introduce another test framework without need.
- Prefer vendored tests for regression coverage; do not add project-owned regression tests when a vendored test exercises the behavior.
- Place vendored tests in `test/vendored/<vendor>/`.
- Vendored test sources MUST remain unchanged from their pinned vendor copies. If a test depends on a facility unavailable in the browser (for example, `cmp` via `system()`), exclude it from browser registration rather than adapting the vendored source.
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

- Steam: non-root user; writable runtime directories. Never store credentials in tracked files or chat.
- Emscripten SDK is installed at `~/emsdk`; source `~/emsdk/emsdk_env.sh` to add its tools to `PATH`.
- Diagnose sandbox/display/GPU failures before changing permissions. No blanket `xhost +`, privileged containers, or Docker-socket shortcuts.
- Host-wide security-policy changes require explicit approval; document risk and restoration. Prefer narrow, scoped permissions.
