// Maps loaded guest ELF code to lifted functions and resolves cross-unit calls.
// Unit ownership comes from ELF images or linker-map object ranges; host import
// wrappers have a separate owner. Content fingerprints cache each unit's code
// and external bindings independently of shared image, loader, and TLS metadata.
// Every unit uses the same bundle-wide guest layout and lifted symbol names.

#include "TraceManager.h"

#include "Lift.h"
#include "lifter/Binary/Loader.h"
#if defined(ELFCONV_X86_BUILD) && ELFCONV_X86_BUILD == 1
#  include "remill/Arch/Runtime/RemillTypes.h"
#elif defined(ELFCONV_AARCH64_BUILD) && ELFCONV_AARCH64_BUILD == 1
#  include "remill/Arch/Runtime/Types.h"
#endif

#include <algorithm>
#include <bfd.h>
#include <cstdint>
#include <functional>
#include <utils/Util.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>

namespace {
constexpr const char *kImportUnitOwner = "__elfconv_imports__";
constexpr const char *kUnmappedUnitOwner = "__elfconv_unmapped__";

std::string Trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool ParseMapHex(const std::string &text, uint64_t &value) {
  try {
    size_t parsed = 0;
    value = std::stoull(text, &parsed, 0);
    return parsed == text.size();
  } catch (...) {
    return false;
  }
}

bool IsCodeMapSection(const std::string &section) {
  return section == ".text" || section.rfind(".text.", 0) == 0 ||
         section == ".init" || section == ".fini";
}

std::string NormalizeMapOwner(std::string owner, const std::string &object_base) {
  owner = Trim(std::move(owner));
  const auto section_suffix = owner.find(":(");
  if (section_suffix != std::string::npos) owner.resize(section_suffix);
  if (owner.find(".o") == std::string::npos && owner.find(".a") == std::string::npos) return {};

  const auto archive_member = owner.find(".a(");
  if (archive_member != std::string::npos) {
    auto archive = std::filesystem::path(owner.substr(0, archive_member + 2));
    if (archive.is_relative()) archive = std::filesystem::path(object_base) / archive;
    return archive.lexically_normal().generic_string() + owner.substr(archive_member + 2);
  }

  auto path = std::filesystem::path(owner);
  if (path.is_relative()) path = std::filesystem::path(object_base) / path;
  return path.lexically_normal().generic_string();
}

template <typename T>
void WriteScalar(std::ofstream &out, const T &value) {
  out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

void WriteString(std::ofstream &out, const std::string &value) {
  const uint64_t size = value.size();
  WriteScalar(out, size);
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
}
}  // namespace

void AArch64TraceManager::LoadLinkerMap(const std::string &path,
                                       const std::string &object_base_path) {
  if (path.empty()) elfconv_runtime_error("Incremental Wasm builds require a linker map.\n");
  object_base = std::filesystem::absolute(object_base_path).lexically_normal().generic_string();
  std::ifstream input(path);
  if (!input) elfconv_runtime_error("Cannot open SM64 linker map: %s\n", path.c_str());

  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    std::string first, second;
    if (!(fields >> first >> second)) continue;

    std::string section;
    std::string address_text;
    std::string size_text;
    std::string owner;
    if (!first.empty() && first.front() == '.') {
      section = first;
      address_text = second;
      if (!(fields >> size_text)) continue;
      std::getline(fields, owner);
    } else {
      address_text = first;
      size_text = second;
      std::getline(fields, owner);
      const auto suffix = owner.find(":(");
      if (suffix == std::string::npos) continue;
      const auto section_begin = suffix + 2;
      const auto section_end = owner.find(')', section_begin);
      if (section_end == std::string::npos) continue;
      section = owner.substr(section_begin, section_end - section_begin);
      owner.resize(suffix);
    }
    if (!IsCodeMapSection(section)) continue;

    uint64_t begin = 0;
    uint64_t size = 0;
    if (!ParseMapHex(address_text, begin) || !ParseMapHex(size_text, size) || size == 0) continue;
    auto normalized_owner = NormalizeMapOwner(std::move(owner), object_base);
    if (normalized_owner.empty() || begin > std::numeric_limits<uint64_t>::max() - size) continue;
    begin += elf_obj.load_bias;
    code_owner_ranges.push_back({begin, begin + size, std::move(normalized_owner)});
  }
  std::sort(code_owner_ranges.begin(), code_owner_ranges.end(),
            [](const CodeOwnerRange &lhs, const CodeOwnerRange &rhs) {
              if (lhs.begin != rhs.begin) return lhs.begin < rhs.begin;
              return lhs.end < rhs.end;
            });
  if (code_owner_ranges.empty()) {
    LOG(WARNING) << "No object code ranges found in linker map " << path
                 << "; functions will share one fallback unit.";
  }
}

void AArch64TraceManager::LoadELFOwners() {
  code_owner_ranges.clear();
  for (const auto &section : elf_obj.sections) {
    if (section.sec_type != BinaryLoader::ELFSection::SEC_TYPE_CODE || !section.size) continue;
    std::string owner = elf_obj.file_name;
    if (elf_obj.i386_imports.count(section.vma)) {
      owner = kImportUnitOwner;
    } else if (!elf_obj.i386_libraries.empty()) {
      const auto library = std::find_if(elf_obj.i386_libraries.begin(), elf_obj.i386_libraries.end(),
          [&](const auto &image) {
            return section.vma >= image.image_begin &&
                   section.vma + section.size <= image.image_end;
          });
      if (library == elf_obj.i386_libraries.end())
        elfconv_runtime_error("Executable section has no ELF owner: %s.\n", section.sec_name.c_str());
      owner = library->path;
    }
    if (owner.find_first_of("\t\r\n") != std::string::npos)
      elfconv_runtime_error("ELF unit paths cannot contain tabs or newlines.\n");
    code_owner_ranges.push_back({section.vma, section.vma + section.size, owner});
  }
  std::sort(code_owner_ranges.begin(), code_owner_ranges.end(),
      [](const auto &left, const auto &right) { return left.begin < right.begin; });
}

void AArch64TraceManager::EnableUnitMode(const std::string &owner, const remill::Arch *arch,
                                        llvm::Module *external_module) {
  if (owner.empty() || !arch || !external_module) {
    elfconv_runtime_error("Invalid incremental unit configuration.\n");
  }
  unit_mode = true;
  unit_owner = owner;
  unit_arch = arch;
  external_declarations = external_module;
}

std::string AArch64TraceManager::GetObjectOwner(uint64_t address) const {
  if (elf_obj.i386_imports.count(address)) return kImportUnitOwner;
  const CodeOwnerRange *best = nullptr;
  auto range = std::upper_bound(
      code_owner_ranges.begin(), code_owner_ranges.end(), address,
      [](uint64_t value, const CodeOwnerRange &candidate) { return value < candidate.begin; });
  while (range != code_owner_ranges.begin()) {
    --range;
    if (address >= range->begin && address < range->end &&
        (!best || range->end - range->begin < best->end - best->begin)) {
      best = &*range;
    }
  }
  return best ? best->owner : kUnmappedUnitOwner;
}

bool AArch64TraceManager::IsSelectedUnitAddress(uint64_t address) const {
  return !unit_mode || GetObjectOwner(address) == unit_owner;
}

void AArch64TraceManager::WriteIncrementalManifest(
    const std::string &manifest_path, const std::string &metadata_fingerprint_path) const {
  std::map<std::string, std::vector<const DisasmFunc *>> units;
  for (const auto &[address, function] : disasm_funcs) {
    units[GetObjectOwner(address)].push_back(&function);
  }

  std::filesystem::create_directories(std::filesystem::path(manifest_path).parent_path());
  std::ofstream manifest(manifest_path, std::ios::trunc);
  if (!manifest) elfconv_runtime_error("Cannot write incremental unit manifest: %s\n",
                                      manifest_path.c_str());

  size_t unit_index = 0;
  for (const auto &[owner, functions] : units) {
    const auto fingerprint_path =
        manifest_path + ".unit-" + std::to_string(unit_index++) + ".fingerprint";
    std::ofstream fingerprint(fingerprint_path, std::ios::binary | std::ios::trunc);
    if (!fingerprint) elfconv_runtime_error("Cannot write unit fingerprint: %s\n",
                                            fingerprint_path.c_str());
    WriteString(fingerprint, owner);
    // Other units' code is independently cached, but their guest addresses and
    // lifted names determine this unit's external call declarations.
    const uint64_t binding_count = disasm_funcs.size();
    WriteScalar(fingerprint, binding_count);
    for (const auto &[address, function] : disasm_funcs) {
      WriteScalar(fingerprint, address);
      WriteString(fingerprint, function.func_name);
    }
    const uint64_t imports = elf_obj.i386_imports.size();
    WriteScalar(fingerprint, imports);
    for (const auto &[address, name] : elf_obj.i386_imports) {
      WriteScalar(fingerprint, address);
      WriteString(fingerprint, name);
    }
    const uint64_t function_count = functions.size();
    WriteScalar(fingerprint, function_count);
    uint64_t owner_range_count = 0;
    for (const auto &range : code_owner_ranges) {
      if (range.owner == owner) ++owner_range_count;
    }
    WriteScalar(fingerprint, owner_range_count);
    for (const auto &range : code_owner_ranges) {
      if (range.owner != owner) continue;
      const uint64_t range_size = range.end - range.begin;
      WriteScalar(fingerprint, range.begin);
      WriteScalar(fingerprint, range_size);
      for (uint64_t offset = 0; offset < range_size; ++offset) {
        const auto byte = memory.find(range.begin + offset);
        if (byte == memory.end()) {
          elfconv_runtime_error("Missing executable byte for owner range at 0x%lx.\n",
                                range.begin + offset);
        }
        fingerprint.put(static_cast<char>(byte->second));
      }
    }
    if (owner == kUnmappedUnitOwner) {
      const auto code_section_count = std::count_if(
          elf_obj.sections.begin(), elf_obj.sections.end(), [](const auto &section) {
            return section.sec_type == BinaryLoader::ELFSection::SEC_TYPE_CODE;
          });
      const uint64_t count = static_cast<uint64_t>(code_section_count);
      WriteScalar(fingerprint, count);
      for (const auto &section : elf_obj.sections) {
        if (section.sec_type != BinaryLoader::ELFSection::SEC_TYPE_CODE) continue;
        WriteString(fingerprint, section.sec_name);
        WriteScalar(fingerprint, section.vma);
        WriteScalar(fingerprint, section.size);
        if (section.size) {
          fingerprint.write(reinterpret_cast<const char *>(section.bytes),
                            static_cast<std::streamsize>(section.size));
        }
      }
    }
    for (const auto *function : functions) {
      const uint64_t address = function->vma;
      const uint64_t size = function->func_size;
      WriteScalar(fingerprint, address);
      WriteScalar(fingerprint, size);
      WriteString(fingerprint, function->func_name);
      for (uint64_t offset = 0; offset < size; ++offset) {
        const auto byte = memory.find(address + offset);
        if (byte == memory.end()) {
          elfconv_runtime_error("Missing executable byte for function at 0x%lx.\n",
                                address + offset);
        }
        fingerprint.put(static_cast<char>(byte->second));
      }
    }
    if (!fingerprint) elfconv_runtime_error("Failed writing unit fingerprint: %s\n",
                                            fingerprint_path.c_str());
    manifest << owner << '\t' << fingerprint_path << '\n';
  }

  std::ofstream metadata(metadata_fingerprint_path, std::ios::binary | std::ios::trunc);
  if (!metadata) elfconv_runtime_error("Cannot write metadata fingerprint: %s\n",
                                      metadata_fingerprint_path.c_str());
  WriteScalar(metadata, entry_point);
  WriteString(metadata, entry_func_lifted_name);
  WriteString(metadata, elf_obj.entry_symbol);
  WriteScalar(metadata, elf_obj.is_stripped);
  WriteScalar(metadata, elf_obj.able_vrp_opt);
  WriteScalar(metadata, elf_obj.e_phent);
  WriteScalar(metadata, elf_obj.e_phnum);
  const uint64_t ph_size = elf_obj.e_phent * elf_obj.e_phnum;
  WriteScalar(metadata, ph_size);
  if (ph_size) metadata.write(reinterpret_cast<const char *>(elf_obj.e_ph),
                              static_cast<std::streamsize>(ph_size));
  const auto metadata_section_count = std::count_if(
      elf_obj.sections.begin(), elf_obj.sections.end(), [](const auto &section) {
        return section.sec_type != BinaryLoader::ELFSection::SEC_TYPE_CODE &&
               section.sec_type != BinaryLoader::ELFSection::SEC_TYPE_UNKNOWN;
      });
  const uint64_t section_count = static_cast<uint64_t>(metadata_section_count);
  WriteScalar(metadata, section_count);
  for (const auto &section : elf_obj.sections) {
    if (section.sec_type == BinaryLoader::ELFSection::SEC_TYPE_CODE ||
        section.sec_type == BinaryLoader::ELFSection::SEC_TYPE_UNKNOWN) continue;
    WriteString(metadata, section.sec_name);
    WriteScalar(metadata, section.vma);
    WriteScalar(metadata, section.size);
    WriteScalar(metadata, section.is_tls);
    if (section.size) {
      metadata.write(reinterpret_cast<const char *>(section.bytes),
                     static_cast<std::streamsize>(section.size));
    }
  }
  const uint64_t initializer_count = elf_obj.i386_initializers.size();
  WriteScalar(metadata, initializer_count);
  for (const auto address : elf_obj.i386_initializers) WriteScalar(metadata, address);
  const uint64_t finalizer_count = elf_obj.i386_finalizers.size();
  WriteScalar(metadata, finalizer_count);
  for (const auto address : elf_obj.i386_finalizers) WriteScalar(metadata, address);
  const uint64_t import_count = elf_obj.i386_imports.size();
  WriteScalar(metadata, import_count);
  for (const auto &[address, name] : elf_obj.i386_imports) {
    WriteScalar(metadata, address);
    WriteString(metadata, name);
  }
  WriteScalar(metadata, elf_obj.tls_static_size);
  WriteScalar(metadata, elf_obj.tls_static_alignment);
  const uint64_t library_count = elf_obj.i386_libraries.size();
  WriteScalar(metadata, library_count);
  for (const auto &library : elf_obj.i386_libraries) {
    WriteString(metadata, library.path);
    WriteString(metadata, library.name);
    WriteScalar(metadata, library.image_begin);
    WriteScalar(metadata, library.image_end);
    const uint64_t needed_count = library.needed.size();
    WriteScalar(metadata, needed_count);
    for (const auto &name : library.needed) WriteString(metadata, name);
    const uint64_t ordinary_exports = library.exports.size();
    WriteScalar(metadata, ordinary_exports);
    for (const auto &[name, address] : library.exports) {
      WriteString(metadata, name);
      WriteScalar(metadata, address);
    }
    const uint64_t initializers = library.initializers.size();
    WriteScalar(metadata, initializers);
    for (const auto address : library.initializers) WriteScalar(metadata, address);
    const uint64_t finalizers = library.finalizers.size();
    WriteScalar(metadata, finalizers);
    for (const auto address : library.finalizers) WriteScalar(metadata, address);
    WriteScalar(metadata, library.tls_size);
    WriteScalar(metadata, library.tls_alignment);
    WriteScalar(metadata, library.tls_distance);
    const uint64_t template_size = library.tls_template.size();
    WriteScalar(metadata, template_size);
    if (template_size) metadata.write(reinterpret_cast<const char *>(library.tls_template.data()),
                                      static_cast<std::streamsize>(template_size));
    const uint64_t export_count = library.tls_exports.size();
    WriteScalar(metadata, export_count);
    for (const auto &[name, offset] : library.tls_exports) {
      WriteString(metadata, name);
      WriteScalar(metadata, offset);
    }
  }
  if (!metadata) elfconv_runtime_error("Failed writing metadata fingerprint: %s\n",
                                      metadata_fingerprint_path.c_str());
}

void AArch64TraceManager::SetLiftedTraceDefinition(uint64_t addr, llvm::Function *lifted_func) {
  traces[addr] = lifted_func;
}

std::string AArch64TraceManager::AddRestDisasmFunc(uint64_t addr) {
  auto rest_fun_name = GetUniqueLiftedFuncName("_ecv_rest_fun", addr);
  if (unit_mode && !IsSelectedUnitAddress(addr)) {
    external_func_names[addr] = rest_fun_name;
    return rest_fun_name;
  }
  auto upper_addr_1 = disasm_funcs.upper_bound(addr);
  auto upper_addr_2 = rest_disasm_funcs.upper_bound(addr);
  uint64_t end_addr;
  if (upper_addr_1 != disasm_funcs.end() && upper_addr_2 != rest_disasm_funcs.end()) {
    end_addr = (uint64_t) std::min(upper_addr_1->first, upper_addr_2->first);
  } else if (upper_addr_1 != disasm_funcs.end()) {
    end_addr = upper_addr_1->first;
  } else if (upper_addr_2 != rest_disasm_funcs.end()) {
    end_addr = upper_addr_2->first;
  } else {
    LOG(FATAL) << "[Bug] does not handle the pattern of having last rest_disasm_func.";
  }
  if (auto *section = elf_obj.GetIncludedSection(addr)) {
    end_addr = std::min<uint64_t>(end_addr, bfd_section_vma(section) + bfd_section_size(section));
  }
  rest_disasm_funcs.insert({addr, DisasmFunc(rest_fun_name, addr, end_addr - addr)});
  return rest_fun_name;
}

llvm::Function *AArch64TraceManager::GetLiftedTraceDeclaration(uint64_t addr) {
  auto trace_it = traces.find(addr);
  if (trace_it != traces.end()) {
    return trace_it->second;
  } else {
    return nullptr;
  }
}

llvm::Function *AArch64TraceManager::GetLiftedTraceDefinition(uint64_t addr) {
  if (unit_mode && !IsSelectedUnitAddress(addr)) {
    if (!unit_arch || !external_declarations) {
      elfconv_runtime_error("External unit declarations are not configured.\n");
    }
    const auto name = GetLiftedFuncName(addr);
    auto *declaration = external_declarations->getFunction(name);
    return declaration ? declaration : unit_arch->DeclareLiftedFunction(name, external_declarations);
  }
  return GetLiftedTraceDeclaration(addr);
}

bool AArch64TraceManager::TryReadExecutableByte(uint64_t addr, uint8_t *byte) {

  auto byte_it = memory.find(addr);
  if (byte_it != memory.end()) {
    *byte = byte_it->second;
    return true;
  } else {
    return false;
  }
}

std::string AArch64TraceManager::GetLiftedFuncName(uint64_t addr) {
  if (disasm_funcs.count(addr) == 1) {
    return disasm_funcs.at(addr).func_name;
  } else if (rest_disasm_funcs.count(addr) == 1) {
    return rest_disasm_funcs.at(addr).func_name;
  } else if (external_func_names.count(addr) == 1) {
    return external_func_names.at(addr);
  } else {
    elfconv_runtime_error("[ERROR] addr (0x%lx) doesn't indicate the entry of function.\n", addr);
  }
}

bool AArch64TraceManager::isFunctionEntry(uint64_t addr) {
  return disasm_funcs.count(addr) == 1 || rest_disasm_funcs.count(addr) == 1;
}

std::string AArch64TraceManager::GetUniqueLiftedFuncName(std::string func_name, uint64_t vma_s) {
  std::stringstream lifted_fn_name;
  lifted_fn_name << func_name << "_____" << std::hex << vma_s;
  return lifted_fn_name.str();
}

uint64_t AArch64TraceManager::GetFuncVMA_E(uint64_t vma_s) {
  if (disasm_funcs.count(vma_s) == 1) {
    return vma_s + disasm_funcs[vma_s].func_size;
  } else if (rest_disasm_funcs.count(vma_s) == 1) {
    return vma_s + rest_disasm_funcs[vma_s].func_size;
  } else {
    elfconv_runtime_error("[ERROR] vma_s (%ld) is not a start address of function.\n", vma_s);
  }
}

uint64_t AArch64TraceManager::GetFuncNums() {
  return disasm_funcs.size() || rest_disasm_funcs.size();
}

void AArch64TraceManager::SetELFData() {

  elf_obj.LoadELF();
  entry_point = elf_obj.entry;

  // Set text section
  elf_obj.SetCodeSection();

  // Set memory of all code section bytes.
  for (auto &[_, code_sec] : elf_obj.code_sections) {
    for (addr_t addr = code_sec.vma; addr < code_sec.vma + code_sec.size; addr++) {
      memory[addr] = code_sec.bytes[addr - code_sec.vma];
    }
  }

  // Make all disasmbled functions data depending on optimization mode.
  if (elf_obj.able_vrp_opt) {

    auto &func_symbols = elf_obj.func_symbols;

    for (size_t i = 0; i < func_symbols.size() - 1; i++) {
      auto lifted_func_name =
          GetUniqueLiftedFuncName(func_symbols[i].sym_name, func_symbols[i].addr);

      // Set program entry point function if applicapable.
      if (entry_point == func_symbols[i].addr) {
        entry_func_lifted_name = lifted_func_name;
      }

      uint64_t func_size = 0;
      // Prefer symbol table size if available
      if (func_symbols[i].size > 0) {
        func_size = func_symbols[i].size;
      } else if (func_symbols[i].in_section == func_symbols[i + 1].in_section) {
        func_size = func_symbols[i + 1].addr - func_symbols[i].addr;
      } else {
        func_size = (bfd_section_vma(func_symbols[i].in_section) +
                     bfd_section_size(func_symbols[i].in_section)) -
                    func_symbols[i].addr;
      }

      disasm_funcs.emplace(func_symbols[i].addr,
                           DisasmFunc(lifted_func_name, func_symbols[i].addr, func_size));
    }

    // Last function.
    auto last_func_symbol = func_symbols.back();
    auto lifted_func_name =
        GetUniqueLiftedFuncName(last_func_symbol.sym_name, last_func_symbol.addr);
    uint64_t func_size = 0;
    // Prefer symbol table size if available
    if (last_func_symbol.size > 0) {
      func_size = last_func_symbol.size;
    } else {
      func_size = (bfd_section_vma(last_func_symbol.in_section) +
                   bfd_section_size(last_func_symbol.in_section)) -
                  last_func_symbol.addr;
    }
    disasm_funcs.emplace(last_func_symbol.addr,
                         DisasmFunc(lifted_func_name, last_func_symbol.addr, func_size));

    if (entry_point == last_func_symbol.addr) {
      entry_func_lifted_name = lifted_func_name;
    }

  } else {
    elfconv_runtime_error("Now not supported for ELF with no eh_frame section.\n");
  }

  if (entry_func_lifted_name.empty()) {
    elfconv_runtime_error("[ERROR] entry_function is not found.\n");
  }

  if (elf_obj.bin_arch != BinaryLoader::ELFObject::ARCH_AARCH64) {
    return;
  }

  // define functions in .plt section (FIXME)
  auto plt_section = elf_obj.code_sections[".plt"];
  if (plt_section.sec_name.empty())
    plt_section = elf_obj.code_sections[".iplt"];
  if (!plt_section.sec_name.empty()) {
    uint64_t ins_i = 0;
    while (ins_i < plt_section.size) {
      auto b_entry = plt_section.vma + ins_i;
      for (; ins_i < plt_section.size;) {
        memory[plt_section.vma + ins_i] = plt_section.bytes[ins_i];
        memory[plt_section.vma + ins_i + 1] = plt_section.bytes[ins_i + 1];
        memory[plt_section.vma + ins_i + 2] = plt_section.bytes[ins_i + 2];
        memory[plt_section.vma + ins_i + 3] = plt_section.bytes[ins_i + 3];
        uint8_t *bts = plt_section.bytes + ins_i;
        ins_i += AARCH64_OP_SIZE;
        if ((bts[0] & 0x1f) == 0x00 && (bts[1] & 0xfc) == 0x00 && bts[2] == 0x1f &&
            bts[3] == 0xd6) { /* br instruction (FIXME) */
          break;
        }
      }
      std::stringstream fn_name;
      fn_name << "fn_plt_" << std::hex << b_entry;
      disasm_funcs.emplace(b_entry,
                           DisasmFunc(fn_name.str(), b_entry, (plt_section.vma + ins_i) - b_entry));
    }
  }

  /* 
    define __wrap_main function (FIXME)
    __libc_start_call_main BLR jump to the instructions as following in _start.
    `nop`
    `b main`
    `nop`
  */
  if (disasm_funcs.count(entry_point) == 1) {
    std::vector<uint64_t> s_t_addrs = {entry_point};
    uint64_t __wrap_main_size = AARCH64_OP_SIZE * 3;
    auto &text_section = elf_obj.code_sections[".text"];
    bool __wrap_main_found = false;
    uint64_t extra_search_size = 100;
    for (size_t j = 0; j < s_t_addrs.size(); j++) {
      auto func_size = disasm_funcs.contains(s_t_addrs[j]) ? disasm_funcs[s_t_addrs[j]].func_size
                                                           : __wrap_main_size;
      auto _s_fn_bytes = &text_section.bytes[s_t_addrs[j] - text_section.vma];
      for (uint64_t i = 0; i + __wrap_main_size <= func_size + extra_search_size; i += 4) {
        if (/* nop or bti c */ ((_s_fn_bytes[i] == 0x1f && _s_fn_bytes[i + 1] == 0x20) ||
                                (_s_fn_bytes[i] == 0x5f && _s_fn_bytes[i + 1] == 0x24)) &&
            _s_fn_bytes[i + 2] == 0x03 && _s_fn_bytes[i + 3] == 0xd5 &&
            /* b <label> */ (_s_fn_bytes[i + 7] & 0xfc) == 0x14 &&
            /* nop */ _s_fn_bytes[i + 8] == 0x1f && _s_fn_bytes[i + 9] == 0x20 &&
            _s_fn_bytes[i + 10] == 0x03 && _s_fn_bytes[i + 11] == 0xd5) {
          uint64_t __wrap_main_vma = s_t_addrs[j] + i;
          disasm_funcs.emplace(__wrap_main_vma,
                               DisasmFunc("__wrap_main", __wrap_main_vma, __wrap_main_size));
          __wrap_main_found = true;
          goto found_entry;
        }
      }
    }
  found_entry:
    if (!__wrap_main_found) {
      elfconv_runtime_error("[ERROR] __wrap_main code block is not found. entry_point: 0x%lx\n",
                            entry_point);
    }
  } else {
    elfconv_runtime_error("[ERROR] Entry function is not defined.\n");
  }
}