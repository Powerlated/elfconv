#!/usr/bin/env python3
"""Lift ELF images, compile their bitcode, and link the elfconv runtime."""
from __future__ import annotations

import argparse
import asyncio
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import sys
import time
from collections.abc import AsyncIterator, Awaitable, Callable
from typing import TypeVar

from rich.console import Console
from rich.live import Live
from rich.spinner import Spinner
from rich.text import Text
from rich.tree import Tree

from pack_preload import pack_preload

Item = TypeVar("Item")
Result = TypeVar("Result")

ROOT = Path(__file__).resolve().parents[1]
COMMON = [ROOT / "src" / path for path in (
    "runtime/Entry.cpp", "runtime/Memory.cpp", "runtime/Runtime.cpp",
    "runtime/VmIntrinsics.cpp", "utils/Util.cpp", "utils/elfconv.cpp")]


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def key(*values: object) -> str:
    return hashlib.sha256(json.dumps(values, sort_keys=True, default=str).encode()).hexdigest()


async def bounded_as_completed(items: list[Item], semaphore: asyncio.Semaphore,
                               worker: Callable[[Item], Awaitable[Result]]
                               ) -> AsyncIterator[Result]:
    """Yield worker results as ready, with bounded concurrency and sibling cleanup."""
    async def run(item: Item) -> Result:
        async with semaphore:
            return await worker(item)

    tasks = [asyncio.create_task(run(item)) for item in items]
    try:
        for task in asyncio.as_completed(tasks):
            yield await task
    finally:
        for task in tasks:
            if not task.done():
                task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


def allocate_chunks(entries: list[tuple[str, Path, str, int, int]],
                    requested: int) -> dict[str, int]:
    """Apportion a global chunk budget by ELF code size, with one per owner."""
    functions = sum(entry[3] for entry in entries)
    target = min(max(requested, len(entries)), functions)
    chunks = {entry[0]: 1 for entry in entries}
    weights = {entry[0]: max(entry[4], 1) for entry in entries}
    for _ in range(target - len(entries)):
        owner = max(
            (entry[0] for entry in entries if chunks[entry[0]] < entry[3]),
            key=lambda value: (weights[value] / chunks[value], value))
        chunks[owner] += 1
    return chunks




def atomic_text(path: Path, text: str) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text)
    temporary.replace(path)


def tool(value: str | None, *names: str) -> str:
    candidates = [value] if value else list(names)
    for candidate in candidates:
        if candidate and (found := shutil.which(candidate)):
            # clang++/em++ select their driver mode from argv[0].
            return os.path.abspath(found)
    raise ValueError(f"Executable not found: {value or ', '.join(names)}")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--input", type=Path, required=True)
    result.add_argument("--target", required=True, choices=[
        f"{arch}-{kind}" for arch in ("i386", "amd64", "aarch64")
        for kind in ("native", "wasm", "wasi32")])
    result.add_argument("--output-dir", type=Path, default=Path.cwd())
    result.add_argument("--elf-name")
    result.add_argument("--lifter")
    result.add_argument("--emcc")
    result.add_argument("--native-cxx")
    result.add_argument("--llvm-dis")
    result.add_argument("--wasi-sdk", type=Path)
    result.add_argument("--wasmedge")
    result.add_argument("--output-executable", type=Path)
    result.add_argument("--entry-symbol")
    result.add_argument("--shared-library", type=Path, action="append", default=[])
    result.add_argument("--mount", action="append", default=[], metavar="HOST_DIR@/GUEST_PATH")
    result.add_argument("--runtime-archive", type=Path,
                        help="Prebuilt runtime archive matching the conversion configuration")
    result.add_argument("--bitcode-path", type=Path)
    result.add_argument("--link-map", type=Path)
    result.add_argument("--object-base", type=Path)
    result.add_argument("--wasm-opt-level", choices=list("0123sz"), default="3")
    result.add_argument("--no-asyncify", action="store_true",
                        help="Do not instrument browser Wasm with Asyncify.")
    result.add_argument("--legacy-gl", choices=("0", "1"), default="1")
    result.add_argument("--lift-jobs", type=int, default=2,
                        help="Maximum concurrent ELF lifting processes")
    result.add_argument("--compile-jobs", type=int, default=8,
                        help="Maximum concurrent compiler jobs")
    result.add_argument("--ir-chunks", type=int, default=0, metavar="N",
                        help="Target total linkable i386 chunks, apportioned across ELF owners.")
    result.add_argument("--debug-func-addr", default="0")
    for name in ("init-wasm", "debug", "float-status", "text-ir", "no-lifted",
                 "no-compiled", "ready-js", "incremental-units", "jspi", "pthreads"):
        result.add_argument(f"--{name}", action="store_true")
    return result


class Display:
    """One live tree; non-terminal output records transitions without escape codes."""
    def __init__(self, name: str, console: Console):
        self.name, self.console = name, console
        self.rows: dict[str, tuple[str, str, float]] = {}
        self.parents: dict[str, str] = {}
        self.spinners: dict[str, Spinner] = {}
        self.live = Live(self.render(), console=console, refresh_per_second=8,
                         auto_refresh=False)

    def update(self, name: str, state: str, detail: str = "",
               parent: str | None = None) -> None:
        if re.search(r" chunk \d+/\d+$", name):
            return
        previous = self.rows.get(name)
        active_states = ("lifting", "compiling", "linking", "running")
        started = (previous[2] if previous and previous[0] in active_states
                   else time.monotonic())
        self.rows[name] = state, detail, started
        if parent:
            self.parents[name] = parent
        if not self.console.is_terminal and (
                previous is None or state != previous[0] or state in ("failed", "done", "cached")):
            text = detail if detail.startswith("[") else f"{name}: {state}" + (
                f" — {detail}" if detail else "")
            self.console.print(Text(text), soft_wrap=True)

    def render(self) -> Tree:
        done = sum(state in ("done", "cached") for state, _, _ in self.rows.values())
        tree = Tree(Text(f"{self.name} [{done}/{len(self.rows)}]"))
        parent_names = set(self.parents.values())
        nodes = {}
        for name, (state, detail, started) in self.rows.items():
            display_name = Path(name).name if Path(name).is_absolute() else name
            text = detail if detail.startswith("[") else f"{display_name}: {state}" + (
                f" — {detail}" if detail else "")
            label = Text(text, style={"failed": "red", "done": "green",
                                     "cached": "dim"}.get(state, ""))
            if state in ("lifting", "compiling", "linking", "running"):
                label.append(f"  {time.monotonic() - started:.1f}s", style="dim")
                if name not in self.spinners:
                    self.spinners[name] = Spinner("dots")
                node = self.spinners[name]
                node.update(text=label)
            else:
                node = label
            nodes[name] = Tree(node) if name in parent_names else node
        for name, node in nodes.items():
            parent = self.parents.get(name)
            if parent in nodes:
                nodes[parent].add(node)
            else:
                tree.add(node)
        return tree

    async def refresh(self) -> None:
        while True:
            if self.console.is_terminal:
                self.live.update(self.render(), refresh=True)
            await asyncio.sleep(0.1)


class Driver:
    def __init__(self, args: argparse.Namespace, display: Display):
        self.a, self.display = args, display
        self.output = args.output_dir.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.cache = self.output / ".elfconv-incremental"
        self.cache.mkdir(exist_ok=True)
        self.logs = self.output / ".elfconv-logs"
        self.logs.mkdir(exist_ok=True)
        self.env = os.environ.copy()
        if (ROOT / "lib").is_dir():
            self.env["LD_LIBRARY_PATH"] = str(ROOT / "lib") + (
                ":" + self.env["LD_LIBRARY_PATH"] if self.env.get("LD_LIBRARY_PATH") else "")
        self.includes = [f"-I{ROOT / path}" for path in ("backend/remill/include", ".", "src")]
        arch = args.target.split("-")[0]
        self.definitions = [] if arch == "i386" else [f"-DELF_IS_{arch.upper()}"]
        if args.debug:
            self.definitions += ["-DELFC_RUNTIME_SYSCALL_DEBUG=1", "-DELFC_RUNTIME_MULSECTIONS_WARNING=1"]
        self.thread_compile = ["-pthread"] if args.pthreads else []
        self.exception_flags = ["-fwasm-exceptions"] if args.target == "i386-wasm" else []
        self.thread_link = self.thread_compile + (["-sPROXY_TO_PTHREAD=1", "-sPTHREAD_POOL_SIZE=2",
            "-sDEFAULT_PTHREAD_STACK_SIZE=2097152", "-sOFFSCREENCANVAS_SUPPORT=1",
            "-sOFFSCREENCANVASES_TO_PTHREAD=#canvas"] if args.pthreads else [])
        self.compiler_identity: dict[str, str] = {}
        self.script_hash = digest(Path(__file__))

    async def run(self, name: str, phase: str, command: list[object]) -> Path:
        self.display.update(name, phase)
        command = [str(item) for item in command]
        label = re.sub(r"[^A-Za-z0-9_.-]", "_", Path(name).name)
        log = self.logs / f"{label}.{phase}.{key(name, phase)[:8]}.log"
        process = None
        try:
            with log.open("wb") as stream:
                stream.write((shlex.join(command) + "\n").encode())
                stream.flush()
                spawn = asyncio.create_task(asyncio.create_subprocess_exec(
                    *command, stdout=stream, stderr=asyncio.subprocess.STDOUT,
                    env=self.env, start_new_session=True))
                try:
                    process = await asyncio.shield(spawn)
                except asyncio.CancelledError:
                    # Retain ownership even if interrupted while the process is spawning.
                    process = await spawn
                    raise
                status = await process.wait()
            if status:
                self.display.update(name, "failed", f"exit {status}; {log}")
                with log.open(errors="replace") as stream:
                    excerpt = "".join(deque(stream, maxlen=30))
                raise RuntimeError(f"{name}: {phase} failed (exit {status})\n{excerpt}\nFull log: {log}")
            with log.open(errors="replace") as stream:
                diagnostics = sum(bool(re.search(
                    r"warning:|Unsupported instruction|kLiftedMismatchedISEL|Instruction decode of|^[EF]\d{8}",
                    line)) for line in stream)
            if diagnostics:
                self.display.console.print(Text(f"{name}: {diagnostics} diagnostics; {log}", style="yellow"), soft_wrap=True)
            return log
        except asyncio.CancelledError:
            self.display.update(name, "cancelled")
            raise
        except OSError:
            self.display.update(name, "failed", str(log))
            raise
        finally:
            if process is not None and process.returncode is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    await asyncio.wait_for(process.wait(), 3)
                except asyncio.TimeoutError:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    await process.wait()

    async def stage(self, name: str, phase: str, command: list[object], outputs: list[Path],
                    dependencies: object, stamp: Path, *, force: bool = False) -> tuple[str, bool]:
        stage_key = key(self.script_hash, command, dependencies)
        if not force and all(path.is_file() for path in outputs) and stamp.is_file() and stamp.read_text() == stage_key:
            self.display.update(name, "cached", phase)
            return stage_key, False
        stamp.unlink(missing_ok=True)
        await self.run(name, phase, command)
        for output in outputs:
            if not output.is_file():
                self.display.update(name, "failed", f"missing output {output}")
                raise RuntimeError(f"{name}: missing output {output}")
        atomic_text(stamp, stage_key)
        return stage_key, True

    def lift_flags(self) -> list[object]:
        a = self.a
        arch, kind = a.target.split("-")
        destination = "emscripten32" if kind == "wasm" else "wasi32" if kind == "wasi32" else platform.machine()
        flags: list[object] = ["--arch", arch, "--target_arch", destination, "--target_elf", a.input,
            "--norm_mode", "1", "--fork_emulation", str(int(kind == "wasm" and arch != "i386")),
            "--float_exception", str(int(a.float_status)), "--dbg_fun_vma", a.debug_func_addr]
        if a.entry_symbol:
            flags += ["--entry_symbol", a.entry_symbol]
        if a.shared_library:
            flags += ["--shared_libraries", ",".join(map(str, a.shared_library))]
        if a.bitcode_path:
            flags += ["--bitcode_path", a.bitcode_path]
        return flags

    async def compiler(self) -> str:
        if not self.a.emcc:
            hint = Path(os.environ.get("EMSDK", "")) / "upstream/emscripten/em++"
            self.a.emcc = tool(str(hint) if hint.is_file() else None, "em++")
        else:
            self.a.emcc = tool(self.a.emcc)
        if self.a.emcc not in self.compiler_identity:
            log = await self.run("Toolchain", "running", [self.a.emcc, "--version"])
            self.compiler_identity[self.a.emcc] = key(digest(Path(self.a.emcc)), log.read_text())
            self.display.update("Toolchain", "done")
        return self.a.emcc

    def html(self) -> None:
        source = (ROOT / "src/browser/i386.html.in").read_text()
        source = source.replace("@ELFCONV_NAME@", self.a.elf_name).replace(
            "@ELFCONV_BROWSER_JSPI@", str(self.a.jspi or self.a.pthreads).lower())
        atomic_text(self.output / f"{self.a.elf_name}.html", source)

    def browser_link_flags(self, incremental: bool) -> list[str]:
        flags = [f"-O{self.a.wasm_opt_level}", *self.thread_link, *self.exception_flags,
                 *(["-g2"] if self.a.wasm_opt_level == "0" else []), *self.includes,
                 "-std=c++17", *self.definitions, "-DELF_IS_I386", "-DADDRESS_SIZE_BITS=32",
                 "-sUSE_SDL=2", f"-sLEGACY_GL_EMULATION={self.a.legacy_gl}",
                 "-sMIN_WEBGL_VERSION=2", "-sMAX_WEBGL_VERSION=2", f"-DECV_LEGACY_GL={self.a.legacy_gl}",
                 "-sJSPI=1" if self.a.jspi or self.a.pthreads else
                 "-sASYNCIFY=0" if self.a.no_asyncify else "-sASYNCIFY=1",
                 "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=335544320",
                 "-sSTACK_SIZE=1048576", "-sEXIT_RUNTIME=1"]
        if incremental:
            flags += ["-DELFCONV_INCREMENTAL_UNITS=1"]
        return flags

    async def incremental(self) -> None:
        a = self.a
        emcc = await self.compiler()
        partition: list[object] = ["--per_elf"] if a.shared_library else [
            "--linker_map", a.link_map, "--object_base", a.object_base]
        flags = self.lift_flags() + partition
        manifest, fingerprint = self.cache / "units.tsv", self.cache / "metadata.fingerprint"
        scan_outputs = [manifest, fingerprint]
        if manifest.is_file():
            scan_outputs += [Path(fields[1]) for line in manifest.read_text().splitlines()
                             if len(fields := line.split("\t")) == 4]
        scan_inputs = [a.input, *a.shared_library]
        if a.link_map:
            scan_inputs.append(a.link_map)
        _, scan_changed = await self.stage("Bundle scan", "running",
            [a.lifter, *flags, "--unit_manifest_out", manifest,
             "--metadata_fingerprint_out", fingerprint], [manifest, fingerprint],
            [digest(Path(a.lifter)), [(str(path), digest(path)) for path in scan_inputs]],
            self.cache / "scan.key", force=any(not path.is_file() for path in scan_outputs))
        self.display.update("Bundle scan", "done" if scan_changed else "cached")
        entries = []
        for line in manifest.read_text().splitlines():
            fields = line.split("\t")
            if (len(fields) != 4 or not fields[0] or not Path(fields[1]).is_file()):
                raise ValueError(f"Malformed unit manifest entry: {line}")
            owner, path, functions, code_size = fields
            function_count, estimated_size = int(functions), int(code_size)
            if function_count < 1 or estimated_size < 0:
                raise ValueError(f"Malformed unit size in manifest entry: {line}")
            entries.append((owner, Path(path), hashlib.sha256(owner.encode()).hexdigest()[:24],
                            function_count, estimated_size))
        semantics = []
        if a.bitcode_path:
            paths = sorted(a.bitcode_path.rglob("*.bc")) if a.bitcode_path.is_dir() else [a.bitcode_path]
            semantics = [(str(path), digest(path)) for path in paths]
        lift_identity = key(digest(Path(a.lifter)), semantics)
        if not entries:
            raise ValueError("The ELF bundle produced no code units")
        chunk_counts = allocate_chunks(entries, a.ir_chunks) if a.ir_chunks else {
            entry[0]: 1 for entry in entries}
        allocation_key = key(flags, lift_identity, a.ir_chunks,
                             [(owner, digest(path)) for owner, path, _, _, _ in entries])
        allocation_path = self.cache / "chunk-allocation.json"
        if a.ir_chunks and allocation_path.is_file():
            allocation = json.loads(allocation_path.read_text())
            counts = allocation.get("counts", {})
            if (allocation.get("key") == allocation_key and counts.keys() == chunk_counts.keys()
                    and all(isinstance(count, int) and count > 0 for count in counts.values())
                    and sum(counts.values()) == sum(chunk_counts.values())):
                chunk_counts = counts
        for owner, _, _, _, _ in entries:
            self.display.update(owner, "queued", f"{chunk_counts[owner]} chunk(s)")
        queued_runtime = [] if a.runtime_archive else [
            f"Runtime/{source.name}" for source in [*COMMON, ROOT / "src/runtime/I386Imports.cpp"]]
        for name in ["Shared metadata", "Unit registry", *queued_runtime, "Final link"]:
            self.display.update(name, "queued")
        compiler_identity = self.compiler_identity[emcc]
        compile_flags = [f"-O{a.wasm_opt_level}", *self.thread_compile, *self.exception_flags]
        compile_slots = asyncio.Semaphore(a.compile_jobs)
        lift_slots = asyncio.Semaphore(a.lift_jobs)
        metadata_bc, metadata_object = self.cache / "metadata.bc", self.cache / "metadata.wasm.o"

        async def compile_metadata() -> str:
            async with lift_slots:
                metadata_key, metadata_changed = await self.stage("Shared metadata", "lifting",
                    [a.lifter, *flags, "--metadata_only", "--bc_out", metadata_bc], [metadata_bc],
                    [digest(fingerprint), lift_identity], self.cache / "metadata.lift.key")
            async with compile_slots:
                metadata_compile_key, _ = await self.stage("Shared metadata", "compiling",
                    [emcc, *compile_flags, "-c", metadata_bc, "-o", metadata_object],
                    [metadata_object], [metadata_key, compiler_identity],
                    self.cache / "metadata.compile.key", force=metadata_changed)
            self.display.update("Shared metadata", "done")
            return metadata_compile_key

        lifted_chunks: dict[str, list[dict[str, object]]] = {}

        async def lift_owner(entry: tuple[str, Path, str, int, int],
                             count: int) -> list[dict[str, object]]:
            owner, fingerprint_path, unit_id, _, code_size = entry
            base = self.cache / f"unit-{unit_id}.bc"
            lift_stamp = self.cache / f"unit-{unit_id}.lift.key"
            dependencies = [digest(fingerprint_path), lift_identity]
            if a.ir_chunks:
                chunk_manifest = self.cache / f"unit-{unit_id}.ir-chunks.tsv"
                if chunk_manifest.is_file():
                    try:
                        self.read_ir_chunk_manifest(chunk_manifest, base)
                    except (OSError, ValueError):
                        lift_stamp.unlink(missing_ok=True)
                paths = self.ir_chunk_paths(base, count)
                lift_key, changed = await self.stage(
                    owner, "lifting",
                    [a.lifter, *flags, "--unit_owner", owner, "--unit_id", unit_id,
                     "--ir_chunks", count, "--ir_chunk_manifest_out", chunk_manifest,
                     "--bc_out", base],
                    [chunk_manifest, *paths], dependencies, lift_stamp)
                requested, bitcodes, sizes = self.read_ir_chunk_manifest(chunk_manifest, base)
                if requested != count or len(bitcodes) != count or any(not path.is_file() for path in bitcodes):
                    raise ValueError(f"{owner}: invalid chunk manifest for {count} chunks")
                self.clean_ir_chunks(base, len(bitcodes))
                return [
                    {"owner": owner, "unit_id": unit_id, "index": index,
                     "bitcode": bitcode, "object": self.cache / f"unit-{unit_id}-chunk-{index}.wasm.o",
                     "lift_key": lift_key, "lift_changed": changed, "size": sizes[index],
                     "count": count}
                    for index, bitcode in enumerate(bitcodes)]
            chunk_manifest = self.cache / f"unit-{unit_id}.ir-chunks.tsv"
            chunk_manifest.unlink(missing_ok=True)
            self.clean_ir_chunks(base, 0)
            lift_key, changed = await self.stage(
                owner, "lifting",
                [a.lifter, *flags, "--unit_owner", owner, "--unit_id", unit_id, "--bc_out", base],
                [base], dependencies, lift_stamp)
            return [{"owner": owner, "unit_id": unit_id, "index": 0, "bitcode": base,
                     "object": self.cache / f"unit-{unit_id}-chunk-0.wasm.o",
                     "lift_key": lift_key, "lift_changed": changed, "size": code_size,
                     "count": 1}]

        completed = {owner: 0 for owner, _, _, _, _ in entries}
        changed_owners = {owner: False for owner, _, _, _, _ in entries}
        compiled_chunks: dict[str, dict[int, tuple[Path, str]]] = {
            owner: {} for owner, _, _, _, _ in entries}

        async def compile_chunk(chunk: dict[str, object]) -> tuple[str, int, Path, str, bool]:
            owner = str(chunk["owner"])
            index = int(chunk["index"])
            unit_id = str(chunk["unit_id"])
            obj = Path(chunk["object"])
            bitcode = Path(chunk["bitcode"])
            total = int(chunk["count"])
            chunk_name = f"{owner} chunk {index + 1}/{total}"
            progress_name = f"{owner}::compile"
            try:
                async with compile_slots:
                    self.display.update(
                        progress_name, "compiling",
                        f"[{completed[owner]}/{total}] ── {Path(owner).name} "
                        f"chunk {index + 1}/{total}: compiling",
                        parent=owner)
                    compile_key, compiled = await self.stage(
                        chunk_name, "compiling",
                        [emcc, *compile_flags, "-c", bitcode, "-o", obj], [obj],
                        [chunk["lift_key"], digest(bitcode), compiler_identity],
                        self.cache / f"unit-{unit_id}-chunk-{index}.compile.key",
                        force=bool(chunk["lift_changed"]))
            except Exception:
                self.display.update(
                    progress_name, "failed",
                    f"[{completed[owner]}/{total}] ── {Path(owner).name} "
                    f"chunk {index + 1}/{total}: failed",
                    parent=owner)
                raise
            return owner, index, obj, compile_key, compiled

        async def lift_compile_pipeline(
                lift_entries: list[tuple[str, Path, str, int, int]]) -> None:
            if not lift_entries:
                return
            condition = asyncio.Condition()
            pending: dict[str, deque[dict[str, object]]] = {}
            ready: deque[str] = deque()
            lifts_done = False

            async def lift_entry(entry: tuple[str, Path, str, int, int]
                                 ) -> tuple[str, list[dict[str, object]]]:
                return entry[0], await lift_owner(entry, chunk_counts[entry[0]])

            async def lift_all() -> None:
                nonlocal lifts_done
                ordered_entries = sorted(lift_entries, key=lambda value: value[4], reverse=True)
                try:
                    async for owner, chunks in bounded_as_completed(
                            ordered_entries, lift_slots, lift_entry):
                        lifted_chunks[owner] = chunks
                        lift_status = "done" if any(
                            bool(chunk["lift_changed"]) for chunk in chunks) else "cached"
                        self.display.update(owner, lift_status, "lifting")
                        async with condition:
                            pending[owner] = deque(chunks)
                            if chunks:
                                ready.append(owner)
                            condition.notify_all()
                finally:
                    async with condition:
                        lifts_done = True
                        condition.notify_all()

            async def compile_ready() -> None:
                while True:
                    async with condition:
                        await condition.wait_for(lambda: ready or lifts_done)
                        if not ready:
                            return
                        owner = ready.popleft()
                        chunk = pending[owner].popleft()
                        if pending[owner]:
                            ready.append(owner)
                        else:
                            del pending[owner]
                    owner, index, obj, compile_key, compiled = await compile_chunk(chunk)
                    compiled_chunks[owner][index] = (obj, compile_key)
                    completed[owner] += 1
                    changed_owners[owner] |= compiled
                    total = len(lifted_chunks[owner])
                    chunk_status = "done" if compiled else "cached"
                    status = "compiling" if completed[owner] < total else (
                        "done" if changed_owners[owner] else "cached")
                    self.display.update(
                        f"{owner}::compile", status,
                        f"[{completed[owner]}/{total}] ── {Path(owner).name} "
                        f"chunk {index + 1}/{total}: {chunk_status}",
                        parent=owner)

            async with asyncio.TaskGroup() as group:
                group.create_task(lift_all())
                for _ in range(a.compile_jobs):
                    group.create_task(compile_ready())

        headers = sorted(path for directory in (ROOT / "src/runtime", ROOT / "src/utils",
                                                ROOT / "backend/remill/include")
                         for path in directory.rglob("*") if path.suffix in (".h", ".hpp", ".inc"))
        header_key = key([(str(path), digest(path)) for path in headers])
        runtime_flags = [*compile_flags, "-sUSE_SDL=2", *self.includes, "-std=c++17", *self.definitions,
                         "-DELF_IS_I386", "-DADDRESS_SIZE_BITS=32", "-DELFCONV_INCREMENTAL_UNITS=1"]
        registry = self.cache / "UnitRegistry.cpp"
        registry_text = self.registry([entry[2] for entry in entries])
        atomic_text(registry, registry_text)
        registry_object = self.cache / "UnitRegistry.wasm.o"
        async def compile_registry() -> tuple[Path, str]:
            async with compile_slots:
                registry_key, _ = await self.stage("Unit registry", "compiling",
                    [emcc, *runtime_flags, "-c", registry, "-o", registry_object], [registry_object],
                    [key(registry_text), header_key, compiler_identity],
                    self.cache / "UnitRegistry.compile.key")
            self.display.update("Unit registry", "done")
            return registry_object, registry_key

        runtime_sources = [] if a.runtime_archive else [*COMMON, ROOT / "src/runtime/I386Imports.cpp"]

        async def compile_runtime(source: Path) -> tuple[Path, str]:
            obj = self.cache / f"runtime-{source.stem}.wasm.o"
            async with compile_slots:
                stage_key, _ = await self.stage(f"Runtime/{source.name}", "compiling",
                    [emcc, *runtime_flags, f"-DECV_LEGACY_GL={a.legacy_gl}", "-c", source, "-o", obj],
                    [obj], [digest(source), header_key, compiler_identity],
                    self.cache / f"runtime-{source.stem}.compile.key")
            self.display.update(f"Runtime/{source.name}", "done")
            return obj, stage_key

        async def run_pipeline() -> tuple[str, tuple[Path, str], list[tuple[Path, str]]]:
            async with asyncio.TaskGroup() as group:
                metadata_task = group.create_task(compile_metadata())
                registry_task = group.create_task(compile_registry())
                runtime_tasks = [group.create_task(compile_runtime(source))
                                 for source in runtime_sources]
                await lift_compile_pipeline(entries)
                if a.ir_chunks:
                    measured_entries = [
                        (*entry[:4], sum(int(chunk["size"]) for chunk in lifted_chunks[entry[0]]))
                        for entry in entries]
                    refined_counts = allocate_chunks(measured_entries, a.ir_chunks)
                    changed_entries = []
                    for entry in entries:
                        owner = entry[0]
                        if refined_counts[owner] != chunk_counts[owner]:
                            chunk_counts[owner] = refined_counts[owner]
                            completed[owner] = 0
                            changed_owners[owner] = False
                            compiled_chunks[owner] = {}
                            changed_entries.append(entry)
                            self.display.update(owner, "queued",
                                                f"{chunk_counts[owner]} rebalanced chunk(s)")
                            self.display.update(
                                f"{owner}::compile", "queued",
                                f"[0/{chunk_counts[owner]}] ── {Path(owner).name}: awaiting relift",
                                parent=owner)
                    await lift_compile_pipeline(changed_entries)
                    atomic_text(allocation_path, json.dumps(
                        {"key": allocation_key, "counts": chunk_counts}, sort_keys=True))
            return (metadata_task.result(), registry_task.result(),
                    [task.result() for task in runtime_tasks])

        metadata_compile_key, (registry_object, registry_key), runtime_results = await run_pipeline()
        if a.runtime_archive:
            runtime_objects, runtime_keys = [a.runtime_archive], [digest(a.runtime_archive)]
        else:
            runtime_objects = [obj for obj, _ in runtime_results]
            runtime_keys = [stage_key for _, stage_key in runtime_results]
        units = [compiled_chunks[owner][index]
                 for owner, _, _, _, _ in entries
                 for index in sorted(compiled_chunks[owner])]
        js, wasm = self.output / f"{a.elf_name}.js", self.output / f"{a.elf_name}.wasm"
        _, linked = await self.stage("Final link", "linking",
            [emcc, *self.browser_link_flags(True), *[obj for obj, _ in units], metadata_object,
             registry_object, *runtime_objects, "-o", js], [js, wasm],
            [metadata_compile_key, registry_key, [stamp for _, stamp in units], runtime_keys, compiler_identity],
            self.cache / "link.key")
        self.display.update("Final link", "done" if linked else "cached")
        self.html()

    @staticmethod
    def ir_chunk_paths(base: Path, count: int) -> list[Path]:
        return [base.with_name(f"{base.stem}.chunk-{index}{base.suffix}")
                for index in range(count)]

    @staticmethod
    def clean_ir_chunks(base: Path, count: int) -> None:
        pattern = re.compile(
            rf"{re.escape(base.stem)}\.chunk-(\d+){re.escape(base.suffix)}")
        for path in base.parent.glob(f"{base.stem}.chunk-*{base.suffix}"):
            match = pattern.fullmatch(path.name)
            if match and int(match[1]) >= count and path.is_file():
                path.unlink()

    def read_ir_chunk_manifest(self, manifest: Path, base: Path) -> tuple[int, list[Path], list[int]]:
        lines = manifest.read_text().splitlines()
        if not lines:
            raise ValueError(f"Empty IR chunk manifest: {manifest}")
        try:
            header = [int(value) for value in lines[0].split("\t")]
            entries = []
            for line in lines[1:]:
                entry = [int(value) for value in line.split("\t")]
                if len(entry) != 2:
                    raise ValueError
                entries.append((entry[0], entry[1]))
        except ValueError as error:
            raise ValueError(f"Malformed IR chunk manifest: {manifest}") from error
        if (len(header) != 2 or header[0] < 1 or header[1] < 1 or
                len(entries) != header[1] or any(
                    index != expected or size < 0 for expected, (index, size) in enumerate(entries))):
            raise ValueError(f"Malformed IR chunk manifest: {manifest}")
        return header[0], self.ir_chunk_paths(base, header[1]), [size for _, size in entries]

    async def chunked(self) -> None:
        a, name = self.a, self.a.elf_name
        emcc = await self.compiler()
        base = self.output / f"{name}.bc"
        manifest = self.cache / f"{name}.ir-chunks.tsv"
        lift_stamp = self.cache / f"{name}.ir-chunks.lift.key"
        if a.no_lifted:
            if not manifest.is_file():
                raise ValueError(f"No IR chunk manifest for {name}; remove --no-lifted")
            requested, chunks, ir_sizes = self.read_ir_chunk_manifest(manifest, base)
            if requested != a.ir_chunks or any(not path.is_file() for path in chunks):
                raise ValueError(f"Existing IR chunks do not match --ir-chunks {a.ir_chunks}")
            lift_key = key(digest(manifest), [(digest(path)) for path in chunks])
            self.display.update(name, "cached", "lifting")
        else:
            previous_outputs = [manifest]
            previous_chunks = []
            if manifest.is_file():
                try:
                    _, previous_chunks, _ = self.read_ir_chunk_manifest(manifest, base)
                    previous_outputs.extend(previous_chunks)
                except ValueError:
                    lift_stamp.unlink(missing_ok=True)
            semantics = []
            if a.bitcode_path:
                paths = sorted(a.bitcode_path.rglob("*.bc")) if a.bitcode_path.is_dir() else [a.bitcode_path]
                semantics = [(str(path), digest(path)) for path in paths]
            lift_identity = key(digest(Path(a.lifter)), semantics)
            lift_key, lift_changed = await self.stage(name, "lifting",
                [a.lifter, *self.lift_flags(), "--ir_chunks", a.ir_chunks,
                 "--ir_chunk_manifest_out", manifest, "--bc_out", base],
                [manifest, *previous_chunks],
                [digest(a.input), [(str(path), digest(path)) for path in a.shared_library],
                 lift_identity], lift_stamp)
            self.display.update(name, "done" if lift_changed else "cached", "lifting")
            requested, chunks, ir_sizes = self.read_ir_chunk_manifest(manifest, base)
            if requested != a.ir_chunks or any(not path.is_file() for path in chunks):
                raise ValueError(f"Lifter produced invalid IR chunks for --ir-chunks {a.ir_chunks}")
        self.clean_ir_chunks(base, len(chunks))

        compiler_identity = self.compiler_identity[emcc]
        compile_flags = [f"-O{a.wasm_opt_level}", *self.thread_compile, *self.exception_flags]
        chunk_objects = [self.cache / f"{name}-ir-chunk-{index}.wasm.o"
                         for index in range(len(chunks))]
        semaphore = asyncio.Semaphore(a.compile_jobs)
        chunk_tasks = []

        async def compile_chunk(index: int, bitcode: Path, obj: Path) -> tuple[Path, str]:
            async with semaphore:
                compile_key, compiled = await self.stage(f"IR chunk {index}", "compiling",
                    [emcc, *compile_flags, "-c", bitcode, "-o", obj], [obj],
                    [lift_key, digest(bitcode), compiler_identity],
                    self.cache / f"{name}-ir-chunk-{index}.compile.key")
            self.display.update(f"IR chunk {index}", "done" if compiled else "cached")
            return obj, compile_key

        if not a.no_compiled:
            chunk_tasks = [
                asyncio.create_task(compile_chunk(index, bitcode, obj))
                for index, (bitcode, obj) in enumerate(zip(chunks, chunk_objects))]
        elif any(not obj.is_file() for obj in chunk_objects):
            raise ValueError(f"Missing compiled IR chunks for {name}; remove --no-compiled")

        runtime_tasks = []
        if not a.runtime_archive:
            sources = [*COMMON, ROOT / "src/runtime/I386Imports.cpp"]
            headers = sorted(path for directory in (ROOT / "src/runtime", ROOT / "src/utils",
                                                    ROOT / "backend/remill/include")
                             for path in directory.rglob("*") if path.suffix in (".h", ".hpp", ".inc"))
            header_key = key([(str(path), digest(path)) for path in headers])
            runtime_flags: list[object] = [
                f"-O{a.wasm_opt_level}", *self.thread_compile, *self.exception_flags,
                *(["-g2"] if a.wasm_opt_level == "0" else []), *self.includes, "-std=c++17",
                *self.definitions, "-DELF_IS_I386", "-DADDRESS_SIZE_BITS=32", "-sUSE_SDL=2",
                f"-DECV_LEGACY_GL={a.legacy_gl}"]

            async def compile_runtime(source: Path) -> tuple[Path, str]:
                obj = self.cache / f"ir-chunk-runtime-{source.stem}.wasm.o"
                async with semaphore:
                    compile_key, compiled = await self.stage(f"Runtime/{source.name}", "compiling",
                        [emcc, *runtime_flags, "-c", source, "-o", obj], [obj],
                        [digest(source), header_key, compiler_identity],
                        self.cache / f"ir-chunk-runtime-{source.stem}.compile.key")
                self.display.update(f"Runtime/{source.name}", "done" if compiled else "cached")
                return obj, compile_key

            runtime_tasks = [asyncio.create_task(compile_runtime(source)) for source in sources]
        tasks = [*chunk_tasks, *runtime_tasks]
        try:
            results = await asyncio.gather(*tasks)
        finally:
            for task in tasks:
                if not task.done():
                    task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
        chunk_results = results[:len(chunk_tasks)]
        runtime_results = results[len(chunk_tasks):]
        if a.no_compiled:
            chunk_keys = [key("reused", digest(obj)) for obj in chunk_objects]
        else:
            chunk_keys = [compile_key for _, compile_key in chunk_results]
        if a.runtime_archive:
            runtime_objects = [a.runtime_archive]
            runtime_keys = [digest(a.runtime_archive)]
        else:
            runtime_objects = [obj for obj, _ in runtime_results]
            runtime_keys = [compile_key for _, compile_key in runtime_results]
        js, wasm = self.output / f"{name}.js", self.output / f"{name}.wasm"
        _, linked = await self.stage("Final link", "linking",
            [emcc, *self.browser_link_flags(False), *chunk_objects, *runtime_objects, "-o", js],
            [js, wasm], [lift_key, chunk_keys, runtime_keys, compiler_identity],
            self.cache / f"{name}.ir-chunks.link.key")
        self.display.update("Final link", "done" if linked else "cached")
        self.html()
        for index, size in enumerate(ir_sizes):
            print(f"IR chunk {index}: {size} definition bytes")


    @staticmethod
    def registry(ids: list[str]) -> str:
        source = '#include "runtime/Memory.h"\nextern "C" {\n'
        declarations = (
            "extern const uint64_t _ecv_fun_vmas_{id}[];",
            "extern const LiftedFunc _ecv_fun_ptrs_{id}[];",
            "extern uint64_t **_ecv_block_address_ptrs_array_{id}[];",
            "extern const uint64_t *_ecv_block_address_vmas_array_{id}[];",
            "extern const uint64_t _ecv_block_address_size_array_{id}[];",
            "extern const uint64_t _ecv_block_address_fn_vma_array_{id}[];",
            "extern const uint64_t _ecv_block_address_array_size_{id};")
        for unit_id in ids:
            source += "\n".join(line.format(id=unit_id) for line in declarations) + "\n"
        source += "extern const EcvLiftedUnit _ecv_lifted_units[] = {\n"
        for unit_id in ids:
            names = ("fun_vmas", "fun_ptrs", "block_address_ptrs_array", "block_address_vmas_array",
                     "block_address_size_array", "block_address_fn_vma_array", "block_address_array_size")
            source += "  {" + ", ".join(f"_ecv_{name}_{unit_id}" for name in names) + "},\n"
        return source + f"}};\nextern const uint64_t _ecv_lifted_unit_count = {len(ids)};\n}}\n"

    def prepare_browser(self) -> None:
        name = self.a.elf_name
        generated_js = self.output / f"{name}.generated.js"
        if not generated_js.is_file():
            raise ValueError(f"Missing generated browser loader: {generated_js}")
        shutil.copyfile(self.output / f"{name}.generated.wasm", self.output / f"{name}.wasm")
        browser = ROOT / "src/browser"
        for source, destination in (("coi-serviceworker.js", "coi-serviceworker.js"), ("process.js", f"{name}.js")):
            shutil.copyfile(browser / source, self.output / destination)
        html = self.output / "main.html"
        if self.a.init_wasm:
            for filename in ("js-kernel.js", "main.html"):
                shutil.copyfile(browser / filename, self.output / filename)
        if not html.is_file():
            raise ValueError("main.html is missing; provide an init page or use --init-wasm")
        source = re.sub(r"initProgram: '[^']*\.wasm'", f"initProgram: '{name}.wasm'", html.read_text())
        match = re.search(r"var binList = (\[[^]]*\])", source)
        if not match:
            raise ValueError("main.html lacks var binList = [...]")
        names = json.loads(match[1])
        if name not in names:
            names.append(name)
        source = source[:match.start(1)] + json.dumps(names) + source[match.end(1):]
        atomic_text(html, source)


    async def single(self) -> None:
        a = self.a
        name, kind = a.elf_name, a.target.split("-")[1]
        ir = self.output / f"{name}.bc"
        if a.no_lifted:
            ir = next((self.output / f"{name}.{suffix}" for suffix in ("ll", "bc")
                       if (self.output / f"{name}.{suffix}").is_file()), ir)
            if not ir.is_file():
                raise ValueError(f"No lifted IR for {name} in {self.output}")
        else:
            await self.run(name, "lifting", [a.lifter, *self.lift_flags(), "--bc_out", ir])
        if a.text_ir and ir.suffix == ".bc":
            disassembler = tool(a.llvm_dis, str(Path(os.environ.get("LLVM_ROOT", "")) / "bin/llvm-dis"),
                                "llvm-dis-23", "llvm-dis")
            text_ir = ir.with_suffix(".ll")
            await self.run(name, "running", [disassembler, ir, "-o", text_ir])
            ir.unlink()
            ir = text_ir
        if kind == "native":
            compiler = tool(a.native_cxx, str(Path(os.environ.get("LLVM_ROOT", "")) / "bin/clang++"),
                            "clang++-23", "clang++")
            flags = ["-O3", "-std=c++20", "-static", *self.includes, *self.definitions, f'-DELFNAME="{name}"']
            obj = self.output / f"{name}.o"
            binary = self.output / f"{name}.{platform.machine()}"
            if not a.no_compiled:
                await self.run(name, "compiling", [compiler, *flags, "-c", ir, "-o", obj])
            await self.run("Final link", "linking", [compiler, *flags, "-o", binary, obj, *COMMON,
                                                      ROOT / "src/runtime/syscalls/SyscallNative.cpp"])
            if a.output_executable:
                binary.replace(a.output_executable)
        elif kind == "wasm":
            emcc = await self.compiler()
            if a.target == "i386-wasm":
                obj = self.output / f"{name}{'.pthread' if a.pthreads else ''}.wasm.o"
                if not a.no_compiled:
                    await self.run(name, "compiling", [emcc, f"-O{a.wasm_opt_level}", *self.thread_compile,
                                                       *self.exception_flags, "-c", ir, "-o", obj])

                if a.runtime_archive:
                    runtime_objects = [a.runtime_archive]
                else:
                    sources = [*COMMON, ROOT / "src/runtime/I386Imports.cpp"]
                    headers = sorted(path for directory in (ROOT / "src/runtime", ROOT / "src/utils",
                                                            ROOT / "backend/remill/include")
                                     for path in directory.rglob("*") if path.suffix in (".h", ".hpp", ".inc"))
                    header_key = key([(str(path), digest(path)) for path in headers])
                    runtime_flags: list[object] = [
                        f"-O{a.wasm_opt_level}", *self.thread_compile, *self.exception_flags,
                        *(["-g2"] if a.wasm_opt_level == "0" else []), *self.includes, "-std=c++17",
                        *self.definitions, "-DELF_IS_I386", "-DADDRESS_SIZE_BITS=32", "-sUSE_SDL=2",
                        f"-DECV_LEGACY_GL={a.legacy_gl}"]
                    semaphore = asyncio.Semaphore(a.compile_jobs)

                    async def compile_runtime(source: Path) -> Path:
                        runtime_obj = self.cache / f"single-runtime-{source.stem}.wasm.o"
                        async with semaphore:
                            await self.stage(f"Runtime/{source.name}", "compiling",
                                [emcc, *runtime_flags, "-c", source, "-o", runtime_obj], [runtime_obj],
                                [digest(source), header_key, self.compiler_identity[emcc]],
                                self.cache / f"single-runtime-{source.stem}.compile.key")
                        self.display.update(f"Runtime/{source.name}", "done")
                        return runtime_obj

                    tasks = [asyncio.create_task(compile_runtime(source)) for source in sources]
                    try:
                        runtime_objects = await asyncio.gather(*tasks)
                    finally:
                        for task in tasks:
                            if not task.done():
                                task.cancel()
                        await asyncio.gather(*tasks, return_exceptions=True)
                await self.run("Final link", "linking",
                    [emcc, *self.browser_link_flags(False), obj, *runtime_objects,
                     "-o", self.output / f"{name}.js"])
                self.html()
            else:
                definitions = [*self.definitions, "-DTARGET_IS_BROWSER=1", f'-DELFNAME="{name}"']
                flags = ["-O3", *self.includes, *definitions]
                obj = self.output / f"{name}.wasm.o"
                if not a.no_compiled:
                    await self.run(name, "compiling", [emcc, *flags, "-c", ir, "-o", obj])
                await self.run("Final link", "linking", [emcc, *flags, "-sASYNCIFY=0", "-sINITIAL_MEMORY=536870912",
                    "-sSTACK_SIZE=16MB", "-sPTHREAD_POOL_SIZE=0", "-pthread", "-sALLOW_MEMORY_GROWTH", "-sEXPORT_ES6",
                    "-lexports.js", "-sENVIRONMENT=web,worker", "-o", self.output / f"{name}.generated.js", obj,
                    *COMMON, ROOT / "src/runtime/syscalls/SyscallBrowser.cpp"])
                if a.mount:
                    pack_preload(a.mount, self.output)
                self.prepare_browser()
        else:
            sdk = a.wasi_sdk or Path(os.environ.get("WASI_SDK_PATH", ""))
            compiler = tool(str(sdk / "bin/clang++"))
            flags = ["-O3", f"--sysroot={sdk / 'share/wasi-sysroot'}", "-D_WASI_EMULATED_SIGNAL",
                "-D_WASI_EMULATED_PROCESS_CLOCKS", "-D_WASI_EMULATED_MMAN", *self.includes, "-fno-exceptions",
                *self.definitions, "-DTARGET_IS_WASI=1", f'-DELFNAME="{name}"']
            obj, wasm = self.output / f"{name}.wasi.o", self.output / f"{name}.wasm"
            if not a.no_compiled:
                await self.run(name, "compiling", [compiler, *flags, "-c", ir, "-o", obj])
            await self.run("Final link", "linking", [compiler, *flags, "-lwasi-emulated-process-clocks",
                "-lwasi-emulated-mman", "-lwasi-emulated-signal", "-o", wasm, obj, *COMMON,
                ROOT / "src/runtime/syscalls/SyscallWasi.cpp"])
            await self.run("WASI optimization", "running", [tool(a.wasmedge, "wasmedge"), "compile", "--optimize", "3",
                                                          wasm, self.output / f"{name}_o3.wasm"])
            self.display.update("WASI optimization", "done")
        self.display.update(name, "done")
        self.display.update("Final link", "done")


def validate(a: argparse.Namespace) -> None:
    a.input = a.input.resolve(strict=True)
    a.shared_library = [path.resolve(strict=True) for path in a.shared_library]
    a.elf_name = a.elf_name or a.input.name
    if Path(a.elf_name).name != a.elf_name or a.elf_name in (".", ".."):
        raise ValueError("--elf-name must be a filename, not a path")
    if a.compile_jobs < 1:
        raise ValueError("--compile-jobs must be positive")
    if a.lift_jobs < 1:
        raise ValueError("--lift-jobs must be positive")
    if a.ir_chunks < 0:
        raise ValueError("--ir-chunks must be non-negative")
    if a.ir_chunks and a.target != "i386-wasm":
        raise ValueError("--ir-chunks requires i386-wasm")
    if a.ir_chunks and (a.incremental_units or a.link_map or
                        a.object_base or a.text_ir or a.ready_js):
        raise ValueError("--ir-chunks cannot be combined with incremental units, linker maps, text IR, or --ready-js")
    if a.pthreads and a.target != "i386-wasm":
        raise ValueError("--pthreads requires i386-wasm")
    if a.runtime_archive:
        if a.target != "i386-wasm":
            raise ValueError("--runtime-archive requires i386-wasm")
        a.runtime_archive = a.runtime_archive.resolve(strict=True)
        if a.legacy_gl != "0" or a.debug or a.float_status:
            raise ValueError("--runtime-archive requires --legacy-gl 0 without --debug or --float-status")
        incremental = bool(a.incremental_units or a.shared_library)
        suffix = ("-incremental" if incremental else "") + ("-pthreads" if a.pthreads else "")
        expected = f"libelfconv-runtime-i386-wasm{suffix}.a"
        if a.runtime_archive.name != expected:
            raise ValueError(f"--runtime-archive must name {expected}")
    if a.entry_symbol and not a.target.startswith("i386-"):
        raise ValueError("--entry-symbol requires i386")
    if a.shared_library and (not a.target.startswith("i386-") or a.incremental_units):
        raise ValueError("Bundled libraries require i386 without linker-map units")
    if any("," in str(path) for path in a.shared_library):
        raise ValueError("Bundled library paths cannot contain commas")
    if a.incremental_units:
        if not a.link_map or not a.link_map.is_file() or not a.object_base or not a.object_base.is_dir():
            raise ValueError("--incremental-units requires --link-map and --object-base")
        a.link_map, a.object_base = a.link_map.resolve(), a.object_base.resolve()
    if a.incremental_units and a.target != "i386-wasm":
        raise ValueError("Incremental units require i386-wasm")
    if (a.incremental_units or (a.shared_library and a.target == "i386-wasm")) and (a.no_lifted or a.no_compiled or a.text_ir):
        raise ValueError("Incremental conversion uses its stage cache; skip/text-IR options require single-ELF conversion")
    if not a.bitcode_path and (ROOT / "bitcode").is_dir():
        a.bitcode_path = ROOT / "bitcode"
    if not a.ready_js and not a.no_lifted:
        default = ROOT / "bin/elflift" if (ROOT / "bin/elflift").is_file() else ROOT / "build/src/lifter/elflift"
        a.lifter = tool(a.lifter or str(default))
    if a.target == "i386-wasm" and not a.runtime_archive and a.lifter:
        incremental = bool(a.incremental_units or a.shared_library)
        suffix = ("-incremental" if incremental else "") + ("-pthreads" if a.pthreads else "")
        sibling_archive = Path(a.lifter).resolve().with_name(
            f"libelfconv-runtime-i386-wasm{suffix}.a")
        if sibling_archive.is_file() and a.legacy_gl == "0" and not a.debug and not a.float_status:
            a.runtime_archive = sibling_archive


async def convert(a: argparse.Namespace, console: Console) -> None:
    display = Display(a.elf_name, console)
    driver = Driver(a, display)
    refresh = asyncio.create_task(display.refresh())
    try:
        if console.is_terminal:
            display.live.start()
        if a.ready_js:
            driver.prepare_browser()
        elif a.shared_library and a.target == "i386-wasm":
            await driver.incremental()
        elif a.ir_chunks:
            await driver.chunked()
        elif a.incremental_units:
            await driver.incremental()
        else:
            await driver.single()
    finally:
        refresh.cancel()
        await asyncio.gather(refresh, return_exceptions=True)
        if console.is_terminal:
            display.live.update(display.render(), refresh=True)
            display.live.stop()
    console.print(Text(f"Outputs: {driver.output}\nTool logs: {driver.logs}"), soft_wrap=True)


def main() -> int:
    args = parser().parse_args()
    console = Console(stderr=True)
    try:
        validate(args)
        asyncio.run(convert(args, console))
    except KeyboardInterrupt:
        console.print("Conversion cancelled; active subprocess groups stopped.", style="yellow")
        return 130
    except (OSError, ValueError, RuntimeError) as error:
        console.print(Text(str(error), style="red"), soft_wrap=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
