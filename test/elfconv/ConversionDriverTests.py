#!/usr/bin/env python3
"""CTest regressions for conversion cache, subprocess ownership, and preload layout."""
import asyncio
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch

from rich.console import Console

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from convert_elf import Display, Driver, bounded_as_completed, digest, parser, tool
from pack_preload import pack_preload


def driver(directory: Path) -> Driver:
    options = parser().parse_args(["--input", __file__, "--target", "i386-wasm",
                                  "--output-dir", str(directory)])
    return Driver(options, Display("regression", Console(file=io.StringIO())))


async def cache(directory: Path) -> None:
    conversion = driver(directory)
    source, output, stamp = (directory / name for name in ("source", "output", "stamp"))
    source.write_text("first image")
    command = [shutil.which("cp"), source, output]

    async def stage():
        return await conversion.stage("image", "compiling", command, [output], digest(source), stamp)

    _, changed = await stage()
    assert changed and output.read_text() == "first image"
    timestamp = output.stat().st_mtime_ns
    _, changed = await stage()
    assert not changed and output.stat().st_mtime_ns == timestamp
    source.write_text("changed image")
    _, changed = await stage()
    assert changed and output.read_text() == "changed image"
    output.unlink()
    _, changed = await stage()
    assert changed and output.read_text() == "changed image"


async def failure(directory: Path) -> None:
    conversion = driver(directory)
    source, output, stamp = (directory / name for name in ("source", "output", "stamp"))
    source.write_text("valid image")
    command = [shutil.which("cp"), source, output]
    await conversion.stage("image", "compiling", command, [output], digest(source), stamp)
    source.unlink()
    try:
        await conversion.stage("image", "compiling", command, [output], "missing input", stamp)
    except RuntimeError:
        pass
    else:
        raise AssertionError("Failed command was accepted")
    assert not stamp.exists(), "Failure retained a reusable cache key"
    assert output.read_text() == "valid image"
    source.write_text("recovered image")
    _, changed = await conversion.stage("image", "compiling", command, [output], digest(source), stamp)
    assert changed and output.read_text() == "recovered image"


async def cancellation(directory: Path) -> None:
    conversion = driver(directory)
    pid_file = directory / "pids"
    task = asyncio.create_task(conversion.run("tree", "running", [
        sys.executable, Path(__file__).with_name("DriverProcessTree.py"), pid_file]))
    try:
        async with asyncio.timeout(5):
            while not pid_file.exists():
                await asyncio.sleep(0.01)
        pids = json.loads(pid_file.read_text())
        task.cancel()
        result = await asyncio.gather(task, return_exceptions=True)
        assert isinstance(result[0], asyncio.CancelledError), f"Expected cancellation, got {result[0]!r}"
        for pid in pids:
            status = Path(f"/proc/{pid}/stat")
            assert not status.exists() or status.read_text().split(") ", 1)[1].startswith("Z"), (
                f"Cancellation left process {pid} running")
    finally:
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)



async def chunk_pool() -> None:

    active = 0
    maximum = 0

    async def worker(value: int) -> int:
        nonlocal active, maximum
        active += 1
        maximum = max(maximum, active)
        try:
            await asyncio.sleep(0.01)
            return value * 2
        finally:
            active -= 1

    results = []
    async for result in bounded_as_completed(
            list(range(8)), asyncio.Semaphore(3), worker):
        results.append(result)
    assert sorted(results) == [i * 2 for i in range(8)]
    assert maximum == 3 and active == 0
    # Phase counters must not hide the ELF identity in either output mode.
    for terminal in (False, True):
        rendered = io.StringIO()
        progress = Display("conversion", Console(
            file=rendered, force_terminal=terminal, color_system=None, width=160))
        owner = "/tmp/server.so"
        with patch("conversion_display.time.monotonic", return_value=10.0):
            progress.update(owner, "lifting", "partitioning IR", counter=(0, 4))
        with patch("conversion_display.time.monotonic", return_value=12.0):
            progress.update(owner, "lifting", "partitioning IR", counter=(1, 4))
            assert progress.label(owner, progress.rows[owner]).plain.endswith("[1/4]  2.0s")
            progress.update(owner, "lifting", "writing bitcode", counter=(0, 4))
            assert progress.label(owner, progress.rows[owner]).plain.endswith("[0/4]  0.0s")
            progress.update(f"{owner}::compile", "compiling", "chunk 1/4",
                            parent=owner, counter=(0, 4))
            progress.console.print(progress.render())
        lines = [line for line in rendered.getvalue().splitlines()
                 if "partitioning IR" in line or "writing bitcode" in line or "chunk 1/4" in line]
        assert lines and all("server.so" in line for line in lines)
        assert all(re.search(r"\[\d+/\d+\]  \d+\.\ds$", line) for line in lines)
    options = parser().parse_args(["--input", __file__, "--target", "i386-wasm",
                                   "--lift-jobs", "3", "--compile-jobs", "4"])
    assert options.lift_jobs == 3
    assert options.compile_jobs == 4

def preload(directory: Path) -> None:
    source, output = directory / "input", directory / "out"
    (source / "empty").mkdir(parents=True)
    (source / "nested").mkdir()
    (source / 'nested/a"b').write_bytes(b"abc\x00")
    (source / "second").write_bytes(b"xyz")
    (source / ".hidden").write_bytes(b"not packed")
    pack_preload([f"{source}@/mnt/data"], output)
    package, = json.loads((output / "preload-manifest.json").read_text())
    data = (output / package["packageDataName"]).read_bytes()
    assert data == b"abc\x00xyz"
    assert package["remote_package_size"] == len(data)
    assert package["directories"] == ["/mnt", "/mnt/data", "/mnt/data/empty", "/mnt/data/nested"]
    entries = {entry["filename"]: data[entry["start"]:entry["end"]] for entry in package["files"]}
    assert entries == {'/mnt/data/nested/a"b': b"abc\x00", "/mnt/data/second": b"xyz"}


def compiler(directory: Path) -> None:
    executable = directory / "compiler-probe"
    subprocess.run([tool(sys.argv[2]), Path(__file__).with_name("DriverCompiler.cpp"),
                    "-o", executable], check=True)
    result = subprocess.run([executable], check=True, capture_output=True, text=True)
    assert result.stdout == "C++ driver\n"


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="elfconv-driver-") as temporary:
        directory = Path(temporary)
        case = sys.argv[1]
        if case == "preload":
            preload(directory)
        elif case == "compiler":
            compiler(directory)
        elif case == "chunk_pool":
            asyncio.run(chunk_pool())
        else:
            asyncio.run({"cache": cache, "failure": failure, "cancellation": cancellation}[case](directory))
        print(f"{case}: passed")
