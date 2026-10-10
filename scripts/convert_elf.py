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

from rich.console import Console
from rich.live import Live
from rich.spinner import Spinner
from rich.text import Text
from rich.tree import Tree

from pack_preload import pack_preload

ROOT = Path(__file__).resolve().parents[1]
COMMON = [ROOT / "src" / path for path in (
    "runtime/Entry.cpp", "runtime/Memory.cpp", "runtime/Runtime.cpp",
    "runtime/VmIntrinsics.cpp", "utils/Util.cpp", "utils/elfconv.cpp")]


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def key(*values: object) -> str:
    return hashlib.sha256(json.dumps(values, sort_keys=True, default=str).encode()).hexdigest()


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
    result.add_argument("--bitcode-path", type=Path)
    result.add_argument("--link-map", type=Path)
    result.add_argument("--object-base", type=Path)
    result.add_argument("--wasm-opt-level", choices=list("0123sz"), default="3")
    result.add_argument("--legacy-gl", choices=("0", "1"), default="1")
    result.add_argument("--conversion-jobs", type=int, default=2)
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
        self.spinners: dict[str, Spinner] = {}
        self.live = Live(self.render(), console=console, refresh_per_second=8,
                         auto_refresh=False)

    def update(self, name: str, state: str, detail: str = "") -> None:
        previous = self.rows.get(name)
        started = previous[2] if previous and previous[0] != "queued" else time.monotonic()
        self.rows[name] = state, detail, started
        if not self.console.is_terminal:
            self.console.print(Text(f"{name}: {state}{' — ' + detail if detail else ''}"), soft_wrap=True)

    def render(self) -> Tree:
        done = sum(state in ("done", "cached") for state, _, _ in self.rows.values())
        tree = Tree(Text(f"{self.name} [{done}/{len(self.rows)}]"))
        for name, (state, detail, started) in self.rows.items():
            label = Text(f"{Path(name).name if Path(name).is_absolute() else name}: {state}", style={"failed": "red", "done": "green",
                         "cached": "dim"}.get(state, ""))
            if detail:
                label.append(f" — {detail}")
            if state in ("lifting", "compiling", "linking", "running"):
                label.append(f"  {time.monotonic() - started:.1f}s", style="dim")
                if name not in self.spinners:
                    self.spinners[name] = Spinner("dots")
                spinner = self.spinners[name]
                spinner.update(text=label)
                tree.add(spinner)
            else:
                tree.add(label)
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
        flags = [f"-O{self.a.wasm_opt_level}", *self.thread_link,
                 *(["-g2"] if self.a.wasm_opt_level == "0" else []), *self.includes,
                 "-std=c++17", *self.definitions, "-DELF_IS_I386", "-DADDRESS_SIZE_BITS=32",
                 "-sUSE_SDL=2", f"-sLEGACY_GL_EMULATION={self.a.legacy_gl}",
                 "-sMIN_WEBGL_VERSION=2", "-sMAX_WEBGL_VERSION=2", f"-DECV_LEGACY_GL={self.a.legacy_gl}",
                 "-sJSPI=1" if self.a.jspi or self.a.pthreads else "-sASYNCIFY=1",
                 "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=335544320", "-sSTACK_SIZE=1048576", "-sEXIT_RUNTIME=1"]
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
        await self.run("Bundle scan", "running", [a.lifter, *flags, "--unit_manifest_out", manifest,
                                                   "--metadata_fingerprint_out", fingerprint])
        self.display.update("Bundle scan", "done")
        entries = []
        for line in manifest.read_text().splitlines():
            owner, separator, path = line.partition("\t")
            if not separator or not owner or not Path(path).is_file():
                raise ValueError(f"Malformed unit manifest entry: {line}")
            entries.append((owner, Path(path), hashlib.sha256(owner.encode()).hexdigest()[:24]))
        if not entries:
            raise ValueError("The ELF bundle produced no code units")
        for owner, _, _ in entries:
            self.display.update(owner, "queued", "lift → compile")
        for name in ["Shared metadata", "Unit registry",
                     *[f"Runtime/{source.name}" for source in [*COMMON, ROOT / "src/runtime/I386Imports.cpp"]],
                     "Final link"]:
            self.display.update(name, "queued")
        semantics = []
        if a.bitcode_path:
            paths = sorted(a.bitcode_path.rglob("*.bc")) if a.bitcode_path.is_dir() else [a.bitcode_path]
            semantics = [(str(path), digest(path)) for path in paths]
        lift_identity = key(digest(Path(a.lifter)), semantics)
        compiler_identity = self.compiler_identity[emcc]
        compile_flags = [f"-O{a.wasm_opt_level}", *self.thread_compile]
        metadata_bc, metadata_object = self.cache / "metadata.bc", self.cache / "metadata.wasm.o"
        metadata_key, metadata_changed = await self.stage("Shared metadata", "lifting",
            [a.lifter, *flags, "--metadata_only", "--bc_out", metadata_bc], [metadata_bc],
            [digest(fingerprint), lift_identity], self.cache / "metadata.lift.key")
        metadata_compile_key, _ = await self.stage("Shared metadata", "compiling",
            [emcc, *compile_flags, "-c", metadata_bc, "-o", metadata_object], [metadata_object],
            [metadata_key, compiler_identity], self.cache / "metadata.compile.key", force=metadata_changed)
        self.display.update("Shared metadata", "done")
        semaphore = asyncio.Semaphore(a.conversion_jobs)

        async def convert_unit(entry: tuple[str, Path, str]) -> tuple[Path, str]:
            owner, fingerprint_path, unit_id = entry
            async with semaphore:
                bc = self.cache / f"unit-{unit_id}.bc"
                obj = self.cache / f"unit-{unit_id}.wasm.o"
                lift_key, changed = await self.stage(owner, "lifting",
                    [a.lifter, *flags, "--unit_owner", owner, "--unit_id", unit_id, "--bc_out", bc],
                    [bc], [digest(fingerprint_path), lift_identity], self.cache / f"unit-{unit_id}.lift.key")
                compile_key, compiled = await self.stage(owner, "compiling",
                    [emcc, *compile_flags, "-c", bc, "-o", obj], [obj],
                    [lift_key, compiler_identity], self.cache / f"unit-{unit_id}.compile.key", force=changed)
                self.display.update(owner, "done" if changed or compiled else "cached", "2/2 stages")
                return obj, compile_key

        tasks = [asyncio.create_task(convert_unit(entry)) for entry in entries]
        try:
            units = await asyncio.gather(*tasks)
        finally:
            for task in tasks:
                if not task.done():
                    task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
        headers = sorted(path for directory in (ROOT / "src/runtime", ROOT / "src/utils", ROOT / "backend/remill/include")
                         for path in directory.rglob("*") if path.suffix in (".h", ".hpp", ".inc"))
        header_key = key([(str(path), digest(path)) for path in headers])
        runtime_flags = [*compile_flags, "-sUSE_SDL=2", *self.includes, "-std=c++17", *self.definitions,
                         "-DELF_IS_I386", "-DADDRESS_SIZE_BITS=32", "-DELFCONV_INCREMENTAL_UNITS=1"]
        registry = self.cache / "UnitRegistry.cpp"
        registry_text = self.registry([entry[2] for entry in entries])
        atomic_text(registry, registry_text)
        registry_object = self.cache / "UnitRegistry.wasm.o"
        registry_key, _ = await self.stage("Unit registry", "compiling",
            [emcc, *runtime_flags, "-c", registry, "-o", registry_object], [registry_object],
            [key(registry_text), header_key, compiler_identity], self.cache / "UnitRegistry.compile.key")
        self.display.update("Unit registry", "done")
        runtime_objects, runtime_keys = [], []
        for source in [*COMMON, ROOT / "src/runtime/I386Imports.cpp"]:
            obj = self.cache / f"runtime-{source.stem}.wasm.o"
            stage_key, _ = await self.stage(f"Runtime/{source.name}", "compiling",
                [emcc, *runtime_flags, f"-DECV_LEGACY_GL={a.legacy_gl}", "-c", source, "-o", obj],
                [obj], [digest(source), header_key, compiler_identity], self.cache / f"runtime-{source.stem}.compile.key")
            self.display.update(f"Runtime/{source.name}", "done")
            runtime_objects.append(obj)
            runtime_keys.append(stage_key)
        js, wasm = self.output / f"{a.elf_name}.js", self.output / f"{a.elf_name}.wasm"
        _, linked = await self.stage("Final link", "linking",
            [emcc, *self.browser_link_flags(True), *[obj for obj, _ in units], metadata_object,
             registry_object, *runtime_objects, "-o", js], [js, wasm],
            [metadata_compile_key, registry_key, [stamp for _, stamp in units], runtime_keys, compiler_identity],
            self.cache / "link.key")
        self.display.update("Final link", "done" if linked else "cached")
        self.html()

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
                                                       "-c", ir, "-o", obj])
                await self.run("Final link", "linking", [emcc, *self.browser_link_flags(False), obj,
                    *COMMON, ROOT / "src/runtime/I386Imports.cpp", "-o", self.output / f"{name}.js"])
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
    if a.conversion_jobs < 1:
        raise ValueError("--conversion-jobs must be positive")
    if a.pthreads and a.target != "i386-wasm":
        raise ValueError("--pthreads requires i386-wasm")
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


async def convert(a: argparse.Namespace, console: Console) -> None:
    display = Display(a.elf_name, console)
    driver = Driver(a, display)
    refresh = asyncio.create_task(display.refresh())
    try:
        if console.is_terminal:
            display.live.start()
        if a.ready_js:
            driver.prepare_browser()
        elif a.incremental_units or (a.shared_library and a.target == "i386-wasm"):
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
