// This file is a part of Julia. License is MIT: https://julialang.org/license

#include "llvm-version.h"
#include "platform.h"
#include "llvm/IR/Mangler.h"
#include <llvm/ADT/StringMap.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#if JL_LLVM_VERSION >= 130000
#include <llvm/ExecutionEngine/Orc/ExecutorProcessControl.h>
#endif
#include "llvm/ExecutionEngine/JITLink/EHFrameSupport.h"
#include "llvm/Support/MemoryBuffer.h"
#include <fstream>
#include <llvm/IR/LegacyPassManagers.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
using namespace llvm;
using namespace llvm::orc;
using namespace llvm::jitlink;
#include "StaticJIT.h"
#include "julia.h"
#include "julia_assert.h"
#include "julia_internal.h"
#include <functional>
#include <iostream>
#include <string>
#include <mutex>
#include "llvm/Support/Timer.h"

StaticJuliaJIT *jl_StaticJuliaJIT = nullptr;
extern "C" JL_DLLEXPORT void *(*jl_staticjit_get_cache)(jl_method_instance_t *, size_t);
extern "C" jl_method_instance_t *jl_method_lookup_internal(jl_value_t *f, jl_value_t **args,
                                                           size_t nargs, size_t world);
extern "C" JL_DLLEXPORT jl_value_t *jl_apply_generic_jit(jl_value_t *f, jl_value_t **args,
                                                         size_t nargs)
{
    jl_value_t *result = NULL;
    int useCache = 0;
    // nargs is the length of args, not including function
    // jl_method_lookup_internal includes function
    if (jl_staticjit_get_cache != NULL) {
        jl_method_instance_t *mi =
            jl_method_lookup_internal(f, args, nargs + 1, jl_world_counter);
        if (mi != NULL) {
            if (mi->def.method->module != jl_core_module) {
                void *callptr = (*jl_staticjit_get_cache)(mi, jl_world_counter);
                jl_value_t *(*jl_callptr)(jl_value_t *, jl_value_t **, size_t) =
                    (jl_value_t * (*)(jl_value_t *, jl_value_t **, size_t)) callptr;
                if (callptr != NULL) {
                    result = jl_callptr(f, args, nargs);
                    useCache = 1;
                }
            }
        }
    }
    if (!useCache) {
        result = jl_apply_generic(f, args, nargs);
    }
    else {
        assert(result != NULL);
    }
    return result;
}

extern TargetMachine *jl_TargetMachine;
// Reuse optimizer from jitlayer.cpp
extern void addTargetPasses(legacy::PassManagerBase *PM, TargetMachine *TM);
extern void addMachinePasses(legacy::PassManagerBase *PM, TargetMachine *TM);
extern void addOptimizationPasses(legacy::PassManagerBase *PM, int opt_level,
                                  bool lower_intrinsics, bool dump_native);
StaticJuliaJIT::StaticJuliaJIT()
  : obj_Buffer(),
    obj_OS(obj_Buffer),
    PM(),
    ES(),
    #if UseJITLink
    Memgr(),
    ObjectLayer(this, ES, Memgr),
    #else
    ObjectLayer(ES, []() { return std::make_unique<SectionMemoryManager>(); }),
    #endif
    // deal with external name of julia.XXX.XXX
    // collect all the symbols of added object file
    JuliaFuncJD(ES.createBareJITDylib("JuliaFuncJD")),
    JuliaRuntimeJD(ES.createBareJITDylib("JuliaRuntime")),
    JuliaInvalidatedJD(ES.createBareJITDylib("JuliaInvalidatedJD")),
    JuliaFallbackJD(ES.createBareJITDylib("JuliaFallbackJD")),
    constpool(),
    MethodBlacklist(nullptr),
    inCompiling(false),
    cacheGraph(this)
{
    initConstPool();
    // since we use ObjectLinkerLayer, which is a static linker, so we doesn't support
    // relocation! so jl_TargetMachine is just fine in this case?
    Triple TheTriple = Triple(jl_TargetMachine->getTargetTriple());
    TM = jl_TargetMachine;
    addTargetPasses(&PM, TM);
    addOptimizationPasses(&PM, jl_options.opt_level);
    addMachinePasses(&PM, TM);
    if (TM->addPassesToEmitFile(PM, obj_OS, nullptr, CGFT_ObjectFile, false)) {
        jl_error("Unable to emit file");
    }
    // std::unique_ptr<EHFrameRegistrar> reg();
    // How to debug object layer? Older LLVM doesn't support object layer debugger...
    // GDB has a JIT interface, see progree in https://reviews.llvm.org/D97335/
    // So we need to dispatch the object linking layer to it
    // ObjectLayer.addPlugin(std::make_unique<EHFrameRegistrationPlugin>(ES,
    // std::make_unique<InProcessEHFrameRegistrar>()));
    /* ELF and Mach uses different debug plugin, they will be merged in near feuture, but
    currently we still need to use different one DebugObjectManagerPlugin
    ObjectLayer.addPlugin()
    */
    auto DL = TM->createDataLayout();
    auto generatorRef = std::move(std::make_unique<JuliaRuntimeSymbolGenerator>(this));
    generator = generatorRef.get();
    JuliaRuntimeJD.addGenerator(std::move(generatorRef));
    JITDylibSearchOrder emptyorder1;
    JuliaRuntimeJD.setLinkOrder(emptyorder1, true);
    JITDylibSearchOrder emptyorder2;
    JuliaFallbackJD.setLinkOrder(emptyorder2, true);
    JITDylibSearchOrder emptyorder3 = {
        {&JuliaFuncJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly},
        {&JuliaRuntimeJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly},
        {&JuliaInvalidatedJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly},
        {&JuliaFallbackJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly}};
    JuliaInvalidatedJD.addGenerator(
        std::make_unique<JuliaInvalidatedFunctionGenerator>(this));
    JuliaInvalidatedJD.setLinkOrder(emptyorder3, true);
    std::string ErrorStr;
    if (sys::DynamicLibrary::LoadLibraryPermanently(nullptr, &ErrorStr)) {
        report_fatal_error("FATAL: unable to dlopen self\n" + ErrorStr);
    }
    JuliaFallbackJD.addGenerator(cantFail(
        orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(DL.getGlobalPrefix())));
    JITDylibSearchOrder linkorder = {
        {&JuliaFuncJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly},
        {&JuliaRuntimeJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly},
        {&JuliaInvalidatedJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly},
        {&JuliaFallbackJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly}};
    JuliaFuncJD.setLinkOrder(linkorder, false);
}

std::map<uint64_t, std::string> has_emitted;
Error StaticJuliaJIT::addJITObject(JITMethodInstanceNode *jitNode, JITDylib &lib)
{
    /*
    Currenly unused, the m is miscalculated anyway
    jl_module_t *m = jl_main_module;
    if (!jitNode->isToplevel){
        if (mi->def.method != nullptr) {
            m = mi->def.method->module;
        }
    }
    */
    // Implicitly merge of symbol table, which might be bad
    generator->registerDependencies(/* m ,*/jitNode->symbolTable);
    return addObjectInternal(ES, ObjectLayer, lib,
                             std::unique_ptr<JuliaObjectFile>(new JuliaObjectFile(
                                 nullptr, /* dep, */ std::move(jitNode->buffer))));
};

Error StaticJuliaJIT::addCachedObject(CachedMethodInstanceNode *cacheNode, JITDylib &lib)
{
    assert(!cacheNode->hasEmitted());
    cacheNode->setEmitted();
    generator->registerDependencies(/* m ,*/ std::move(cacheNode->symbolTable));
    return addObjectInternal(ES, ObjectLayer, lib,
                             // Currently JL_Module unused, should be fine
                             std::unique_ptr<JuliaObjectFile>(new JuliaObjectFile(
                                 nullptr, /* dep, */ std::move(cacheNode->buffer))));
};

// This function is part of cache implementation, don't invoke this function arbitarily
llvm::Expected<void *> StaticJuliaJIT::tryLookupSymbol(std::string &name)
{
    SymbolStringPtr namerefptr = ES.intern(StringRef(name));
    auto symOrErr = ES.lookup({&JuliaFuncJD}, namerefptr);
    auto Err = symOrErr.takeError();
    // In our new implementation, err can only happen when symbol resolution has error
    // That is, maybe our symbol table is error.
    // Let's ignore this now (TODO : make error more robust)
    // The essential problem here is that we don't know how LLVM handle symbol resolution
    // error.
    if (!Err) {
        auto addr = symOrErr.get().getAddress();
        has_emitted[addr] = name;
        return (void *)addr;
    }
    else {
        cantFail(JuliaFuncJD.remove({namerefptr}));
        return Err;
    }
}

// This function is used to debug things
void* StaticJuliaJIT::probeDebugSymbol(std::string &name)
{
    SymbolStringPtr namerefptr = ES.intern(StringRef(name));
    auto symOrErr = ES.lookup({&JuliaFuncJD}, namerefptr);
    auto Err = symOrErr.takeError();
    // In our new implementation, err can only happen when symbol resolution has error
    // That is, maybe our symbol table is error.
    // Let's ignore this now (TODO : make error more robust)
    // The essential problem here is that we don't know how LLVM handle symbol resolution
    // error.
    if (!Err) {
        auto addr = symOrErr.get().getAddress();
        has_emitted[addr] = name;
        return (void *)addr;
    }
    else {
        return nullptr;
    }
}

llvm::Expected<void *> StaticJuliaJIT::getFunctionPointerInternal(jl_method_instance_t *mi,
                                                                  size_t world,
                                                                  GeneratedPointerType pty)
{
    std::string fname;
    if (pty == InvokePointer) {
        fname = "julia::method::invoke::";
    }
    else {
        fname = "julia::method::specfunc::";
    }
    llvm::raw_string_ostream(fname) << name_from_method_instance(mi);
    // TODO : test whether cache is emitted! we need to emit cache!!!!!
    MethodInstanceNode *miNode = cacheGraph.lookUpNode(mi);
    if (miNode != nullptr) {
        if (auto cacheNode = dyn_cast<CachedMethodInstanceNode>(miNode)) {
            if (!cacheNode->hasEmitted()){
                cacheGraph.emitCachedMethodInstanceNode(cacheNode);
            }
        }
        // must be compiled at this point
        return std::move(tryLookupSymbol(fname));
    }
    else {
        compileMethodInstanceCached(mi, world);
        // TODO : be careful of this function, actually it's better to remove it after
        // looking up but currently we already emit object file after
        // compileMethodInstanceCached, so not a big deal.
        cacheGraph.detachIfToplevelMethodInstance(mi);
        return std::move(tryLookupSymbol(fname));
    }
}


void StaticJuliaJIT::removeUnusedExternal(llvm::Module *M)
{
    for (auto I = M->global_objects().begin(), E = M->global_objects().end(); I != E;) {
        GlobalObject *F = &*I;
        ++I;
        if (F->isDeclaration()) {
            if (F->use_empty()) {
                F->eraseFromParent();
            }
        }
    }
}

/*
    Find the unique cached code instance for a method instance. If there are more than one,
   raise a warning. This is to prevent bad cache and we only use method instance for codegen
   instead of code instance. So a one-to-one corresponding is importance here. We must be
   careful about Julia's GC and ensure all the data are correctly rooted.
*/
static jl_code_instance_t *jl_unique_rettype_inferred(jl_method_instance_t *mi,
                                                      size_t min_world, size_t max_world)
{
    jl_code_instance_t *ci = NULL;
    jl_code_instance_t *codeinst = jl_atomic_load_relaxed(&mi->cache);
    while (codeinst) {
        if (codeinst->min_world <= min_world && max_world <= codeinst->max_world) {
            jl_value_t *code = codeinst->inferred;
            if (code && (code == jl_nothing || jl_ir_flag_inferred((jl_array_t *)code))) {
                // if we already find a code instance. Raise a warning
                if (ci != NULL) {
                    jl_safe_printf(
                        "Warning: More than one matched code instance! (maybe some bootstraped code?)\n");
                    // jl_(ci);
                    // jl_(codeinst);
                }
                ci = codeinst;
            }
        }
        codeinst = jl_atomic_load_relaxed(&codeinst->next);
    }
    return ci;
}

// A convenience helper to prepare code instance for every subroutine
static jl_code_instance_t *prepare_code_instance(jl_method_instance_t *mi, size_t world)
{
    jl_code_info_t *src = NULL;
    JL_GC_PUSH1(&src);
    // Firstly, check whether we have already cached required code instance
    jl_code_instance_t *codeinst = jl_unique_rettype_inferred(mi, world, world);
    if (codeinst) {
        // Return immediately if we have a matched cache.
        JL_GC_POP();
        return codeinst;
    }
    // we have no cached codeinst, create a new one
    // inferred codeinst needs inferred code info to determine return type and other
    // properties
    assert(
        (jl_is_method(mi->def.method) && jl_symbol_name(mi->def.method->name)[0] != '@') ||
        jl_is_module(mi->def.module));
    // now we perform type inference
    // It seems that we need to call type inference multiple time if we don't cache the
    // inferred IR.
    src = jl_type_infer(mi, world, 0);
    // at this point we already get inferred code info
    assert(src && jl_is_code_info(src));
    assert(src->inferred);
    jl_code_instance_t *lookup_codeinst =
        jl_get_method_inferred(mi, src->rettype, src->min_world, src->max_world);
    // type inference should create a new code instance for this method instance
    assert(lookup_codeinst != NULL);
    // mark this field as jl_nothing
    // A special case is the const return function
    // Either jl_type_infer has produced and cached code info in codeinst->inferred
    // Or it's a const return function, which won't cache code info
    if (src->inferred && !lookup_codeinst->inferred) {
        lookup_codeinst->inferred = jl_nothing;
    }
    assert((lookup_codeinst->inferred != NULL) || (lookup_codeinst->rettype_const != NULL));
    JL_GC_POP();
    return lookup_codeinst;
}
extern void clearGlobalRefs();
void StaticJuliaJIT::compileMethodInstancePatched(jl_method_instance_t* mi, CachedMethodInstanceNode* cacheNode, size_t world)
{
    assert(!cacheNode->hasEmitted());
    jl_code_instance_t *ci = prepare_code_instance(mi, world);
    jl_value_t *src = NULL;
    JL_GC_PUSH1(&src);
    src = ci->inferred;
    // Reinfer the function to get code info
    assert(src != NULL);
    if (src == jl_nothing) {
        src = (jl_value_t *)jl_type_infer(ci->def, world, 0);
        assert(src != jl_nothing && jl_is_code_info(src));
    }
    src = (jl_value_t *)jl_uncompress_ir(mi->def.method, ci, (jl_array_t *)src);
    // compile current method instance
    jl_codegen_params_t params;
    // prevent Julia uses cache internally
    params.cache = false;
    params.world = world;
    clearGlobalRefs();
    jl_compile_result_t result =
        emit_function(mi, (jl_code_info_t *)src, ci->rettype, params);
    clearGlobalRefs();
    JL_GC_POP();
    std::unique_ptr<llvm::Module> mod = std::move(std::get<0>(result));
    removeUnusedExternal(mod.get());
    PM.run(*mod);
    removeUnusedExternal(mod.get());
    // now we need to add object buffer
    llvm::SmallVector<char, 0U> emptyBuffer;
    obj_Buffer.swap(emptyBuffer);
    std::unique_ptr<llvm::MemoryBuffer> ObjBuffer(
        new SmallVectorMemoryBuffer(std::move(emptyBuffer)));
    cacheNode->buffer = std::move(ObjBuffer);
    assert(cacheNode->symbolTable);
    cantFail(addCachedObject(cacheNode, JuliaFuncJD));
}

void StaticJuliaJIT::compileMethodInstanceCached(jl_method_instance_t *mi, size_t world,
                                                 JITMethodInstanceNode *parentNode)
{
    // We have to use a parentNode here to ensure the correctness
    // this is because we need to root the children before parent finish compiling
    // Consider this scenario:
    // f -> g and g -> f, we compile f, then we compile g, if we didn't mark g as f's
    // dependency before we call markNoMoreDeps on g we will encounter error

    // Firstly we test whether this method instance is already compiled
    MethodInstanceNode *cacheNode = cacheGraph.lookUpNode(mi);
    if (cacheNode != nullptr) {
        if (parentNode != nullptr) {
            cacheGraph.addChild(parentNode, cacheNode);
        }
        return;
    }
    JITMethodInstanceNode *miNode = cacheGraph.createJITMethodInstanceNode(mi);
    if (parentNode != nullptr) {
        cacheGraph.addChild(parentNode, (MethodInstanceNode *)miNode);
    }
    // Otherwise we prepare to compile the method instance
    jl_code_instance_t *ci = prepare_code_instance(mi, world);
    jl_value_t *src = NULL;
    JL_GC_PUSH1(&src);
    src = ci->inferred;
    // Reinfer the function to get code info
    assert(src != NULL);
    if (src == jl_nothing) {
        src = (jl_value_t *)jl_type_infer(ci->def, world, 0);
        assert(src != jl_nothing && jl_is_code_info(src));
    }
    src = (jl_value_t *)jl_uncompress_ir(mi->def.method, ci, (jl_array_t *)src);
    // compile current method instance
    jl_codegen_params_t params;
    // prevent Julia uses cache internally
    params.cache = false;
    params.world = world;
    clearGlobalRefs();
    jl_compile_result_t result =
        emit_function(mi, (jl_code_info_t *)src, ci->rettype, params);
    clearGlobalRefs();
    JL_GC_POP();
    assert(std::get<0>(result));
    cacheGraph.installCompiledOutput(miNode, std::move(std::get<0>(result)));
    while (!params.workqueue.empty()) {
        jl_code_instance_t *subci = std::get<0>(params.workqueue.back());
        compileMethodInstanceCached(subci->def, world, miNode);
        params.workqueue.pop_back();
    }
    cacheGraph.markNoMoreDeps(miNode);
}

/*
void StaticJuliaJIT::compileMethodInstance(jl_method_instance_t *mi, size_t world)
{
    // Step 1 : Prepare code instance for all the functions we may use during codegen.
    // we forcly infer the entry function, to ensure every subroutine it calls has an
    // associated code instance. This is required by emit_invoke, since it will lookup code
    // instance during codegen (it won't generate code instance automatically). We also
    // require the code instance is inferred. Uninferred toplevel code and macro expand is
    // not handled.
    // To prepare code instance, we firstly lookup cache of jl_method_instance_t, if found
    // then we succeed. Otherwise we need to generate type inferred code info and use
    // information from it to create new code instance.
    jl_code_instance_t *first_codeinst = prepare_code_instance(mi, world);
    std::vector<jl_code_instance_t *> workqueue = {first_codeinst};
    llvm::legacy::PassManager &PM = jl_StaticJuliaJIT->PM;
    std::vector<std::unique_ptr<MemoryBuffer>> temporaryBuffer;
    // Prevent object files being added to JIT repeatly
    UniqueVector<jl_method_instance_t*> hasCompiled;
    while (!workqueue.empty()) {
        jl_code_instance_t *curr_codeinst = workqueue.back();
        workqueue.pop_back();
        jl_method_instance_t *curr_methinst = curr_codeinst->def;
        if (hasCompiled.has(curr_methinst)){
            continue;
        }
        // Step one: use unsafe cache of static jit compiler
        void* ptr1 = probeInvokePointer(curr_methinst, world);
        //miName = "julia::method::specfunc::";
        //llvm::raw_string_ostream(miName) << name_from_method_instance(curr_methinst);
        //void* ptr2 = tryLookupSymbol(miName);
        if (ptr1 != nullptr){
            continue;
        }
        jl_code_instance_t *new_curr_codeinst =
            prepare_code_instance(curr_methinst, world);
        // In some rare cases this will happen for bootstrap code. Raise a warning here.
        if (new_curr_codeinst != curr_codeinst) {
            // jl_(new_curr_codeinst);
            // jl_(curr_codeinst);
            jl_safe_printf("Two different code instance, Shouldn't be uninferred!");
            curr_codeinst = new_curr_codeinst;
        }
        curr_methinst = curr_codeinst->def;
        if (curr_codeinst->inferred == NULL || (jl_value_t *)curr_codeinst == jl_nothing) {
            jl_(curr_codeinst);
            jl_error("Shouldn't be uninferred!");
        }
        jl_value_t *src = NULL;
        JL_GC_PUSH1(&src);
        src = curr_codeinst->inferred;
        // Reinfer the function to get code info
        assert(src != NULL);
        if (src == jl_nothing) {
            src = (jl_value_t *)jl_type_infer(curr_codeinst->def, world, 0);
            assert(src != jl_nothing && jl_is_code_info(src));
        }
        src = (jl_value_t *)jl_uncompress_ir(curr_methinst->def.method, curr_codeinst,
                                             (jl_array_t *)src);

        // compile current method instance
        jl_codegen_params_t params;
        params.cache = false;
        params.world = world;
        // params.params->lookup
        // TODO: whether the method instance already has code instance attached on it???
        // the call_target is produced by the emit_invoke if the code instance is found
        // otherwise a jl_invoke is emitted
        jl_compile_result_t result = emit_function(curr_methinst, (jl_code_info_t *)src,
                                                   curr_codeinst->rettype, params);
        JL_GC_POP();
        // the call target is in params.workqueue, we record the dependency in emitted
        // map;
        while (!params.workqueue.empty()) {
            jl_code_instance_t* subci = std::get<0>(params.workqueue.back());
            if (!hasCompiled.has(subci->def)){
                workqueue.push_back(subci);
            }
            params.workqueue.pop_back();
        }
        std::unique_ptr<llvm::Module> mod = std::move(std::get<0>(result));
        removeUnusedExternal(mod.get());
        (void)writeOutput(mod.get(), curr_methinst, UnoptIR);
        // run pass can trigger undefine symbol error, so we need to emit external
        // symbol correctly this will also emit object file to objOS

        //    We should add a IR verification pass here, since sometimes imagecodegen will
        //produce incorrect IR. But verifyModule doesn't work, it seems that strict check
can
        //only happen if we try to parse LLVM IR (or use a debug version of LLVM).
        //}

        PM.run(*mod);
        removeUnusedExternal(mod.get());
        (void)writeOutput(mod.get(), curr_methinst, OptIR);
        (void)writeOutput(mod.get(), curr_methinst, ObjFile);
        // now we need to add object buffer
        llvm::SmallVector<char, 0U> emptyBuffer;
        obj_Buffer.swap(emptyBuffer);
        temporaryBuffer.emplace_back(new SmallVectorMemoryBuffer(std::move(emptyBuffer)));
        hasCompiled.push_back(curr_methinst);
    }
    for (size_t i = 0;i < temporaryBuffer.size();i++){
        auto ObjBuffer = std::move(temporaryBuffer[i]);
        auto objorerr = object::ObjectFile::createObjectFile(ObjBuffer->getMemBufferRef());
        if (Error Err = objorerr.takeError()) {
            llvm::errs() << Err;
            jl_error("Unable to create object files!");
        }
        std::unique_ptr<llvm::object::ObjectFile> objfile = std::move(objorerr.get());
        (void)addObject(std::move(objfile), std::move(ObjBuffer));

    }
    auto& q = hasCompiled.queue;
    for (auto i = (int)q.size()-1;i >-1;i--){
        auto ptr1 = probeInvokePointer(q[i], world);
        assert(ptr1 != nullptr);
    }
    return;
}
*/
static void verify_root_type(jl_binding_t *b, jl_value_t *ty)
{
    assert(b->constp);
    assert(jl_isa(b->value, ty));
}


// root some constant value
void StaticJuliaJIT::initConstPool()
{
    jl_binding_t *stringpool_binding =
        jl_get_binding(jl_main_module, jl_symbol("StringPool"));
    assert(stringpool_binding != NULL);
    // TODO: verify the string pool type...
    // since we only use global rooted type, there is no need to create GC frame.
    // jl_box_int64(1);
    // verify_root_type(stringpool_binding, (jl_value_t *)jl_array_type);
    constpool[StaticJuliaJIT::ConstantType::String] =
        jl_atomic_load_relaxed(&(stringpool_binding->value));

    jl_binding_t *symbolpool_binding =
        jl_get_binding(jl_main_module, jl_symbol("SymbolPool"));
    assert(symbolpool_binding != NULL);
    // TODO: verify the string pool type...
    // since we only use global rooted type, there is no need to create GC frame.
    // jl_box_int64(1);
    // verify_root_type(symbolpool_binding, (jl_value_t *)jl_array_type);
    constpool[StaticJuliaJIT::ConstantType::Symbol] =
        jl_atomic_load_relaxed(&(symbolpool_binding->value));


    jl_binding_t *bitvalue_pool = jl_get_binding(jl_main_module, jl_symbol("BitValuePool"));
    assert(bitvalue_pool != NULL);
    // TODO: verify the string pool type...
    // since we only use global rooted type, there is no need to create GC frame.
    // jl_box_int64(1);
    // verify_root_type(bitvalue_pool, (jl_value_t *)jl_array_type);
    constpool[StaticJuliaJIT::ConstantType::BitValue] =
        jl_atomic_load_relaxed(&(bitvalue_pool->value));

    jl_binding_t *type_pool = jl_get_binding(jl_main_module, jl_symbol("TypePool"));
    assert(type_pool != NULL);
    // TODO: verify the string pool type...
    // since we only use global rooted type, there is no need to create GC frame.
    // jl_box_int64(1);
    // verify_root_type(type_pool, (jl_value_t *)jl_array_type);
    constpool[StaticJuliaJIT::ConstantType::Type] =
        jl_atomic_load_relaxed(&(type_pool->value));

    jl_binding_t *mbnd = jl_get_binding(jl_main_module, jl_symbol("MethodBlacklist"));
    assert(mbnd != NULL);
    // TODO: verify the string pool type...
    // since we only use global rooted type, there is no need to create GC frame.
    // jl_box_int64(1);
    MethodBlacklist = jl_atomic_load_relaxed(&(mbnd->value));
}

#ifndef HAVE_SSP
extern JL_DLLEXPORT void __stack_chk_fail(void);
#endif

/*
extern "C" JL_DLLEXPORT JITTargetAddress jl_tryLookupSymbol(jl_value_t *symbol)
{
    std::string sname = jl_symbol_name_((jl_sym_t *)symbol);
    return (JITTargetAddress)jl_StaticJuliaJIT->tryLookupSymbol(sname);
}
*/
std::set<jl_method_instance_t *> invalidated_method_instance;
extern void (*invalid_compiled_cache_handle)(jl_method_instance_t *);
void invalidate_cache(jl_method_instance_t *mi)
{
    invalidated_method_instance.insert(mi);
}

extern "C" JL_DLLEXPORT void jl_setoutput_dir(jl_value_t *dir)
{
    assert(jl_is_string(dir));
    std::string root = (char *)jl_string_ptr(dir);
    jl_StaticJuliaJIT->needCacheOutput = true;
    jl_StaticJuliaJIT->opg.setRootPath(root);
}

extern "C" JL_DLLEXPORT void jl_disable_output()
{
    jl_StaticJuliaJIT->needCacheOutput = false;
}

extern "C" JL_DLLEXPORT void jl_init_staticjit(jl_value_t* outDir)
{
    assert(jl_is_string(outDir));
    assert(jl_StaticJuliaJIT == nullptr);
    jl_StaticJuliaJIT = new StaticJuliaJIT();
    jl_setoutput_dir(outDir);
}

void StaticJuliaJIT::addStaticLib(std::string libPath, std::vector<JuliaLibConfig> &configs)
{
    ErrorOr<std::unique_ptr<MemoryBuffer>> FileOrErr =
        MemoryBuffer::getFile(StringRef(libPath));
    if (std::error_code EC = FileOrErr.getError()) {
        jl_error("Unable to open archive file");
    }
    llvm::Error err = Error::success();
    llvm::object::Archive ar(FileOrErr.get().get()->getMemBufferRef(), err);
    assert(!err);
    std::unordered_map<std::string, llvm::object::Archive::Child> objects;
    for (auto &ch : ar.children(err)) {
        if (err){
            assert(0);
        }
        if (auto eName = ch.getName()){
            std::string name = eName.get().str();
            objects.insert({name, ch}); 
        }
        else{
            assert(0);
        }
    }
    assert(!err);
    for (auto &config : configs) {
        // If the object file is a cached one, we check whether the object file is indeed
        // cached in the Graph.
        if (config.isCached) {
            auto miNode = cacheGraph.lookUpNode(config.miName);
            if (miNode == nullptr) {
                llvm::errs() << "Cached object " << config.miName << " doesn't exist\n";
                llvm::errs() << "Do you forget to load it?";
                auto cacheNode = dyn_cast<CachedMethodInstanceNode>(miNode);
                assert(cacheNode);
                assert(cacheNode->hasEmitted());
            }
        }
        // If we need to load an uncached invalidated object file, we should invoke JIT to
        // emit one.
        else if (!config.isRelocatable) {
            if (cacheGraph.lookUpNode(config.miName)){
                // already compiled, do nothing here
                continue;
            }
            std::string newName = jl_name_from_method_instance(config.mi);
            if (config.miName != newName){
                llvm::errs() << "Inconsistent name for : \n";
                llvm::errs() << "Compiled as : " << config.miName << '\n';
                llvm::errs() << "Regenerated as : " << newName << '\n';
                //abort();
            }
            llvm::errs() << "Recompile non relocatable" << config.miName << "\n";
            auto cacheNode = cacheGraph.createPatchedMethodInstanceNode(config.miName, libPath, config.objFileName);
            compileMethodInstancePatched(config.mi, cacheNode, jl_world_counter);
        }
        else {
            // Otherwise we simply load the object file into JIT.
            // If the miNode is already compiled, ignore it
            if (cacheGraph.lookUpNode(config.miName)){
                continue;
            }
            auto iter = objects.find(config.objFileName);
            assert(iter != objects.end());
            auto &ch = iter->second;
            // How to deal with symbol tables ???
            // where should the symbol tables sit!
            auto membufRef = ch.getMemoryBufferRef();
            if (!membufRef){
                assert(0);
            }
            auto membuf =
                llvm::MemoryBuffer::getMemBufferCopy(membufRef->getBuffer());
            auto cacheNode = cacheGraph.createUnemittedCachedMethodInstanceNode(
                config.miName, libPath, iter->first, std::move(config.externalSymbols),
                std::move(membuf));
            cantFail(addCachedObject(cacheNode, JuliaFuncJD));
        }
    }
}

extern void *decodeJuliaValue(jl_module_t *mod, std::string &str);

#define hex2uint8(h) (((h) >= 'A') ? ((h) - 'A' + 10) : (h - '0'))

void decodeInputString(std::string &str)
{
    std::string decodeStr(str.size() / 2, 0);
    assert(str.size() % 2 == 0);
    char *ptr = &decodeStr[0];
    for (size_t i = 0; i < str.size() / 2; i++) {
        // little endianness
        ptr[i] = (hex2uint8(str[2 * i + 1]) << 4) + hex2uint8(str[2 * i]);
    }
    str = decodeStr;
}
#undef hex2uint8

std::vector<JuliaLibConfig> openConfig(std::string path)
{
    std::ifstream fs;
    fs.open(path);
    std::vector<JuliaLibConfig> configs;
    std::string nodeTypeString;
    int nodeType;
    std::string miName;
    while (1) {
        JuliaLibConfig config;
        std::getline(fs, nodeTypeString);
        // try to read a new line to see whether we have more item
        if (fs.eof() || nodeTypeString.size() == 0) {
            break;
        }
        std::getline(fs, miName);
        decodeInputString(miName);
        config.miName = miName;
        assert(nodeTypeString.size() == 1);
        nodeType = nodeTypeString[0] - '0';
        if (nodeType == 0){
            std::string isRelocatableString;
            std::getline(fs, isRelocatableString);
            bool isRelocatable = isRelocatableString[0] == '1';
            config.miName = miName;
            config.isRelocatable = isRelocatable;
            config.isCached = false;
            jl_value_t *mi = nullptr;
            if (!isRelocatable){
                std::string typeString;
                std::getline(fs, typeString);
                jl_value_t **args;
                JL_GC_PUSHARGS(args, 3);
                args[0] = jl_eval_string("BuildSystem.lookUpMethodInstance");
                args[1] = (jl_value_t *)decodeJuliaValue(jl_main_module, typeString);
                args[2] = jl_box_uint64(jl_world_counter);
                mi = jl_apply(args, 3);
                assert(jl_is_method_instance(mi));
                JL_GC_POP();
            }
            config.mi = (jl_method_instance_t*)mi;
            std::string objFileName;
            std::getline(fs, objFileName);
            decodeInputString(objFileName);
            config.objFileName = objFileName;
            std::string pairNumString;
            std::getline(fs, pairNumString);
            size_t pairNum = 0;
            for (auto i = pairNumString.begin(); i!= pairNumString.end(); i++){
                char c = *i;
                pairNum = 10 * pairNum + (c - '0');
            }
            SymbolTable externalSymbols;
            for (size_t i = 0; i < pairNum; i++) {
                std::pair<std::string, std::string> p;
                std::getline(fs, p.first, '\t');
                decodeInputString(p.first);
                std::getline(fs, p.second);
                externalSymbols.insert(p);
            }
            config.externalSymbols = std::move(std::make_unique<SymbolTable>(std::move(externalSymbols)));
            configs.push_back(std::move(config));
        }  
        else if (nodeType == 1){
            std::string libName;
            std::string objName;
            std::getline(fs, libName);
            decodeInputString(libName);
            std::getline(fs, objName);
            decodeInputString(objName);
        }
        /*
        else if (nodeType == 2){
            std::string pluginName;
            std::getline(fs, pluginName);
            decodeInputString(pluginName);
            assert(0);
        }
        */
        else{
            assert(0);
        }
    }
    fs.close();
    return configs;
}

extern "C" JL_DLLEXPORT void jl_add_static_libs(jl_value_t *objectfilearray)
{
    assert(jl_StaticJuliaJIT != nullptr);
    assert(jl_subtype(jl_typeof(objectfilearray), (jl_value_t *)jl_array_type));
    std::vector<std::string> objectfiles;
    for (unsigned int i = 0; i < jl_array_len(objectfilearray); i++) {
        jl_value_t *item = jl_arrayref((jl_array_t *)objectfilearray, i);
        assert(jl_is_tuple(item));
        std::string libPath = jl_string_ptr(jl_fieldref(item, 0));
        std::string configPath = jl_string_ptr(jl_fieldref(item, 1));
        std::vector<JuliaLibConfig> configs = openConfig(configPath);
        // Timer timer;
        // timer.init(StringRef(libPath), StringRef(libPath));
        // timer.startTimer();
        jl_StaticJuliaJIT->addStaticLib(libPath, configs);
        // timer.stopTimer();
    }
}

/*
void loadStaticLib(StaticJuliaJIT* jit, llvm::object::Archive& ar, std::map<std::string,
JuliaLibConfig>& config){ for (auto& Child:ar.children()){ auto childName = Child.getName();
        assert(childName);
        std::string& n = childName->str();
        assert(config.find(n) != config.end());
        JuliaLibConfig childConfig = config[n];
    }
}
*/



std::map<jl_method_instance_t *, uint64_t> miPool;
extern "C" JL_DLLEXPORT void *jl_force_jit(jl_method_instance_t *mi, size_t world)
{
    // toplevel method instance should not be cached
    jl_sigatomic_begin();
    JL_LOCK(&jl_codegen_lock);
    bool isToplevel = !jl_is_method(mi->def.method);
    // We use a simple cache to accelerate function calling, which is of course
    // memory-inefficient. But since most method instance can be compiled, so it would be
    // relatively rare to get into this branch.
    auto iter = miPool.find(mi);
    if (iter != miPool.end()) {
        JL_UNLOCK(&jl_codegen_lock);
        jl_sigatomic_end();
        return (void *)iter->second;
    }
    if (jl_StaticJuliaJIT->inCompiling) {
        // llvm::errs() << "Codegen lock is held by only one task, this should not happen!\n";
        // jl_(mi);
        // abort();
        JL_UNLOCK(&jl_codegen_lock);
        jl_sigatomic_end();
        return nullptr;
    }
    jl_StaticJuliaJIT->inCompiling = true;
    llvm::Expected<void *> ptr = jl_StaticJuliaJIT->getInvokePointer(mi, world);
    jl_StaticJuliaJIT->inCompiling = false;
    if (!ptr){
        llvm::errs() << jl_name_from_method_instance(mi) << "Failed name lookup procedure.";
        exit(1);
    }
    if (!isToplevel) {
        miPool[mi] = (uint64_t)*ptr;
    }
    if (*ptr == NULL){
        abort();
    }
    JL_UNLOCK(&jl_codegen_lock);
    jl_sigatomic_end();
    return *ptr;
}

extern "C" JL_DLLEXPORT void *jl_get_spec_ptr(jl_method_instance_t *mi, size_t world)
{
    return *(jl_StaticJuliaJIT->getSpecPointer(mi, world));
}

extern "C" JL_DLLEXPORT void *jl_get_invoke_ptr(jl_method_instance_t *mi, size_t world)
{
    return *(jl_StaticJuliaJIT->getInvokePointer(mi, world));
}

extern "C" JL_DLLEXPORT void *jl_simple_multijit(jl_method_instance_t *mi, size_t world)
{
    std::string invokeName = "julia::method::invoke::";
    auto iter = miPool.find(mi);
    void *ptr = nullptr;
    jl_array_t *arr = (jl_array_t *)jl_StaticJuliaJIT->MethodBlacklist;
    for (size_t i = 0; i < jl_array_len(arr); i++) {
        if ((jl_value_t *)(mi->def.method) == jl_ptrarrayref(arr, i)) {
            return ptr;
        }
    }
    ptr = jl_force_jit(mi, world);
    return ptr;
    if (iter == miPool.end()) {
        miPool[mi] = 0;
        goto ret;
    }
    iter->second += 1;
    if (iter->second < 10) {
        goto ret;
    }
    ptr = jl_force_jit(mi, world);
ret:
    return ptr;
}

class AdressSort {
public:
    bool operator()(std::pair<std::string, uint64_t> &v1,
                    std::pair<std::string, uint64_t> &v2) const
    {
        return v1.second < v2.second;
    };
};

std::vector<std::pair<std::string, uint64_t>> resolve_address()
{
    std::vector<std::pair<std::string, uint64_t>> queue;
    /*
    for (auto i = has_emitted.begin(); i != has_emitted.end(); i++) {
        std::string invokename = "julia::method::invoke::";
        std::string specname = "julia::method::specfunc::";
        const char *mname = name_from_method_instance(i->first);
        llvm::raw_string_ostream(invokename) << mname;
        llvm::raw_string_ostream(specname) << mname;
        auto invoke_addr = jl_lookup((jl_value_t *)jl_symbol(invokename.c_str()));
        auto spec_addr = jl_lookup((jl_value_t *)jl_symbol(specname.c_str()));
        queue.push_back({invokename, invoke_addr});
        queue.push_back({specname, spec_addr});
    }
    */
    std::sort(queue.begin(), queue.end(), AdressSort());
    return queue;
}
// A cheap way to debug assembly code, we have all the information for emitted symbol.
void lookup_address(uint64_t addr)
{
    std::map<uint64_t, std::string> has_emitted_specfunc;
    for (auto &KV : has_emitted) {
        if (startsWith(KV.second, "julia::method::invoke::")) {
            std::string specName = "julia::method::specfunc::";
            specName += KV.second.substr(strlen("julia::method::invoke::"));
            void* result = jl_StaticJuliaJIT->probeDebugSymbol(specName);
            if (result) {
                has_emitted_specfunc[(uint64_t)result] = specName;
            }
        }
        has_emitted_specfunc[KV.first] = KV.second;
    }
    auto iter = has_emitted_specfunc.upper_bound(addr);
    for (auto i = has_emitted_specfunc.begin(); i != has_emitted_specfunc.end(); i++) {
        llvm::errs() << i->second << " : " << format("%p", (void *)i->first) << "\n";
    }
    if (iter == has_emitted_specfunc.end() ||
        (iter->first != addr && iter == has_emitted_specfunc.begin())) {
        llvm::errs() << "Address not found!\n";
    }
    else {
        if (iter->first != addr) {
            iter--;
        }
        llvm::errs() << "\n==========\nAddress :\n";
        llvm::errs() << iter->second << " : " << format("%p", (void *)iter->first) << "\n";
        llvm::errs() << "\n==========\nEnd of Address :\n";
    }
}
void debug_address()
{
    auto queue = resolve_address();
    llvm::errs() << "\n==========\nAddress :\n";
    for (auto i = queue.begin(); i != queue.end(); i++) {
        llvm::errs() << i->first << " : " << format("%p", (void *)i->second) << "\n";
    }
    llvm::errs() << "\n==========\nEnd of Address :\n";
}
/*
extern "C" JL_DLLEXPORT void* jl_lookup(jl_value_t* s){
    std::string name = jl_symbol_name_((jl_sym_t*)s);
    return jl_StaticJuliaJIT->tryLookupSymbol(name);
}
*/
/*
extern "C" JL_DLLEXPORT void jl_touch_jit(jl_method_instance_t *mi)
{
    jl_init_staticjit();
    std::string s1;
    std::string s2;
    llvm::raw_string_ostream(s1)
        << "julia::method::invoke::" << name_from_method_instance(mi);
    (void)jl_lookup((jl_value_t *)jl_symbol(s1.c_str()));
    llvm::raw_string_ostream(s2)
        << "julia::method::specfunc::" << name_from_method_instance(mi);
    (void)jl_lookup((jl_value_t *)jl_symbol(s2.c_str()));
}
*/
/*
extern "C" JL_DLLEXPORT void jl_touch_symbol(jl_value_t *s)
{
    assert(jl_is_string(s));
    jl_init_staticjit();
    std::string sname = jl_string_ptr(s);
    jl_StaticJuliaJIT->tryLookupSymbol(sname);
}
*/
llvm::raw_fd_ostream *internal_debug_stream = NULL;
extern "C" JL_DLLEXPORT void jl_set_debug_stream(jl_value_t *path)
{
    std::error_code ec;
    assert(jl_is_string(path));
    internal_debug_stream = new llvm::raw_fd_ostream(StringRef(jl_string_ptr(path)), ec);
    return;
}
extern "C" JL_DLLEXPORT void (*jl_preload_binary_handler)(jl_module_t*);
extern "C" JL_DLLEXPORT void jl_preload_binary(jl_module_t* m){
    jl_binding_t* bnd = jl_get_binding(jl_main_module, jl_symbol("jl_preload_handler"));
    if (bnd == nullptr){
        abort();
    }
    jl_value_t* f = jl_atomic_load_acquire(&(bnd->value));
    if (f != nullptr){
        (void)jl_apply_generic(f, (jl_value_t**)&(m->name), 1);
    }
}

extern "C" JL_DLLEXPORT void jl_set_preload_binary_handler(){
    jl_preload_binary_handler = jl_preload_binary;
}
/*
extern "C" JL_DLLEXPORT void jl_invalidate(jl_value_t* mi)
{
    assert(jl_is_method_instance(mi));
    jl_StaticJuliaJIT->insertInvalidatedFunction((jl_method_instance_t*)mi);
    return;
}
*/
extern std::string encodeJuliaValue(jl_value_t *v, bool &needInvalidated,
                                    jl_binding_t *bnd);
extern std::string encodeJLBinding(jl_binding_t *bnd, bool &needInvalidated);
extern std::string juliaValueToString(jl_value_t *v);
/*
extern llvm::raw_fd_ostream* internal_debug_stream;

class MayEmptyStream{};
// Actually should pass by reference, but that requires a lots of writing
template<typename T>
MayEmptyStream& operator<<(MayEmptyStream& in, T&& c){
    if (internal_debug_stream != NULL){
        *internal_debug_stream << c;
        internal_debug_stream->flush();
    }
    return in;
}

MayEmptyStream& operator<<(MayEmptyStream& in, jl_value_t* v){
    if (internal_debug_stream != NULL){
        *internal_debug_stream << juliaValueToString(v).c_str();
        internal_debug_stream->flush();
    }
    return in;
}
MayEmptyStream debugStream;
*/
// prevent string containing '\0' character
void makeSafe(std::string &s)
{
    char *ptr = &s[0];
    for (size_t i = 0; i < s.size(); i++) {
        char c = ptr[i];
        if (c == '\0' || c == '@' || c == '\\' || c == '/') {
            ptr[i] = '_';
        }
    }
}

/*
    Really bad, we use this to trace external values, but actually this should be done with
   Julia's ctx and collect them all at once Instead of using an external function
   
   TODO : make the name really really correct !!!!!
*/
// defined in JuliaValueCoder.cpp
bool checkRelocatable(jl_method_instance_t *mi){
    auto miNode = jl_StaticJuliaJIT->cacheGraph.lookUpNode(mi);
    if (JITMethodInstanceNode *jitNode = dyn_cast<JITMethodInstanceNode>(miNode)){
        return jitNode->isRelocatable;
    }
    else if (CachedMethodInstanceNode *cacheNode = dyn_cast<CachedMethodInstanceNode>(miNode)){
        if (cacheNode->hasEmitted()){
            llvm::errs() << "This cached node should not have emitted already" << jl_name_from_method_instance(mi);
            abort();
        }
        else{
            // conservatively mark as relocatable
            return true;
        }
    }
    else{
        abort();
    }
}

extern jl_binding_t* tryGetGlobalRef(jl_value_t* v);
void registerExternalValue(jl_method_instance_t *mi, jl_value_t *v, std::string &llvmName,
                           jl_binding_t *bnd)
{
    bool needInvalidated = !jl_is_method(mi->def.value);
    std::string tmp = "";
    // fast path, it one 
    if (checkRelocatable(mi) && !needInvalidated){
        tmp = encodeJuliaValue(v, needInvalidated, bnd);
    }else{
        needInvalidated = true;
    }
    if (needInvalidated) {
        jl_StaticJuliaJIT->cacheGraph.markMethodInstanceAsNonRelocatable(mi);
        llvmName = "jlptr::";
        llvm::raw_string_ostream(llvmName)
            << (uint64_t)v << "::" << juliaValueToString(jl_typeof(v));
    }
    else {
        llvmName = "jlvalue::";
        if (jl_is_mutable(jl_typeof(v))) {
            llvm::raw_string_ostream(llvmName) << (void*)v;
        }
        if (bnd == nullptr){
            bnd = tryGetGlobalRef(v);
        }
        if (bnd != nullptr) {
            llvm::raw_string_ostream(llvmName)
                << juliaValueToString((jl_value_t *)bnd->owner) << '.'
                << jl_symbol_name_(bnd->name) << "::" << juliaValueToString(jl_typeof(jl_atomic_load_relaxed(&(bnd->value))));
        }
        else{
            // TODO : this name is incorrect, this may cause problem if the value is really large...
            // So we need to maintain our own jl_ routine...
            llvm::raw_string_ostream(llvmName) << juliaValueToString(v);
        }
    }
    makeSafe(llvmName);
    jl_StaticJuliaJIT->cacheGraph.addDependency(mi, llvmName, tmp);
}

void registerExprValue(jl_method_instance_t *mi, jl_value_t *v, std::string &llvmName){
    assert(jl_is_string(v));
    llvmName = "jlexpr::";
    llvm::raw_string_ostream(llvmName) << jl_string_ptr(v);
    makeSafe(llvmName);
    bool needInvalidated = false;
    const std::string &tmp = encodeJuliaValue(v, needInvalidated, nullptr);
    assert(!needInvalidated);
    jl_StaticJuliaJIT->cacheGraph.addDependency(mi, llvmName, tmp);
}

void registerJLBinding(jl_method_instance_t *mi, std::string &llvmName, jl_binding_t *bnd,
                       bool isSlot)
{
    bool needInvalidated = false;
    const std::string &tmp = encodeJLBinding(bnd, needInvalidated);
    if (needInvalidated) {
        jl_StaticJuliaJIT->cacheGraph.markMethodInstanceAsNonRelocatable(mi);
    }
    if (isSlot) {
        // emit the pointer of value field of a binding
        llvmName = "jlslot::";
    }
    else {
        // emit binding
        llvmName = "jlbnd::";
    }
    llvm::raw_string_ostream(llvmName) << juliaValueToString((jl_value_t *)bnd->owner)
                                       << '.' << jl_symbol_name_(bnd->name);
    makeSafe(llvmName);
    jl_StaticJuliaJIT->cacheGraph.addDependency(mi, llvmName, tmp);
}

// out is a Vector{Tuple{MethodInstance, MethodInstanceState}}
/*
struct MethodInstanceState
    miName::String
    isCached::Bool
    isNonRelocatable::Bool
    unOptIRFilePath::String
    optIRFilePath::String
    objectFilePath::String
    externalSymbols::Vector{Any}
end
*/
jl_value_t* convertStdStringToJLString(const std::string& s){
    return jl_pchar_to_string(s.data(), s.size());
}

class DumpGraphHelper{
    public:
    DumpGraphHelper(StaticJuliaJIT* jit, jl_array_t* nodeArray) : jit(jit), nodeArray(nodeArray) {
        j_jitNode_ty = (jl_datatype_t*)jl_eval_string("BuildSystem.JITMethodInstance");
        j_cacheNode_ty = (jl_datatype_t*)jl_eval_string("BuildSystem.CachedMethodInstance");
        j_pluginNode_ty = (jl_datatype_t*)jl_eval_string("BuildSystem.PluginMethodInstance");
    };
    jl_array_t* dumpGraph(){
        auto& graph = jit->cacheGraph;
        for (auto& KV:graph.getMiNodeMap()){
            dumpNode(KV.second);
        }
        return nodeArray;
    }
    jl_value_t* dumpNode(MethodInstanceNode* miNode){
        auto iter = nodeMap.find(miNode);
        if (iter != nodeMap.end()){
            return iter->second;
        }
        jl_value_t* miName = nullptr;
        JL_GC_PUSH1(&miName);
        std::string& tmp = miNode->miName;
        miName = jl_pchar_to_string(tmp.data(), tmp.size());
        if (auto jitNode = dyn_cast<JITMethodInstanceNode>(miNode)){
            jl_value_t* dependencies = nullptr;
            jl_value_t* isRelocatable = nullptr;
            jl_value_t* symbolTable = nullptr;
            jl_value_t* unOptIRFilePath = nullptr;
            jl_value_t* optIRFilePath = nullptr;
            jl_value_t* objectFilePath = nullptr;
            jl_value_t* j_jitNode = nullptr;
            JL_GC_PUSH7(&dependencies, &isRelocatable, &symbolTable, &unOptIRFilePath, &optIRFilePath, &objectFilePath, &j_jitNode);
            dependencies = (jl_value_t*)jl_alloc_array_1d(jl_array_any_type, (size_t)0);
            isRelocatable = jl_box_bool(jitNode->isRelocatable);
            symbolTable = (jl_value_t*)jl_alloc_array_1d(jl_array_any_type, (size_t)0);
            unOptIRFilePath = convertStdStringToJLString(jitNode->unOptIRFilePath);
            optIRFilePath = convertStdStringToJLString(jitNode->optIRFilePath);
            objectFilePath = convertStdStringToJLString(jitNode->objectFilePath);
            j_jitNode = jl_new_struct(j_jitNode_ty, jitNode->mi, miName, dependencies, isRelocatable, symbolTable, unOptIRFilePath, optIRFilePath, objectFilePath);
            nodeMap[miNode] = j_jitNode;
            jl_array_ptr_1d_push(nodeArray, j_jitNode);
            JL_GC_POP();
            JL_GC_POP();
            // we need to add node before we resolve symbols, otherwise we will have stack overflow.
            for (auto ch:jitNode->dependencies.queue){
                jl_array_ptr_1d_push((jl_array_t*)dependencies, dumpNode(ch));
            }
            {
                jl_value_t* symbol = nullptr;
                jl_value_t* code = nullptr;
                jl_value_t* svec = nullptr;
                JL_GC_PUSH3(&symbol, &code, &svec);
                for (auto& KV:jitNode->symbolTable){
                    symbol = convertStdStringToJLString(KV.first);
                    code = convertStdStringToJLString(KV.second);
                    svec = (jl_value_t*)jl_svec2(symbol, code);
                    jl_array_ptr_1d_push((jl_array_t*)symbolTable, svec);
                }
                JL_GC_POP();
            }
            return j_jitNode;
        }
        else if (auto cacheNode = dyn_cast<CachedMethodInstanceNode>(miNode)){
            jl_value_t* libName = nullptr;
            jl_value_t* objName = nullptr;
            jl_value_t* j_cacheNode = nullptr;
            JL_GC_PUSH3(&libName, &objName, &j_cacheNode);
            libName = convertStdStringToJLString(cacheNode->libName);
            objName = convertStdStringToJLString(cacheNode->objName);
            j_cacheNode = jl_new_struct(j_cacheNode_ty, miName, libName, objName);
            jl_array_ptr_1d_push(nodeArray, j_cacheNode);
            nodeMap[miNode] = j_cacheNode;
            JL_GC_POP();
            JL_GC_POP();
            return j_cacheNode;
        }
        else if (auto pluginNode = dyn_cast<PluginMethodInstanceNode>(miNode)){
            jl_value_t* pluginName = nullptr;
            jl_value_t* j_pluginNode = nullptr;
            JL_GC_PUSH2(&pluginName, &j_pluginNode);
            std::string& tmp = pluginNode->pluginName;
            pluginName = jl_pchar_to_string(tmp.data(), tmp.size());
            j_pluginNode = jl_new_struct(j_pluginNode_ty, miName, pluginName);
            jl_array_ptr_1d_push(nodeArray, j_pluginNode);
            nodeMap[miNode] = j_pluginNode;
            JL_GC_POP();
            JL_GC_POP();
            return j_pluginNode;
        }
        else{
            assert(0);
        }
    }
    StaticJuliaJIT* jit;
    // Output of translated node
    jl_array_t* nodeArray;
    // Map, used to translate call graph
    std::unordered_map<MethodInstanceNode*, jl_value_t*> nodeMap;
    jl_datatype_t *j_jitNode_ty;
    jl_datatype_t *j_cacheNode_ty;
    jl_datatype_t *j_pluginNode_ty;
};

extern "C" JL_DLLEXPORT void jl_dump_graph(jl_array_t* out){
    DumpGraphHelper helper(jl_StaticJuliaJIT, out);
    (void)helper.dumpGraph();
}