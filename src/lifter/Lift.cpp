/*
 * Copyright (c) 2018 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "remill/BC/ABI.h"
#include "remill/BC/TraceLifter.h"
#include "remill/BC/Util.h"

#include <cstdint>
#include <sstream>
#include <algorithm>
#include <memory>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>
#include <chrono>
#if defined(__linux__)
#  include <signal.h>
#  include <utils/Util.h>
#  include <utils/elfconv.h>
#endif

#include "Lift.h"
#include "MainLifter.h"
#include "TraceManager.h"

#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/IPO/Internalize.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/SplitModule.h>
#include <remill/BC/HelperMacro.h>
#include <remill/BC/InstructionLifter.h>
#include <remill/BC/Lifter.h>
#include <remill/BC/Optimizer.h>
#include <utils/Util.h>

DEFINE_string(bc_out, "", "Name of the file in which to place the generated bitcode.");

DEFINE_string(os, REMILL_OS,
              "Operating system name of the code being "
              "translated. Valid OSes: linux, macos, windows, solaris.");
DEFINE_string(arch, REMILL_ARCH,
              "Architecture of the code being translated. "
              "Valid architectures: x86, amd64 (with or without "
              "`_avx` or `_avx512` appended), aarch64, aarch32");
DEFINE_string(target_elf, "DUMMY_ELF", "Name of the target ELF binary");
DEFINE_string(entry_symbol, "", "Exported i386 shared-library entry with int(int,char**) ABI.");
DEFINE_string(shared_libraries, "", "Comma-separated i386 shared libraries bundled for guest dlopen.");
DEFINE_uint64(dbg_fun_vma, 0, "Function Address of the debug target");
DEFINE_string(bitcode_path, "", "Function Name of the debug target");
DEFINE_string(target_arch, "", "Target Architecture for conversion");
DEFINE_string(float_exception, "0", "Whether the floating-point exception status is set or not");
DEFINE_string(linker_map, "", "Linker map for final-ELF incremental unit ownership.");
DEFINE_string(object_base, "", "Base directory for relative object paths in the linker map.");
DEFINE_bool(per_elf, false, "Partition i386 bundled code by ELF image, with shared metadata.");
DEFINE_string(unit_manifest_out, "", "Write unit owners and content fingerprints, then exit.");
DEFINE_string(metadata_fingerprint_out, "", "Write the process-wide ELF metadata fingerprint.");
DEFINE_string(unit_owner, "", "Lift only functions owned by this linker-map object.");
DEFINE_string(unit_id, "", "Stable suffix used for this incremental unit's metadata symbols.");
DEFINE_uint64(max_ir_chunks_per_elf, 0, "Maximum linkable chunks per ELF; 1 skips partitioning.");
DEFINE_string(lift_profile_out, "", "Write per-function lift timing and LLVM IR metrics as TSV.");
DEFINE_bool(lift_progress, false, "Print live [lifted/known total] function counts.");
DEFINE_string(ir_chunk_manifest_out, "", "Write chunk count and IR sizes for --max_ir_chunks_per_elf.");
DEFINE_bool(analyze_only, false, "Print ELF section and function metrics without lifting.");
DEFINE_bool(metadata_only, false, "Emit only process-wide ELF metadata.");
DEFINE_string(
    norm_mode, "0",
    "Whether the test mode is on or off");  // We use `test_mode` for not only test mode but also `no VRP` mode.
DEFINE_string(fork_emulation, "",
              "enable the function of emulating fork syscall on emscripten browser.");

ArchName remill::EcvReg::target_elf_arch;

extern "C" void debug_stream_out_sigaction(int sig, siginfo_t *info, void *ctx) {
  std::cout << remill::ECV_DEBUG_STREAM.str();
  std::cout << "(Custom) Segmantation Fault." << std::endl;
  exit(EXIT_FAILURE);
}

void lift_set_sigaction() {
#if defined(__linux__)
  struct sigaction segv_action;
  segv_action.sa_flags = SA_SIGINFO;
  segv_action.sa_sigaction = debug_stream_out_sigaction;
  if (sigaction(SIGSEGV, &segv_action, NULL) < 0) {
    elfconv_runtime_error("sigaction for SIGSEGV failed.\n");
  }
#endif
}

static void EmitI386Libraries(llvm::Module &module, const BinaryLoader::ELFObject &object) {
  auto &context = module.getContext();
  auto *word = llvm::Type::getInt32Ty(context);
  auto *pointer = llvm::PointerType::getUnqual(context);
  auto *symbol_type = llvm::StructType::get(context, {pointer, word, word});
  auto *library_type = llvm::StructType::get(
      context, {pointer, pointer, pointer, pointer, pointer, word, pointer, pointer, word, word,
                pointer, word, word, word, word});
  auto emit = [&](const std::string &name, llvm::Constant *data) -> llvm::Constant * {
    return new llvm::GlobalVariable(module, data->getType(), true,
        llvm::GlobalValue::ExternalLinkage, data, name);
  };
  auto string = [&](const std::string &name, const std::string &value) {
    return emit(name, llvm::ConstantDataArray::getString(context, value));
  };
  auto functions = [&](const std::string &name, const std::vector<uint32_t> &values, uint32_t end) {
    std::vector<uint32_t> data(values.size() + 1);
    std::copy(values.begin(), values.end(), data.begin());
    data.back() = end;
    return emit(name, llvm::ConstantDataArray::get(context, data));
  };
  std::vector<llvm::Constant *> libraries;
  libraries.reserve(object.i386_libraries.size());
  for (size_t i = 0; i < object.i386_libraries.size(); ++i) {
    const auto &library = object.i386_libraries[i];
    const std::string prefix = "_ecv_i386_library_" + std::to_string(i);
    const auto slash = library.path.find_last_of('/');
    const std::string filename = library.path.substr(slash == std::string::npos ? 0 : slash + 1);
    std::vector<uint32_t> dependencies;
    for (const auto &needed : library.needed) {
      for (size_t j = 1; j < object.i386_libraries.size(); ++j) {
        if (object.i386_libraries[j].name == needed) {
          dependencies.push_back(j);
          break;
        }
      }
    }
    std::vector<llvm::Constant *> symbols;
    symbols.reserve(library.exports.size() + library.tls_exports.size());
    auto ordinary = library.exports.begin();
    auto tls = library.tls_exports.begin();
    while (ordinary != library.exports.end() || tls != library.tls_exports.end()) {
      const bool is_tls = tls != library.tls_exports.end() &&
          (ordinary == library.exports.end() || tls->first < ordinary->first);
      const auto &symbol = is_tls ? *tls++ : *ordinary++;
      symbols.push_back(llvm::ConstantStruct::get(symbol_type,
          {string(prefix + "_symbol_" + std::to_string(symbols.size()), symbol.first),
           llvm::ConstantInt::get(word, symbol.second),
           llvm::ConstantInt::get(word, is_tls ? i + 1 : 0)}));
    }
    auto *symbol_data = llvm::ConstantArray::get(
        llvm::ArrayType::get(symbol_type, symbols.size()), symbols);
    libraries.push_back(llvm::ConstantStruct::get(library_type, {
        string(prefix + "_path", library.path), string(prefix + "_name", library.name),
        string(prefix + "_filename", filename),
        functions(prefix + "_dependencies", dependencies, UINT32_MAX),
        emit(prefix + "_symbols", symbol_data), llvm::ConstantInt::get(word, symbols.size()),
        functions(prefix + "_initializers", library.initializers, 0),
        functions(prefix + "_finalizers", library.finalizers, 0),
        llvm::ConstantInt::get(word, library.image_begin),
        llvm::ConstantInt::get(word, library.image_end),
        emit(prefix + "_tls_template", llvm::ConstantDataArray::get(context, library.tls_template)),
        llvm::ConstantInt::get(word, library.tls_template.size()),
        llvm::ConstantInt::get(word, library.tls_size),
        llvm::ConstantInt::get(word, library.tls_alignment),
        llvm::ConstantInt::get(word, library.tls_distance)}));
  }
  emit("_ecv_i386_libraries", llvm::ConstantArray::get(
      llvm::ArrayType::get(library_type, libraries.size()), libraries));
  emit("_ecv_i386_library_count", llvm::ConstantInt::get(word, libraries.size()));
  emit("_ecv_i386_tls_static_size", llvm::ConstantInt::get(word, object.tls_static_size));
  emit("_ecv_i386_tls_static_alignment", llvm::ConstantInt::get(word, object.tls_static_alignment));
}

namespace {

void PrintLiftingPhase(std::string_view phase, std::size_t completed = 0,
                       std::size_t total = 0) {
  if (!FLAGS_lift_progress) return;
  std::cout << "lifting - " << phase;
  if (total) std::cout << " [" << completed << "/" << total << "]";
  std::cout << std::endl;
}

void PrintBitcodeBytes(std::uint64_t bytes) {
  std::cout << "lifting - writing bitcode [" << bytes << " bytes]" << std::endl;
}

void StoreBitcode(llvm::Module &module, std::string_view path) {
  remill::StoreModuleToFile(&module, path, false,
                           FLAGS_lift_progress ? PrintBitcodeBytes : nullptr);
}

std::uint64_t ModuleIRDefinitionSize(const llvm::Module &module) {
  std::uint64_t size = 0;
  for (const auto &global : module.global_values()) {
    if (global.isDeclaration()) continue;
    std::string text;
    llvm::raw_string_ostream stream(text);
    global.print(stream);
    stream.flush();
    size += text.size();
  }
  return size;
}

std::filesystem::path IRChunkPath(const std::filesystem::path &base,
                                  unsigned index) {
  return base.parent_path() /
         (base.stem().string() + ".chunk-" + std::to_string(index) +
          base.extension().string());
}

std::string EscapeTSV(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '\\': escaped += "\\\\"; break;
      case '\t': escaped += "\\t"; break;
      case '\r': escaped += "\\r"; break;
      case '\n': escaped += "\\n"; break;
      default: escaped += character; break;
    }
  }
  return escaped;
}

std::uint64_t FunctionIRDefinitionSize(const llvm::Function &function) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  function.print(stream);
  stream.flush();
  return text.size();
}

void WriteLiftProfileRecord(std::ofstream &profile, std::uint64_t address,
                            std::string_view name, const llvm::Function &function,
                            std::uint64_t input_bytes, std::uint64_t lift_us) {
  std::uint64_t instruction_count = 0;
  for (const auto &block : function) {
    instruction_count += block.size();
  }
  profile << "0x" << std::hex << address << std::dec << '\t'
          << EscapeTSV(name) << '\t' << input_bytes << '\t' << lift_us << '\t'
          << instruction_count << '\t' << function.size() << '\t'
          << FunctionIRDefinitionSize(function) << '\n';
}

void PrintELFAnalysis(const AArch64TraceManager &manager, bool per_elf) {
  const auto &elf = manager.elf_obj;
  std::map<std::string, std::vector<const DisasmFunc *>> owners;
  for (const auto &[address, function] : manager.disasm_funcs) {
    const auto owner = per_elf ? manager.GetObjectOwner(address) : elf.file_name;
    owners[owner].push_back(&function);
  }

  std::cout << "ELF analysis (no lifting)\n"
            << "File: " << elf.file_name << '\n'
            << "Architecture: " << elf.bin_arch_str << " (" << elf.bits << "-bit)\n"
            << "Entry: 0x" << std::hex << elf.entry << std::dec << '\n'
            << "Stripped: " << (elf.is_stripped ? "yes" : "no") << '\n';
  for (const auto &[owner, functions] : owners) {
    std::vector<const DisasmFunc *> by_size = functions;
    std::sort(by_size.begin(), by_size.end(), [](const auto *left, const auto *right) {
      return left->func_size < right->func_size;
    });
    std::uint64_t function_extent_bytes = 0;
    std::cout << "\nOwner: " << owner << '\n'
              << "Function entries: " << functions.size() << '\n';
    for (const auto *function : functions) {
      function_extent_bytes += function->func_size;
    }
    std::cout << "Function extent bytes (sum; may overlap): "
              << function_extent_bytes << '\n';

    std::uint64_t executable_bytes = 0;
    std::cout << "Executable sections:\n";
    for (const auto &[name, section] : elf.code_sections) {
      if (per_elf && manager.GetObjectOwner(section.vma) != owner) continue;
      executable_bytes += section.size;
      std::cout << "  " << name << "\t0x" << std::hex << section.vma << std::dec
                << "\t" << section.size << " bytes\n";
    }
    std::cout << "Executable section bytes: " << executable_bytes << '\n';
    if (by_size.empty()) continue;
    const auto percentile = [&](std::size_t percent) {
      return by_size[(by_size.size() - 1) * percent / 100]->func_size;
    };
    std::cout << "Function extent bytes: min=" << by_size.front()->func_size
              << " median=" << percentile(50) << " p90=" << percentile(90)
              << " p99=" << percentile(99) << " max=" << by_size.back()->func_size
              << " mean=" << function_extent_bytes / by_size.size() << '\n'
              << "Largest function extents:\n";
    for (std::size_t index = 0; index < std::min<std::size_t>(10, by_size.size()); ++index) {
      const auto *function = by_size[by_size.size() - 1 - index];
      std::cout << "  0x" << std::hex << function->vma << std::dec << "\t"
                << function->func_size << " bytes\t" << function->func_name << '\n';
    }
  }
}

void StoreIRChunks(llvm::Module &module) {
  std::size_t function_count = 0;
  for (const auto &function : module) {
    if (!function.isDeclaration()) ++function_count;
  }
  if (!function_count) {
    elfconv_runtime_error("Cannot split an LLVM module with no function definitions.\n");
  }

  const auto output_count = static_cast<unsigned>(
      std::min<std::uint64_t>(FLAGS_max_ir_chunks_per_elf, function_count));
  // Split directly into final chunks. Intermediate partitions multiply
  // CloneModule's module-wide work and require another linking pass.
  const std::filesystem::path output_base(FLAGS_bc_out);
  const std::filesystem::path manifest_path(FLAGS_ir_chunk_manifest_out);
  if (!manifest_path.parent_path().empty()) {
    std::filesystem::create_directories(manifest_path.parent_path());
  }
  std::ofstream manifest(manifest_path, std::ios::trunc);
  if (!manifest) {
    elfconv_runtime_error("Cannot write IR chunk manifest: %s\n",
                          FLAGS_ir_chunk_manifest_out.c_str());
  }
  manifest << FLAGS_max_ir_chunks_per_elf << '\t' << output_count << '\n';
  if (output_count == 1) {
    StoreBitcode(module, IRChunkPath(output_base, 0).string());
    manifest << 0 << '\t' << ModuleIRDefinitionSize(module) << '\n';
    return;
  }
  unsigned written = 0;
  PrintLiftingPhase("partitioning IR", 0, output_count);
  llvm::SplitModule(module, output_count,
      [&](std::unique_ptr<llvm::Module> chunk) {
        const auto chunk_path = IRChunkPath(output_base, written);
        StoreBitcode(*chunk, chunk_path.string());
        const auto size = ModuleIRDefinitionSize(*chunk);
        manifest << written << '\t' << size << '\n';
        std::cout << "IR chunk " << written << ": " << size
                  << " definition bytes\n";
        PrintLiftingPhase("partitioning IR", ++written, output_count);
      }, false, true);
}

}  // namespace

int main(int argc, char *argv[]) {
  // set custom signal handler for SIGSEGV.
  lift_set_sigaction();
  bool eflags_elision = true;
  int remaining = 1;
  bool options = true;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (options && argument == "-feflags-elision") {
      eflags_elision = true;
    } else if (options && argument == "-fno-eflags-elision") {
      eflags_elision = false;
    } else {
      if (argument == "--") options = false;
      argv[remaining++] = argv[i];
    }
  }
  argc = remaining;
  argv[argc] = nullptr;
  google::SetUsageMessage(
      "Lift ELF code to LLVM bitcode.\n"
      "  -f[no-]eflags-elision  Enable/disable arithmetic flag liveness and elision.\n"
      "Enabled by default; the last occurrence wins.");
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  if (FLAGS_max_ir_chunks_per_elf) {
    const bool has_unit_args =
        !FLAGS_unit_owner.empty() && !FLAGS_unit_id.empty();
    if (FLAGS_max_ir_chunks_per_elf > std::numeric_limits<unsigned>::max() ||
        FLAGS_arch != "i386" || FLAGS_target_arch != "emscripten32" ||
        FLAGS_bc_out.empty() || FLAGS_ir_chunk_manifest_out.empty() ||
        (FLAGS_per_elf != has_unit_args) || !FLAGS_linker_map.empty() ||
        !FLAGS_unit_manifest_out.empty() || FLAGS_metadata_only) {
      elfconv_runtime_error(
          "--max_ir_chunks_per_elf requires an i386 Emscripten lift with --bc_out and "
          "--ir_chunk_manifest_out, optionally for a --per_elf unit.\n");
    }
  } else if (!FLAGS_ir_chunk_manifest_out.empty()) {
    elfconv_runtime_error("--ir_chunk_manifest_out requires --max_ir_chunks_per_elf.\n");
  }

  AArch64TraceManager manager(FLAGS_target_elf);
  manager.elf_obj.entry_symbol = FLAGS_entry_symbol;
  if (!FLAGS_shared_libraries.empty()) {
    if (FLAGS_arch != "i386")
      elfconv_runtime_error("--shared_libraries requires --arch i386.\n");
    std::istringstream paths(FLAGS_shared_libraries);
    std::string path;
    while (std::getline(paths, path, ',')) {
      if (path.empty()) elfconv_runtime_error("Empty bundled library path.\n");
      manager.elf_obj.shared_library_paths.push_back(path);
    }
    if (!FLAGS_linker_map.empty() ||
        (!FLAGS_per_elf && (FLAGS_metadata_only || !FLAGS_unit_owner.empty())))
      elfconv_runtime_error("Bundled library units require --per_elf.\n");
  }
  manager.elf_obj.analyze_only = FLAGS_analyze_only;
  manager.SetELFData();
  if (FLAGS_arch == "i386") {
    auto register_callback = [&](uint32_t address) {
      printf("ELF callback 0x%x already registered: %d\n", address,
             int(manager.disasm_funcs.count(address)));
      if (manager.disasm_funcs.count(address)) return;
      auto *section = manager.elf_obj.GetIncludedSection(address);
      if (!section || !(bfd_section_flags(section) & SEC_CODE))
        elfconv_runtime_error("Invalid i386 callback function address: 0x%x.\n", address);
      uint64_t end = bfd_section_vma(section) + bfd_section_size(section);
      const auto next = manager.disasm_funcs.upper_bound(address);
      if (next != manager.disasm_funcs.end() &&
          manager.elf_obj.GetIncludedSection(next->first) == section)
        end = next->first;
      if (end <= address)
        elfconv_runtime_error("Invalid i386 callback function extent: 0x%x.\n", address);
      const auto name = manager.GetUniqueLiftedFuncName("elf_callback", address);
      manager.disasm_funcs.emplace(address, DisasmFunc(name, address, end - address));
    };
    for (const auto address : manager.elf_obj.i386_initializers) register_callback(address);
    for (const auto address : manager.elf_obj.i386_finalizers) register_callback(address);
  }
  if (!FLAGS_linker_map.empty()) manager.LoadLinkerMap(FLAGS_linker_map, FLAGS_object_base);
  if (FLAGS_per_elf) {
    if (FLAGS_arch != "i386" || !FLAGS_linker_map.empty())
      elfconv_runtime_error("--per_elf requires i386 without a linker map.\n");
    manager.LoadELFOwners();
  }
  if (FLAGS_analyze_only) {
    if (!FLAGS_unit_manifest_out.empty() || !FLAGS_unit_owner.empty() ||
        !FLAGS_unit_id.empty() || FLAGS_metadata_only || !FLAGS_bc_out.empty() ||
        FLAGS_max_ir_chunks_per_elf || !FLAGS_lift_profile_out.empty()) {
      elfconv_runtime_error(
          "--analyze_only cannot be combined with lift, unit, or bitcode-output options.\n");
    }
    PrintELFAnalysis(manager, FLAGS_per_elf);
    return 0;
  }

  if (!FLAGS_unit_manifest_out.empty()) {
    if (FLAGS_metadata_fingerprint_out.empty()) {
      elfconv_runtime_error("--unit_manifest_out requires --metadata_fingerprint_out.\n");
    }
    manager.WriteIncrementalManifest(FLAGS_unit_manifest_out, FLAGS_metadata_fingerprint_out);
    return 0;
  }
  manager.target_arch = FLAGS_target_arch;
  const bool unit_mode = !FLAGS_unit_owner.empty();
  if (unit_mode != !FLAGS_unit_id.empty() || (unit_mode && FLAGS_metadata_only)) {
    elfconv_runtime_error("Incremental unit owner/id and metadata-only options are inconsistent.\n");
  }

  llvm::LLVMContext context;
  auto os_name = remill::GetOSName(REMILL_OS);
  auto arch_name = remill::GetArchName(FLAGS_arch);
  if (FLAGS_arch == "i386") {
    arch_name = remill::kArchX86;
  }
  if (manager.elf_obj.bits != (arch_name == remill::kArchX86 ? 32 : 64)) {
    elfconv_runtime_error("ELF class does not match --arch.\n");
  }
  auto arch = remill::Arch::Build(&context, os_name,
                                  arch_name);  // e.g., arch = std::unique_ptr<AArch64Arch>
  auto module = FLAGS_bitcode_path.empty()
                    ? remill::LoadArchSemantics(arch.get())
                    : remill::LoadArchSemantics(arch.get(), {FLAGS_bitcode_path.c_str()});
  llvm::Module external_declarations("incremental-unit-declarations", context);
  if (unit_mode) {
    external_declarations.setDataLayout(module->getDataLayout());
    external_declarations.setTargetTriple(module->getTargetTriple());
    manager.EnableUnitMode(FLAGS_unit_owner, arch.get(), &external_declarations);
  }


  // Set various lifting config.
  auto lift_config = LiftConfig(FLAGS_float_exception == "1",
                                FLAGS_norm_mode == "1" || FLAGS_fork_emulation == "1" ||
                                    arch_name == remill::kArchX86,
                                arch_name, FLAGS_fork_emulation == "1",
                                !manager.elf_obj.is_stripped);
  lift_config.eflags_elision = eflags_elision;
#if defined(PRINT_FUNC_ADDR)
  if (FLAGS_dbg_fun_vma > 0) {
    lift_config.dbg_fun_vma = FLAGS_dbg_fun_vma;
  }
#endif

  remill::EcvReg::target_elf_arch = arch_name;
  remill::IntrinsicTable intrinsics(module.get());
  MainLifter main_lifter(arch.get(), &manager, lift_config);

  if (FLAGS_metadata_only) {
    main_lifter.SetCommonMetaData(lift_config);
    arch->DeclareLiftedFunction(manager.entry_func_lifted_name, module.get());
    main_lifter.SetEntryPoint(manager.entry_func_lifted_name);
  } else if (unit_mode) {
    main_lifter.SetRuntimeManagerClass();
    main_lifter.DeclareHelperFunction();
    main_lifter.SetOptMode(manager.elf_obj.able_vrp_opt, lift_config.norm_mode);
    main_lifter.SetUnitOutputSuffix(FLAGS_unit_id);
  } else {
    main_lifter.SetCommonMetaData(lift_config);
  }
  if (!unit_mode && arch_name == remill::kArchX86) {
    EmitI386Libraries(*module, manager.elf_obj);
    new llvm::GlobalVariable(*module, llvm::Type::getInt32Ty(context), true,
                            llvm::GlobalValue::ExternalLinkage,
                            llvm::ConstantInt::get(llvm::Type::getInt32Ty(context),
                                                  !FLAGS_entry_symbol.empty()),
                            "_ecv_i386_function_entry");
  }

  for (const auto &[address, name] : manager.elf_obj.i386_imports) {
    if (FLAGS_metadata_only ||
        (unit_mode && !manager.IsSelectedUnitAddress(address))) continue;
    auto *wrapper = arch->DeclareLiftedFunction(manager.GetLiftedFuncName(address), module.get());
    auto callee = module->getOrInsertFunction("__ecv_i386_" + name, wrapper->getFunctionType());
    auto *block = llvm::BasicBlock::Create(context, "import", wrapper);
    llvm::IRBuilder<> ir(block);
    std::vector<llvm::Value *> args;
    for (auto &arg : wrapper->args()) args.push_back(&arg);
    ir.CreateCall(callee, args);
    ir.CreateRetVoid();
    manager.SetLiftedTraceDefinition(address, wrapper);
  }
  if (!unit_mode && arch_name == remill::kArchX86) {
    auto emit_functions = [&](const char *name, std::vector<uint32_t> addresses) {
      addresses.push_back(0);
      auto *data = llvm::ConstantDataArray::get(context, addresses);
      new llvm::GlobalVariable(*module, data->getType(), true,
                               llvm::GlobalValue::ExternalLinkage, data, name);
    };
    emit_functions("_ecv_i386_initializers", manager.elf_obj.i386_initializers);
    emit_functions("_ecv_i386_finalizers", manager.elf_obj.i386_finalizers);
  }

  std::unordered_map<uint64_t, const char *> addr_fun_name_map;
  std::set<uint64_t> fin_addrs;
  if (!FLAGS_metadata_only) {
    // Lift every function.

  std::ofstream lift_profile;
  if (!FLAGS_lift_profile_out.empty()) {
    lift_profile.open(FLAGS_lift_profile_out);
    if (!lift_profile) {
      elfconv_runtime_error("Cannot write lift profile: %s\n",
                            FLAGS_lift_profile_out.c_str());
    }
    lift_profile << "address\tfunction\tinput_bytes\tlift_us\tir_instructions\t"
                    "ir_blocks\tir_text_bytes\n";
  }
  std::size_t lifted_functions = 0;
  std::size_t known_functions = 0;
  if (FLAGS_lift_progress) {
    for (const auto &[address, function] : manager.disasm_funcs) {
      if (unit_mode && !manager.IsSelectedUnitAddress(address)) continue;
      ++known_functions;
      if (manager.GetLiftedTraceDefinition(address)) ++lifted_functions;
    }
  }
  auto last_progress = std::chrono::steady_clock::now();
  const auto print_progress = [&](bool force = true) {
    if (!FLAGS_lift_progress ||
        (!force && std::chrono::steady_clock::now() - last_progress <
                       std::chrono::milliseconds(250))) return;
    std::cout << "[" << lifted_functions << "/"
              << known_functions + manager.rest_disasm_funcs.size()
              << "] lifting funcs" << std::endl;
    last_progress = std::chrono::steady_clock::now();
  };
  const auto trace_completed = [&](uint64_t, llvm::Function *) {
    if (!FLAGS_lift_progress) return;
    ++lifted_functions;
    print_progress(false);
  };
  print_progress();
  do {
    manager.rest_disasm_funcs.clear();
    known_functions = 0;
    if (FLAGS_lift_progress) {
      for (const auto &[address, function] : manager.disasm_funcs)
        if (!unit_mode || manager.IsSelectedUnitAddress(address)) ++known_functions;
    }
    for (const auto &[addr, dasm_func] : manager.disasm_funcs) {
      if (fin_addrs.contains(addr)) {
        continue;
      }
      if (unit_mode && !manager.IsSelectedUnitAddress(addr)) continue;
      addr_fun_name_map[addr] = dasm_func.func_name.c_str();
      auto &lifted_fun_name = addr_fun_name_map[addr];
      const auto lift_start = std::chrono::steady_clock::now();
      if (!main_lifter.Lift(dasm_func.vma, lifted_fun_name, trace_completed)) {
        elfconv_runtime_error("[ERROR] Failed to Lift \"%s\"\n", lifted_fun_name);
      }
      const auto lift_us = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - lift_start).count();
      auto lifted_fn = manager.GetLiftedTraceDefinition(dasm_func.vma);
      lifted_fn->setName(lifted_fun_name);
      if (lift_profile) {
        WriteLiftProfileRecord(lift_profile, addr, lifted_fun_name, *lifted_fn,
                               dasm_func.func_size,
                               static_cast<std::uint64_t>(lift_us));
      }
      fin_addrs.insert(addr);
    }
    for (auto &[rest_addr, disasm_func] : manager.rest_disasm_funcs) {
      if (!unit_mode || manager.IsSelectedUnitAddress(rest_addr)) {
        manager.disasm_funcs.insert({rest_addr, disasm_func});
      }
    }
  } while (!manager.rest_disasm_funcs.empty());
  print_progress();
  PrintLiftingPhase("finalizing IR");

  if (arch_name == remill::kArchX86) {
    // Guest longjmp unwinds lifted frames to the runtime invocation boundary.
    for (auto &function : *module) {
      function.removeFnAttr(llvm::Attribute::NoUnwind);
      for (auto &block : function)
        for (auto &instruction : block)
          if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
            call->removeFnAttr(llvm::Attribute::NoUnwind);
    }
  }

  // If we lift with `test_mode`, we should disable `able_vrp_opt`.
  manager.elf_obj.able_vrp_opt = !lift_config.norm_mode;

  // Subsequence process of lifting.
  if (unit_mode) {
    main_lifter.SubseqForIncrementalUnit(addr_fun_name_map);
  } else if (manager.elf_obj.able_vrp_opt) {
    main_lifter.SubseqOfLifting(addr_fun_name_map);
  } else {
    main_lifter.SubseqForNoOptLifting(addr_fun_name_map);
    printf("[\x1b[32mINFO\x1b[0m] detected funcs num: %ld\n", addr_fun_name_map.size());
/* Debug functions */
// Insert `debug_state_machine` to the every instruction.
#if defined(OPT_REAL_REGS_DEBUG)
    for (auto lifted_func : main_lifter.impl->lifted_funcs) {
      auto &entry_bb_start_inst = *lifted_func->getEntryBlock().begin();
      auto debug_state_machine_fun = module->getFunction("debug_state_machine");
      llvm::CallInst::Create(debug_state_machine_fun, {}, "", &entry_bb_start_inst);
    }
#endif
  }
  }

  // Prepare and validate the LLVM Module.
  PrintLiftingPhase("preparing module");
  if (manager.target_arch == "emscripten32" || manager.target_arch == "wasi32") {
    arch->PrepareModule(module.get());
  } else {
    auto host_arch = remill::Arch::Build(&context, os_name, remill::GetArchName(REMILL_ARCH));
    host_arch->PrepareModule(module.get());
  }
  std::set<std::string> unit_exports;
  if (arch_name == remill::kArchX86) {
    PrintLiftingPhase("removing unused IR");
    // Instruction selectors are a lifting-time registry, not runtime roots.
    llvm::removeFromUsedLists(*module, [](llvm::Constant *value) {
      auto *global = llvm::dyn_cast<llvm::GlobalValue>(value->stripPointerCasts());
      return global && (global->getName().starts_with("ISEL_") ||
                        global->getName().starts_with("COND_"));
    });
    llvm::ModulePassManager passes;
    if (unit_mode) {
      for (const auto &[_, function_name] : addr_fun_name_map) {
        unit_exports.insert(function_name);
      }
    }
    passes.addPass(llvm::InternalizePass([&](const llvm::GlobalValue &global) {
      return global.getName().starts_with("_ecv_") || unit_exports.contains(global.getName().str());
    }));
    passes.addPass(llvm::GlobalDCEPass());
    llvm::PassBuilder builder;
    llvm::ModuleAnalysisManager analyses;
    builder.registerModuleAnalyses(analyses);
    passes.run(*module, analyses);
  }
  if (FLAGS_max_ir_chunks_per_elf && unit_mode) {
    for (auto &global : module->global_values()) {
      if (global.isDeclaration()) continue;
      const auto name = global.getName();
      if (name.starts_with("_ecv_") || name.starts_with("llvm.") ||
          unit_exports.contains(name.str())) continue;
      global.setName(name + "." + FLAGS_unit_id);
    }
  }
  if (manager.target_arch == "emscripten32") {
    module->setTargetTriple(llvm::Triple("wasm32-unknown-emscripten"));
    module->setDataLayout(
        "e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-i128:128-f128:64-n32:64-S128-ni:1:10:20");
  } else if (manager.target_arch == "wasi32") {
    module->setTargetTriple(llvm::Triple("wasm32-unknown-wasi"));
    module->setDataLayout(
        "e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-i128:128-n32:64-S128-ni:1:10:20");
  }
  if (FLAGS_max_ir_chunks_per_elf) {
    StoreIRChunks(*module);
  } else {
    StoreBitcode(*module, FLAGS_bc_out);
  }

  PrintLiftingPhase("releasing module");
  return 0;
}

