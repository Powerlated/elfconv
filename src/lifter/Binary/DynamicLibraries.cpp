#include "Loader.h"

#include <gelf.h>
#include <filesystem>
#include <unistd.h>
#include "utils/elfconv.h"
#include "utils/Util.h"
#include "utils/I386MemoryLayout.h"

using namespace BinaryLoader;

void ELFObject::ReadI386DynamicMetadata(I386Library &library, uint32_t bias) {
  elf_version(EV_CURRENT);
  int fd = open(library.path.c_str(), O_RDONLY);
  Elf *elf = fd < 0 ? nullptr : elf_begin(fd, ELF_C_READ, nullptr);
  GElf_Ehdr header;
  if (!elf || !gelf_getehdr(elf, &header) || header.e_machine != EM_386 ||
      gelf_getclass(elf) != ELFCLASS32)
    elfconv_runtime_error("Bundled libraries require i386 ELF: %s.\n", library.path.c_str());
  if (dependency_object && header.e_type != ET_DYN)
    elfconv_runtime_error("Bundled dependency must be a shared object: %s.\n", library.path.c_str());
  const uint32_t effective_bias = header.e_type == ET_DYN ? bias : 0;
  size_t count;
  if (elf_getphdrnum(elf, &count)) elfconv_runtime_error("Invalid library program headers.\n");
  for (size_t i = 0; i < count; ++i) {
    GElf_Phdr ph;
    if (!gelf_getphdr(elf, i, &ph)) elfconv_runtime_error("Invalid library program header.\n");
    if (dependency_object && ph.p_type == PT_INTERP)
      elfconv_runtime_error("Bundled library cannot have an interpreter: %s.\n", library.path.c_str());
    if (ph.p_type == PT_TLS) {
      const uint64_t alignment = std::max<uint64_t>(ph.p_align, 1);
      if ((alignment & (alignment - 1)) || alignment > 0x100000 ||
          ph.p_filesz > ph.p_memsz || ph.p_memsz > 0x00f00000)
        elfconv_runtime_error("Invalid or oversized i386 TLS segment.\n");
      library.tls_size = ph.p_memsz;
      library.tls_alignment = alignment;
      library.tls_first_byte = ph.p_vaddr & (alignment - 1);
      library.tls_vma = ph.p_vaddr + effective_bias;
      library.tls_template.resize(ph.p_filesz);
      if (ph.p_filesz) {
        auto *data = elf_getdata_rawchunk(elf, ph.p_offset, ph.p_filesz, ELF_T_BYTE);
        if (!data || data->d_size != ph.p_filesz)
          elfconv_runtime_error("Invalid i386 TLS template.\n");
        memcpy(library.tls_template.data(), data->d_buf, ph.p_filesz);
      }
    }
    if (ph.p_type == PT_LOAD) {
      library.image_begin = std::min(library.image_begin, ph.p_vaddr + effective_bias);
      library.image_end = std::max(library.image_end, ph.p_vaddr + ph.p_memsz + effective_bias);
    }
  }
  library.name = std::filesystem::path(library.path).filename().string();
  Elf_Scn *scn = nullptr;
  while ((scn = elf_nextscn(elf, scn))) {
    GElf_Shdr sh;
    if (!gelf_getshdr(scn, &sh)) elfconv_runtime_error("Invalid library section.\n");
    if (sh.sh_type != SHT_DYNAMIC && sh.sh_type != SHT_DYNSYM) continue;
    Elf_Data *data = elf_getdata(scn, nullptr);
    if (!data || !sh.sh_entsize) elfconv_runtime_error("Invalid library dynamic table.\n");
    for (size_t i = 0; i < sh.sh_size / sh.sh_entsize; ++i) {
      if (sh.sh_type == SHT_DYNAMIC) {
        GElf_Dyn dynamic;
        if (!gelf_getdyn(data, i, &dynamic)) elfconv_runtime_error("Invalid library dynamic entry.\n");
        if (dynamic.d_tag != DT_NEEDED && dynamic.d_tag != DT_SONAME) continue;
        const char *name = elf_strptr(elf, sh.sh_link, dynamic.d_un.d_val);
        if (!name) elfconv_runtime_error("Invalid library dependency name.\n");
        if (dynamic.d_tag == DT_SONAME) library.name = name;
        else library.needed.emplace_back(name);
      } else {
        GElf_Sym symbol;
        if (!gelf_getsym(data, i, &symbol)) elfconv_runtime_error("Invalid library export.\n");
        const unsigned visibility = GELF_ST_VISIBILITY(symbol.st_other);
        if (symbol.st_shndx == SHN_UNDEF || GELF_ST_BIND(symbol.st_info) == STB_LOCAL ||
            visibility == STV_HIDDEN || visibility == STV_INTERNAL) continue;
        const unsigned type = GELF_ST_TYPE(symbol.st_info);
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE && type != STT_TLS) continue;
        const char *name = elf_strptr(elf, sh.sh_link, symbol.st_name);
        if (!name) elfconv_runtime_error("Invalid library export name.\n");
        if (type == STT_TLS) {
          if (symbol.st_value >= library.tls_size)
            elfconv_runtime_error("TLS export outside its module: %s.\n", name);
          library.tls_exports.emplace(name, symbol.st_value);
          continue;
        }
        library.exports.emplace(name, symbol.st_value + (symbol.st_shndx == SHN_ABS ? 0 : effective_bias));
      }
    }
  }
  elf_end(elf);
  close(fd);
}

void ELFObject::LoadI386Libraries() {
  I386Library main;
  main.path = file_name;
  ReadI386DynamicMetadata(main, i386_memory::kMainImageStart);
  i386_libraries.push_back(std::move(main));
  shared_symbols = i386_libraries[0].exports;
  // Reserve libraries below the heap, away from guest TLS and import strings.
  uint64_t next = i386_memory::kLibraryStart;
  for (const auto &path : shared_library_paths) {
    auto object = std::make_unique<ELFObject>(path);
    object->dependency_object = true;
    object->load_bias = next;
    I386Library library;
    library.path = path;
    object->ReadI386DynamicMetadata(library, object->load_bias);
    next = (library.image_end + 0x1ffff) & ~uint64_t(0xffff);
    if (next > i386_memory::kLibraryEnd)
      elfconv_runtime_error("Bundled i386 libraries exceed the reserved guest image region.\n");
    for (const auto &existing : i386_libraries) {
      if (existing.name == library.name)
        elfconv_runtime_error("Duplicate bundled library name: %s.\n", library.name.c_str());
    }
    for (const auto &[name, address] : library.exports) shared_symbols.emplace(name, address);
    i386_libraries.push_back(std::move(library));
    shared_objects.push_back(std::move(object));
  }
  uint64_t distance = 0;
  for (size_t i = 0; i < i386_libraries.size(); ++i) {
    auto &library = i386_libraries[i];
    if (library.tls_size) {
      const uint64_t mask = library.tls_alignment - 1;
      distance = ((distance + library.tls_size + library.tls_first_byte + mask) & ~mask)
                 - library.tls_first_byte;
      if (distance > 0x00f00000)
        elfconv_runtime_error("Bundled i386 TLS exceeds the reserved TLS region.\n");
      library.tls_distance = distance;
      tls_static_alignment = std::max(tls_static_alignment, library.tls_alignment);
    }
    for (const auto &[name, offset] : library.tls_exports)
      shared_tls_symbols.emplace(name, I386TlsSymbol{static_cast<uint32_t>(i + 1), offset});
  }
  const uint64_t mask = tls_static_alignment - 1;
  distance = (distance + mask) & ~mask;
  if (distance + 64 + 8 * (i386_libraries.size() + 2) > 0x00f00000)
    elfconv_runtime_error("i386 static TLS exceeds the reserved TLS region.\n");
  tls_static_size = distance;
  tls_libraries = &i386_libraries;
  LoadELFBFD();
  i386_libraries[0].initializers = i386_initializers;
  i386_libraries[0].finalizers = i386_finalizers;
  CaptureI386TLS(i386_libraries[0]);
  for (size_t i = 0; i < shared_objects.size(); ++i) {
    auto &object = *shared_objects[i];
    object.shared_symbol_scope = &shared_symbols;
    object.shared_tls_scope = &shared_tls_symbols;
    object.tls_libraries = &i386_libraries;
    object.tls_module = i + 2;
    object.LoadELFBFD();
    i386_libraries[i + 1].initializers = object.i386_initializers;
    i386_libraries[i + 1].finalizers = object.i386_finalizers;
    object.CaptureI386TLS(i386_libraries[i + 1]);
    for (auto &section : object.sections) {
      section.sec_name = "library" + std::to_string(i + 1) + ":" + section.sec_name;
      sections.push_back(section);
    }
    func_symbols.insert(func_symbols.end(), object.func_symbols.begin(), object.func_symbols.end());
    i386_imports.insert(object.i386_imports.begin(), object.i386_imports.end());
  }
  std::sort(func_symbols.begin(), func_symbols.end(),
            [](const auto &a, const auto &b) { return a.addr < b.addr; });
}

void ELFObject::CaptureI386TLS(I386Library &library) {
  size_t offset = 0;
  while (offset < library.tls_template.size()) {
    bool found = false;
    for (const auto &section : sections) {
      const uint64_t address = library.tls_vma + offset;
      if (!section.is_tls || address < section.vma || address - section.vma >= section.size)
        continue;
      const size_t length = std::min<uint64_t>(library.tls_template.size() - offset,
                                              section.size - (address - section.vma));
      memcpy(library.tls_template.data() + offset, section.bytes + address - section.vma, length);
      offset += length;
      found = true;
      break;
    }
    if (!found) elfconv_runtime_error("Unmapped relocated i386 TLS template.\n");
  }
}
