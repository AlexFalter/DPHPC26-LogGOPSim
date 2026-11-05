#include "binary_reading/OrcDecoder.hpp"

#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>

#include <bit>
#include <memory>
#include <mutex>
#include <vector>

namespace traceschema {

static void build_decoder(llvm::Module& module, const DecodePlan& plan) {
    auto& context = module.getContext();
    llvm::IRBuilder<> builder(context);
    auto* byte = builder.getInt8Ty();
    auto* word = builder.getInt64Ty();
    auto* pointer = llvm::PointerType::get(context, 0);
    auto* function = llvm::Function::Create(
        llvm::FunctionType::get(builder.getInt32Ty(), {pointer, pointer}, false),
        llvm::Function::ExternalLinkage, "decode", module);
    auto* record = function->getArg(0);
    auto* targets = function->getArg(1);
    auto* entry = llvm::BasicBlock::Create(context, "entry", function);
    auto* reject = llvm::BasicBlock::Create(context, "reject", function);
    builder.SetInsertPoint(reject);
    builder.CreateRet(builder.getInt32(0));
    builder.SetInsertPoint(entry);

    const auto constant = [&](std::uint64_t value) { return builder.getInt64(value); };
    const auto require = [&](llvm::Value* condition) {
        auto* next = llvm::BasicBlock::Create(context, "", function);
        builder.CreateCondBr(condition, next, reject);
        builder.SetInsertPoint(next);
    };
    const auto load_byte_offset = [&](llvm::Type* type, llvm::Value* base, std::uint64_t offset) {
        return builder.CreateAlignedLoad(type, builder.CreateConstInBoundsGEP1_64(byte, base, offset), llvm::MaybeAlign(1));
    };

    const auto steps = plan.get_steps();
    std::vector<llvm::Value*> codes;
    for (const auto& step : steps) {
        llvm::Value* code = load_byte_offset(word, record, step.byte);
        if constexpr (std::endian::native == std::endian::big)
            code = builder.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, code);
        code = builder.CreateLShr(code, step.shift);
        if (step.spill) {
            auto* high = builder.CreateZExt(load_byte_offset(byte, record, step.byte + 8), word);
            code = builder.CreateOr(code, builder.CreateShl(high, 64 - step.shift));
        }
        code = builder.CreateAnd(code, constant(step.mask));
        codes.push_back(code);
        llvm::Value* value = builder.CreateAdd(code, constant(step.base));
        if (step.maximum != step.mask) {
            auto* in_range = builder.CreateICmpULE(code, constant(step.maximum));
            if (step.wildcard) {
                require(builder.CreateOr(in_range, builder.CreateICmpEQ(code, constant(step.maximum + 1))));
                value = builder.CreateSelect(in_range, value, constant(step.wildcard_bits));
            } else {
                require(in_range);
            }
        }
        auto* target = builder.CreateAlignedLoad(
            pointer, builder.CreateConstInBoundsGEP1_64(pointer, targets, step.target), llvm::MaybeAlign(alignof(void*)));
        builder.CreateAlignedStore(builder.CreateTrunc(value, builder.getIntNTy(step.size * 8)),
                                   builder.CreateConstInBoundsGEP1_64(byte, target, step.offset), llvm::MaybeAlign(1));
    }
    const auto type_step = plan.get_type_step();
    const auto peer_step = plan.get_peer_step();
    if (type_step >= 0 && peer_step >= 0 && steps[peer_step].wildcard) {
        auto* send = builder.CreateICmpEQ(builder.CreateAdd(codes[type_step], constant(steps[type_step].base)),
                                          constant(std::uint64_t(OperationType::Send)));
        auto* any = builder.CreateICmpEQ(codes[peer_step], constant(steps[peer_step].maximum + 1));
        require(builder.CreateNot(builder.CreateAnd(send, any)));
    }
    builder.CreateRet(builder.getInt32(1));
}

static void optimize(llvm::Module& module) {
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager call_graph;
    llvm::ModuleAnalysisManager modules;
    llvm::PassBuilder passes;
    passes.registerModuleAnalyses(modules);
    passes.registerCGSCCAnalyses(call_graph);
    passes.registerFunctionAnalyses(functions);
    passes.registerLoopAnalyses(loops);
    passes.crossRegisterProxies(loops, functions, call_graph, modules);
    passes.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2).run(module, modules);
}

static JitDecodeFunction compile(const DecodePlan& plan) {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    });
    auto jit = llvm::orc::LLJITBuilder().create();
    if (!jit) {
        llvm::consumeError(jit.takeError());
        return nullptr;
    }
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("decoder", *context);
    module->setDataLayout((*jit)->getDataLayout());
#if LLVM_VERSION_MAJOR >= 21
    module->setTargetTriple((*jit)->getTargetTriple());
#else
    module->setTargetTriple((*jit)->getTargetTriple().str());
#endif
    build_decoder(*module, plan);
    if (llvm::verifyModule(*module)) return nullptr;
    optimize(*module);
    if (auto error = (*jit)->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
        llvm::consumeError(std::move(error));
        return nullptr;
    }
    auto symbol = (*jit)->lookup("decode");
    if (!symbol) {
        llvm::consumeError(symbol.takeError());
        return nullptr;
    }
    const auto decode = symbol->toPtr<JitDecodeFunction>();
    // The engine owns the compiled code and must outlive the function pointer; same lifetime as a dlopen handle.
    static std::mutex mutex;
    static std::vector<std::unique_ptr<llvm::orc::LLJIT>> engines;
    const std::lock_guard lock(mutex);
    engines.push_back(std::move(*jit));
    return decode;
}

} // namespace traceschema

extern "C" traceschema::JitDecodeFunction loggopsim_compile_orc_decoder(const traceschema::DecodePlan& plan) {
    return traceschema::compile(plan);
}
