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

`convert_elf.py --pthreads` enables Emscripten `-pthread`, JSPI,
`PROXY_TO_PTHREAD`, and the worker pool. `run_browser.py` compiles the native
reference with `-pthread`, converts with `--pthreads`, and launches the generated
page using the installed Playwright Chromium setup. It reuses
`test/browser/test-server.js`, which serves localhost with COOP and COEP headers.
The browser test fails unless `crossOriginIsolated`, `SharedArrayBuffer`, and
both JSPI APIs are available; it also checks browser abort/exit status, page
errors, and exact stdout parity. Unsupported-instruction diagnostics fail before
browser launch. Node itself does not execute the guest: it launches Playwright,
which drives real Chromium workers.

Prerequisites are the existing i386 differential-test toolchain plus Chromium
installed for Playwright (`npm install --prefix test/browser` and
`npx --prefix test/browser playwright install chromium`). CTest registration is
opt-in with `ELFCONV_GLIBC_PTHREAD_BROWSER_TESTS=ON`.

## Main verification

With CMake configured for i386 tests and
`-DELFCONV_GLIBC_PTHREAD_BROWSER_TESTS=ON`, run:

```sh
cmake --build build --target elflift
ctest --test-dir build -R '^i386_glibc_pthread_(tsd3|tsd4)$' --output-on-failure
```

Native-only checks (not lifted coverage; require GCC multilib):

```sh
gcc -m32 -pthread -O2 test/vendored/glibc/pthread/tst-tsd3.c -o /tmp/tst-tsd3 && /tmp/tst-tsd3
gcc -m32 -pthread -O2 test/vendored/glibc/pthread/tst-tsd4.c -o /tmp/tst-tsd4 && /tmp/tst-tsd4
```
