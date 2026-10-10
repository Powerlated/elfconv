/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "remill/BC/Optimizer.h"

#include <glog/logging.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>

namespace remill {

void OptimizeModule(const remill::Arch *, llvm::Module *module,
                    std::function<llvm::Function *(void)> generator, OptimizationGuide guide) {
  if (guide.verify_input) CHECK(!llvm::verifyModule(*module, &llvm::errs()));

  llvm::PipelineTuningOptions tuning;
  tuning.SLPVectorization = guide.slp_vectorize;
  tuning.LoopVectorization = guide.loop_vectorize;
  tuning.LoopUnrolling = true;
  tuning.MergeFunctions = false;
  tuning.InlinerThreshold = 250;
  llvm::PassBuilder builder(nullptr, tuning);
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager calls;
  llvm::ModuleAnalysisManager modules;

  llvm::TargetLibraryInfoImpl libraries(module->getTargetTriple());
  libraries.disableAllFunctions();  // Lifted imports are not native libc calls.
  functions.registerPass([&] { return llvm::TargetLibraryAnalysis(libraries); });
  builder.registerModuleAnalyses(modules);
  builder.registerCGSCCAnalyses(calls);
  builder.registerFunctionAnalyses(functions);
  builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, calls, modules);

  auto function_passes = builder.buildFunctionSimplificationPipeline(
      llvm::OptimizationLevel::O3, llvm::ThinOrFullLTOPhase::None);
  while (auto *function = generator()) {
    if (!function->isDeclaration()) function_passes.run(*function, functions);
  }
  auto module_passes = builder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
  module_passes.run(*module, modules);
  if (guide.verify_output) CHECK(!llvm::verifyModule(*module, &llvm::errs()));
}

void OptimizeBareModule(llvm::Module *module, OptimizationGuide guide) {
  auto function = module->begin();
  OptimizeModule(nullptr, module, [&]() -> llvm::Function * {
    return function == module->end() ? nullptr : &*function++;
  }, guide);
}

}  // namespace remill
