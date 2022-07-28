#include "DefinitionGenerator.h"
Error JuliaInvalidatedFunctionGenerator::tryToGenerate(LookupState &LS, LookupKind K, JITDylib &JD,
                        JITDylibLookupFlags JDLookupFlags,
                        const SymbolLookupSet &LookupSet)
    {
    /*
    SymbolMap smap;
    SymbolNameVector v = LookupSet.getSymbolNames();
    for (auto i = v.begin(); i != v.end(); i++) {
        std::string s((**i).str().c_str());
        if (startsWith(s, "julia::method::")){
            auto iter = jit->invalidatedFunctionNameMapping.find(s);
            if (iter != jit->invalidatedFunctionNameMapping.end()){
                jit->compileMethodInstancePatched(iter->second, jl_world_counter);
            }
        }
    }
    TODO : Check whether invalidation is correct. Currently we compile when we load object, so this is unnecessary any more.
    */
    return llvm::ErrorSuccess();
}

extern void* resolveExternJLValue(std::string s, std::string code, std::function<void(jl_value_t*)> onResolve);

void rootValue(StaticJuliaJIT* jit, jl_value_t* v){
    if (v != NULL) {
        // for safety, root all the value we have used.
        JL_GC_PUSH1(&v);
        jl_value_t *jl_type_pool = jit->constpool[StaticJuliaJIT::ConstantType::Type];
        jl_array_ptr_1d_push((jl_array_t *)jl_type_pool, v);
        JL_GC_POP();
    }
}

void JuliaRuntimeSymbolGenerator::onResolve(jl_value_t* v){
    rootValue(jit, v);
}

void JuliaRuntimeSymbolGenerator::registerDependencies(SymbolTable& dep){
        for (auto& KV: dep){
            auto iter = dependencies.find(KV.first);
            if (iter != dependencies.end()){
                // todo : why they can be different ? 
                /*
                if (iter->second != KV.second){
                    llvm::errs() << "Inconsistent symbol table item!" << '\n';
                    llvm::errs() << "symbol : " << KV.first << '\n';
                    llvm::errs() << "previous one : " << iter->second << '\n';
                    llvm::errs() << "current one : " << KV.second << '\n';
                }
                */
            }
            else{
                dependencies[KV.first] = KV.second;
            }
        }
    }
void* JuliaRuntimeSymbolGenerator::getExternNameAddr(std::string &s)
{   
    std::function<void(jl_value_t*)> f = [this](jl_value_t* v){this->onResolve(v);};
    auto iter = dependencies.find(s);
    if (iter != dependencies.end()){
        return resolveExternJLValue(s, iter->second, f);
    }
    else{
        // jl_internal and such thing
        return resolveExternJLValue(s, "", f);
    }
}