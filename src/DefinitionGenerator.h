#ifndef JL_DEFINITIONGENERATOR_H
#define JL_DEFINITIONGENERATOR_H
#include <unordered_map>
#include <julia.h>
#include <llvm/Support/Error.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include "llvm-version.h"
#if JL_LLVM_VERSION >= 130000
#include <llvm/ExecutionEngine/Orc/ExecutorProcessControl.h>
#endif
#include "JITUtil.h"
#include "StaticJIT.h"
using namespace llvm;
using namespace llvm::orc;
using namespace llvm::jitlink;
using SymbolTable = std::unordered_map<std::string, std::string>;
class StaticJuliaJIT;
class JuliaInvalidatedFunctionGenerator : public DefinitionGenerator {
public:
    JuliaInvalidatedFunctionGenerator(StaticJuliaJIT *jit) : jit(jit){};
    Error tryToGenerate(LookupState &LS, LookupKind K, JITDylib &JD,
                        JITDylibLookupFlags JDLookupFlags,
                        const SymbolLookupSet &LookupSet) override;
    StaticJuliaJIT *jit;
};

class JuliaRuntimeSymbolGenerator : public DefinitionGenerator {
public:
    JuliaRuntimeSymbolGenerator(StaticJuliaJIT *jit) : jit(jit){};
    Error tryToGenerate(LookupState &LS, LookupKind K, JITDylib &JD,
                        JITDylibLookupFlags JDLookupFlags,
                        const SymbolLookupSet &LookupSet) override
    {
        SymbolMap smap;
        SymbolNameVector v = LookupSet.getSymbolNames();
        for (auto i = v.begin(); i != v.end(); i++) {
            std::string s = (**i).str();
            void* addr = getExternNameAddr(s);
            // We only generate runtime value symbol, not invoke pointer or spec pointer
            smap[*i] = JITEvaluatedSymbol((JITTargetAddress)addr, JITSymbolFlags::FlagNames::Exported);
        }
        return JD.define(
            std::make_unique<AbsoluteSymbolsMaterializationUnit>(std::move(smap)));
    };
    // used for JIT dependencies
    void registerDependencies(SymbolTable& dep);
    // used for cached object
    void registerDependencies(std::unique_ptr<SymbolTable> dep){
        registerDependencies(*dep);
    }
    // working horse to resolve symbols
    void* getExternNameAddr(std::string &s);
    void onResolve(jl_value_t *);
    SymbolTable dependencies;
    StaticJuliaJIT *jit;
};
#endif
// A materializtion unit used for resolving external variables. They are mainly produced by
// literal_pointer_val and ccall. Currently this is unused,
/*
class JuliaRuntimeSymbolMaterializationUnit : public MaterializationUnit {
public:
    JuliaRuntimeSymbolMaterializationUnit(SymbolFlagsMap syms)
      : MaterializationUnit(syms, nullptr){};
    StringRef getName() const { return "<Julia Runtime Symbols>"; };
    void materialize(std::unique_ptr<MaterializationResponsibility> R) override
    {
        // we should assign address to symbol here, like absolute materializetion unit
        llvm::errs() << "Materializing runtime symbol";
        auto symbols = R->getRequestedSymbols();
        for (auto i = symbols.begin(); i != symbols.end(); i++) {
            std::string s = (**i).str();
            llvm::errs() << s;
        }
        SymbolMap smap;
        // If we can't find the symbol, silently assign a 0x100 to that symbol. This will

        for (auto i = SymbolFlags.begin(); i != SymbolFlags.end(); i++) {
            smap[(*i).first] = JITEvaluatedSymbol(0x100, (*i).second);
        }
        cantFail(R->notifyResolved(smap));
        cantFail(R->notifyEmitted());
        llvm::errs() << "Reaching here";
    };
    void discard(const JITDylib &JD, const SymbolStringPtr &Name) override
    {
        // should has no effect
        return;
    }
};
*/
