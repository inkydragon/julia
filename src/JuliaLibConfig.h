#ifndef JL_JULIALIBCONFIG_H
#define JL_JULIALIBCONFIG_H
#include <string>
#include <unordered_map>
#include "julia.h"
using SymbolTable = std::unordered_map<std::string, std::string>;
class JuliaLibConfig{
    public:
    jl_method_instance_t* mi;
    // Name of method instance
    std::string miName;
    // Name of the object file, this is slightly different from miName
    std::string objFileName;
    // whether this refers to a MI defined in other libs
    bool isCached;
    // whether this is a relocatable MI, if not, we need to invoke JIT at runtime
    bool isRelocatable;
    // symbol tables
    std::unique_ptr<SymbolTable> externalSymbols;
};

#endif