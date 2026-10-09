# QEMU i386 differential fixture

Vendored from [QEMU](https://github.com/qemu/qemu/tree/f9587d4045c67cd0d8d8bdcd5d0bb5b6b395b63c/tests/tcg/i386),
commit `f9587d4045c67cd0d8d8bdcd5d0bb5b6b395b63c`.
The five files under `vendor/` are unmodified upstream instruction-test sources;
`vendor/COPYING` preserves QEMU's license notice and GPL v2 text. See individual
source headers for copyright and licensing terms.

The harness compiles a non-PIE 32-bit Linux ELF, runs it natively, lifts that same
ELF to Emscripten Wasm, executes it with Node, and compares stdout. No downloaded
sources or QEMU emulator are needed when running the fixture.

## Run

Prerequisites: an x86 Linux host that can execute i386 ELF files, GCC with 32-bit
libc development files, a built x86 `elflift`, Emscripten, Node, and Python 3.

```sh
cmake -S . -B build/llvm16 \
  -DELFCONV_QEMU_I386_TESTS=ON \
  -DELFCONV_EMCC=/path/to/emsdk/upstream/emscripten/em++
cmake --build build/llvm16 --target elflift
ctest --test-dir build/llvm16 -R '^qemu_i386_' --output-on-failure
```

Use your existing configured x86 build directory instead of `build/llvm16` when
appropriate. The option defaults to OFF because it requires external toolchains
and the conformance tests currently expose failures. Enabled tests fail normally:
there are no expected-failure annotations or mismatch allowlists.

For a standalone run:

```sh
python3 tests/elfconv/qemu-i386/run.py \
  --lifter build/llvm16/lifter/elflift \
  --emcc /path/to/emsdk/upstream/emscripten/em++ \
  --profile core --output-dir /tmp/elfconv-qemu-core
```

`--profile full` executes the original upstream main. The `core` entrypoint keeps
upstream arithmetic/flags, shifts/rotates, bit operations, bit scans/counts,
multiply/divide, branches, loops, BCD, exchange/compare-exchange, string operations,
addressing, ENTER, and conversion tests. It omits floating point, segmentation,
16-bit code, exceptions/signals, self-modifying code, single stepping, and the
miscellaneous test containing segment/far-control-flow operations. Unused upstream
functions are discarded at link time; their sources remain vendored.

Each profile saves compilation, native execution, conversion, and lifted execution
stdout/stderr under its output directory, along with `output.diff` when conversion
and execution reach comparison. Unsupported-instruction diagnostics, crashes,
timeouts, or differing output all produce a failing exit status. Conversion and
execution failures are never treated as successful partial coverage.

## Defined-behavior instruction regressions

Five standalone fixtures compare architecturally defined results and flags:

- `qemu_i386_shift_defined`: SHL/SHR/SAR, including zero/masked-zero counts and
  narrow-width SAR carry behavior (57 cases).
- `qemu_i386_double_shift_defined`: SHLD/SHRD zero-count preservation, count-one
  flags, interior counts, and the 16-bit width boundary (32 cases).
- `qemu_i386_rotate_defined`: ROL/ROR zero-count preservation and carry updates
  for full-width byte/word rotations (22 cases).
- `qemu_i386_bit_count_defined`: TZCNT/LZCNT/POPCNT register and memory forms,
  zero inputs, low/high set bits, dense inputs, and defined flags at 16/32 bits
  (96 cases).
- `qemu_i386_x87_sm64_defined`: x87 arithmetic across host calls, FUCOMIP
  comparisons, finite loads after a masked invalid operation, all four guest
  rounding modes, FISTP integer boundaries, FRNDINT, single-precision
  multiplication, and 80-bit spills (123 output rows). Integer/bit-pattern
  output avoids dependence on guest long-double formatting.

```sh
ctest --test-dir build/llvm16 -R '^qemu_i386_.*_defined$' --output-on-failure
```

The harness also accepts `--source tests/elfconv/qemu-i386/shift-defined.c`
instead of `--profile` to run an individual standalone fixture.

The semantics fixes preserve flags and values at masked-zero counts, retain
32-bit register zero-extension in 64-bit mode, define SAR carry after shifting
out the sign bit, avoid operand-width host shifts in SHLD/SHRD, and update rotate
carry when a nonzero masked count produces a full-width rotation.

References: [Intel SDM Volume 2A–2D](https://cdrdv2-public.intel.com/782156/325383-sdm-vol-2abcd.pdf),
SAL/SAR/SHL/SHR pp. 4-593–595, SHLD pp. 4-627–629, SHRD pp. 4-631–633,
and RCL/RCR/ROL/ROR pp. 4-527–529; [AMD APM Volumes 1–5](https://docs.amd.com/api/khub/documents/SLs_hsYJwsu9rrIjE0rGxA/content),
Volume 1 §3.3.7.

Each of the three shift/rotate fixtures failed at a boundary case before its
semantics rebuild. The original core output also exposed bit-count failures.
All 207 defined-behavior cases now match native output across the four fixtures.
The vendored QEMU `core` profile completes: native and Wasm both exit 0 with
4,371 output rows and no unsupported instruction sites. All LOOP/LOOPE/LOOPNE,
bit-count, decimal-adjust, and exchange rows match native output.

There are 62 remaining raw positional differences, all in architecturally
undefined behavior: 60 SHLD/SHRD word rows with counts 17–31, and two SHL rows
whose carry flag is undefined because the count equals the byte/word width.
All other 4,309 rows match exactly. The raw conformance test intentionally still
fails rather than masking those differences. The `full` profile was rerun;
native exits 0, but lifting still fails with `(Custom) Segmantation Fault.`

LOOP selection now uses effective address size, not operand size. A 16-bit
address-size override decrements CX, preserves upper ECX, and branches on CX;
32-bit addressing in 64-bit mode decrements ECX and clears upper RCX.
XED explicitly enables TZCNT/LZCNT decoding instead of selecting their legacy
BSF/BSR aliases. POPCNT clears arithmetic flags except ZF, which reflects a zero
source. DAS, AAA, AAM, and AAD now have semantics; AAM follows the existing
faulting-instruction next-PC contract and rejects a zero radix.

The rebuild also exposed and repaired a missing 64-bit preprocessor guard
around `CVTSS2SI_64` in `CONVERT.cpp`.

The runtime implements 8/16/32/64-bit memory compare-exchange, returning the
observed memory value through `expected` and writing `desired` only on equality.
Unaligned operands use `memcpy`. These operations rely on the existing
single-threaded guest execution model; they do not provide host-thread atomicity.

After relifting the unchanged SM64 i386 ELF, its WebGL2 boot logo renders the full
SUPER MARIO 64 lettering without shader-expression overrides. Captured shaders
use `texVal0.rgb * vInput1` rather than `vec3(0.0, 0.0, 0.0) * vInput1`, and
WebGL reports no error. The combiner's shader-ID packing uses a zero-count SHL
for its first input; the former zero-count implementation could erase that input.

## SM64-relevant x87 fixes

The isolated arithmetic portion of upstream `test_fops` initially differed in
7 of 8 rows. The SM64 x87 fixture differed in 87 of 123 rows before the fixes;
all 123 rows now match native execution. The original arithmetic portion also
matches all 8 rows, both with floating formatting and with raw bit-pattern output.

Upstream Remill at
[`56918a8c2554088e93389e97d292f4035286506c`](https://github.com/lifting-bits/remill/blob/56918a8c2554088e93389e97d292f4035286506c/lib/Arch/X86/Semantics/X87.cpp)
already guards FLD NaN quieting with the current operand's signaling-NaN test,
not the sticky invalid-operation flag. That fix is ported here. Elfconv's runtime
also incorrectly returned the requested clear mask as observed exceptions; it
now tests and clears host exceptions and is no longer declared pure.

Guest FLDCW/FNSTCW preserve the control word. FISTP and FRNDINT use its rounding
bits directly, including ties-to-even, because Wasm does not expose a mutable
host rounding-mode register. Invalid integer conversions use the checked
conversion helpers without first performing an out-of-range C++ cast.
The binary80 storage decoder preserves representable NaN payloads rather than
unconditionally setting their low bit.

The unchanged SM64 ELF was relifted with these fixes. Chromium rendered the
Mario-head title screen and the animated Bowser attract-mode scene; the page
remained running without an abort or browser errors, and WebGL reported error 0.

This is not full x87 conformance: arithmetic still uses binary64 precision.
The upstream comparison fixture's FNSTENV/FLDENV helper, packed BCD,
environment save/restore, and long-double printf are not covered by this fixture.
SM64's inspected ELF does not contain those environment instructions.

## Initial observed results

On the initial GCC/native versus LLVM 16 elfconv/Emscripten run:

- `core`: native exits 0 with 4,371 output rows. Wasm exits 1 after 4,226 rows,
  aborting on missing `__remill_compare_exchange_memory_32`.
- The conversion reports 27 unsupported instruction sites: POPCNT, DAS, AAA,
  AAM, and AAD.
- There are 144 positional differences in the common output prefix, including
  SHRD/SHLD flag differences with a zero shift count and 16-bit LOOP differences.
- `full`: native exits 0; lifting fails with `(Custom) Segmantation Fault.`

These are observations, not 144 proven instruction bugs. QEMU also exercises
architecturally undefined results/flags (for example, 16-bit SHLD/SHRD counts above
16), and newer instructions depend on the native CPU. Raw native parity is a
useful diagnostic, but each mismatch needs architectural classification before
changing semantics. The fixture deliberately preserves those differences rather
than hiding them. Later core cases are not covered by the initial Wasm run because
execution aborts at compare-exchange.
