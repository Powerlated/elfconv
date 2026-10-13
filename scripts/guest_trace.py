#!/usr/bin/env python3
"""Compact guest-stack mapping and bounded, module-relative assembly excerpts."""
import argparse
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys


@dataclass(frozen=True)
class Library:
    path: Path
    begin: int
    end: int


def sha256(path: Path) -> str:
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def llvm_string(value: str) -> str:
    data = bytearray()
    for token in re.findall(r'\\[0-9A-Fa-f]{2}|[^\\]', value):
        data.extend(bytes.fromhex(token[1:]) if token.startswith('\\') else token.encode('ascii'))
    return data.rstrip(b'\0').decode('utf-8', errors='surrogateescape')


def read_libraries(metadata: Path, llvm_dis: str | None) -> list[Library]:
    process = None
    if metadata.suffix == '.bc':
        executable = llvm_dis or shutil.which('llvm-dis')
        if not executable:
            raise ValueError('Bitcode requires --llvm-dis matching the LLVM version')
        process = subprocess.Popen([executable, str(metadata), '-o', '-'],
                                   stdout=subprocess.PIPE, text=True)
        source = process.stdout
    else:
        source = metadata.open()
    paths, table = {}, None
    try:
        # Stream past large section-byte arrays; retain only library metadata.
        for line in source:
            if line.startswith('@_ecv_i386_library_'):
                match = re.match(r'(@_ecv_i386_library_\d+_path) = .*? c"(.*)"(?:,|$)', line)
                if match:
                    paths[match[1]] = Path(llvm_string(match[2]))
            elif line.startswith('@_ecv_i386_libraries ='):
                table = line
    finally:
        source.close()
        if process and process.wait():
            raise ValueError('llvm-dis failed; no library mapping produced')
    if table is None:
        raise ValueError('Missing _ecv_i386_libraries in metadata')
    libraries = []
    for row in re.findall(r'\{ ptr @_ecv_i386_library_\d+_path, [^}]+\}', table):
        fields = row.strip('{} ').split(', ')
        if len(fields) != 15 or fields[0][4:] not in paths:
            raise ValueError('Unsupported I386DynamicLibrary metadata layout')
        libraries.append(Library(paths[fields[0][4:]], int(fields[8].removeprefix('i32 ')),
                                 int(fields[9].removeprefix('i32 '))))
    count = re.search(r'constant \[(\d+) x', table)
    if not count or len(libraries) != int(count[1]):
        raise ValueError('Incomplete library mapping; no addresses inferred')
    for previous, current in zip(sorted(libraries, key=lambda lib: lib.begin),
                                 sorted(libraries, key=lambda lib: lib.begin)[1:]):
        if previous.end > current.begin:
            raise ValueError('Overlapping guest-library ranges')
    return libraries


def stack(args: argparse.Namespace) -> None:
    libraries = read_libraries(args.metadata, args.llvm_dis)
    ghidra_bases = dict(args.ghidra_base)
    if len(ghidra_bases) != len(args.ghidra_base):
        raise ValueError('Specify each Ghidra module base only once')
    text = sys.stdin.read() if args.stack == '-' else Path(args.stack).read_text()
    frames = {}
    for match in re.finditer(r'([^\s(]+)_____([0-9a-fA-F]+)\b', text):
        address = int(match[2], 16)
        frames.setdefault(address, match[1].rsplit('.wasm.', 1)[-1])
    if not frames:
        raise ValueError('No guest-address-tagged Wasm frames in stack')
    print(f'metadata={args.metadata} sha256={sha256(args.metadata)}')
    matched = set()
    print('guest\tmodule+RVA\t' + ('ghidra\t' if ghidra_bases else '') + 'symbol')
    for address, symbol in list(frames.items())[:args.max_frames]:
        library = next((lib for lib in libraries if lib.begin <= address < lib.end), None)
        if library is None:
            location, ghidra = 'UNMAPPED', '-'
        else:
            matched.add(library)
            rva = address - library.begin
            location = f'{library.path.name}+0x{rva:x}'
            base = ghidra_bases.get(str(library.path), ghidra_bases.get(library.path.name))
            ghidra = f'0x{base + rva:x}' if base is not None else '-'
        print(f'0x{address:08x}\t{location}\t' +
              (ghidra + '\t' if ghidra_bases else '') + symbol)
    if len(frames) > args.max_frames:
        print(f'... {len(frames) - args.max_frames} unique frames omitted; increase --max-frames explicitly')
    for library in sorted(matched, key=lambda lib: lib.begin):
        identity = sha256(library.path) if library.path.is_file() else 'MISSING'
        print(f'module={library.path} guest=[0x{library.begin:x},0x{library.end:x}) sha256={identity}')


def elf_image_begin(path: Path) -> int:
    with path.open('rb') as source:
        header = source.read(52)
        if len(header) != 52 or header[:6] != b'\x7fELF\x01\x01':
            raise ValueError('Assembly mapping requires a little-endian ELF32 binary')
        offset = struct.unpack_from('<I', header, 28)[0]
        stride, count = struct.unpack_from('<HH', header, 42)
        if stride < 32:
            raise ValueError('Invalid ELF32 program-header size')
        starts = []
        for index in range(count):
            source.seek(offset + index * stride)
            program = source.read(32)
            if len(program) != 32:
                raise ValueError('Truncated ELF32 program headers')
            if struct.unpack_from('<I', program)[0] == 1:
                starts.append(struct.unpack_from('<I', program, 8)[0])
    if not starts:
        raise ValueError('No ELF load segments')
    return min(starts)


def assembly(args: argparse.Namespace) -> None:
    if not 1 <= args.bytes <= 256:
        raise ValueError('--bytes must be 1..256; split larger investigations into explicit queries')
    begin = elf_image_begin(args.elf) + args.rva
    if args.rva < 0 or begin + args.bytes > 0x100000000:
        raise ValueError('Assembly range outside ELF32 address space')
    command = [args.objdump, '-d', f'--start-address={begin}',
               f'--stop-address={begin + args.bytes}', str(args.elf)]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    header = f'module={args.elf} sha256={sha256(args.elf)} RVA=0x{args.rva:x} native=0x{begin:x}'
    args.output.write_text(header + '\n' + result.stdout)
    lines = [line.strip() for line in result.stdout.splitlines() if re.match(r'\s*[0-9a-fA-F]+:', line)]
    if not lines:
        raise ValueError(f'No instructions at requested range; full output: {args.output}')
    print(header)
    print('\n'.join(lines[:args.max_lines]))
    print(f'saved={args.output} instructions={len(lines)} omitted={max(0, len(lines) - args.max_lines)}')


def module_base(value: str) -> tuple[str, int]:
    try:
        module, address = value.rsplit('=', 1)
        base = int(address, 0)
        if not module or base < 0:
            raise ValueError()
        return module, base
    except ValueError:
        raise argparse.ArgumentTypeError('Expected MODULE=ADDRESS with a nonnegative address')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    trace = commands.add_parser('stack', help='Deduplicate and map guest-tagged stack frames')
    trace.add_argument('--metadata', type=Path, required=True)
    trace.add_argument('--llvm-dis')
    trace.add_argument('--stack', default='-', help='Stack file; default: stdin')
    trace.add_argument('--ghidra-base', type=module_base, action='append', default=[],
                       metavar='MODULE=ADDRESS',
                       help='Verified base for this module name/path only; repeat for other modules')
    trace.add_argument('--max-frames', type=int, default=32)
    trace.set_defaults(action=stack)
    asm = commands.add_parser('asm', help='Bounded assembly at an explicitly known module RVA')
    asm.add_argument('--elf', type=Path, required=True)
    asm.add_argument('--rva', type=lambda value: int(value, 0), required=True)
    asm.add_argument('--bytes', type=int, default=64)
    asm.add_argument('--max-lines', type=int, default=32)
    asm.add_argument('--objdump', default='objdump')
    asm.add_argument('--output', type=Path, required=True)
    asm.set_defaults(action=assembly)
    args = parser.parse_args()
    try:
        if getattr(args, 'max_frames', 1) < 1 or getattr(args, 'max_lines', 1) < 1:
            raise ValueError('Output limits must be positive')
        args.action(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
