#!/usr/bin/env bash

ECV_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

source "${ECV_DIR}/scripts/elfconv.sh"

if [[ -n "${CLEAN}" ]]; then
  name=$(basename -- "$1")
  out="${ECV_OUT_DIR:-${PWD}}"
  rm -f -- "${out}/${name}.bc" "${out}/${name}.ll" "${out}/${name}.o" \
    "${out}/${name}.wasm" "${out}/${name}.wasm.o" "${out}/${name}.wasi.o" \
    "${out}/${name}.js" "${out}/${name}.generated.js" "${out}/${name}.html"
fi

main "$@"
