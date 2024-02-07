#include <string>
#include "julia.h"
#include "julia_internal.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include <bit>
#include <unordered_map>
#include <vector>
#include <functional>

inline bool startsWith(std::string& s, std::string&& substr)
{
    return substr.length() <= s.length() && equal(substr.begin(), substr.end(), s.begin());
}

using SSAId = uint32_t;
enum JLTypeTag : uint16_t;
using BytesBuffer = llvm::SmallVector<char, 100>;
class JuliaValueEncoder {
    using ScopeChain = llvm::SmallVector<jl_tvar_t *, 10>;
    using SSAArray = llvm::SmallVector<SSAId, 10>;
    using Scope = llvm::SmallVector<std::pair<jl_tvar_t *, SSAId>, 10>;
    using SmallJLValueArray = llvm::SmallVector<jl_value_t *, 10>;
    SSAId encodeSymbolAsBytes(jl_sym_t *sym);
    SSAId encodeJLStringAsBytes(jl_value_t *jlstr);
    SSAId encodeBinding(jl_module_t *mod, jl_sym_t *sym);
    SSAId encodeBindingInternal(jl_module_t *mod, jl_sym_t *sym, JLTypeTag tag);
    SSAId encodeSingleton(jl_value_t *v);
    SSAId encodeDataType(jl_datatype_t *dt);
    SSAId encodeVararg(jl_value_t* v);
    SSAId encodeRuntimePtr(jl_value_t *v);
    SSAId encodeImmutable(jl_value_t *v);
    SSAId encodePrimitive(jl_value_t *v);
    SSAId encodePureTag(JLTypeTag tag);
    SSAId encodeTypeVar(jl_tvar_t *tvar, bool isDef);
    SSAId encodeUnionAll(jl_value_t *v);
    void collectUnionComponent(SmallJLValueArray &arr, jl_value_t *v);
    SSAId encodeUnion(jl_value_t *v);
    SSAId encodeTypeName(jl_typename_t *t);
    SSAId encodeJuliaValue(jl_value_t *v);
    public:
    bool isRelocatable = true;
    std::string encodeExternalJuliaValue(jl_value_t *v);
    std::string encodeJLBinding(jl_binding_t* bnd);
    private:
    std::vector<BytesBuffer> SSAValues;
    Scope scope;
    public:
    static std::unordered_map<jl_value_t *, jl_binding_t *> bindings;
    // We decide to generate name by our own;
    std::string llvmName;
};

class JuliaValueDecoder {
    public:
    JuliaValueDecoder(jl_module_t *mod, jl_array_t *rootArray, std::string &str);
    void* decodeJuliaValue();
    bool isRelocatable = true;
    private:
    inline jl_value_t *evalSSA(SSAId id) { return jl_arrayref(SSAValues, id); };
    // providing evaluation context
    jl_module_t *JLModule;
    // Note : this array should be rooted
    jl_array_t *SSAValues;
    std::vector<uint8_t> bytes;
    llvm::BinaryStreamReader reader;
};
