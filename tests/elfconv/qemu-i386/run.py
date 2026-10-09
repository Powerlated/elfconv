#!/usr/bin/env python3
"""Compile one ELF, execute it natively and through elfconv, compare stdout."""
import argparse
import difflib
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lifter', required=True)
    parser.add_argument('--emcc', required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--node', default='node')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--profile', choices=('core', 'full'), default='core')
    linkage = parser.add_mutually_exclusive_group()
    linkage.add_argument('--pie', action='store_true', help='Build a position-independent executable')
    linkage.add_argument('--shared', action='store_true', help='Build and invoke an i386 shared library')
    parser.add_argument('--source', type=Path,
                        help='Run a standalone instruction regression instead of an upstream profile')
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    root = source.parents[2]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = output / (args.source.stem if args.source else 'qemu-i386-' + args.profile)

    def run(stage, command, timeout):
        try:
            result = subprocess.run(command, capture_output=True, text=True,
                                    errors='replace', timeout=timeout)
        except subprocess.TimeoutExpired as error:
            for stream in ('stdout', 'stderr'):
                data = getattr(error, stream) or b''
                if isinstance(data, bytes):
                    data = data.decode(errors='replace')
                (output / (stage + '.' + stream)).write_text(data)
            print(f'{stage}: timed out after {timeout}s; logs: {output}')
            return None
        for stream in ('stdout', 'stderr'):
            (output / (stage + '.' + stream)).write_text(getattr(result, stream))
        print(f'{stage}: exit {result.returncode}')
        return result

    linkage_flags = ['-fPIC', '-shared'] if args.shared else (
        ['-fPIE', '-pie'] if args.pie else ['-fno-pie', '-no-pie'])
    command = [args.cc, '-m32', '-O2', *linkage_flags,
               '-fno-stack-protector', '-ffunction-sections', '-fdata-sections',
               '-Wl,--gc-sections', str(args.source.resolve() if args.source else source / 'runner.c')]
    if not args.source and args.profile == 'full':
        command += ['-DQEMU_I386_FULL', str(source / 'vendor/test-i386-code16.S')]
    command += ['-lm', '-o', str(binary)]
    compiled = run('compile', command, 120)
    if compiled is None or compiled.returncode:
        print(f'Compilation failed; see {output}/compile.stderr')
        return 1
    native_command = [str(binary)]
    if args.shared:
        runner = output / 'shared-runner'
        compiled_runner = run('runner', [args.cc, '-m32',
            str(root / 'examples/sm64-shared-runner.c'), '-ldl', '-o', str(runner)], 120)
        if compiled_runner is None or compiled_runner.returncode:
            return 1
        native_command = [str(runner), str(binary), 'guest-argument']
    native = run('native', native_command, 120)
    if native is None or native.returncode:
        print(f'Native reference failed; logs: {output}')
        return 1
    converted = run('convert', [args.cmake,
        f'-DELFCONV_INPUT={binary}', '-DELFCONV_TARGET=i386-wasm',
        f'-DELFCONV_OUTPUT_DIR={output / "wasm"}',
        f'-DELFCONV_LIFTER={args.lifter}', f'-DELFCONV_EMCC={args.emcc}',
        *(['-DELFCONV_ENTRY_SYMBOL=main'] if args.shared else []),
        '-DELFCONV_LEGACY_GL=0', '-P', str(root / 'cmake/ConvertElf.cmake')], 600)
    if converted is None or converted.returncode:
        print(f'Conversion failed; logs: {output}/convert.stdout and convert.stderr')
        return 1
    unsupported = [line for line in (converted.stdout + converted.stderr).splitlines()
                   if 'Unsupported instruction' in line]
    if unsupported:
        print(f'Unsupported instruction sites: {len(unsupported)} (see conversion logs)')
    lifted = run('lifted', [args.node, str(output / 'wasm' / (binary.name + '.js')),
                          *(['guest-argument'] if args.shared else [])], 120)
    if lifted is None:
        return 1
    expected = native.stdout.splitlines(keepends=True)
    actual = lifted.stdout.splitlines(keepends=True)
    differences = list(difflib.unified_diff(expected, actual,
                       fromfile='native', tofile='lifted'))
    (output / 'output.diff').write_text(''.join(differences))
    mismatches = sum(a != b for a, b in zip(expected, actual))
    print(f'Native rows: {len(expected)}; lifted rows: {len(actual)}; '
          f'positional mismatches in common prefix: {mismatches}')
    if differences:
        print(''.join(differences[:80]), end='')
    print(f'Complete stdout, stderr, and diff: {output}')
    if args.pie and lifted.stderr != native.stderr:
        print('PIE stderr differs from native reference')
        return 1
    # Unsupported instructions cannot count as a successful conformance run,
    # even if the lifter emits a no-op and the output happens to match.
    return int(bool(lifted.returncode or unsupported or differences))


if __name__ == '__main__':
    sys.exit(main())
