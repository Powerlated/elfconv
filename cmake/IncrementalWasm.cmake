if(NOT ELFCONV_LINK_MAP OR NOT EXISTS "${ELFCONV_LINK_MAP}")
  message(FATAL_ERROR "Incremental Wasm conversion requires a linker map: ${ELFCONV_LINK_MAP}")
endif()
if(NOT ELFCONV_OBJECT_BASE OR NOT IS_DIRECTORY "${ELFCONV_OBJECT_BASE}")
  message(FATAL_ERROR "Incremental Wasm conversion requires ELFCONV_OBJECT_BASE")
endif()
if(NOT EXISTS "${ELFCONV_LIFTER}")
  message(FATAL_ERROR "ELF lifter not found: ${ELFCONV_LIFTER}")
endif()
if(NOT DEFINED ELFCONV_EMCC OR ELFCONV_EMCC STREQUAL "")
  find_program(ELFCONV_EMCC NAMES em++ HINTS "$ENV{EMSDK}/upstream/emscripten" REQUIRED)
endif()
if(NOT EXISTS "${ELFCONV_EMCC}")
  message(FATAL_ERROR "Emscripten C++ driver not found: ${ELFCONV_EMCC}")
endif()
if(NOT DEFINED ELFCONV_WASM_OPT_LEVEL OR ELFCONV_WASM_OPT_LEVEL STREQUAL "")
  set(ELFCONV_WASM_OPT_LEVEL 3)
endif()
if(NOT ELFCONV_WASM_OPT_LEVEL MATCHES "^[0123sz]$")
  message(FATAL_ERROR "ELFCONV_WASM_OPT_LEVEL must be 0, 1, 2, 3, s, or z")
endif()
if(NOT DEFINED ELFCONV_LEGACY_GL OR ELFCONV_LEGACY_GL STREQUAL "")
  set(ELFCONV_LEGACY_GL 1)
endif()

execute_process(
  COMMAND "${ELFCONV_EMCC}" --version
  RESULT_VARIABLE emcc_version_result
  OUTPUT_VARIABLE emcc_version
  ERROR_VARIABLE emcc_version_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT emcc_version_result EQUAL 0)
  message(FATAL_ERROR "Unable to query Emscripten version: ${emcc_version_error}")
endif()
file(SHA256 "${ELFCONV_LIFTER}" lifter_hash)
file(SHA256 "${ELFCONV_INPUT}" elf_hash)
file(SHA256 "${ELFCONV_LINK_MAP}" linker_map_hash)
file(SHA256 "${ELFCONV_ROOT}/cmake/IncrementalWasm.cmake" incremental_script_hash)

set(semantics_files)
if(DEFINED ELFCONV_BITCODE_PATH AND IS_DIRECTORY "${ELFCONV_BITCODE_PATH}")
  file(GLOB_RECURSE semantics_files LIST_DIRECTORIES FALSE "${ELFCONV_BITCODE_PATH}/*.bc")
elseif(DEFINED ELFCONV_BITCODE_PATH AND EXISTS "${ELFCONV_BITCODE_PATH}")
  list(APPEND semantics_files "${ELFCONV_BITCODE_PATH}")
endif()
list(SORT semantics_files)
set(semantics_hash_material "${ELFCONV_BITCODE_PATH}|")
foreach(semantics_file IN LISTS semantics_files)
  file(SHA256 "${semantics_file}" semantics_file_hash)
  string(APPEND semantics_hash_material "${semantics_file}=${semantics_file_hash};")
endforeach()
string(SHA256 semantics_hash "${semantics_hash_material}")

file(GLOB_RECURSE runtime_headers LIST_DIRECTORIES FALSE
  "${ELFCONV_RUNTIME_DIR}/*.h" "${ELFCONV_RUNTIME_DIR}/*.hpp" "${ELFCONV_RUNTIME_DIR}/*.inc"
  "${ELFCONV_UTILS_DIR}/*.h" "${ELFCONV_UTILS_DIR}/*.hpp" "${ELFCONV_UTILS_DIR}/*.inc"
  "${ELFCONV_ROOT}/backend/remill/include/*.h"
  "${ELFCONV_ROOT}/backend/remill/include/*.hpp"
  "${ELFCONV_ROOT}/backend/remill/include/*.inc"
)
list(SORT runtime_headers)
set(runtime_header_material)
foreach(header IN LISTS runtime_headers)
  file(SHA256 "${header}" header_hash)
  string(APPEND runtime_header_material "${header}=${header_hash};")
endforeach()
string(SHA256 runtime_headers_hash "${runtime_header_material}")

set(runtime_sources ${ELFCONV_COMMON_RUNTIME_SOURCES} "${ELFCONV_RUNTIME_DIR}/I386Imports.cpp")
set(runtime_source_hashes)
foreach(source IN LISTS runtime_sources)
  file(SHA256 "${source}" source_hash)
  list(APPEND runtime_source_hashes "${source}=${source_hash}")
endforeach()
string(JOIN ";" runtime_source_hashes_text ${runtime_source_hashes})
string(JOIN ";" runtime_definitions_text ${ELFCONV_RUNTIME_DEFINITIONS})
string(JOIN ";" runtime_includes_text ${ELFCONV_RUNTIME_INCLUDE_FLAGS})
set(pipeline_material
  "${ELFCONV_INPUT}|${ELFCONV_TARGET}|${ELFCONV_OBJECT_BASE}|${ELFCONV_LIFTER}|${ELFCONV_EMCC}|"
  "${elf_hash}|${linker_map_hash}|${lifter_hash}|${incremental_script_hash}|${semantics_hash}|"
  "${runtime_source_hashes_text}|${runtime_headers_hash}|${runtime_definitions_text}|"
  "${runtime_includes_text}|${ELFCONV_WASM_OPT_LEVEL}|${ELFCONV_WASM_JSPI}|"
  "${ELFCONV_LEGACY_GL}|${ELFCONV_FLOAT_EXCEPTION}|${ELFCONV_DEBUG}|"
  "${ELFCONV_RUNTIME_DIR}/I386Imports.cpp"
)
string(SHA256 pipeline_key "${pipeline_material}")
set(cache_dir "${ELFCONV_OUTPUT_DIR}/.elfconv-incremental")
file(MAKE_DIRECTORY "${cache_dir}")
set(output_js "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.js")
set(output_wasm "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.wasm")
set(output_html "${ELFCONV_OUTPUT_DIR}/${ELFCONV_NAME}.html")
set(pipeline_key_file "${cache_dir}/pipeline.key")
if(EXISTS "${output_js}" AND EXISTS "${output_wasm}" AND EXISTS "${output_html}" AND
   EXISTS "${pipeline_key_file}")
  file(READ "${pipeline_key_file}" previous_pipeline_key)
  if(previous_pipeline_key STREQUAL pipeline_key)
    message(STATUS "Incremental Wasm outputs are current")
    return()
  endif()
endif()

set(manifest "${cache_dir}/units.tsv")
set(metadata_fingerprint "${cache_dir}/metadata.fingerprint")
_elfconv_execute("Incremental unit scan" "${ELFCONV_LIFTER}"
  --arch i386 --target_arch emscripten32 --target_elf "${ELFCONV_INPUT}"
  --linker_map "${ELFCONV_LINK_MAP}" --object_base "${ELFCONV_OBJECT_BASE}"
  --unit_manifest_out "${manifest}" --metadata_fingerprint_out "${metadata_fingerprint}"
  --norm_mode 1 --fork_emulation 0 --float_exception "${ELFCONV_FLOAT_EXCEPTION}"
)
file(SHA256 "${metadata_fingerprint}" metadata_fingerprint_hash)
set(metadata_lift_key
  "${metadata_fingerprint_hash}|${lifter_hash}|${incremental_script_hash}|${semantics_hash}|"
  "${ELFCONV_TARGET}|i386|emscripten32|norm=1|fork=0|${ELFCONV_FLOAT_EXCEPTION}"
)
string(SHA256 metadata_lift_key "${metadata_lift_key}")
set(metadata_bc "${cache_dir}/metadata.bc")
set(metadata_object "${cache_dir}/metadata.wasm.o")
set(metadata_lift_key_file "${cache_dir}/metadata.lift.key")
set(metadata_compile_key_file "${cache_dir}/metadata.compile.key")
set(metadata_lifted FALSE)
if(EXISTS "${metadata_lift_key_file}")
  file(READ "${metadata_lift_key_file}" previous_metadata_lift_key)
else()
  set(previous_metadata_lift_key "")
endif()
if(NOT previous_metadata_lift_key STREQUAL metadata_lift_key OR NOT EXISTS "${metadata_bc}")
  _elfconv_execute("Incremental metadata lifting" "${ELFCONV_LIFTER}"
    --arch i386 --target_arch emscripten32 --target_elf "${ELFCONV_INPUT}"
    --metadata_only --bc_out "${metadata_bc}" --norm_mode 1 --fork_emulation 0
    --float_exception "${ELFCONV_FLOAT_EXCEPTION}"
  )
  file(WRITE "${metadata_lift_key_file}" "${metadata_lift_key}")
  set(metadata_lifted TRUE)
endif()
set(metadata_compile_key "${metadata_lift_key}|${ELFCONV_EMCC}|${emcc_version}|${ELFCONV_WASM_OPT_LEVEL}")
string(SHA256 metadata_compile_key "${metadata_compile_key}")
if(metadata_lifted OR NOT EXISTS "${metadata_object}")
  _elfconv_execute("Incremental metadata Wasm compilation" "${ELFCONV_EMCC}"
    "-O${ELFCONV_WASM_OPT_LEVEL}" -c "${metadata_bc}" -o "${metadata_object}"
  )
  file(WRITE "${metadata_compile_key_file}" "${metadata_compile_key}")
elseif(EXISTS "${metadata_compile_key_file}")
  file(READ "${metadata_compile_key_file}" previous_metadata_compile_key)
  if(NOT previous_metadata_compile_key STREQUAL metadata_compile_key)
    _elfconv_execute("Incremental metadata Wasm compilation" "${ELFCONV_EMCC}"
      "-O${ELFCONV_WASM_OPT_LEVEL}" -c "${metadata_bc}" -o "${metadata_object}"
    )
    file(WRITE "${metadata_compile_key_file}" "${metadata_compile_key}")
  endif()
else()
  _elfconv_execute("Incremental metadata Wasm compilation" "${ELFCONV_EMCC}"
    "-O${ELFCONV_WASM_OPT_LEVEL}" -c "${metadata_bc}" -o "${metadata_object}"
  )
  file(WRITE "${metadata_compile_key_file}" "${metadata_compile_key}")
endif()

file(STRINGS "${manifest}" unit_lines)
set(unit_objects)
set(unit_ids)
set(unit_compile_keys)
set(unit_lift_flags --arch i386 --target_arch emscripten32 --target_elf "${ELFCONV_INPUT}"
  --linker_map "${ELFCONV_LINK_MAP}" --object_base "${ELFCONV_OBJECT_BASE}"
  --norm_mode 1 --fork_emulation 0 --float_exception "${ELFCONV_FLOAT_EXCEPTION}"
)
foreach(line IN LISTS unit_lines)
  string(FIND "${line}" "\t" owner_end)
  if(owner_end LESS 1)
    message(FATAL_ERROR "Malformed incremental unit manifest entry: ${line}")
  endif()
  string(SUBSTRING "${line}" 0 ${owner_end} owner)
  math(EXPR fingerprint_start "${owner_end} + 1")
  string(SUBSTRING "${line}" ${fingerprint_start} -1 fingerprint)
  if(NOT EXISTS "${fingerprint}")
    message(FATAL_ERROR "Missing unit fingerprint: ${fingerprint}")
  endif()
  string(SHA256 unit_id_hash "${owner}")
  string(SUBSTRING "${unit_id_hash}" 0 24 unit_id)
  file(SHA256 "${fingerprint}" unit_fingerprint_hash)
  set(unit_lift_key
    "${unit_fingerprint_hash}|${owner}|${lifter_hash}|${incremental_script_hash}|${semantics_hash}|"
    "${ELFCONV_TARGET}|i386|emscripten32|norm=1|fork=0|${ELFCONV_FLOAT_EXCEPTION}"
  )
  string(SHA256 unit_lift_key "${unit_lift_key}")
  set(unit_bc "${cache_dir}/unit-${unit_id}.bc")
  set(unit_object "${cache_dir}/unit-${unit_id}.wasm.o")
  set(unit_lift_key_file "${cache_dir}/unit-${unit_id}.lift.key")
  set(unit_compile_key_file "${cache_dir}/unit-${unit_id}.compile.key")
  if(EXISTS "${unit_lift_key_file}")
    file(READ "${unit_lift_key_file}" previous_unit_lift_key)
  else()
    set(previous_unit_lift_key "")
  endif()
  set(unit_lifted FALSE)
  if(NOT previous_unit_lift_key STREQUAL unit_lift_key OR NOT EXISTS "${unit_bc}")
    _elfconv_execute("Lifting object unit ${owner}" "${ELFCONV_LIFTER}"
      ${unit_lift_flags} --unit_owner "${owner}" --unit_id "${unit_id}" --bc_out "${unit_bc}"
    )
    file(WRITE "${unit_lift_key_file}" "${unit_lift_key}")
    set(unit_lifted TRUE)
  endif()
  set(unit_compile_key "${unit_lift_key}|${ELFCONV_EMCC}|${emcc_version}|${ELFCONV_WASM_OPT_LEVEL}")
  string(SHA256 unit_compile_key "${unit_compile_key}")
  if(EXISTS "${unit_compile_key_file}")
    file(READ "${unit_compile_key_file}" previous_unit_compile_key)
  else()
    set(previous_unit_compile_key "")
  endif()
  if(unit_lifted OR NOT previous_unit_compile_key STREQUAL unit_compile_key OR
     NOT EXISTS "${unit_object}")
    _elfconv_execute("Compiling object unit ${owner}" "${ELFCONV_EMCC}"
      "-O${ELFCONV_WASM_OPT_LEVEL}" -c "${unit_bc}" -o "${unit_object}"
    )
    file(WRITE "${unit_compile_key_file}" "${unit_compile_key}")
  endif()
  list(APPEND unit_objects "${unit_object}")
  list(APPEND unit_ids "${unit_id}")
  list(APPEND unit_compile_keys "${unit_compile_key}")
endforeach()
if(NOT unit_ids)
  message(FATAL_ERROR "The final ELF produced no incremental code units")
endif()

set(registry_source "#include \"runtime/Memory.h\"\n")
foreach(unit_id IN LISTS unit_ids)
  string(APPEND registry_source "extern \"C\" {\n")
  string(APPEND registry_source "extern const uint64_t _ecv_fun_vmas_${unit_id}[];\n")
  string(APPEND registry_source "extern const LiftedFunc _ecv_fun_ptrs_${unit_id}[];\n")
  string(APPEND registry_source "extern uint64_t **_ecv_block_address_ptrs_array_${unit_id}[];\n")
  string(APPEND registry_source "extern const uint64_t *_ecv_block_address_vmas_array_${unit_id}[];\n")
  string(APPEND registry_source "extern const uint64_t _ecv_block_address_size_array_${unit_id}[];\n")
  string(APPEND registry_source "extern const uint64_t _ecv_block_address_fn_vma_array_${unit_id}[];\n")
  string(APPEND registry_source "extern const uint64_t _ecv_block_address_array_size_${unit_id};\n}\n")
endforeach()
list(LENGTH unit_ids unit_count)
string(APPEND registry_source "extern \"C\" {\nextern const EcvLiftedUnit _ecv_lifted_units[] = {\n")
foreach(unit_id IN LISTS unit_ids)
  string(APPEND registry_source
    "  {_ecv_fun_vmas_${unit_id}, _ecv_fun_ptrs_${unit_id}, _ecv_block_address_ptrs_array_${unit_id}, _ecv_block_address_vmas_array_${unit_id}, _ecv_block_address_size_array_${unit_id}, _ecv_block_address_fn_vma_array_${unit_id}, _ecv_block_address_array_size_${unit_id}},\n"
  )
endforeach()
string(APPEND registry_source "};\nextern const uint64_t _ecv_lifted_unit_count = ${unit_count};\n}\n")
set(registry_source_file "${cache_dir}/UnitRegistry.cpp")
file(WRITE "${registry_source_file}" "${registry_source}")
string(SHA256 registry_source_hash "${registry_source}")
set(registry_compile_key
  "${registry_source_hash}|${ELFCONV_EMCC}|${emcc_version}|${ELFCONV_WASM_OPT_LEVEL}|"
  "${runtime_headers_hash}|${runtime_definitions_text}|${runtime_includes_text}|ELF_IS_I386|ADDRESS_SIZE_BITS=32"
)
string(SHA256 registry_compile_key "${registry_compile_key}")
set(registry_object "${cache_dir}/UnitRegistry.wasm.o")
set(registry_key_file "${cache_dir}/UnitRegistry.compile.key")
if(EXISTS "${registry_key_file}")
  file(READ "${registry_key_file}" previous_registry_key)
else()
  set(previous_registry_key "")
endif()
if(NOT previous_registry_key STREQUAL registry_compile_key OR NOT EXISTS "${registry_object}")
  _elfconv_execute("Compiling unit registry" "${ELFCONV_EMCC}"
    "-O${ELFCONV_WASM_OPT_LEVEL}" -sUSE_SDL=2 ${ELFCONV_RUNTIME_INCLUDE_FLAGS}
    -std=c++17 ${ELFCONV_RUNTIME_DEFINITIONS} -DELF_IS_I386 -DADDRESS_SIZE_BITS=32
    -DELFCONV_INCREMENTAL_UNITS=1 -c "${registry_source_file}" -o "${registry_object}"
  )
  file(WRITE "${registry_key_file}" "${registry_compile_key}")
endif()

set(runtime_compile_flags "-O${ELFCONV_WASM_OPT_LEVEL};-sUSE_SDL=2;-std=c++17;${ELFCONV_RUNTIME_DEFINITIONS};-DELF_IS_I386;-DADDRESS_SIZE_BITS=32;-DELFCONV_INCREMENTAL_UNITS=1;-DECV_LEGACY_GL=${ELFCONV_LEGACY_GL};${ELFCONV_RUNTIME_INCLUDE_FLAGS}")
string(JOIN ";" runtime_compile_flags_text ${runtime_compile_flags})
set(runtime_objects)
set(runtime_compile_keys)
foreach(source IN LISTS runtime_sources)
  get_filename_component(source_name "${source}" NAME_WE)
  file(SHA256 "${source}" source_hash)
  set(runtime_compile_key
    "${source_hash}|${runtime_headers_hash}|${runtime_compile_flags_text}|${ELFCONV_EMCC}|${emcc_version}"
  )
  string(SHA256 runtime_compile_key "${runtime_compile_key}")
  set(runtime_object "${cache_dir}/runtime-${source_name}.wasm.o")
  set(runtime_key_file "${cache_dir}/runtime-${source_name}.compile.key")
  if(EXISTS "${runtime_key_file}")
    file(READ "${runtime_key_file}" previous_runtime_key)
  else()
    set(previous_runtime_key "")
  endif()
  if(NOT previous_runtime_key STREQUAL runtime_compile_key OR NOT EXISTS "${runtime_object}")
    _elfconv_execute("Compiling runtime ${source_name}" "${ELFCONV_EMCC}"
      "-O${ELFCONV_WASM_OPT_LEVEL}" -sUSE_SDL=2 ${ELFCONV_RUNTIME_INCLUDE_FLAGS}
      -std=c++17 ${ELFCONV_RUNTIME_DEFINITIONS} -DELF_IS_I386 -DADDRESS_SIZE_BITS=32
      -DELFCONV_INCREMENTAL_UNITS=1 "-DECV_LEGACY_GL=${ELFCONV_LEGACY_GL}"
      -c "${source}" -o "${runtime_object}"
    )
    file(WRITE "${runtime_key_file}" "${runtime_compile_key}")
  endif()
  list(APPEND runtime_objects "${runtime_object}")
  list(APPEND runtime_compile_keys "${runtime_compile_key}")
endforeach()

if(ELFCONV_WASM_JSPI)
  set(ELFCONV_BROWSER_JSPI true)
  set(suspension_flags -sJSPI=1)
else()
  set(ELFCONV_BROWSER_JSPI false)
  set(suspension_flags -sASYNCIFY=1)
endif()
set(debug_flags)
if(ELFCONV_WASM_OPT_LEVEL STREQUAL "0")
  set(debug_flags -g2)
endif()
set(link_material "${metadata_compile_key}|${registry_compile_key}|${unit_compile_keys}|${runtime_compile_keys}|${runtime_definitions_text}|${ELFCONV_WASM_OPT_LEVEL}|${ELFCONV_WASM_JSPI}|${ELFCONV_LEGACY_GL}|webgl=2|${ELFCONV_FLOAT_EXCEPTION}|${ELFCONV_DEBUG}")
file(SHA256 "${ELFCONV_ROOT}/browser/i386.html.in" html_template_hash)
string(APPEND link_material "|${html_template_hash}|${pipeline_key}")
string(SHA256 link_key "${link_material}")
set(link_key_file "${cache_dir}/link.key")
if(EXISTS "${link_key_file}")
  file(READ "${link_key_file}" previous_link_key)
else()
  set(previous_link_key "")
endif()
if(NOT previous_link_key STREQUAL link_key OR NOT EXISTS "${output_js}" OR NOT EXISTS "${output_wasm}")
  _elfconv_execute("Incremental i386 browser link" "${ELFCONV_EMCC}"
    "-O${ELFCONV_WASM_OPT_LEVEL}" ${debug_flags} ${ELFCONV_RUNTIME_INCLUDE_FLAGS}
    -std=c++17 ${ELFCONV_RUNTIME_DEFINITIONS} -DELF_IS_I386 -DADDRESS_SIZE_BITS=32
    -sUSE_SDL=2 "-sLEGACY_GL_EMULATION=${ELFCONV_LEGACY_GL}"
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
    "-DECV_LEGACY_GL=${ELFCONV_LEGACY_GL}" -DELFCONV_INCREMENTAL_UNITS=1
    ${suspension_flags} -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=335544320
    -sSTACK_SIZE=1048576 -sEXIT_RUNTIME=1
    ${unit_objects} "${metadata_object}" "${registry_object}" ${runtime_objects}
    -o "${output_js}"
  )
  file(WRITE "${link_key_file}" "${link_key}")
endif()
configure_file("${ELFCONV_ROOT}/browser/i386.html.in" "${output_html}" @ONLY)
file(WRITE "${pipeline_key_file}" "${pipeline_key}")
message(STATUS "Incremental Wasm units: ${unit_count}")
