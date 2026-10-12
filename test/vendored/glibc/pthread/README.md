# glibc NPTL tests

`upstream/` is a source snapshot of the standard NPTL `tests` targets from glibc `nptl/Makefile` at commit `04e750e75b73957cf1c791535a3f4319534a52fc` (101 targets, including conditional additions). It also preserves the corresponding NPTL companion sources, `test-skeleton.c`, and glibc's `support/` test helpers. Source layout and bytes match that upstream revision. `COPYING.LESSERv2` contains the license.

Only the standalone adaptations `tst-tsd3.c` and `tst-tsd4.c` at this directory's root are currently registered and run through elfconv's native/Wasm browser harness. The remaining upstream tests are preserved as sources; they depend on glibc's full test environment and/or Linux-specific behavior and are not claimed as elfconv coverage.

- `upstream/nptl/tst-signal3.c` exercises `kill`; it remains unregistered because
  browser process-signal delivery is unsupported.