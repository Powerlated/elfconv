#include <gtest/gtest.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <remill/BC/Optimizer.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>
#include <remill/BC/InstructionLifter.h>
#include <remill/BC/Util.h>
#include <remill/BC/IntrinsicTable.h>

remill::ArchName remill::EcvReg::target_elf_arch;

TEST(LlvmMigration, CrossContextGlobalAddressPreservesOffsetAndGepBounds) {
  llvm::LLVMContext source_context, destination_context;
  llvm::Module source("source", source_context), destination("destination", destination_context);
  destination.setDataLayout("e-p:64:64-i64:64");
  auto *array = llvm::ArrayType::get(llvm::Type::getInt32Ty(source_context), 8);
  auto *global = new llvm::GlobalVariable(source, array, false,
      llvm::GlobalValue::ExternalLinkage, llvm::ConstantAggregateZero::get(array), "data");
  llvm::Constant *indices[] = {
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(source_context), 0),
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(source_context), 3)};
  const llvm::ConstantRange range(llvm::APInt(64, 0), llvm::APInt(64, 32));
  auto *address = llvm::ConstantExpr::getGetElementPtr(
      array, global, indices, llvm::GEPNoWrapFlags::inBounds(), range);
  auto *source_type = llvm::FunctionType::get(llvm::PointerType::get(source_context, 0), false);
  auto *function = llvm::Function::Create(source_type, llvm::GlobalValue::ExternalLinkage,
                                         "address", source);
  llvm::ReturnInst::Create(source_context, address,
                           llvm::BasicBlock::Create(source_context, "entry", function));
  auto *destination_type = llvm::FunctionType::get(
      llvm::PointerType::get(destination_context, 0), false);
  auto *cloned = llvm::Function::Create(destination_type, llvm::GlobalValue::ExternalLinkage,
                                       "address", destination);
  remill::ValueMap values;
  remill::TypeMap types;
  remill::MDMap metadata;
  remill::CloneFunctionInto(function, cloned, values, types, metadata);

  ASSERT_FALSE(llvm::verifyModule(destination));
  auto *result = llvm::cast<llvm::ReturnInst>(cloned->getEntryBlock().getTerminator())->getReturnValue();
  auto *gep = llvm::dyn_cast<llvm::GEPOperator>(result);
  ASSERT_NE(gep, nullptr);
  EXPECT_EQ(&gep->getSourceElementType()->getContext(), &destination_context);
  EXPECT_EQ(llvm::cast<llvm::GlobalVariable>(gep->getPointerOperand())->getParent(), &destination);
  EXPECT_TRUE(gep->isInBounds());
  ASSERT_TRUE(gep->getInRange().has_value());
  EXPECT_EQ(*gep->getInRange(), range);
  llvm::APInt offset(64, 0);
  ASSERT_TRUE(gep->accumulateConstantOffset(destination.getDataLayout(), offset));
  EXPECT_EQ(offset.getSExtValue(), 12);
}

namespace {
uint64_t GuestStrlen(const char *text) {
  uint64_t length = 0;
  while (text[length]) {
    ++length;
  }
  return length + 100;
}
}  // namespace

TEST(LlvmMigration, OptimizerPreservesGuestLibcCallSemantics) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::LLVMContext context;
  auto module = std::make_unique<llvm::Module>("guest_libc", context);
  llvm::IRBuilder<> builder(context);
  auto *integer = builder.getInt64Ty();
  auto *libc_type = llvm::FunctionType::get(integer, {builder.getPtrTy()}, false);
  auto *libc = llvm::Function::Create(libc_type, llvm::GlobalValue::ExternalLinkage,
                                     "strlen", *module);
  auto *entry = llvm::Function::Create(llvm::FunctionType::get(integer, false),
      llvm::GlobalValue::ExternalLinkage, "probe", *module);
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", entry));
  builder.CreateRet(builder.CreateCall(libc, {builder.CreateGlobalString("abc")}));
  remill::OptimizeBareModule(module.get(), {.verify_input = true, .verify_output = true});
  libc = module->getFunction("strlen");
  ASSERT_NE(libc, nullptr);
  std::string error;
  std::unique_ptr<llvm::ExecutionEngine> engine(
      llvm::EngineBuilder(std::move(module)).setEngineKind(llvm::EngineKind::JIT)
          .setErrorStr(&error).create());
  ASSERT_NE(engine, nullptr) << error;
  engine->addGlobalMapping(libc, reinterpret_cast<void *>(&GuestStrlen));
  auto probe = reinterpret_cast<uint64_t (*)()>(engine->getFunctionAddress("probe"));
  ASSERT_NE(probe, nullptr);
  EXPECT_EQ(probe(), 103);
}

#ifdef ELFCONV_TEST_SEMANTICS
TEST(LlvmMigration, TailCallTerminatesEmptyAndNonemptyBlocks) {
  llvm::LLVMContext context;
  auto module = remill::LoadModuleFromFile(&context, ELFCONV_TEST_SEMANTICS);
  ASSERT_NE(module, nullptr);
  remill::IntrinsicTable intrinsics(module.get());
  for (bool nonempty : {false, true}) {
    auto *function = llvm::Function::Create(intrinsics.lifted_function_type,
        llvm::GlobalValue::ExternalLinkage, nonempty ? "nonempty" : "empty", *module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    if (nonempty) {
      llvm::IRBuilder<> builder(entry);
      builder.CreateStore(builder.getInt8(7), function->getArg(remill::kStatePointerArgNum));
    }
    auto *call = remill::AddTerminatingTailCall(
        entry, intrinsics.function_return, intrinsics, 0,
        function->getArg(remill::kPCArgNum));
    ASSERT_FALSE(llvm::verifyFunction(*function));
    EXPECT_EQ(call->getCalledFunction(), intrinsics.function_return);
    EXPECT_TRUE(call->isTailCall());
    EXPECT_TRUE(llvm::isa<llvm::ReturnInst>(entry->getTerminator()));
  }
}

TEST(LlvmMigration, TailCallRejectsExistingControlFlow) {
  llvm::LLVMContext context;
  auto module = remill::LoadModuleFromFile(&context, ELFCONV_TEST_SEMANTICS);
  ASSERT_NE(module, nullptr);
  remill::IntrinsicTable intrinsics(module.get());
  auto *function = llvm::Function::Create(intrinsics.lifted_function_type,
      llvm::GlobalValue::ExternalLinkage, "terminated", *module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  llvm::ReturnInst::Create(context, entry);
  EXPECT_DEATH(
      remill::AddTerminatingTailCall(entry, intrinsics.function_return, intrinsics, 0,
                                    function->getArg(remill::kPCArgNum)),
      "Cannot append a tail call to a terminated block");
  EXPECT_FALSE(llvm::verifyFunction(*function));
}
#endif
