#ifndef JL_STATICJIT_H
#define JL_STATICJIT_H
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
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <sstream>
using namespace llvm;
using namespace llvm::orc;
using namespace llvm::jitlink;
#include "julia.h"
#include "julia_assert.h"
#include "julia_internal.h"
#include "jitlayers.h"
#include "DefinitionGenerator.h"
#include "CacheGraph.h"
#include "JuliaLibConfig.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#define UseJITLink 0
#if UseJITLink
#include "JuliaObjectLinkingLayer.h"
#else
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "RTDyldObject.h"
#include "ObjectFileInterface.h"
#endif

extern "C" JL_DLLEXPORT void jl_module_to_string(std::stringstream &s, jl_module_t *m);
extern std::pair<std::unique_ptr<Module>, jl_llvm_functions_t>
emit_function(jl_method_instance_t *lam, jl_code_info_t *src, jl_value_t *jlrettype,
              jl_codegen_params_t &params, bool vaOverride = false);
enum GeneratedPointerType { InvokePointer = 0x1, SpecPointer = 0x2 };
// Add an object to JuliaObjectLinkingLayer. This is lazy, and ownership is transfered to
// ObjectMaterializationUnit. Materizalition happens when we look up symbols. Once
// materialization is done, the memory is released. This is an helper function

#if UseJITLink
inline Error addObjectInternal(ExecutionSession &ES, JuliaObjectLinkingLayer &OL, JITDylib &JD,
                        std::unique_ptr<JuliaObjectFile> ObjectFile)
{
    auto RT = JD.getDefaultResourceTracker();
    return JD.define(ObjectMaterializationUnit::Create(OL, std::move(ObjectFile)),
                     std::move(RT));
};
#else
inline Error addObjectInternal(ExecutionSession &ES, RTDyldObjectLinkingLayer &OL, JITDylib &JD,
                        std::unique_ptr<JuliaObjectFile> ObjectFile)
{
    auto eI = getObjectFileInterface(ES,  *(ObjectFile->MemoryBuffer));
    if (!eI){
        llvm::errs() << eI.takeError();
    }
    auto RT = JD.getDefaultResourceTracker();
    return JD.define(ObjectMaterializationUnit::Create(*eI, OL, std::move(ObjectFile)),
                     std::move(RT));
};
#endif

class JuliaRuntimeSymbolGenerator;
class StaticJuliaJIT {
public:
    StaticJuliaJIT();
    friend class JuliaRuntimeSymbolGenerator;
    void initConstPool();
    llvm::Expected<void*> tryLookupSymbol(std::string &);
    void removeUnusedExternal(llvm::Module *M);
    Error addJITObject(JITMethodInstanceNode* jitNode, JITDylib& lib);
    // TODO : fix this function
    Error addCachedObject(CachedMethodInstanceNode* cacheNode, JITDylib& lib);
    void addStaticLib(std::string libPath, std::vector<JuliaLibConfig>& configs);
    llvm::Expected<void*> getFunctionPointerInternal(jl_method_instance_t *mi, size_t world, GeneratedPointerType pty);
    llvm::Expected<void*> getInvokePointer(jl_method_instance_t *mi, size_t world) {
        return getFunctionPointerInternal(mi, world, InvokePointer);
    };
    llvm::Expected<void*> getSpecPointer(jl_method_instance_t *mi, size_t world) {
        return getFunctionPointerInternal(mi, world, SpecPointer);
    };
    // TODO : fix this function, reconsider invalidated function handle
    void compileMethodInstancePatched(jl_method_instance_t* mi, CachedMethodInstanceNode* cacheNode, size_t world);
    void* probeDebugSymbol(std::string &name);
    void compileMethodInstanceCached(jl_method_instance_t *mi, size_t world, JITMethodInstanceNode* parentNode = nullptr);
    std::string getPath(std::string& miName, OutputFileType fty)
    {
        return opg.getPath(miName, fty);
    }
    std::string writeOutput(llvm::Module *mod, std::string& miName,
                                OutputFileType fty)
    {
        if (!needCacheOutput){
            return "";
        }
        std::string path = getPath(miName, fty);
        if (fty == ObjFile) {
            assert(obj_Buffer.size() > 0);
            std::ofstream file(path, std::ios::binary);
            file.write(obj_Buffer.data(), obj_Buffer.size());
            return path;
        }
        std::error_code ec;
        llvm::raw_fd_ostream os(llvm::StringRef(path), ec);
        if (ec) {
            // Todo : at some time julia has remove the intermediate directory...
            // llvm::errs() << ec.message().c_str();
            needCacheOutput = false;
            return "";
        }
        else{
            mod->print(os, nullptr);
            os.close();
            return path;
        }
    }
    enum ConstantType { String, Symbol, BitValue, Type };
    llvm::TargetMachine *TM;

    SmallVector<char, 0> obj_Buffer;
    raw_svector_ostream obj_OS;
    llvm::legacy::PassManager PM;

    llvm::orc::ExecutionSession ES;
    #if UseJITLink
    llvm::jitlink::InProcessMemoryManager Memgr;
    JuliaObjectLinkingLayer ObjectLayer;
    #else
    // SectionMemoryManager Memgr;
    RTDyldObjectLinkingLayer ObjectLayer;
    #endif
    JITDylib &JuliaFuncJD; // contain all the loaded object code
    JITDylib &JuliaRuntimeJD; // resolve builtin function name
    JITDylib &JuliaInvalidatedJD;
    JITDylib &JuliaFallbackJD; // For some reason, some internal symbols didn't get a right
                               // name...
    std::unordered_map<StaticJuliaJIT::ConstantType, jl_value_t *> constpool;
    jl_value_t *MethodBlacklist;
    bool inCompiling;
    ObjectPathGenerator opg;
    bool needCacheOutput = false;
    CacheGraph cacheGraph;
    JuliaRuntimeSymbolGenerator* generator;
};
#endif