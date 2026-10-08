#!/usr/bin/env bash
#
# Usage:
#   VERSION=v0.3.0 bash release.sh
#
# Environment variables:
#   VERSION  (required) Release version string (e.g. v0.3.0).
#            Used for the output tarball name: elfconv-<VERSION>-linux-<arch>.tar.gz
#
# Examples:
#   VERSION=v0.3.0 bash release.sh        # build release package and create tarball
#   bash release.sh clean                  # remove built artifacts
#

set -e

GREEN="\033[32m"
ORANGE="\033[33m"
RED="\033[31m"
NC="\033[0m"

setting() {

  RELEASE_DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
  ELFCONV_DIR=${RELEASE_DIR}/../
  BUILD_DIR=${ELFCONV_DIR}/build
  ELFCONV_ARCH_DIR=${BUILD_DIR}/backend/remill/lib/Arch
  RUNTIME_DIR=${ELFCONV_DIR}/runtime
  UTILS_DIR=${ELFCONV_DIR}/utils
  BROWSER_DIR=${ELFCONV_DIR}/browser
  OUTDIR=${RELEASE_DIR}/outdir
  BINDIR=${OUTDIR}/bin
  BITCODEDIR=${OUTDIR}/bitcode
  LIBDIR=${OUTDIR}/lib
  OUTOUTDIR=${OUTDIR}/out

  HOST_ARCH=$(uname -m)
  case "${HOST_ARCH}" in
    x86_64)  ARCH_LABEL="amd64" ;;
    aarch64) ARCH_LABEL="aarch64" ;;
    *)       ARCH_LABEL="${HOST_ARCH}" ;;
  esac


}

main() {
  
  setting

  if [ ! -d "$OUTDIR" ]; then
    mkdir "$OUTDIR"
    echo "[${GREEN}INFO${NC}] outdir was generated."
  fi

  # clean existing outdir/
  if [ "$1" = "clean" ]; then
    rm -rf "$BINDIR" "$BITCODEDIR" "$LIBDIR" "$OUTOUTDIR" \
      "${OUTDIR}/browser" "${OUTDIR}/cmake" "${OUTDIR}/runtime" \
      "${OUTDIR}/utils" "${OUTDIR}/backend" "${OUTDIR}/thirdparty" \
      "${OUTDIR}/scripts" "${OUTDIR}/elfconv.sh" "${OUTDIR}/xterm-pty" *.tar.gz
    exit 0
  fi

  # set elflift
  mkdir -p $BINDIR
  cmake --build "${BUILD_DIR}" --target elflift
  if file "${BUILD_DIR}/lifter/elflift" | grep -q "dynamically linked"; then
    echo -e "[${ORANGE}WARNING${NC}] elflift is dynamically linked file."
  fi
  
  if cp ${BUILD_DIR}/lifter/elflift $BINDIR; then
    echo -e "[${GREEN}INFO${NC}] Set elflift."
  else
    echo -e "[${RED}ERROR${NC}] Faild to set elflift."
    exit 1
  fi
  mkdir -p "${LIBDIR}"

  while IFS= read -r so_path; do
    if [[ -n "${so_path}" && -f "${so_path}" ]]; then
      cp -L "${so_path}" "${LIBDIR}/"
      echo -e "[${GREEN}INFO${NC}] Bundled $(basename ${so_path})."
    fi
  done < <(ldd "${BUILD_DIR}/lifter/elflift" \
    | grep -vE 'linux-vdso|ld-linux|libc\.so|libm\.so|libgcc_s|libstdc\+\+|libpthread|libdl\.so|librt\.so' \
    | awk '{print $3}' \
    | grep -v '^$')

  # set semantics *.bc file
  mkdir -p $BITCODEDIR
  case "${ARCH_LABEL}" in
    aarch64)
      cp ${ELFCONV_ARCH_DIR}/AArch64/Runtime/aarch64.bc $BITCODEDIR
      echo -e "[${GREEN}INFO${NC}] Set semantics aarch64.bc."
      ;;
    amd64)
      cp ${ELFCONV_ARCH_DIR}/X86/Runtime/amd64.bc $BITCODEDIR
      cp ${ELFCONV_ARCH_DIR}/X86/Runtime/x86.bc $BITCODEDIR
      echo -e "[${GREEN}INFO${NC}] Set semantics amd64.bc, x86.bc."
      ;;
    *)
      echo -e "[${RED}ERROR${NC}] Unsupported architecture: ${HOST_ARCH}"
      exit 1
      ;;
  esac
  

  # Package the CMake converter and runtime sources.
  mkdir -p "${OUTDIR}/browser" "${OUTDIR}/cmake" "${OUTDIR}/runtime" \
    "${OUTDIR}/utils" "${OUTDIR}/backend/remill/include" \
    "${OUTDIR}/thirdparty/nlohmann" "${OUTDIR}/xterm-pty"
  cp -R "${BROWSER_DIR}/." "${OUTDIR}/browser/"
  cp -R "${ELFCONV_DIR}/cmake/." "${OUTDIR}/cmake/"
  cp -R "${RUNTIME_DIR}/." "${OUTDIR}/runtime/"
  cp -R "${UTILS_DIR}/." "${OUTDIR}/utils/"
  cp -R "${ELFCONV_DIR}/backend/remill/include/." "${OUTDIR}/backend/remill/include/"
  cp -R "${ELFCONV_DIR}/thirdparty/nlohmann/." "${OUTDIR}/thirdparty/nlohmann/"
  cp "${ELFCONV_DIR}/xterm-pty/emscripten-pty.js" "${OUTDIR}/xterm-pty/"

  cp ${RELEASE_DIR}/README.md ${OUTDIR}/README.md
  echo -e "[${GREEN}INFO${NC}] Set README.md."

  if [[ -z "${VERSION}" ]]; then
    echo -e "[${RED}ERROR${NC}] VERSION is not set. Usage: VERSION=v0.3.0 bash release.sh"
    exit 1
  fi
  TARNAME="elfconv-${VERSION}-linux-${ARCH_LABEL}.tar.gz"
  cd "${RELEASE_DIR}"
  tar -czf "${TARNAME}" -C "${RELEASE_DIR}" outdir
  echo -e "[${GREEN}INFO${NC}] Created ${TARNAME}."

}

main "$@"