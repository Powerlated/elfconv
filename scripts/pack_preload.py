#!/usr/bin/env python3
"""Pack host directories into the browser runtime's preload data and manifest."""
import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import sys


def pack_preload(mounts: list[str], output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    packages = []
    for spec in mounts:
        host, separator, mount = spec.rpartition("@")
        directory = Path(host).resolve()
        if not separator or not mount.startswith("/") or not directory.is_dir():
            raise ValueError(f"Invalid mount: {spec}; expected host_dir@/mount/path")
        prefix = PurePosixPath(mount)
        data_name = mount.strip("/").replace("/", "_") + ".data"
        files, directories, offset = [], set(), 0
        paths = sorted(path for path in directory.rglob("*")
                       if not any(part.startswith(".") for part in path.relative_to(directory).parts))
        with (output / data_name).open("wb") as data:
            for path in paths:
                guest = prefix / path.relative_to(directory).as_posix()
                if path.is_dir():
                    directories.add(str(guest))
                elif path.is_file():
                    start = offset
                    with path.open("rb") as stream:
                        shutil.copyfileobj(stream, data)
                    offset = data.tell()
                    files.append({"filename": str(guest), "start": start, "end": offset})
                    directories.add(str(guest.parent))
        if str(prefix) != "/":
            directories.add(str(prefix))
        for value in list(directories):
            directories.update(str(parent) for parent in PurePosixPath(value).parents if str(parent) != "/")
        directories.discard("/")
        packages.append({"packageDataName": data_name, "remote_package_size": offset,
                         "directories": sorted(directories), "files": files})
    temporary = output / "preload-manifest.json.tmp"
    temporary.write_text(json.dumps(packages) + "\n")
    temporary.replace(output / "preload-manifest.json")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mount", action="append", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    try:
        pack_preload(args.mount, args.output_dir)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
