#include "julia.h"
#include "llvm/Support/MemoryBuffer.h"
#include <string>
#include <unordered_map>
class JuliaObjectFile {
public:
    JuliaObjectFile(jl_module_t *JLModule,
                    /* std::unordered_map<std::string, std::string>& SymbolDependencies,*/
                    std::unique_ptr<llvm::MemoryBuffer> MemoryBuffer)
      : JLModule(JLModule),
        //SymbolDependencies(SymbolDependencies),
        MemoryBuffer(std::move(MemoryBuffer))
      {};
    // TODO : Currently useless, I need a way to find out how to manage backend's JITLib 
    jl_module_t *JLModule;
    // TODO : LLVM's JITDylib is different from MaterilizationUnit
    // we use one MU for one object file (MI), and every MI has a symbol tables (external dependency)
    // but only JITDylib has associated symbol tables in LLVM
    // so this field doesn't make quite sense
    // we can only have one symbol table for one JITDylib, that is, for the bundles of MI
    // we have to merge all the symbol dependencies at Symbol Generators (and get only one symbol table!)
    // std::unordered_map<std::string, std::string> SymbolDependencies;
    std::unique_ptr<llvm::MemoryBuffer> MemoryBuffer;
};
