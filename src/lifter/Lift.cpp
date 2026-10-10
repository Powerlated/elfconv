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
#if defined(__linux__)
#  include <signal.h>
#  include <utils/Util.h>
#  include <utils/elfconv.h>
#endif

#include "Lift.h"
#include "MainLifter.h"
#include "TraceManager.h"

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
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

int main(int argc, char *argv[]) {
  // set custom signal handler for SIGSEGV.
  lift_set_sigaction();
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

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
  manager.SetELFData();
  if (FLAGS_per_elf) {
    if (FLAGS_arch != "i386" || !FLAGS_linker_map.empty())
      elfconv_runtime_error("--per_elf requires i386 without a linker map.\n");
    manager.LoadELFOwners();
  }
  if (!FLAGS_linker_map.empty()) manager.LoadLinkerMap(FLAGS_linker_map, FLAGS_object_base);
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

  // Set wasm32-unknown-wasi and wasm32 data layout if necessary.
  if (manager.target_arch == "wasi32") {
    auto wasm32_dl =
        llvm::DataLayout("e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-n32:64-S128-ni:1:10:20");
    module->setDataLayout(wasm32_dl.getStringRepresentation());
    llvm::Triple wasm32_triple;
    wasm32_triple.setArch(llvm::Triple::wasm32);
    wasm32_triple.setVendor(llvm::Triple::UnknownVendor);
    wasm32_triple.setOS(llvm::Triple::WASI);
    module->setTargetTriple(wasm32_triple.str());
  }

  // Set various lifting config.
  auto lift_config = LiftConfig(FLAGS_float_exception == "1",
                                FLAGS_norm_mode == "1" || FLAGS_fork_emulation == "1" ||
                                    arch_name == remill::kArchX86,
                                arch_name, FLAGS_fork_emulation == "1",
                                !manager.elf_obj.is_stripped);
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

  do {
    manager.rest_disasm_funcs.clear();
    for (const auto &[addr, dasm_func] : manager.disasm_funcs) {
      if (fin_addrs.contains(addr)) {
        continue;
      }
      if (unit_mode && !manager.IsSelectedUnitAddress(addr)) continue;
      addr_fun_name_map[addr] = dasm_func.func_name.c_str();
      auto &lifted_fun_name = addr_fun_name_map[addr];
      if (!main_lifter.Lift(dasm_func.vma, lifted_fun_name)) {
        elfconv_runtime_error("[ERROR] Failed to Lift \"%s\"\n", lifted_fun_name);
      }
      // Set function name
      auto lifted_fn = manager.GetLiftedTraceDefinition(dasm_func.vma);
      lifted_fn->setName(lifted_fun_name);
      fin_addrs.insert(addr);
    }
    for (auto &[rest_addr, disasm_func] : manager.rest_disasm_funcs) {
      if (!unit_mode || manager.IsSelectedUnitAddress(rest_addr)) {
        manager.disasm_funcs.insert({rest_addr, disasm_func});
      }
    }
  } while (!manager.rest_disasm_funcs.empty());

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
  auto host_arch = remill::Arch::Build(&context, os_name, remill::GetArchName(REMILL_ARCH));
  host_arch->PrepareModule(module.get());
  if (arch_name == remill::kArchX86) {
    // Instruction selectors are a lifting-time registry, not runtime roots.
    llvm::removeFromUsedLists(*module, [](llvm::Constant *value) {
      auto *global = llvm::dyn_cast<llvm::GlobalValue>(value->stripPointerCasts());
      return global && (global->getName().startswith("ISEL_") ||
                        global->getName().startswith("COND_"));
    });
    llvm::legacy::PassManager passes;
    std::set<std::string> unit_exports;
    if (unit_mode) {
      for (const auto &[_, function_name] : addr_fun_name_map) {
        unit_exports.insert(function_name);
      }
    }
    passes.add(llvm::createInternalizePass([&](const llvm::GlobalValue &global) {
      return global.getName().startswith("_ecv_") || unit_exports.contains(global.getName().str());
    }));
    passes.add(llvm::createGlobalDCEPass());
    passes.run(*module);
    if (manager.target_arch == "emscripten32") {
      module->setTargetTriple("wasm32-unknown-emscripten");
      module->setDataLayout("e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-n32:64-S128-ni:1:10:20");
    }
  }
  // Make LLVM bitcode file.
  remill::StoreModuleToFile(module.get(), FLAGS_bc_out);

  return 0;
}
