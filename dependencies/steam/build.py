#!/usr/bin/env python3
"""Fetch and build the pinned i386 gbe_fork Steam API guest library."""

from pathlib import Path
import os
import shutil
import subprocess
import sys

UPSTREAM = "https://github.com/Detanup01/gbe_fork.git"
REVISION = "7103add7ca6ef3ef8353279219880068f2494fc8"
ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "build" / "gbe_fork"


def run(args, *, cwd=None, env=None):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(args, cwd=cwd, env=env, check=True)


def main():
    if sys.platform != "linux":
        raise SystemExit("gbe_fork guest build requires Linux")
    for command in ("git", "make", "cmake"):
        if shutil.which(command) is None:
            raise SystemExit(f"required command not found: {command}")

    try:
        jobs = int(os.environ.get("ELFCONV_STEAM_BUILD_JOBS", "4"))
        if jobs < 1:
            raise ValueError
    except ValueError as exc:
        raise SystemExit("ELFCONV_STEAM_BUILD_JOBS must be a positive integer") from exc

    if not SOURCE.exists():
        SOURCE.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--filter=blob:none", "--depth", "1",
             UPSTREAM, str(SOURCE)])
    if not (SOURCE / ".git").exists():
        raise SystemExit(f"not a gbe_fork checkout: {SOURCE}")

    current = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=SOURCE, text=True
    ).strip()
    if current != REVISION:
        if subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=SOURCE, text=True
        ).strip():
            raise SystemExit(f"refusing to replace modified checkout at {SOURCE}")
        run(["git", "fetch", "--depth", "1", "origin", REVISION], cwd=SOURCE)
        run(["git", "checkout", "--detach", REVISION], cwd=SOURCE)
    run(["git", "submodule", "update", "--init", "--recursive"], cwd=SOURCE)

    premake = SOURCE / "third-party" / "common" / "linux" / "premake" / "premake5"
    if not premake.is_file():
        raise SystemExit(f"upstream premake executable not found: {premake}")
    premake.chmod(premake.stat().st_mode | 0o100)
    env = os.environ.copy()
    env["CMAKE_GENERATOR"] = "Unix Makefiles"

    # The upstream target links these 32-bit dependencies; building x64/ARM or
    # overlay targets is unnecessary for the guest Steam API library.
    dependencies = ("ssq", "zlib", "curl", "protobuf", "mbedtls", "opus", "portaudio", "sdl")
    run([str(premake), "--file=premake5-deps.lua", "--32-build", f"--j={jobs}",
         *[f"--ext-{name}" for name in dependencies],
         *[f"--build-{name}" for name in dependencies],
         "--os=linux", "gmake"], cwd=SOURCE, env=env)
    # Upstream --genproto selects host-arch install64. The built i386 protoc
    # runs on the Linux multilib host and keeps generated code/runtime versions matched.
    protoc = SOURCE / "build/deps/linux/gmake/protobuf/install32/bin/protoc"
    proto_output = SOURCE / "proto_gen/linux"
    (proto_output / "tf2").mkdir(parents=True, exist_ok=True)
    run([str(protoc), "dll/gc_steam/steammessages.proto", "-I./dll/gc_steam",
         f"--cpp_out={proto_output}"], cwd=SOURCE)
    run([str(protoc), *[path.relative_to(SOURCE) for path in sorted((SOURCE / "dll/gc_tf2").glob("*.proto"))],
         "-I./dll/gc_steam", "-I./dll/gc_tf2", f"--cpp_out={proto_output / 'tf2'}"], cwd=SOURCE)
    run([str(protoc), "dll/net.proto", "-I./dll", f"--cpp_out={proto_output}"], cwd=SOURCE)
    run([str(premake), "--file=premake5.lua", "--os=linux", "gmake"], cwd=SOURCE, env=env)

    build_dir = SOURCE / "build/project/gmake/linux"
    run(["make", "-j", str(jobs), "config=release_x86", "api_regular"], cwd=build_dir)

    library = SOURCE / "build/linux/gmake/release/regular/x86/libsteam_api.so"
    if not library.is_file():
        raise SystemExit(f"expected i386 Steam API output was not produced: {library}")
    print(library)


if __name__ == "__main__":
    main()
