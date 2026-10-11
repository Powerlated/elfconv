#!/usr/bin/env python3
"""Compile, lift, and compare one i386 pthread fixture in Chromium."""
import argparse
import difflib
import os
from pathlib import Path
import subprocess
import sys


def run(stage, command, *, cwd, env=None, timeout=120):
    print(f"{stage}: {' '.join(map(str, command))}")
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True,
                            text=True, errors="replace", timeout=timeout)
    (cwd / f"{stage}.stdout").write_text(result.stdout)
    (cwd / f"{stage}.stderr").write_text(result.stderr)
    print(f"{stage}: exit {result.returncode}")
    if result.returncode:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lifter", required=True)
    parser.add_argument("--emcc", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--node", required=True)
    parser.add_argument("--browser-dir", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[4]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = output / args.source.stem
    native = run("native-compile", [args.cc, "-m32", "-O2", "-fno-pie", "-no-pie",
        "-pthread", str(args.source.resolve()), "-o", str(binary)], cwd=output, timeout=120)
    if native.returncode:
        return 1
    native = run("native", [str(binary)], cwd=output, timeout=120)
    if native.returncode:
        print("Native reference failed")
        return 1

    wasm_dir = output / "wasm"
    converted = run("convert", [sys.executable, str(root / "scripts/convert_elf.py"),
        "--input", str(binary), "--target", "i386-wasm", "--output-dir", str(wasm_dir),
        "--lifter", args.lifter, "--emcc", args.emcc, "--legacy-gl", "0", "--pthreads"],
        cwd=output, timeout=900)
    if converted.returncode:
        print("Conversion failed")
        return 1
    diagnostics = converted.stdout + converted.stderr
    for log in (wasm_dir / ".elfconv-logs").glob("*.log"):
        diagnostics += log.read_text(errors="replace")
    unsupported = [line for line in diagnostics.splitlines()
                   if any(marker in line for marker in (
                       "Unsupported instruction", "kLiftedMismatchedISEL", "Instruction decode of"))]
    if unsupported:
        print(f"Unsupported instruction sites: {len(unsupported)}")
        return 1

    html = wasm_dir / f"{binary.name}.html"
    if not html.is_file():
        print(f"Missing generated browser page: {html}")
        return 1
    browser_dir = args.browser_dir.resolve()
    playwright = browser_dir / "node_modules/@playwright/test/cli.js"
    playwright_package = browser_dir / "node_modules/@playwright/test"
    if not playwright.is_file() or not playwright_package.is_dir():
        print(f"Missing Playwright dependency in {browser_dir}/node_modules; install test/browser dependencies")
        return 1
    env = os.environ.copy()
    env.update({
        "ELFCONV_PLAYWRIGHT_PACKAGE": str(playwright_package),
        "ELFCONV_PTHREAD_NODE": str(Path(args.node).resolve()),
        "ELFCONV_PTHREAD_SERVE_DIR": str(wasm_dir),
        "ELFCONV_PTHREAD_TEST_HTML": str(html),
        "ELFCONV_PTHREAD_NATIVE_STDOUT": str(output / "native.stdout"),
        "ELFCONV_PTHREAD_NATIVE_STDERR": str(output / "native.stderr"),
        "ELFCONV_PTHREAD_OUTPUT_DIR": str(output),
    })
    browser = run("chromium", [args.node, str(playwright), "test", "--config",
        str(Path(__file__).with_name("playwright.config.js")),
        str(Path(__file__).with_name("pthread.spec.js"))],
        cwd=output, env=env, timeout=240)
    if browser.returncode:
        return 1
    for stream in ("stdout", "stderr"):
        actual = (output / f"lifted.{stream}").read_text(errors="replace")
        expected = getattr(native, stream)
        if actual != expected:
            diff = "".join(difflib.unified_diff(expected.splitlines(keepends=True),
                actual.splitlines(keepends=True), fromfile=f"native.{stream}",
                tofile=f"lifted.{stream}"))
            (output / f"{stream}.diff").write_text(diff)
            print(diff, end="")
            return 1
    print(f"Native and browser stdout/stderr match; logs: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
