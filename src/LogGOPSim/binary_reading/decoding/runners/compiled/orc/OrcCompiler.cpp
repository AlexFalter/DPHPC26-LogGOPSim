// The loggopsim_orc plugin: builds the decode plan as LLVM IR and compiles it in-process with
// LLVM ORC. It is a separate library so that only runs selecting orc load LLVM; OrcLoader.cpp
// opens it and calls loggopsim_compile_with_orc_v2.
#include "binary_reading/decoding/runners/compiled/CompiledRunner.hpp"

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
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace traceschema {

// The same function as the C compiler's: per step one unaligned load, shift, mask, range check
// and store; returns 0 for records the exact decoder must handle.
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
    const auto load_at = [&](llvm::Type* type, llvm::Value* base, std::uint64_t offset) {
        return builder.CreateAlignedLoad(type, builder.CreateConstInBoundsGEP1_64(byte, base, offset),
                                         llvm::MaybeAlign(1));
    };

    const auto steps = plan.get_steps();
    std::vector<llvm::Value*> offsets;
    for (const auto& [source, destination] : steps) {
        llvm::Value* offset = load_at(word, record, source.first_byte);
        if constexpr (std::endian::native == std::endian::big)
            offset = builder.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, offset);
        offset = builder.CreateLShr(offset, source.shift);
        if (source.spill) {
            auto* high = builder.CreateZExt(load_at(byte, record, source.first_byte + 8), word);
            offset = builder.CreateOr(offset, builder.CreateShl(high, 64 - source.shift));
        }
        offset = builder.CreateAnd(offset, constant(source.mask));
        offsets.push_back(offset);
        llvm::Value* value = builder.CreateAdd(offset, constant(source.base));
        if (source.maximum_offset != source.mask) {
            auto* in_range = builder.CreateICmpULE(offset, constant(source.maximum_offset));
            if (source.wildcard) {
                auto* any = builder.CreateICmpEQ(offset, constant(source.maximum_offset + 1));
                require(builder.CreateOr(in_range, any));
                value = builder.CreateSelect(in_range, value, constant(destination.wildcard_value));
            } else {
                require(in_range);
            }
        }
        auto* target = builder.CreateAlignedLoad(
            pointer, builder.CreateConstInBoundsGEP1_64(pointer, targets, std::to_underlying(destination.target)),
            llvm::MaybeAlign(alignof(void*)));
        builder.CreateAlignedStore(builder.CreateTrunc(value, builder.getIntNTy(destination.size * 8)),
                                   builder.CreateConstInBoundsGEP1_64(byte, target, destination.offset),
                                   llvm::MaybeAlign(1));
    }
    const auto type_step = plan.get_type_step();
    const auto peer_step = plan.get_peer_step();
    if (type_step && peer_step && steps[*peer_step].source.wildcard) {
        const auto& type = steps[*type_step].source;
        const auto& peer = steps[*peer_step].source;
        auto* send = builder.CreateICmpEQ(builder.CreateAdd(offsets[*type_step], constant(type.base)),
                                          constant(std::uint64_t(OperationType::Send)));
        auto* any = builder.CreateICmpEQ(offsets[*peer_step], constant(peer.maximum_offset + 1));
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

// Everything the generated function depends on: every step and the send-to-ANY check.
static std::string get_plan_key(const DecodePlan& plan) {
    std::string key;
    const auto append = [&key](std::uint64_t value) { key.append(reinterpret_cast<const char*>(&value), sizeof value); };
    append(plan.get_type_step().value_or(~std::size_t{0}));
    append(plan.get_peer_step().value_or(~std::size_t{0}));
    for (const auto& [source, destination] : plan.get_steps()) {
        for (const auto value : {std::uint64_t{source.first_byte}, std::uint64_t{source.shift}, std::uint64_t{source.spill},
                                 std::uint64_t{source.wildcard}, source.mask, source.base, source.maximum_offset,
                                 std::uint64_t{std::to_underlying(destination.target)}, std::uint64_t{destination.offset},
                                 std::uint64_t{destination.size}, destination.wildcard_value})
            append(value);
    }
    return key;
}

static CompiledCode compile_new(const DecodePlan& plan);

// The reuse cache owns one compiled engine per plan. Uncached engines are instead
// owned by their runners and are released when those runners are destroyed.
// LOGGOPSIM_ORC_REUSE=0 compiles every time, to measure compilation.
static CompiledCode compile(const DecodePlan& plan) {
    static std::mutex mutex;
    static std::unordered_map<std::string, CompiledCode> compiled;
    const auto key = get_plan_key(plan);
    const std::lock_guard lock(mutex);
    const char* const reuse = std::getenv("LOGGOPSIM_ORC_REUSE");
    if (reuse && std::string_view(reuse) == "0") return compile_new(plan);
    if (const auto found = compiled.find(key); found != compiled.end()) return found->second;
    const auto function = compile_new(plan);
    if (function) compiled.emplace(key, function);
    return function;
}

// Called with compile's lock held.
static CompiledCode compile_new(const DecodePlan& plan) {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    });
    auto jit = llvm::orc::LLJITBuilder().create();
    if (!jit) {
        llvm::consumeError(jit.takeError());
        return {};
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
    if (llvm::verifyModule(*module)) return {};
    optimize(*module);
    if (auto error = (*jit)->addIRModule(llvm::orc::ThreadSafeModule(std::move(module), std::move(context)))) {
        llvm::consumeError(std::move(error));
        return {};
    }
    auto symbol = (*jit)->lookup("decode");
    if (!symbol) {
        llvm::consumeError(symbol.takeError());
        return {};
    }
    const auto decode = symbol->toPtr<CompiledRunner::Function>();
    return {decode, std::shared_ptr<llvm::orc::LLJIT>(std::move(*jit))};
}

} // namespace traceschema

// Versioned plugin entry point: the caller receives ownership as well as the address.
extern "C" int loggopsim_compile_with_orc_v2(const traceschema::DecodePlan* plan,
                                            traceschema::CompiledCode* result) {
    *result = traceschema::compile(*plan);
    return static_cast<bool>(*result);
}
