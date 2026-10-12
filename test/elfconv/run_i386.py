#!/usr/bin/env python3
"""Compile one ELF, execute it natively and through elfconv, compare output."""
import argparse
import difflib
from pathlib import Path
import os
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lifter', required=True)
    parser.add_argument('--emcc', required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--node', default='node')
    parser.add_argument('--objcopy', default='objcopy')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--no-builtin', action='store_true', help='Exercise libc adapters rather than compiler builtins')
    parser.add_argument('--profile', choices=('core', 'full'), default='core')
    linkage = parser.add_mutually_exclusive_group()
    linkage.add_argument('--pie', action='store_true', help='Build a position-independent executable')
    linkage.add_argument('--shared', action='store_true', help='Build and invoke an i386 shared library')
    linkage.add_argument('--dynamic', action='store_true', help='Build bundled dlopen libraries')
    linkage.add_argument('--rtti', action='store_true', help='Bundle real compiler C++ RTTI code')
    parser.add_argument('--startup-linked', action='store_true',
                        help='Link the dynamic fixture against its plugin at startup')
    parser.add_argument('--tls', action='store_true', help='Exercise bundled ELF TLS')
    parser.add_argument('--tls-dialect', choices=('gnu', 'gnu2'), default='gnu')
    parser.add_argument('--tls-model', choices=('global-dynamic', 'initial-exec', 'local-dynamic'),
                        default='global-dynamic')
    parser.add_argument('--source', type=Path,
                        help='Run a standalone instruction regression instead of an upstream profile')
    parser.add_argument('--stdin-file', type=Path,
                        help='Feed this file to both native and lifted executions')
    parser.add_argument('--runtime-library-path', type=Path, action='append', default=[],
                        help='Add a directory to native/lifted LD_LIBRARY_PATH (repeatable)')
    parser.add_argument('--extra-source', type=Path, action='append', default=[],
                        help='Add a source file to a standalone fixture')
    parser.add_argument('--compile-flag', action='append', default=[],
                        help='Add one compiler flag to a standalone fixture (repeatable)')
    parser.add_argument('--link-flag', action='append', default=[],
                        help='Add one linker argument after standalone fixture sources (repeatable)')
    parser.add_argument('--wasm-opt-level', choices=list('0123sz'),
                        help='Set the Emscripten optimization level for the lifted test')
    parser.add_argument('--pthreads', action='store_true',
                        help='Link with pthread support and enable Emscripten pthreads')
    parser.add_argument('--no-asyncify', action='store_true',
                        help='Disable Asyncify for the converted test binary')
    parser.add_argument('--browser', action='store_true',
                        help='Run lifted output in Chromium using the browser test harness')
    parser.add_argument('--compare-stderr', action='store_true',
                        help='Require stderr to match between native and lifted runs')
    args = parser.parse_args()
    if args.browser and (not args.pthreads or not args.source):
        parser.error('--browser requires --pthreads and --source')
    if args.startup_linked and not args.dynamic:
        parser.error('--startup-linked requires --dynamic')
    if args.tls and not args.dynamic:
        parser.error('--tls requires --dynamic')
    if (args.extra_source or args.compile_flag or args.link_flag or args.runtime_library_path
            or args.stdin_file or args.compare_stderr) and not args.source:
        parser.error('--stdin-file/--extra-source/--compile-flag/--link-flag/'
                     '--runtime-library-path/--compare-stderr require --source')
    root = Path(__file__).resolve().parents[2]
    source = root / 'test/elfconv' / ('i386' if args.source or args.dynamic or args.rtti else 'qemu-i386')
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    binary = output / (args.source.stem if args.source else 'qemu-i386-' + args.profile)

    def run(stage, command, timeout, env=None):
        stdin = args.stdin_file.open('rb') if args.stdin_file and stage in ('native', 'lifted') else None
        try:
            environment = env
            if stage in ('native', 'lifted') and args.runtime_library_path:
                environment = os.environ.copy()
                paths = [str(path.resolve()) for path in args.runtime_library_path]
                inherited = environment.get('LD_LIBRARY_PATH')
                if inherited:
                    paths.append(inherited)
                environment['LD_LIBRARY_PATH'] = os.pathsep.join(paths)
            result = subprocess.run(command, capture_output=True, text=True,
                                    errors='replace', timeout=timeout,
                                    cwd=output if stage in ('native', 'lifted', 'browser') or args.dynamic or args.rtti else None,
                                    env=environment, stdin=stdin)
        except subprocess.TimeoutExpired as error:
            for stream in ('stdout', 'stderr'):
                data = getattr(error, stream) or b''
                if isinstance(data, bytes):
                    data = data.decode(errors='replace')
                (output / (stage + '.' + stream)).write_text(data)
            print(f'{stage}: timed out after {timeout}s; logs: {output}')
            return None
        finally:
            if stdin is not None:
                stdin.close()
        for stream in ('stdout', 'stderr'):
            (output / (stage + '.' + stream)).write_text(getattr(result, stream))
        print(f'{stage}: exit {result.returncode}')
        return result

    linkage_flags = ['-fPIC', '-shared'] if args.shared else (
        ['-fPIE', '-pie'] if args.pie or args.dynamic or args.rtti else ['-fno-pie', '-no-pie'])
    libraries = []
    if args.dynamic:
        dependency = output / 'libdependency.so'
        plugin = output / 'libplugin.so'
        prefix = 'tls' if args.tls else 'dynamic'
        tls_flags = [f'-mtls-dialect={args.tls_dialect}', f'-ftls-model={args.tls_model}'] if args.tls else []
        for name, target, extras in (
                (prefix + '-dependency', dependency, ['-Wl,-soname,libdependency.so']),
                (prefix + '-plugin', plugin, ['-Wl,-soname,libplugin.so',
                    f'-L{output}', '-ldependency', '-Wl,-rpath,$ORIGIN'])):
            result = run(name, [args.cc, '-m32', '-O2', '-fPIC', '-shared',
                '-fno-stack-protector', *tls_flags, str(source / (name + '.c')), *extras,
                '-Wl,-z,defs', '-o', str(target)], 120)
            if result is None or result.returncode:
                return 1
            stripped = run(name + '-strip-callbacks', [args.objcopy,
                '--strip-symbol=initialize', '--strip-symbol=finalize', str(target)], 120)
            if stripped is None or stripped.returncode:
                return 1
        libraries = [dependency, plugin]
    if args.rtti:
        archive = run('rtti-archive', [args.cxx, '-m32', '-print-file-name=libsupc++.a'], 30)
        if archive is None or archive.returncode:
            return 1
        provider = output / 'librtti.so'
        required = ('_ZTVN10__cxxabiv117__class_type_infoE',
                    '_ZTVN10__cxxabiv120__si_class_type_infoE',
                    '_ZTVN10__cxxabiv121__vmi_class_type_infoE', '__dynamic_cast')
        built = run('rtti-provider', [args.cxx, '-m32', '-shared', '-nostdlib',
            *[f'-Wl,-u,{symbol}' for symbol in required], archive.stdout.strip(),
            '-lssp_nonshared', '-Wl,-soname,librtti.so', '-o', str(provider)], 120)
        if built is None or built.returncode:
            return 1
        plugin = output / 'libplugin.so'
        built = run('rtti-plugin', [args.cxx, '-m32', '-O2', '-std=c++11',
            '-fno-exceptions', '-fPIC', '-shared', str(source / 'rtti-plugin.cpp'),
            f'-L{output}', '-lrtti', '-Wl,-rpath,$ORIGIN', '-Wl,-z,defs',
            '-Wl,-soname,libplugin.so', '-o', str(plugin)], 120)
        if built is None or built.returncode:
            return 1
    command = [args.cxx if args.rtti else args.cc, '-m32', '-O2', *linkage_flags,
               *(['-pthread'] if args.pthreads else []),
               *args.compile_flag, '-fno-stack-protector', '-ffunction-sections', '-fdata-sections',
               '-Wl,--gc-sections']
    if args.source:
        command += [str(args.source.resolve()),
                    *[str(path.resolve()) for path in args.extra_source]]
    else:
        command += [str(source / 'runner.c')]
    if args.no_builtin:
        command += ['-fno-builtin']
    if not args.source and args.profile == 'full':
        command += ['-DQEMU_I386_FULL', str(source / 'vendor/test-i386-code16.S')]
    command += [*args.link_flag, '-lm', '-o', str(binary)]
    if args.tls:
        command += ['-fstack-protector-all']
    if args.dynamic:
        command += ['-rdynamic', '-ldl', f'-L{output}', '-Wl,-rpath,$ORIGIN']
        if args.startup_linked:
            command += ['-DSTARTUP_LINKED', '-lplugin']
    if args.rtti:
        command += ['-std=c++11', '-fno-exceptions', '-rdynamic', '-ldl',
                    f'-L{output}', '-lrtti', '-Wl,-rpath,$ORIGIN']
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
    converted = run('convert', [sys.executable, str(root / 'scripts/convert_elf.py'),
        '--input', str(binary), '--target', 'i386-wasm',
        '--output-dir', str(output / 'wasm'),
        '--lifter', args.lifter, '--emcc', args.emcc,
        *(['--entry-symbol', 'main'] if args.shared else []),
        *(['--wasm-opt-level', args.wasm_opt_level] if args.wasm_opt_level else []),
        *[argument for library in libraries for argument in ('--shared-library', str(library))],
        *(['--pthreads'] if args.pthreads else []),
        *(['--no-asyncify'] if args.no_asyncify else []),
        '--legacy-gl', '0'], 600)
    if converted is None or converted.returncode:
        print(f'Conversion failed; logs: {output}/convert.stdout and convert.stderr')
        return 1
    tool_output = converted.stdout + converted.stderr
    for log in (output / 'wasm' / '.elfconv-logs').glob('*.log'):
        tool_output += log.read_text(errors='replace')
    unsupported = [line for line in tool_output.splitlines()
                   if any(marker in line for marker in (
                       'Unsupported instruction', 'kLiftedMismatchedISEL', 'Instruction decode of'))]
    if unsupported:
        print(f'Unsupported instruction sites: {len(unsupported)} (see conversion logs)')
    if args.browser:
        browser_dir = root / 'test/browser'
        playwright = browser_dir / 'node_modules/@playwright/test/cli.js'
        playwright_package = browser_dir / 'node_modules/@playwright/test'
        if not playwright.is_file() or not playwright_package.is_dir():
            print(f'Missing Playwright dependency in {browser_dir}/node_modules')
            return 1
        environment = os.environ.copy()
        environment.update({
            'ELFCONV_PLAYWRIGHT_PACKAGE': str(playwright_package),
            'ELFCONV_PTHREAD_NODE': str(Path(args.node).resolve()),
            'ELFCONV_PTHREAD_SERVE_DIR': str(output / 'wasm'),
            'ELFCONV_PTHREAD_TEST_HTML': str(output / 'wasm' / (binary.name + '.html')),
            'ELFCONV_PTHREAD_NATIVE_STDOUT': str(output / 'native.stdout'),
            'ELFCONV_PTHREAD_NATIVE_STDERR': str(output / 'native.stderr'),
            'ELFCONV_PTHREAD_OUTPUT_DIR': str(output),
        })
        browser_tests = args.source.resolve().parent
        browser = run('browser', [args.node, str(playwright), 'test', '--config',
            str(browser_tests / 'playwright.config.js'),
            str(browser_tests / 'pthread.spec.js')], 240, env=environment)
        if browser is None or browser.returncode:
            return 1
        lifted_stdout = (output / 'lifted.stdout').read_text(errors='replace')
        lifted_stderr = (output / 'lifted.stderr').read_text(errors='replace')
        lifted_returncode = 0
    else:
        lifted = run('lifted', [args.node, str(output / 'wasm' / (binary.name + '.js')),
                              *(['guest-argument'] if args.shared else [])], 120)
        if lifted is None:
            return 1
        lifted_stdout = lifted.stdout
        lifted_stderr = lifted.stderr
        lifted_returncode = lifted.returncode
    expected = native.stdout.splitlines(keepends=True)
    actual = lifted_stdout.splitlines(keepends=True)
    differences = list(difflib.unified_diff(expected, actual,
                       fromfile='native', tofile='lifted'))
    (output / 'output.diff').write_text(''.join(differences))
    mismatches = sum(a != b for a, b in zip(expected, actual))
    print(f'Native rows: {len(expected)}; lifted rows: {len(actual)}; '
          f'positional mismatches in common prefix: {mismatches}')
    if differences:
        print(''.join(differences[:80]), end='')
    print(f'Complete stdout, stderr, and diff: {output}')
    if (args.pie or args.browser or args.compare_stderr) and lifted_stderr != native.stderr:
        print('Lifted stderr differs from native reference')
        return 1
    # Unsupported instructions cannot count as a successful conformance run,
    # even if the lifter emits a no-op and the output happens to match.
    return int(bool(lifted_returncode or unsupported or differences))


if __name__ == '__main__':
    sys.exit(main())
