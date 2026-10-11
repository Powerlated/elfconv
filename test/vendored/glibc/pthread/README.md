# glibc pthread oracle

Source: glibc `04e750e75b73957cf1c791535a3f4319534a52fc`, `nptl/tst-tsd3.c` and
`nptl/tst-tsd4.c`. Both preserve upstream public-API assertions and replace
glibc's `test-skeleton.c` wrapper with `main`; the explanatory comment in
`tst-tsd4.c` was clarified. License: LGPL-2.1-or-later;
`COPYING.LESSERv2` is from that revision. No glibc support-library dependency.

| Required behavior | Existing upstream test | Adaptation / coverage | Modes / gaps |
|---|---|---|---|
| Per-thread key isolation; destructor iterations; create/join | `nptl/tst-tsd3.c` | Creates two keys with alternating destructors that reassign one another seven times; joins the worker, verifies all destructor passes completed and both keys remain unset in the main thread. The latter asserts main-thread values are isolated from the worker's values. | Native: compile with `-pthread`. Lifted: browser only, with real Emscripten pthreads and JSPI. |
| Destructor iteration minimum; key cleanup; create/join | `nptl/tst-tsd4.c` | A destructor repeatedly reassigns its key. After joining, asserts the count reaches `PTHREAD_DESTRUCTOR_ITERATIONS` and key data is clear. It checks the minimum count, not an upper bound; the source documents NPTL stop behavior. Replaces `test-skeleton.c` with `main`. | Same native/browser modes. |
| Detach; thread attributes; TLS; condition clock/timed wait; cancellation/cleanup | — | No assertions for these behaviors in this slice. | Add further existing upstream public-API tests; not claimed as coverage here. |

## Browser integration

`convert_elf.py` supports `--pthreads`; it enables Emscripten `-pthread`, JSPI,
`PROXY_TO_PTHREAD`, and a worker pool. Generated pthread pages require a
JSPI-capable browser and cross-origin isolation (`SharedArrayBuffer`), so serve
the generated HTML from a secure origin with COOP/COEP headers (the existing
browser test server/service-worker pattern is relevant). Node invocation by
`run_i386.py` is not a valid threaded oracle: its existing compile/conversion
path does not enable `--pthreads` and does not launch real guest threads.

The per-slice CMake file requires an explicit
`ELFCONV_I386_PTHREAD_BROWSER_RUNNER` when browser tests are enabled. Main
integration must provide that runner with `--lifter`, `--emcc`, `--cc`,
`--source`, and `--output-dir` options and register it only on a JSPI-capable,
cross-origin-isolated browser lane. The runner must compile the native reference
with `-pthread`, convert with `--pthreads`, serve and launch the generated page,
and fail on browser abort, missing completion, nonzero status, unsupported
imports, or output/status mismatch. Do not normalize native-versus-lifted
failures.

## Main verification

After the browser runner is integrated and CMake is configured with
`-DELFCONV_GLIBC_PTHREAD_BROWSER_TESTS=ON` and
`-DELFCONV_I386_PTHREAD_BROWSER_RUNNER=/path/to/runner.py`, run:

```sh
cmake --build build --target elflift
ctest --test-dir build -R '^i386_glibc_pthread_(tsd3|tsd4)$' --output-on-failure
```

Native-only checks (not lifted coverage; require GCC multilib):

```sh
gcc -m32 -pthread -O2 test/vendored/glibc/pthread/tst-tsd3.c -o /tmp/tst-tsd3 && /tmp/tst-tsd3
gcc -m32 -pthread -O2 test/vendored/glibc/pthread/tst-tsd4.c -o /tmp/tst-tsd4 && /tmp/tst-tsd4
```
