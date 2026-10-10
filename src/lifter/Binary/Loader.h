#pragma once

#include <algorithm>
#include <bfd.h>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <libdwarf/dwarf.h>
#include <libdwarf/libdwarf.h>
#include <libelf.h>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace BinaryLoader {

class ELFSymbol;
class ELFSection;
class ELFObject;

class ELFSymbol {
 public:
  enum SymbolType {
    SYM_TYPE_FUNC = 1,
    SYM_TYPE_LVAR = 2,
    SYM_TYPE_GVAR = 3,
    SYM_TYPE_UNKNOWN = 4,
  };

  ELFSymbol(SymbolType __sym_type, std::string __sym_name, uintptr_t __addr,
            bfd_section *__in_section, uint64_t __size = 0)
      : sym_type(__sym_type),
        sym_name(__sym_name),
        addr(__addr),
        in_section(__in_section),
        size(__size) {}
  ELFSymbol::SymbolType sym_type;
  std::string sym_name;
  uintptr_t addr;
  bfd_section *in_section;
  uint64_t size;  // Function size from symbol table
};

class ELFSection {
 public:
  enum SectionType {
    SEC_TYPE_CODE = 0,
    SEC_TYPE_DATA = 1,
    SEC_TYPE_READONLY = 2,
    SEC_TYPE_UNKNOWN = 3,
  };

  ELFSection(ELFObject *__elf_obj, ELFSection::SectionType __sec_type, std::string __sec_name,
             uint64_t __vma, uint64_t __size, uint8_t *__bytes)
      : elf_obj(__elf_obj),
        sec_type(__sec_type),
        sec_name(__sec_name),
        vma(__vma),
        size(__size),
        bytes(__bytes) {}
  ~ELFSection() {}

  ELFObject *elf_obj;
  ELFSection::SectionType sec_type;
  std::string sec_name;
  uint64_t vma;
  uint64_t size;
  uint8_t *bytes;
  bool is_tls = false;
  bool has_contents = true;
};

class ELFObject {
 public:
  enum BinaryType : uint8_t {
    BIN_TYPE_ELF = 0,
    BIN_TYPE_UNKNOWN = 1,
  };
  enum BinaryArch : uint8_t {
    ARCH_AARCH64 = 0,
    ARCH_AMD64 = 1,
    ARCH_I386 = 2,
    ARCH_UNKNOWN = 3,
  };

  struct CodeSection {
    std::string sec_name;
    uintptr_t vma;
    uint8_t *bytes;
    uint64_t size;
    CodeSection(std::string __sec_name, uintptr_t __vma, uint8_t *__bytes, uint64_t __size)
        : sec_name(__sec_name),
          vma(__vma),
          bytes(__bytes),
          size(__size) {}
    CodeSection() {}
  };

  void LoadELF();
  void SetCodeSection();
  asection *GetIncludedSection(uint64_t vma);
  void R2Detect();
  int GetSblFromEhFrame();
  int ParseEhFrame(Dwarf_Debug dbg);
  void GetFuncFromEhFrame(uint64_t *_entry, size_t *_size, Dwarf_Debug dbg, Dwarf_Fde fde);
  void DebugSections();
  void DebugStaticSymbols();
  void DebugBinary();

  ELFObject(std::string __file_name) : file_name(__file_name), bfd_inited(false) {
    is_stripped = false;
  }

  std::string file_name;
  bool bfd_inited;
  bfd *bfd_h;
  const bfd_arch_info_type *bfd_arch_info;
  ELFObject::BinaryType bin_type;
  std::string bin_type_str;
  ELFObject::BinaryArch bin_arch;
  std::string bin_arch_str;
  uint32_t bits;
  uintptr_t entry;
  uint32_t load_bias = 0;
  std::string entry_symbol;
  struct I386Library {
    std::string path, name;
    uint64_t image_end = 0;
    uint64_t image_begin = UINT32_MAX;
    std::vector<std::string> needed;
    std::map<std::string, uint32_t> exports;
    std::vector<uint32_t> initializers, finalizers;
    std::map<std::string, uint32_t> tls_exports;
    std::vector<uint8_t> tls_template;
    uint32_t tls_size = 0, tls_alignment = 1, tls_first_byte = 0, tls_distance = 0;
    uint64_t tls_vma = 0;
  };
  std::vector<std::string> shared_library_paths;
  std::vector<I386Library> i386_libraries;
  std::map<std::string, uint32_t> shared_symbols;
  const std::map<std::string, uint32_t> *shared_symbol_scope = nullptr;
  std::vector<std::unique_ptr<ELFObject>> shared_objects;
  bool dependency_object = false;
  struct I386TlsSymbol { uint32_t module, offset; };
  std::map<std::string, I386TlsSymbol> shared_tls_symbols;
  const std::map<std::string, I386TlsSymbol> *shared_tls_scope = nullptr;
  const std::vector<I386Library> *tls_libraries = nullptr;
  uint32_t tls_module = 1;
  uint32_t tls_static_size = 0, tls_static_alignment = 16;
  std::vector<ELFSection> sections;
  std::unordered_map<uint64_t, ELFSymbol> func_symbols_map;
  std::vector<ELFSymbol> func_symbols;
  std::unordered_map<std::string, CodeSection> code_sections;

  // ELF32 import entry addresses and their host symbol names.
  std::map<uint64_t, std::string> i386_imports;
  std::vector<uint32_t> i386_initializers;
  std::vector<uint32_t> i386_finalizers;
  unsigned long symbol_table_size;

  uint64_t e_phent;
  uint64_t e_phnum;
  uint8_t *e_ph;

  bool is_stripped;
  bool able_vrp_opt;

 private:
  void OpenELF();
  void LoadELFBFD();
  void LoadStaticSymbolsBFD();
  void LoadDynamicSymbolsBFD();
  void LoadSectionsBFD();
  void ResolveI386Imports();
  void ReadI386DynamicMetadata(I386Library &library, uint32_t bias);
  void LoadI386Libraries();
  void CaptureI386TLS(I386Library &library);
  void GetEhdr();
};
}  // namespace BinaryLoader