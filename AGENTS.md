# Orientation

Read this before any feature work. User-facing build/play/usage details: README.md. Read the relevant README section before editing.

## System

Two processes, one shared player:

- Minecraft (Fabric mod `sourcemc`, `dev.sourcemc.bridge`, in `bridge-base/minecraft`).
- Retail Linux Portal (p1), injected via LD_PRELOAD `build/portal-bridge.so`. Source: `tools/portal_bridge.cpp`; headers: `bridge-base/portal/p1-bridge/include/`.

IPC: shared memory `runtime/portal/bridge.shm` (control MC→native, state/rays/entities native→MC, damage MC→native). Protocol: `bridge-base/shared/include/bridge_protocol.h`; Java mirror `dev.sourcemc.bridge.link.Protocol` + `ControlState`/`GameState` + `BridgeShm`. GPU frames: MC renders world/depth/normals into shared Vulkan-backed images; Portal composites them under its own viewmodel/HUD (`PortalGBuffer`, `FramePassthrough`, `compositor.h`).

Authority split: Portal owns player movement, collision, native triggers, boss defeat. MC owns hearts/death counter, mobs, terrain blocks, UI, camera (MC drives Portal via `CameraSync`). SDL input routed by Portal, consumed by MC (`input_router.h`, `PortalInput`).

## Feature entry points

- MC first-person/hand render: `client/mixin/GameRendererMixin`, `ItemInHandRendererMixin`.
- Native viewmodel/HUD/crosshair: `render_hook.h` (IClientMode + `CalcViewModelView` vtable hooks), `CTRL_PORTAL_GUN` flag.
- Native input/buttons/camera: `MinecraftMixin`, `PortalInput`, `CameraSync` (flags `CTRL_*`); native side `source_movement.h`, `portal_gun.h`, `camera_math.h`.
- Damage MC→native: `CombatBridge` → damage ring → `gameplay.h`.
- Terrain/collision: `TerrainManager`, `TerrainBlock` (MC); `block_collision.h`, `collision.h` (native); ray mailbox `OFF_RAYS`.
- Cube collision: native terrain rays omit manifest-matched cube props; placement/player traces retain them. MC cube proxies use solid entity boxes, not terrain voxels. Restart both games after cutover; nearby saved terrain resampled.
- Native rigid bodies: `block_physics.h`; snapshot boxes → cached VPhysics shapes/static bodies. Preserve unchanged bodies; wake props on edits; drop handles before environment destruction.

## Protocol changes

New control/state field must edit, in one commit: `bridge_protocol.h`, `Protocol.java` (offset constant), `ControlState`/`GameState`, `BridgeShm` write/read, native consumer. Take bytes from `reserved*` fields; keep `static_assert` sizes; keep Java offsets in sync. After rebuild restart both games; never mix old/new builds.

## Build and verify

- Both components: `python3 tools/game.py build` (downloads/uses `.tools/jdk`; plain `gradlew` needs `JAVA_HOME=$PWD/.tools/jdk`).
- Java tests: `sh bridge-base/minecraft/gradlew -p bridge-base/minecraft test --no-daemon`.
- Native constraints: `-m32`, `-fno-rtti -fno-exceptions`, no libm (use `p2::sincos`), `-Werror`.

## Hazards

- p1 hooks pin retail `client.so`/`engine.so`/`server.so` SHA256 and audited vtable/data offsets. Offsets from Ghidra only (`python3 tools/game.py audit`); never guess, never copy p2 offsets.
- In-game verification manual; launcher: `python3 tools/game.py`.
- Logs: `[portal-bridge]` prefix → `runtime/portal-diagnostics/`; MC → `bridge-base/minecraft/run/logs/latest.log`.

# Execution

Delegation allowed only to GPT-6 Luna agents, high reasoning effort. Main agent owns integration and verification.

Ghidra unavailable: download official release into `.tools/` before binary analysis.

## Native reverse-engineering bugfixes

Start from user observation; ground truth. Trace affected bridge path before binary analysis.
Rendering: `render_hook.h`, `compositor.h`, `compositor_shaders.h`, `tools/portal_bridge.cpp`.
Reuse existing audited hooks, pinned hashes, GL-state restoration, native test helpers.

Current Portal version based on Source SDK 2013. Code references: clone
`https://github.com/ValveSoftware/source-sdk-2013.git` into `.tools/source-sdk-2013`.
Use SDK for semantics; retail Ghidra remains ABI/offset authority.
Portal-specific gameplay/render/collision: consult
`https://github.com/SonicEraZoR/Portal-Base` (clone to `.tools/portal-base` if needed),
not just generic Source SDK. Community port, not retail-build proof; use for
behavioral leads only. Retail Ghidra remains authority for ABI, offsets, and exact
binary behavior.

### Ghidra: reuse, not scripts per query

1. `python3 tools/game.py audit-state`. Check active module/project; offsets are module-specific.
2. Reuse analyzed project. `audit`/`audit-search` remember successful module, project, Ghidra/JDK paths, report directory in `runtime/portal-audit-state.json`.
3. `--module engine|client|server|PATH` switches selection. Omit on subsequent calls.
4. New module only: add `--import-module` once. Missing cached project: reimport; never silently assume analyzed.
5. New projects: `runtime/portal-ghidra`, not `.tools/`; Ghidra rejects dot-prefixed project directories. Existing saved project may be under `/tmp`; preserve analysis, don't assume reboot persistence.

```sh
python3 tools/game.py audit-search --module engine --match CEngineVGui::Paint
python3 tools/game.py audit --offset 0x4ed480 --excerpt param_2 --context 3
```

- Reports/log paths printed. Read matching ranges, not whole decompilation/assembly.
- `--match`: repeatable case-insensitive literal; per-query references/counts. PIC candidates require instruction verification, not proof alone.
- String search includes undefined ASCII in `.rodata`; scan once per batch. Raw PIC matches remain candidates.
- `--offset`: module-relative, repeatable; functions recovered from ELF unwind bounds; data offsets dump words.
- Once a function/data symbol is identified from evidence, MUST name it in saved Ghidra project: `AGENTFOUND_<inferred_symbol_name>`. No speculative names for unverified candidates.
- Both commands accept repeatable `--rename OFFSET=NAME`; module-relative offset, identifier name, `AGENTFOUND_` prefix added if absent. Functions resolve to entry; data labels use exact address. Names persist; ELF unchanged.
- `--excerpt`: repeatable case-insensitive literal; filters C and assembly with `--context`. Assembly addresses module-relative; gaps explicit. `--assembly` opt-in.
- `audit-search --match operand:0x1542`: literal scalar operands; references/enclosing functions. Check instructions before inferring field meaning.
- Report headers identify module/image base. Convert Ghidra addresses by subtracting reported image base; retail modules here use `0x10000`, not zero.
- Helpers: `tools/PortalAudit.java`, `PortalSearch.java`, `PortalProgram.java`; launcher `portal_analysis.py`. Repeated query/recovery churn → extend reusable command/helper; no recurring throwaway scripts.
- Recovery recognizes exact x86 PIC-thunk bytes; repairs no-return flags/call fixups. No ELF changes.
- Headless exit zero doesn't prove script success. Launcher requires fresh, complete report; failed audit preserves published report and saved context.
- Valve SDK explains names/interfaces; retail Ghidra proves ABI, offsets, arguments, call ordering. Never transplant SDK/p2 offsets.
- Validate pinned hash, opcode/target or vtable method before patching. Preserve original call, calling convention, rollback, executable-page protection.

### Token-efficient tracing

- Before audit: name exact unknown, required evidence, affected bridge path.
- Read existing audited code/comments and SDK 2013 implementation first. Derive
  candidate methods/call chain; retail proves only needed ABI/behavior.
- Batch independent `--match`/`--offset` queries per module. Follow dependencies;
  no broad symbol/string dumps or whole-interface exploration.
- Default `--excerpt` + small `--context`; read matching report ranges only.
  No full-function/full-report reads unless narrow excerpts miss needed evidence.
- Assembly only for specific ABI operands, indirect-call slots, PIC candidates,
  patch targets, or unreliable decompilation. Read relevant instructions only;
  never request decompilation plus full assembly by default.
- Empty search: change evidence source/query once; do not repeat variants blindly.
  Check string/data references, factory registration, or SDK-guided callers.
- Bad PIC-thunk/function recovery: repair saved analysis or reusable helper;
  do not repeatedly compensate with large assembly dumps.
- Keep compact verified facts in relevant code comments: module/hash, relative
  address/vtable slot, signature/layout, ownership/order, audit evidence.
  Separate candidates from proven facts; reuse after binary identity validation.
- Repeated vtable/address-arithmetic queries: extend existing audit helpers for
  compact slot→target/bounds/excerpt reports; no per-query scripts.
- Stop tracing when required ABI/behavior proven. Next: implementation + isolated
  behavioral smoke; no unrelated binary inventory.

### Render-stage traps

- Main world composite: before native translucent renderables; main-view gate excludes portal/reflection/skybox/shadow views.
- HUD composite: audited `RenderView → VGui_PreRender` call, before native panels. Swap-time HUD covers pause menu; `PostRenderVGui` can already follow menu painting.
- Source ToGL target orientation varies by stage. Current world/pre-VGUI HUD sampling flips Y; never carry swap-target orientation into earlier target.
- `GLState` restores engine state; preserve active target/viewport. Depth, premultiplied alpha, native glass/UI ordering all matter.
- GPU planes/protocol: `bridge-base/shared/include/gpu_frames.h`; MC producer `PortalGBuffer`; native receiver/compositor. Trace both sides, not shader alone.
- Retail stencil portal views inherit `CurrentViewID` (can be main=0). Match transformed portal camera before main dispatch; retain native stencil/translucent pass.
- Portal fast-clip projection differs from `ViewSetup.nearZ/farZ`, including main views near crossings. Compare/write depth with audited active shader projection; preserve exit-plane clip and native stencil.
- MC portal pass: sorted frustum-only resident sections. Preserve main connectivity graph/list/camera across pass; virtual camera may lie behind solid wall.

### Prove before delivering

- Build: `python3 tools/game.py build`.
- Isolated native/GL/Ghidra suite: `JAVA_HOME=$PWD/.tools/jdk sh bridge-base/minecraft/gradlew -p test test --no-daemon`.
- Focus render smoke: same command with `--tests CompositingTest`; real GL pixels, no game automation.
- Orientation tests need distinct rows; one pixel/uniform fill cannot detect inversion.
- Tests protect pixels/depth/alpha/order or decompiled behavior; no source-text/wiring assertions.
- Actual menu/glass/particles verification manual only. Report isolated evidence separately from unobserved in-game behavior. Restart both games after rebuild.


Decompiled Minecraft sources (mojmap, 1.21.1) must stay available. Jars:

- client: `bridge-base/minecraft/.gradle/loom-cache/minecraftMaven/net/minecraft/minecraft-clientOnly-9b5ff62f35/1.21.1-loom.mappings.1_21_1.layered+hash.2198-v2/minecraft-clientOnly-9b5ff62f35-1.21.1-loom.mappings.1_21_1.layered+hash.2198-v2-sources.jar`
- common: `bridge-base/minecraft/.gradle/loom-cache/minecraftMaven/net/minecraft/minecraft-common-9b5ff62f35/1.21.1-loom.mappings.1_21_1.layered+hash.2198-v2/minecraft-common-9b5ff62f35-1.21.1-loom.mappings.1_21_1.layered+hash.2198-v2-sources.jar`

Read class source via member path: `read '<jar>:net/minecraft/client/renderer/ItemInHandRenderer.java'`.

All Minecraft-implementation questions: answer by reading these sources. Never answer from memory; no guessing vanilla behavior.

Missing jars (paths change with MC version; glob `minecraftMaven/**/*-sources.jar` first): `JAVA_HOME=$PWD/.tools/jdk PATH=$PWD/.tools/jdk/bin:$PATH sh bridge-base/minecraft/gradlew -p bridge-base/minecraft genSources --no-daemon`. Never delete the result. JDK lives in `.tools/jdk`; plain `sh gradlew` fails without `JAVA_HOME`.

# Testing

Permanent tests are allowed in this repository. Add focused regression tests when they protect consumer-visible behavior, boundaries, invariants, or failure cases.

All future Java or multi-language tests: JUnit; Java orchestrates.
Java tests driven by Gradle; no standalone shell test runners.
Multi-language tests: own folder per test; Java in `.java`, C++ in `.cpp`, other languages in separate native source files. No embedded source strings.
Shared Java test utilities compile C++/other-language helpers; reuse across tests.

All in-game testing must be done manually. Do not automate gameplay, input, or in-game verification. Builds and isolated tests that do not run either game may still be automated.

# Commits

Commit early and commit often. Keep commits focused on coherent, verified changes.
Commit messages: one line only; no body.

# Markdown style

All repository `.md` files: extremely terse, telegraphic. Minimum words; meaning intact. Fragments, omitted articles, incorrect grammar allowed if clear. No filler.

Tests document behavior; no need to repeat it in README.

# README

User-facing usage only: setup, build, launch, play, controls, troubleshooting.
No technical internals; those live in code comments, tests, and AGENTS.md.
Agent contribution to README.md stays minimal.

# Steam in Docker

Run Steam as UID 1000, not root. The existing `node` user has UID 1000; Steam's sandbox check passed under that user and failed under root.

From a root shell in the container:

```sh
setpriv --reuid=1000 --regid=1000 --init-groups \
    /workspace/.tools/steam/launch.sh
```

The ignored `.tools/steam` directory must be writable by UID 1000.

## Container sandbox prerequisites

Steam uses Bubblewrap and requires nested user and mount namespaces. For the Docker setup exercised here, recreate the container with these security options (a restart alone does not apply them):

```yaml
security_opt:
  - seccomp=unconfined
  - apparmor=unconfined
```

Equivalent `docker run` arguments:

```sh
--security-opt seccomp=unconfined \
--security-opt apparmor=unconfined
```

These options remove the container's seccomp and AppArmor confinement. Use them only for this trusted game container. Do not add `--privileged`, `CAP_SYS_ADMIN`, or mount the Docker socket as a shortcut.

## Host user-namespace policy

Check these settings on the Linux Docker host, not inside the container:

```sh
sysctl user.max_user_namespaces
sysctl kernel.unprivileged_userns_clone
sysctl kernel.apparmor_restrict_unprivileged_userns
```

Some distributions do not expose every key. `user.max_user_namespaces` must be nonzero; `kernel.unprivileged_userns_clone`, when present, must be `1`. Change only a setting that is actually blocking namespaces:

```sh
sudo sysctl -w user.max_user_namespaces=28633
sudo sysctl -w kernel.unprivileged_userns_clone=1
```

On the host used here, `kernel.apparmor_restrict_unprivileged_userns=1` still restricted namespace capabilities even with `apparmor=unconfined`. The host audit log showed a transition to `unprivileged_userns` followed by a `sys_admin` denial. The temporary workaround was:

```sh
sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0
```

This relaxes a host-wide security policy, not just this container. Do not apply or persist it automatically; obtain the host owner's approval. Restore the previous value after the game session (it was `1` on this host):

```sh
sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=1
```

A narrower alternative is a host-installed custom AppArmor profile allowing `userns,`, assigned to this container instead of `apparmor=unconfined`. That alternative has not been exercised here.

## Verification and diagnostics

From a root shell inside the recreated container, verify namespace creation as the actual Steam user:

```sh
setpriv --reuid=1000 --regid=1000 --init-groups \
    unshare --user --map-root-user --mount true
```

Success is exit status 0 with no output. Then use the Steam launch command above; a successful namespace check alone does not prove the full Steam sandbox works. Read startup failures from `.tools/steam/logs/console-linux.txt`.

For namespace failures, inspect host audit messages before relaxing more permissions:

```sh
sudo journalctl -k --since "10 minutes ago" \
    | grep -Ei 'apparmor|denied|userns|uid_map'
```

Display/GPU access and 32-bit graphics libraries are separate prerequisites. When sharing the host X11/XWayland display, mount an authorization-cookie file readable by UID 1000; never use blanket `xhost +`. X11 access permits interaction with other applications in that desktop session, so prefer a dedicated session when isolation matters. Authenticate to Steam through its GUI; never store credentials in tracked files or chat.
