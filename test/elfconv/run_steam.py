#!/usr/bin/env python3
"""Exercise real gbe_fork Steam interfaces and callbacks natively and lifted."""
import argparse
import difflib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def command(args, *, cwd, env, timeout, log):
    print("+", " ".join(map(str, args)), flush=True)
    log.mkdir(parents=True, exist_ok=True)
    try:
        result = subprocess.run(args, cwd=cwd, env=env, capture_output=True,
                                text=True, errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired as error:
        for stream in ("stdout", "stderr"):
            data = getattr(error, stream) or b""
            if isinstance(data, bytes):
                data = data.decode(errors="replace")
            (log / f"timeout.{stream}").write_text(data)
        raise RuntimeError(f"command timed out after {timeout}s: {args[0]}") from error
    (log / "stdout").write_text(result.stdout)
    (log / "stderr").write_text(result.stderr)
    return result


def require_i386(readelf, path):
    result = subprocess.run([readelf, "-h", str(path)], capture_output=True,
                            text=True, errors="replace", check=True)
    if not re.search(r"^\s*Class:\s+ELF32\s*$", result.stdout, re.MULTILINE) or \
            not re.search(r"^\s*Machine:\s+Intel 80386\s*$", result.stdout, re.MULTILINE):
        raise RuntimeError(f"not an i386 ELF: {path}")


def runtime_library(cxx, name, output, readelf):
    result = subprocess.run([cxx, "-m32", f"-print-file-name={name}"],
                            capture_output=True, text=True, check=True)
    candidate = Path(result.stdout.strip())
    if not candidate.is_absolute():
        candidate = Path.cwd() / candidate
    if not candidate.is_file():
        raise RuntimeError(f"32-bit compiler runtime {name} not found: {result.stdout.strip()}")
    candidate = candidate.resolve(strict=True)
    require_i386(readelf, candidate)
    bundled = output / name
    shutil.copy2(candidate, bundled)
    return bundled


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lifter", required=True)
    parser.add_argument("--emcc", required=True)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--node", default="node")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--steam-api", type=Path, required=True)
    parser.add_argument("--gbe-source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    steam_api = args.steam_api.resolve(strict=True)
    steam_source = args.gbe_source.resolve(strict=True)
    require_i386(args.readelf, steam_api)

    (output / "steam_appid.txt").write_text("400\n")
    home = output / "home"
    home.mkdir(exist_ok=True)
    xdg_data = home / ".local" / "share"
    xdg_data.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["HOME"] = str(home)
    env["XDG_DATA_HOME"] = str(xdg_data)

    stdcxx = runtime_library(args.cxx, "libstdc++.so.6", output, args.readelf)
    gcc_s = runtime_library(args.cxx, "libgcc_s.so.1", output, args.readelf)
    env["LD_LIBRARY_PATH"] = os.pathsep.join(
        item for item in (str(output), str(steam_api.parent), env.get("LD_LIBRARY_PATH", "")) if item
    )

    executable = output / "steam-main"
    built = command([args.cxx, "-m32", "-std=c++17", "-O2", "-fno-pie", "-no-pie",
                     "-pthread", "-I", str(steam_source / "sdk"),
                     str(root / "test/elfconv/i386/steam-main.cpp"), str(steam_api),
                     "-Wl,-rpath,$ORIGIN", "-Wl,-z,defs", "-o", str(executable)],
                    cwd=output, env=env, timeout=120, log=output / "compile")
    if built.returncode:
        sys.stderr.write(built.stderr)
        return 1

    native = command([str(executable)], cwd=output, env=env, timeout=60,
                     log=output / "native")
    if native.returncode:
        sys.stderr.write(native.stderr)
        return 1
    wasm_dir = output / "wasm"
    runtime_archive = Path(args.lifter).resolve().with_name(
        "libelfconv-runtime-i386-wasm-incremental.a")
    if not runtime_archive.is_file():
        print(f"Missing prebuilt Wasm runtime archive: {runtime_archive}; build elflift first",
              file=sys.stderr)
        return 1
    converted = command([sys.executable, str(root / "scripts/convert_elf.py"),
                         "--input", str(executable), "--target", "i386-wasm",
                         "--output-dir", str(wasm_dir), "--lifter", args.lifter,
                         "--emcc", args.emcc, "--legacy-gl", "0",
                         "--runtime-archive", str(runtime_archive),
                         "--shared-library", str(steam_api),
                         "--shared-library", str(stdcxx),
                         "--shared-library", str(gcc_s)],
                        cwd=output, env=env, timeout=900, log=output / "convert")
    if converted.returncode:
        sys.stderr.write(converted.stdout + converted.stderr)
        return 1

    lifted = command([args.node, str(wasm_dir / "steam-main.js")], cwd=output,
                     env=env, timeout=120, log=output / "lifted")
    if lifted.returncode:
        sys.stderr.write(lifted.stderr)
        return 1

    difference = "".join(difflib.unified_diff(
        native.stdout.splitlines(keepends=True), lifted.stdout.splitlines(keepends=True),
        fromfile="native", tofile="lifted"))
    (output / "output.diff").write_text(difference)
    if difference:
        sys.stderr.write(difference)
        return 1
    sys.stdout.write(native.stdout)
    print(f"Steam native/Wasm outputs match; logs: {output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Steam fixture error: {error}", file=sys.stderr)
        sys.exit(1)
