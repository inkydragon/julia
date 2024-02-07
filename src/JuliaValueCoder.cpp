#include "JuliaValueCoder.h"
enum JLTypeTag : uint16_t {
    JL_CONST = 0,
    JL_BOTTOM,
    JL_EMPTY_TUPLE,
    JL_TYPEOF_EMPTY_TUPLE,
    JL_TYPEOF_BOTTOM,
    // used for union construction
    JL_UNION,
    JL_VARARG_TYPE,
    JL_TUPLE,
    JL_NAMED_TUPLE,
    JL_INLINE,
    JL_SYMBOL,
    JL_STRING,
    JL_PTR,
    JL_TYPENAME,
    JL_FUNCTION = 1 << 8,
    JL_GLOBAL_REF,
    JL_APPLY_TYPE,
    JL_NEW_PRIMITIVE,
    JL_NEW_IMMUTABLE,
    JL_NEW_TVAR,
    JL_NEW_UNIONALL,
    JL_SINGLETON,
    JL_NEW_VARARG,
    JL_BND // return a jl_binding_t*, cannot used in normal evaluation
};

uint8_t hex2uint8(char h)
{
    if (h >= 'A') {
        return (h - 'A') + 10;
    }
    else {
        return h - '0';
    }
}
char uint82hex(uint8_t u)
{
    if (u >= 10) {
        return 'A' + (u - 10);
    }
    else {
        return u + '0';
    }
}

std::vector<uint8_t> decodeStringAsBytes(std::string &str)
{
    assert(str.size() % 2 == 0);
    std::vector<uint8_t> bytes(str.size() / 2);
    for (size_t i = 0; i < str.size() / 2; i++) {
        // little endianness
        bytes[i] = (hex2uint8(str[2 * i + 1]) << 4) + hex2uint8(str[2 * i]);
    }
    return bytes;
}

std::string encodeBytesAsString(BytesBuffer &str)
{
    std::vector<char> bytes(str.size() * 2 + 1);
    for (size_t i = 0; i < str.size(); i++) {
        // little endianness
        uint8_t byte = str[i];
        bytes[2 * i] = uint82hex(byte & 0b1111);
        bytes[2 * i + 1] = uint82hex(byte >> 4);
    }
    bytes[str.size() * 2] = '\0';
    return bytes.data();
}

/*
void printLiteralSymbol(llvm::raw_string_ostream &out, jl_sym_t *name){
    char *sn = jl_symbol_name(name);
    if (name == jl_symbol("'")){
        out << "Symbol(\"'\")";
    }
    if (jl_is_operator(sn) || jl_is_identifier(sn)){
        out << ':' << sn;
    } 
    else{
        out << "Symbol(" << sn << ')';
    }
}
*/
/*
    When print a symbol, we need to ensure the symbol is correctly displayed if it contains
   strange symbols
*/
/*
void printIdentifier(llvm::raw_string_ostream &out, jl_sym_t *name, bool isAccessor)
{
    const char *sn = jl_symbol_name(name);
    int non_id = 0;
    int op = 0; // whether the symbol is an operator, note that operator can only appear at
                // the end of accessing chain !
    if (name == jl_symbol("'")) {
        if (isAccessor) {
            out << "adjoint";
        }
        return;
    }
    if (jl_is_operator(sn) && isAccessor) {
        op = 1;
    }
    else if (!(jl_is_identifier(sn))) {
        non_id = 1;
    }
    if (non_id) {
        out << "var\"";
    }
    else if (op) {
        out << ":(";
    }
    out << sn;
    if (non_id) {
        out << "\"";
    }
    else if (op) {
        out << ")";
    }
    return;
}
*/
std::unordered_map<jl_value_t *, jl_binding_t *> JuliaValueEncoder::bindings;
// A simple wrapper, ensure that every output is encoded as bytes
// llvm::raw_svector_ostream will format output, so it's not quite appropriate for out
// usage.
/*
extern "C" JL_DLLEXPORT void (*jl_set_restored_module_handle)(jl_module_t *);
void jl_set_restored_module_handle_impl(jl_module_t *mod)
{
    // reduce module to it's root module
    while (1) {
        if (mod == jl_main_module || mod == jl_base_module || mod == jl_core_module) {
            break;
        }
        if (mod != mod->parent) {
            mod = mod->parent;
        }
        else {
            break;
        }
    }
    jl_(mod);
    JuliaValueEncoder::topModules[(const char *)jl_symbol_name_(mod->name)] = mod;
}

extern "C" JL_DLLEXPORT void jl_set_restored_module_handle_setter()
{
    jl_set_restored_module_handle = jl_set_restored_module_handle_impl;
}
*/
class RawByteStream : public llvm::raw_svector_ostream {
public:
    explicit RawByteStream(llvm::SmallVectorImpl<char> &O) : llvm::raw_svector_ostream(O){};
    // TODO : Not portable
    // Maybe use llvm to handle this?
    bool isBigEndian() const { return llvm::sys::IsBigEndianHost; }
    template<typename T>
    RawByteStream &encodeInteger(T v)
    {
        if (isBigEndian()) {
            for (int i = (int)(sizeof(T)) - 1; i > -1; i--) {
                T mask = 0b11111111;
                uint8_t b = (v & (mask << (8 * i))) >> (8 * i);
                llvm::raw_svector_ostream::write((char)b);
            }
        }
        else {
            for (size_t i = 0; i < sizeof(T); i++) {
                T mask = 0b11111111;
                uint8_t b = (v & (mask << (8 * i))) >> (8 * i);
                llvm::raw_svector_ostream::write((char)b);
            }
        }
        return *this;
    }
    RawByteStream &operator<<(uint8_t i) { return encodeInteger(i); }
    RawByteStream &operator<<(uint16_t i) { return encodeInteger(i); }
    RawByteStream &operator<<(uint32_t i) { return encodeInteger(i); }
    RawByteStream &operator<<(uint64_t i) { return encodeInteger(i); }
    RawByteStream &operator<<(const char *str)
    {
        llvm::raw_svector_ostream::write(str, strlen(str) + 1);
        return *this;
    }
    template<typename T>
    RawByteStream &operator<<(llvm::SmallVectorImpl<T> &O)
    {
        size_t bytenum = sizeof(T) * O.size();
        llvm::raw_svector_ostream::write((const char *)O.data(), bytenum);
        return *this;
    }
    void write(uint8_t *str, size_t len)
    {
        llvm::raw_svector_ostream::write((const char *)str, len);
    }
};

SSAId JuliaValueEncoder::encodeSymbolAsBytes(jl_sym_t *sym)
{
    const char *str = jl_symbol_name_(sym);
    // we also copy the trailing zero here.
    size_t bytenum = strlen(str) + 1;
    SSAValues.emplace_back();
    auto &result = SSAValues.back();
    RawByteStream os(result);
    os << (uint16_t)JL_SYMBOL << uint8_t(1) << uint8_t(0) << (uint32_t)bytenum;
    os.write((uint8_t *)str, bytenum);

    // encode symbol in llvmName
    /*
    llvm::raw_string_ostream os(llvmName);
    printLiteralSymbol(os, sym);
    */
    return SSAValues.size() - 1;
}

SSAId JuliaValueEncoder::encodeJLStringAsBytes(jl_value_t *jlstr)
{
    assert(jl_is_string(jlstr));
    const char *str = jl_string_ptr(jlstr);
    size_t bytenum = jl_string_len(jlstr) + 1;
    SSAValues.emplace_back();
    auto &result = SSAValues.back();
    RawByteStream os(result);
    os << (uint16_t)JL_STRING << uint8_t(1) << uint8_t(0) << (uint32_t)bytenum;
    os.write((uint8_t *)str, bytenum);

    // encode string in llvmName, escaped is not needed?
    // this should be fine
    // llvm::raw_string_ostream(llvmName) << '"' << jl_string_ptr(jlstr) << '"';
    return SSAValues.size() - 1;
}

SSAId JuliaValueEncoder::encodeBindingInternal(jl_module_t *mod, jl_sym_t *sym,
                                               JLTypeTag tag)
{
    llvm::SmallVector<jl_module_t *, 10> mods;
    while (mod->parent != mod) {
        mods.push_back(mod);
        mod = mod->parent;
    }
    mods.push_back(mod);
    SSAArray modIds;
    for (auto i = mods.rbegin(); i != mods.rend(); i++) {
        SSAId modSSAId = encodeSymbolAsBytes((*i)->name);
        modIds.push_back(modSSAId);
    }
    assert(modIds.size() == mods.size());
    SSAId nameSSAId = encodeSymbolAsBytes(sym);
    SSAValues.emplace_back();
    auto &result = SSAValues.back();
    RawByteStream os(result);
    // encode all the module and binding name
    os << (uint16_t)tag << uint8_t(1) << uint8_t(0)
       << (uint32_t)(sizeof(SSAId) * (modIds.size() + 1));
    for (auto id : modIds) {
        os << id;
    }
    os << nameSSAId;

    // encode binding in llvmname
    /*
    llvm::raw_string_ostream os(llvmName);
    for (auto m : mods) {
        printUnescapedSymbol(os, m->name, false);
        os << '.';
    }
    printUnescapedSymbol(os, jl_symbol_name_(sym), true);
    */
    return SSAValues.size() - 1;
}

SSAId JuliaValueEncoder::encodeBinding(jl_module_t *mod, jl_sym_t *sym)
{
    return encodeBindingInternal(mod, sym, JL_GLOBAL_REF);
}

SSAId JuliaValueEncoder::encodeSingleton(jl_value_t *v)
{
    assert(jl_is_datatype_singleton((jl_datatype_t *)jl_typeof(v)));
    SSAId typeId = encodeDataType((jl_datatype_t *)(jl_typeof(v)));
    SSAValues.emplace_back();
    auto &result = SSAValues.back();
    RawByteStream os(result);
    os << (uint16_t)JL_SINGLETON << uint8_t(1) << uint8_t(0) << (uint32_t)(sizeof(SSAId));
    os << typeId;
    // encode singelton in llvmname
    // llvm::raw_string_ostream(llvmName) << "()";
    return SSAValues.size() - 1;
}

SSAId JuliaValueEncoder::encodeDataType(jl_datatype_t *dt)
{
    // We need to distinguish different kinds of DataType
    // 1. Datatype without parameters
    // They should be constructed by directly referring to the typename's name
    // For example, typeof(sin) refers to Base.var"#sin", Int refers to Int, #1 refers
    // to Main.var"#1#2"
    // 2. Datatype with parameters, they should be constructed by typename and
    // parameters
    jl_module_t *mod = dt->name->module;
    jl_sym_t *name = dt->name->name;
    size_t len = jl_svec_len(dt->parameters);
    SSAId principleId;
    principleId = encodeBinding(mod, name);
    if (len == 0) {
        return principleId;
    }
    // llvm::raw_string_ostream(llvmName) << '{';
    SSAArray paramIDs;
    for (size_t i = 0; i < len; i++) {
        jl_value_t *param = jl_svecref((void *)dt->parameters, i);
        paramIDs.push_back(encodeJuliaValue(param));
        /*
        if (i != len - 1){
            llvm::raw_string_ostream(llvmName) << ' ,';
        }
        */
    }
    // llvm::raw_string_ostream(llvmName) << '}';
    SSAValues.emplace_back();
    auto &result = SSAValues.back();
    RawByteStream os(result);
    os << (uint16_t)JL_APPLY_TYPE << uint8_t(1) << uint8_t(0)
       << (uint32_t)(sizeof(SSAId) * (paramIDs.size() + 1));
    os << principleId;
    os << paramIDs;
    return SSAValues.size() - 1;
}

SSAId JuliaValueEncoder::encodeRuntimePtr(jl_value_t *v)
{
    uint64_t ptrint = (uint64_t)v;
    SSAValues.emplace_back();
    isRelocatable = false;
    auto &result = SSAValues.back();
    RawByteStream os(result);
    // llvm::raw_string_ostream(llvmName) << "@dyn:(" << llvm::format("%p", v) << ')';
    os << (uint16_t)JL_PTR << uint8_t(1) << uint8_t(0) << (uint32_t)sizeof(uint64_t)
       << ptrint;
    return SSAValues.size() - 1;
}

SSAId JuliaValueEncoder::encodeImmutable(jl_value_t *v)
{
    assert(jl_is_immutable_datatype(jl_typeof(v)) && !jl_is_primitivetype(jl_typeof(v)));
    jl_datatype_t *dt = (jl_datatype_t *)jl_typeof(v);
    size_t nfield = jl_datatype_nfields(dt);
    SSAArray fieldIds;
    SSAId typeId = encodeDataType(dt);
    // llvm::raw_string_ostream(llvmName) << '(';
    for (size_t i = 0; i < nfield; i++) {
        fieldIds.push_back(encodeJuliaValue(jl_get_nth_field(v, i)));
        /*
        if (i != nfield - 1){
            llvm::raw_string_ostream(llvmName) << " ,";
        }
        */
    }
    // llvm::raw_string_ostream(llvmName) << ')';
    SSAValues.emplace_back();
    auto &result = SSAValues.back();
    RawByteStream os(result);
    os << (uint16_t)JL_NEW_IMMUTABLE << uint8_t(1) << uint8_t(0)
       << (uint32_t)(sizeof(SSAId) * (fieldIds.size() + 1));
    os << typeId << fieldIds;
    return SSAValues.size() - 1;
}

SSAId JuliaValueEncoder::encodePrimitive(jl_value_t *v)
{
    assert(jl_is_primitivetype(jl_typeof(v)));
    jl_datatype_t *dt = (jl_datatype_t *)(jl_typeof(v));
    SSAId typeId = encodeDataType(dt);
    size_t bytesize = jl_datatype_size(dt);
    /*
    llvm::raw_string_ostream(llvmName) << '(';
    switch (bytesize)
    {
    case 0:
        break;
    case 1:
        llvm::raw_string_ostream(llvmName) << llvm::format("%lld", *((int8_t*)v));
        break;
    case 2;
        llvm::raw_string_ostream(llvmName) << llvm::format("%lld", *((int16_t*)v));
        break;
    case 4;
        llvm::raw_string_ostream(llvmName) << llvm::format("%lld", *((int32_t*)v));
        break;
    case 8;
        llvm::raw_string_ostream(llvmName) << llvm::format("%lld", *((int64_t*)v));
        break;
    default:
        const char* bytePtr = (const char* v);
        llvm::raw_string_ostream(llvmName) << "0x";
        for (size_t i = 0; i < bytesize; i++){
            llvm::raw_string_ostream(llvmName) << llvm::format("%2x", *(bytePtr + i));
        }
    }
    llvm::raw_string_ostream(llvmName) << ')';
    */
    SSAValues.emplace_back();
    auto &result = SSAValues.back();
    RawByteStream os(result);
    os << (uint16_t)JL_NEW_PRIMITIVE << uint8_t(1) << uint8_t(0)
       << (uint32_t)(sizeof(SSAId) + bytesize);
    os << typeId;
    os.write((uint8_t *)v, bytesize);
    return SSAValues.size() - 1;
}

SSAId JuliaValueEncoder::encodePureTag(JLTypeTag tag)
{
    SSAValues.emplace_back();
    auto &result = SSAValues.back();
    RawByteStream os(result);
    os << (uint16_t)tag << uint8_t(0) << uint8_t(0);
    return SSAValues.size() - 1;
}
SSAId JuliaValueEncoder::encodeTypeVar(jl_tvar_t *tvar, bool isDef)
{
    if (isDef) {
        SSAId lbId = encodeJuliaValue(tvar->lb);
        SSAId ubId = encodeJuliaValue(tvar->ub);
        SSAId nameId = encodeSymbolAsBytes(tvar->name);
        SSAValues.emplace_back();
        auto &result = SSAValues.back();
        RawByteStream os(result);
        os << (uint16_t)JL_NEW_TVAR << uint8_t(1) << uint8_t(0);
        os << (uint32_t)(3 * sizeof(SSAId));
        os << nameId << lbId << ubId;
        return SSAValues.size() - 1;
    }
    else {
        // search from inside out
        for (auto i = scope.rbegin(); i != scope.rend(); i++) {
            if (i->first == tvar) {
                return i->second;
            }
        }
        return encodeTypeVar(tvar, true);
    }
    return -1;
}
SSAId JuliaValueEncoder::encodeUnionAll(jl_value_t *v)
{
    assert(jl_is_unionall(v));
    SSAArray typeVarIds;
    while (jl_is_unionall(v)) {
        jl_unionall_t *uv = (jl_unionall_t *)v;
        auto typeVarId = encodeTypeVar(uv->var, true);
        typeVarIds.push_back(typeVarId);
        scope.push_back({uv->var, typeVarId});
        v = uv->body;
    }
    SSAId bodyId = encodeJuliaValue(v);
    for (size_t i = 0; i < typeVarIds.size(); i++) {
        scope.pop_back();
    }
    for (auto i = typeVarIds.rbegin(); i != typeVarIds.rend(); i++) {
        SSAId tvarId = *i;
        SSAValues.emplace_back();
        auto &result = SSAValues.back();
        RawByteStream os(result);
        os << (uint16_t)JL_NEW_UNIONALL << uint8_t(1) << uint8_t(0)
           << (uint32_t)(sizeof(SSAId) * 2);
        os << tvarId;
        os << bodyId;
        bodyId = SSAValues.size() - 1;
    }
    return bodyId;
}
void JuliaValueEncoder::collectUnionComponent(SmallJLValueArray &arr, jl_value_t *v)
{
    if (jl_is_uniontype(v)) {
        jl_uniontype_t *uv = (jl_uniontype_t *)v;
        collectUnionComponent(arr, uv->a);
        collectUnionComponent(arr, uv->b);
    }
    else {
        arr.push_back(v);
    }
}
SSAId JuliaValueEncoder::encodeUnion(jl_value_t *v)
{
    assert(jl_is_uniontype(v));
    SmallJLValueArray arr;
    collectUnionComponent(arr, v);
    SSAArray cIds;
    for (jl_value_t *i : arr) {
        cIds.push_back(encodeJuliaValue(i));
    }
    SSAId unionId = encodePureTag(JL_UNION);
    SSAValues.emplace_back();
    auto &result = SSAValues.back();
    RawByteStream os(result);
    os << (uint16_t)JL_APPLY_TYPE << uint8_t(1) << uint8_t(0)
       << (uint32_t)(sizeof(SSAId) * (cIds.size() + 1));
    os << unionId;
    os << cIds;
    return SSAValues.size() - 1;
}
SSAId JuliaValueEncoder::encodeTypeName(jl_typename_t *t)
{
    // For normal type, typename refers to the wrapper
    // For global function type, typename refers to the typename directly
    // For lambda function type, typename refers to the wrapper
    SSAId nameBinding = encodeBinding(t->module, t->name);
    SSAValues.emplace_back();
    auto &result = SSAValues.back();
    RawByteStream os(result);
    os << (uint16_t)JL_TYPENAME << uint8_t(1) << uint8_t(0) << (uint32_t)(sizeof(SSAId));
    os << nameBinding;
    return SSAValues.size() - 1;
}
SSAId JuliaValueEncoder::encodeVararg(jl_value_t *v)
{
    jl_vararg_t *vm = (jl_vararg_t *)v;
    SSAId TId = ~(SSAId)0;
    SSAId NId = ~(SSAId)0;
    if (vm->T) {
        TId = encodeJuliaValue(vm->T);
    }
    if (vm->N) {
        NId = encodeJuliaValue(vm->N);
    }
    SSAValues.emplace_back();
    auto &result = SSAValues.back();
    RawByteStream os(result);
    os << (uint16_t)JL_NEW_VARARG << uint8_t(1) << uint8_t(0)
       << (uint32_t)(sizeof(SSAId) * 2);
    os << TId;
    os << NId;
    return SSAValues.size() - 1;
}
SSAId JuliaValueEncoder::encodeJuliaValue(jl_value_t *v)
{
    if (jl_typeof(v) == (jl_value_t *)jl_module_type) {
        jl_module_t *m = (jl_module_t *)v;
        return encodeBinding(m->parent, m->name);
    }
    else if (v == (jl_value_t *)jl_bottom_type) {
        return encodePureTag(JL_BOTTOM);
    }
    else if (v == (jl_value_t *)jl_emptytuple) {
        return encodePureTag(JL_EMPTY_TUPLE);
    }
    else if (v == (jl_value_t *)jl_emptytuple_type) {
        return encodePureTag(JL_TYPEOF_EMPTY_TUPLE);
    }
    else if (v == (jl_value_t *)jl_typeofbottom_type) {
        return encodePureTag(JL_TYPEOF_BOTTOM);
    }
    else if (v == (jl_value_t *)jl_vararg_type) {
        return encodePureTag(JL_VARARG_TYPE);
    }
    else if (jl_typeof(v) == (jl_value_t *)jl_vararg_type) {
        return encodeVararg(v);
    }
    else if (v == (jl_value_t *)jl_tuple_type) {
        return encodePureTag(JL_TUPLE);
    }
    else if (v == (jl_value_t *)jl_namedtuple_type) {
        return encodePureTag(JL_NAMED_TUPLE);
    }
    else if (jl_is_string(v)) {
        return encodeJLStringAsBytes(v);
    }
    else if (jl_is_symbol(v)) {
        return encodeSymbolAsBytes((jl_sym_t *)v);
    }
    else if (jl_is_datatype(v)) {
        return encodeDataType((jl_datatype_t *)v);
    }
    else if (jl_is_unionall(v)) {
        return encodeUnionAll(v);
    }
    else if (jl_is_typevar(v)) {
        return encodeTypeVar((jl_tvar_t *)v, false);
    }
    else if (jl_is_typename(v)) {
        return encodeTypeName((jl_typename_t *)v);
    }
    else if (jl_is_datatype(jl_typeof(v)) &&
             (jl_is_datatype_singleton((jl_datatype_t *)jl_typeof(v)))) {
        return encodeSingleton(v);
    }
    else if (jl_is_primitivetype(jl_typeof(v))) {
        return encodePrimitive(v);
    }
    else if (bindings.find(v) != bindings.end()) {
        jl_binding_t *bnd = bindings[v];
        return encodeBinding(bnd->owner, bnd->name);
    }
    else if (jl_is_immutable(jl_typeof(v))) {
        return encodeImmutable(v);
    }
    else if (jl_is_mutable_datatype(jl_typeof(v))) {
        return encodeRuntimePtr(v);
    }
    assert(0);
}
extern std::string juliaValueToString(jl_value_t *v);
std::string JuliaValueEncoder::encodeExternalJuliaValue(jl_value_t *v)
{
    std::string str;
    llvm::raw_string_ostream os(str);
    // concate all the SSAValue together
    encodeJuliaValue(v);
    for (auto &SSAValue : SSAValues) {
        os << encodeBytesAsString(SSAValue);
    }
    // llvm::dbgs() << "encoding:" << juliaValueToString(v) << os.str() << '\n';
    return str;
}

std::string JuliaValueEncoder::encodeJLBinding(jl_binding_t *bnd)
{
    (void)encodeBindingInternal(bnd->owner, bnd->name, JL_BND);
    std::string str;
    llvm::raw_string_ostream os(str);
    // concate all the SSAValue together
    for (auto &SSAValue : SSAValues) {
        os << encodeBytesAsString(SSAValue);
    }
    return str;
}

// used in codegen.cpp
void recordGlobalRef(jl_module_t *mod, jl_sym_t *name)
{
    jl_binding_t *bnd = jl_get_binding(mod, name);
    jl_value_t *v = jl_atomic_load(&(bnd->value));
    JuliaValueEncoder::bindings[v] = bnd;
}
// used in StaticJIT.cpp
jl_binding_t* tryGetGlobalRef(jl_value_t* v){
    auto iter = JuliaValueEncoder::bindings.find(v);
    if (iter != JuliaValueEncoder::bindings.end()){
        return iter->second;
    }
    else{
        return nullptr;
    }
}
std::string encodeJuliaValue(jl_value_t *v, bool &needInvalidated, jl_binding_t *bnd)
{
    JuliaValueEncoder encoder;
    if (bnd != nullptr && bnd->constp && jl_atomic_load_relaxed(&(bnd->value)) != NULL) {
        recordGlobalRef(bnd->owner, bnd->name);
    }
    auto value = encoder.encodeExternalJuliaValue(v);
    needInvalidated = !encoder.isRelocatable;
    return value;
}

std::string encodeJLBinding(jl_binding_t *bnd, bool &needInvalidated)
{
    JuliaValueEncoder encoder;
    auto value = encoder.encodeJLBinding(bnd);
    needInvalidated = !encoder.isRelocatable;
    return value;
}
// TODO: should we decode bytes here?
JuliaValueDecoder::JuliaValueDecoder(jl_module_t *mod, jl_array_t *rootArray,
                                     std::string &str)
  : JLModule(mod),
    SSAValues(rootArray),
    bytes(decodeStringAsBytes(str)),
    reader(llvm::StringRef((const char *)bytes.data(), bytes.size()),
           llvm::support::endian::system_endianness()){};

jl_vararg_t *jl_wrap_vararg(jl_value_t *t, jl_value_t *n)
{
    if (n) {
        if (jl_is_typevar(n)) {
            // TODO: this is disabled due to #39698; it is also inconsistent
            // with other similar checks, where we usually only check substituted
            // values and not the bounds of variables.
            /*
            jl_tvar_t *N = (jl_tvar_t*)n;
            if (!(N->lb == jl_bottom_type && N->ub == (jl_value_t*)jl_any_type))
                jl_error("TypeVar in Vararg length must have bounds Union{} and Any");
            */
        }
        else if (!jl_is_long(n)) {
            jl_type_error_rt("Vararg", "count", (jl_value_t *)jl_long_type, n);
        }
        else if (jl_unbox_long(n) < 0) {
            jl_errorf("Vararg length is negative: %zd", jl_unbox_long(n));
        }
    }
    jl_task_t *ct = jl_current_task;
    jl_vararg_t *vm =
        (jl_vararg_t *)jl_gc_alloc(ct->ptls, sizeof(jl_vararg_t), jl_vararg_type);
    vm->T = t;
    vm->N = n;
    return vm;
}
std::unordered_map<std::string, jl_module_t*> imported_modules;
void inspect_modules(){
    for (auto& KV:imported_modules){
        llvm::dbgs() << KV.first << '\n';
    }
}
extern JL_DLLEXPORT void jl_register_module_impl(jl_module_t* m){
    imported_modules[jl_symbol_name_(m->name)] = m;
}
extern JL_DLLEXPORT void (*jl_register_module)(jl_module_t*);
extern "C" JL_DLLEXPORT void jl_set_register_module_handle(){
    jl_register_module = jl_register_module_impl;
}

void *JuliaValueDecoder::decodeJuliaValue()
{
    JLTypeTag tag;
    uint16_t tagValue;
    uint8_t isInline;
    uint8_t hasChild;
    SSAId i = 0;
    while (!reader.empty()) {
        assert(reader.bytesRemaining() >= 4);
        assert(!reader.readInteger(tagValue));
        tag = (JLTypeTag)tagValue;
        assert(!reader.readInteger(isInline));
        assert(!reader.readInteger(hasChild));
        assert(hasChild == 0);
        uint32_t bytenum = 0;
        if (isInline) {
            assert(reader.bytesRemaining() >= 4);
            assert(!reader.readInteger(bytenum));
            assert(reader.bytesRemaining() >= bytenum);
        }
        switch (tag) {
        case (JL_CONST): assert(0); break;
        case (JL_BOTTOM):
            jl_array_ptr_1d_push(SSAValues, (jl_value_t *)jl_bottom_type);
            break;
        case (JL_EMPTY_TUPLE): {
            jl_array_ptr_1d_push(SSAValues, (jl_value_t *)jl_emptytuple);
            break;
        }
        case (JL_TYPEOF_EMPTY_TUPLE): {
            jl_array_ptr_1d_push(SSAValues, (jl_value_t *)jl_emptytuple_type);
            break;
        }
        case (JL_TYPEOF_BOTTOM):
            jl_array_ptr_1d_push(SSAValues, (jl_value_t *)jl_typeofbottom_type);
            break;
        case (JL_UNION):
            jl_array_ptr_1d_push(SSAValues, (jl_value_t *)jl_uniontype_type);
            break;
        case (JL_VARARG_TYPE):
            jl_array_ptr_1d_push(SSAValues, (jl_value_t *)jl_vararg_type);
            break;
        case (JL_TUPLE):
            jl_array_ptr_1d_push(SSAValues, (jl_value_t *)jl_tuple_type);
            break;
        case (JL_NAMED_TUPLE):
            jl_array_ptr_1d_push(SSAValues, (jl_value_t *)jl_namedtuple_type);
            break;
        case (JL_INLINE): assert(0); break;
        case (JL_SYMBOL): {
            assert(isInline);
            llvm::ArrayRef<uint8_t> Bytes;
            jl_value_t *result = nullptr;
            JL_GC_PUSH1(&result);
            assert(!reader.readBytes(Bytes, bytenum));
            assert(Bytes.back() == '\0');
            result = (jl_value_t *)jl_symbol((const char *)Bytes.data());
            jl_array_ptr_1d_push(SSAValues, result);
            JL_GC_POP();
            break;
        }
        case (JL_STRING): {
            assert(isInline);
            llvm::ArrayRef<uint8_t> Bytes;
            jl_value_t *result = nullptr;
            JL_GC_PUSH1(&result);
            assert(!reader.readBytes(Bytes, bytenum));
            assert(Bytes.back() == '\0');
            result = jl_pchar_to_string((const char *)Bytes.data(), Bytes.size() - 1);
            jl_array_ptr_1d_push(SSAValues, result);
            JL_GC_POP();
            break;
        }
        case (JL_PTR): {
            uint64_t ptrint;
            assert(!reader.readInteger(ptrint));
            jl_array_ptr_1d_push(SSAValues, (jl_value_t *)ptrint);
            isRelocatable = false;
            break;
        }
        case (JL_TYPENAME): {
            SSAId typeId;
            assert(!reader.readInteger(typeId));
            jl_value_t *t = evalSSA(typeId);
            jl_typename_t *tname;
            if (jl_is_datatype(t)) {
                tname = ((jl_datatype_t *)t)->name;
            }
            else {
                assert(jl_is_unionall(t));
                while (jl_is_unionall(t)) {
                    t = ((jl_unionall_t *)t)->body;
                }
                assert(jl_is_datatype(t));
                tname = ((jl_datatype_t *)t)->name;
            }
            jl_array_ptr_1d_push(SSAValues, (jl_value_t *)tname);
            break;
        }
        case (JL_FUNCTION): assert(0); break;
        case (JL_GLOBAL_REF): {
            llvm::ArrayRef<uint8_t> Bytes;
            assert(!reader.readBytes(Bytes, bytenum));
            size_t SSAnum = bytenum / sizeof(SSAId);
            assert(bytenum % sizeof(SSAId) == 0);
            llvm::ArrayRef<SSAId> params((SSAId *)Bytes.data(), SSAnum);
            jl_module_t *rootModule = JLModule;
            jl_value_t *result = nullptr;
            assert(params.size() >= 1);
            jl_value_t *rootModuleSymbol_ = evalSSA(params[0]);
            assert(jl_is_symbol(rootModuleSymbol_));
            jl_sym_t *rootModuleSymbol = (jl_sym_t *)rootModuleSymbol_;
            std::string moduleName = (const char *)jl_symbol_name_(rootModuleSymbol);
            if (rootModuleSymbol == jl_symbol("Core")) {
                rootModule = jl_core_module;
            }
            else if (rootModuleSymbol == jl_symbol("Base")) {
                rootModule = jl_base_module;
            }
            else if (rootModuleSymbol == jl_symbol("Main")) {
                rootModule = jl_main_module;
            }
            else if (rootModuleSymbol == JLModule->name) {
                rootModule = JLModule;
            }
            else {
                rootModule = jl_main_module;
            }
            for (size_t i = 0; i < params.size(); i++) {
                jl_value_t *paramValue = evalSSA(params[i]);
                assert(jl_is_symbol(paramValue));
                jl_value_t *tmp = nullptr;
                jl_binding_t *bnd = jl_get_binding(rootModule, (jl_sym_t *)paramValue);
                if (bnd == nullptr){
                    if (i == 0){
                        auto iter = imported_modules.find(jl_symbol_name_((jl_sym_t*)paramValue));
                        if (iter != imported_modules.end()){
                            tmp = (jl_value_t*)iter->second;
                        }
                    }
                    if (tmp == nullptr){
                        llvm::errs() << jl_symbol_name((jl_sym_t *)paramValue) << "doesn't exist";
                        assert(0);
                    }
                }
                else{
                    tmp = jl_atomic_load(&(bnd->value));
                }
                if (i != params.size() - 1) {
                    assert(jl_is_module(tmp));
                    rootModule = (jl_module_t *)tmp;
                }
                else {
                    result = tmp;
                }
            }
            assert(result);
            jl_array_ptr_1d_push(SSAValues, result);
            break;
        }
        case (JL_APPLY_TYPE): {
            llvm::ArrayRef<uint8_t> Bytes;
            assert(!reader.readBytes(Bytes, bytenum));
            size_t SSAnum = bytenum / sizeof(SSAId);
            assert(bytenum % sizeof(SSAId) == 0);
            llvm::ArrayRef<SSAId> params((SSAId *)Bytes.data(), SSAnum);
            // all parameters are rooted, so we don't need to re-root
            // notice that params also contain constructor
            std::vector<jl_value_t *> paramValues(params.size());
            for (size_t i = 0; i < params.size(); i++) {
                paramValues[i] = evalSSA(params[i]);
            }
            jl_value_t *result = nullptr;
            JL_GC_PUSH1(&result);
            result = jl_apply_type(paramValues[0], &(paramValues.data()[1]),
                                   paramValues.size() - 1);
            jl_array_ptr_1d_push(SSAValues, result);
            JL_GC_POP();
            assert(result);
            break;
        }
        case (JL_NEW_PRIMITIVE): {
            SSAId typeId;
            assert(bytenum >= 1);
            assert(!reader.readInteger(typeId));
            llvm::ArrayRef<uint8_t> Bytes;
            // We have read SSAId
            bytenum = bytenum - sizeof(SSAId);
            assert(!reader.readBytes(Bytes, bytenum));
            jl_value_t *result = nullptr;
            JL_GC_PUSH1(&result);
            jl_value_t *_dt = evalSSA(typeId);
            assert(jl_is_primitivetype(_dt));
            assert((size_t)jl_datatype_size(_dt) == bytenum);
            result = jl_new_bits(_dt, Bytes.data());
            jl_array_ptr_1d_push(SSAValues, result);
            JL_GC_POP();
            break;
        }
        case (JL_NEW_IMMUTABLE): {
            llvm::ArrayRef<uint8_t> Bytes;
            assert(!reader.readBytes(Bytes, bytenum));
            size_t SSAnum = bytenum / sizeof(SSAId);
            assert(bytenum % sizeof(SSAId) == 0);
            llvm::ArrayRef<SSAId> params((SSAId *)Bytes.data(), SSAnum);
            // all parameters are rooted, so we don't need to re-root
            // notice that params also contain constructor
            std::vector<jl_value_t *> paramValues(params.size());
            for (size_t i = 0; i < params.size(); i++) {
                paramValues[i] = evalSSA(params[i]);
            }
            jl_value_t *result = nullptr;
            JL_GC_PUSH1(&result);
            result = jl_new_structv((jl_datatype_t *)paramValues[0],
                                    &(paramValues.data()[1]), paramValues.size() - 1);
            jl_array_ptr_1d_push(SSAValues, result);
            JL_GC_POP();
            assert(result);
            break;
        }
        case (JL_NEW_TVAR): {
            SSAId nameid;
            assert(!reader.readInteger(nameid));
            SSAId lbid;
            assert(!reader.readInteger(lbid));
            SSAId upid;
            assert(!reader.readInteger(upid));
            jl_value_t *name = evalSSA(nameid);
            assert(jl_is_symbol(name));
            jl_value_t *lb = evalSSA(lbid);
            jl_value_t *up = evalSSA(upid);
            jl_value_t *result = nullptr;
            JL_GC_PUSH1(&result);
            result = (jl_value_t *)jl_new_typevar((jl_sym_t *)name, lb, up);
            jl_array_ptr_1d_push(SSAValues, result);
            JL_GC_POP();
            break;
        }
        case (JL_NEW_UNIONALL): {
            SSAId tvarid;
            assert(!reader.readInteger(tvarid));
            SSAId bodyid;
            assert(!reader.readInteger(bodyid));
            jl_value_t *tvar = evalSSA(tvarid);
            jl_value_t *body = evalSSA(bodyid);
            assert(jl_is_typevar(tvar));
            jl_value_t *result = nullptr;
            JL_GC_PUSH1(&result);
            result = jl_type_unionall((jl_tvar_t *)tvar, body);
            jl_array_ptr_1d_push(SSAValues, result);
            JL_GC_POP();
            break;
        }
        case (JL_SINGLETON): {
            SSAId typeId;
            assert(!reader.readInteger(typeId));
            jl_value_t *tmp = evalSSA(typeId);
            assert(jl_is_datatype(tmp));
            assert(((jl_datatype_t *)tmp)->instance);
            jl_array_ptr_1d_push(SSAValues, ((jl_datatype_t *)tmp)->instance);
            break;
        }
        case (JL_NEW_VARARG): {
            SSAId TId;
            assert(!reader.readInteger(TId));
            SSAId NId;
            assert(!reader.readInteger(NId));
            jl_value_t *T = nullptr;
            if (TId != ~(SSAId)0) {
                T = evalSSA(TId);
            }
            jl_value_t *N = nullptr;
            if (NId != ~(SSAId)0) {
                N = evalSSA(NId);
            }
            jl_value_t *result = nullptr;
            JL_GC_PUSH1(&result);
            result = (jl_value_t *)jl_wrap_vararg(T, N);
            ;
            jl_array_ptr_1d_push(SSAValues, result);
            JL_GC_POP();
            break;
        }
        default:
            if (tag == JL_BND) {
                llvm::ArrayRef<uint8_t> Bytes;
                assert(!reader.readBytes(Bytes, bytenum));
                size_t SSAnum = bytenum / sizeof(SSAId);
                assert(bytenum % sizeof(SSAId) == 0);
                llvm::ArrayRef<SSAId> params((SSAId *)Bytes.data(), SSAnum);
                jl_module_t *rootModule = JLModule;
                assert(params.size() >= 1);
                for (size_t i = 0; i < params.size(); i++) {
                    jl_value_t *paramValue = evalSSA(params[i]);
                    assert(jl_is_symbol(paramValue));
                    jl_value_t* tmp = nullptr;
                    jl_binding_t *bnd = jl_get_binding(rootModule, (jl_sym_t *)paramValue);
                    if (bnd == nullptr){
                        if (i == 0){
                            auto iter = imported_modules.find(jl_symbol_name_((jl_sym_t *)paramValue));
                            if (iter != imported_modules.end()){
                                tmp = (jl_value_t*)iter->second;
                            }
                        }
                        if (tmp == nullptr){
                            llvm::errs() << jl_symbol_name((jl_sym_t *)paramValue) << "doesn't exist";
                            assert(0);
                        }
                    }
                    else{
                        tmp = jl_atomic_load(&(bnd->value));
                    }
                    if (i != params.size() - 1) {
                        assert(jl_is_module(tmp));
                        rootModule = (jl_module_t *)tmp;
                    }
                    else {
                        // we need to return last binding;
                        return (void *)bnd;
                    }
                }
            }
            assert(0);
            break;
        }
        i += 1;
    }
    assert(jl_array_len(SSAValues) > 0);
    jl_value_t *result = jl_arrayref(SSAValues, jl_array_len(SSAValues) - 1);
    return result;
}

std::string juliaValueToString(jl_value_t *v)
{
    ios_t os;
    ios_mem(&os, 100);
    int n = jl_static_show((uv_stream_t *)&os, v);
    assert(os.bpos == n);
    std::string str(os.buf, n);
    ios_close(&os);
    return str;
}

// Maybe we should use more ArrayRef here!
void *decodeJuliaValue(jl_module_t *mod, std::string &str)
{
    jl_array_t *arr = nullptr;
    JL_GC_PUSH1(&arr);
    // allocate an array here
    arr = jl_alloc_array_1d((jl_value_t *)jl_array_any_type, 0);
    JuliaValueDecoder decoder(mod, arr, str);
    void *result = decoder.decodeJuliaValue();
    JL_GC_POP();
    return result;
}

void *decodeJLBinding(jl_module_t *mod, std::string &str)
{
    jl_array_t *arr = nullptr;
    JL_GC_PUSH1(&arr);
    // allocate an array here
    arr = jl_alloc_array_1d((jl_value_t *)jl_array_any_type, 0);
    JuliaValueDecoder decoder(mod, arr, str);
    void *result = decoder.decodeJuliaValue();
    JL_GC_POP();
    return result;
}

extern "C" JL_DLLEXPORT jl_value_t *jl_apply_generic_jit(jl_value_t *f, jl_value_t **args,
                                                         size_t nargs);
std::unordered_map<std::string, std::function<void *()>> specialSymbols = {
    {"__stack_chk_guard", [] { return (void *)&__stack_chk_guard; }},
    {"__stack_chk_fail", [] { return nullptr; }},
    {"__sigsetjmp", [] { return (void *)__sigsetjmp; }},
    {"jl_setjmp_name", [] { return (void *)jl_setjmp_name; }},
    {"jlvalue::top%Type{T}", [] { return (void *)jl_type_type->body; }},
    {"jlvalue::top%Ptr{T}", [] { return (void *)jl_pointer_type->body; }},
    {"ijl_apply_generic", [] { return (void *)jl_apply_generic_jit; }},
    {"jl_apply_generic", [] { return (void *)jl_apply_generic_jit; }},
    {"jl_world_counter", [] { return (void *)&jl_world_counter; }},
    {"ijl_world_counter", [] { return (void *)&jl_world_counter; }},
    {"julia::internal::jl_apply_generic", [] { return (void *)jl_apply_generic_jit; }},
    {"julia::internal::ijl_apply_generic", [] { return (void *)jl_apply_generic_jit; }}};


void *resolveExternJLValue(std::string s, std::string code,
                           std::function<void(jl_value_t *)> onResolve)
{
    void *value = nullptr;
    jl_module_t *mod = jl_main_module;
    auto iter = specialSymbols.find(s);
    if (iter != specialSymbols.end()) {
        return iter->second();
    }
    else if (startsWith(s, "jlexpr::")) {
        jl_value_t *exprStr = (jl_value_t *)decodeJuliaValue(mod, code);
        assert(jl_is_string(exprStr));
        jl_value_t *expr = nullptr;
        jl_value_t *filename = nullptr;
        JL_GC_PUSH2(&expr, &filename);
        filename = jl_cstr_to_string(jl_filename);
        expr = jl_parse_string(jl_string_ptr(exprStr), jl_string_len(exprStr), 0, 1);
        assert(jl_is_svec(expr));
        assert(jl_is_expr(jl_svec_ref((jl_svec_t*)expr, 0)));
        llvm::errs() << "Generate Expr :" << juliaValueToString(jl_svec_ref((jl_svec_t*)expr, 0));
        JL_GC_POP();
        return (void *)expr;
    }
    else if (startsWith(s, "jlbnd::")) {
        assert(mod);
        value = decodeJuliaValue(mod, code);
        assert(value);
    }
    else if (startsWith(s, "jlptr::")) {
        size_t offset = strlen("jlptr::");
        uint64_t i = 0;
        while (1) {
            char c = s[offset];
            if (c != ':') {
                i = 10 * i + (c - '0');
            }
            else {
                break;
            }
            offset += 1;
        }
        return (void *)i;
    }
    else if (startsWith(s, "jlslot::")) {
        assert(mod);
        value = decodeJuliaValue(mod, code);
        assert(value);
        // A double pointer for slot.
        value = (void *)&(((jl_binding_t *)value)->value);
    }
    else if (startsWith(s, "jlvalue::")) {
        assert(mod);
        value = decodeJLBinding(mod, code);
        onResolve((jl_value_t *)value);
        assert(value);
    }
    else if (startsWith(s, "julia::dylib::")) {
        size_t headOffset = strlen("julia::dylib::");
        size_t symOffset = s.find("::", headOffset);
        assert(symOffset != s.npos);
        std::string lib = s.substr(headOffset, symOffset - headOffset);
        std::string sym = s.substr(symOffset + 2);
        void *libhandle = jl_get_library_(lib.c_str(), 1);
        void *ptr;
        assert(jl_dlsym(libhandle, sym.c_str(), &ptr, 1));
        value = ptr;
    }
    else if (startsWith(s, "julia::internal::")) {
        size_t headOffsest = strlen("julia::internal::");
        std::string sym = s.substr(headOffsest);
        void *ptr;
        assert(jl_dlsym(jl_RTLD_DEFAULT_handle, sym.c_str(), &ptr, 1));
        value = ptr;
    }
    else if (startsWith(s, "ijl") || startsWith(s, "jl_")) {
        void *ptr;
        assert(jl_dlsym(jl_RTLD_DEFAULT_handle, s.c_str(), &ptr, 1));
        value = ptr;
    }
    else {
        // strange thing, it seems llvm will emit some symbols like fmod.
        void *ptr;
        assert(jl_dlsym(jl_RTLD_DEFAULT_handle, s.c_str(), &ptr, 1));
        value = ptr;
    }
    /*

        llvm::errs() << "Symbol not found, shouldn't happen here!" << '\n';
        llvm::errs() << s;
        assert(0);
    */
    assert(value != nullptr);
    return value;
}
/*
std::string juliaSymbolToString(jl_sym_t* sym){

}
std::string juliaBindingToString(jl_module_t* mod, jl_sym_t* sym){
    std::string str;
    llvm::SmallVector<jl_module_t *, 10> mods;
    while (mod->parent != mod) {
        mods.push_back(mod);
    }
    mods.push_back(mod);
    for (auto m:mods){
        llvm::raw_string_ostream(str) << juliaSymbolToString(m->name) << '.';
    }
    llvm::raw_string_ostream(str) << juliaSymbolToString(sym);
    return str;
}

std::string juliaBindingToString(jl_binding_t* bnd){
    return juliaBindingToString(bnd->owner, bnd->name);
}
*/
// This function is used to produce readable name for method instance and other values
extern "C" JL_DLLEXPORT jl_value_t *jl_decode_string(jl_value_t *s)
{
    assert(jl_is_string(s));
    size_t n = jl_string_len(s) / 2;
    assert(jl_string_len(s) % 2 == 0);
    const char *str = jl_string_ptr(s);
    jl_value_t *newStr = nullptr;
    JL_GC_PUSH1(&newStr);
    newStr = jl_alloc_string(n);
    char *newStrPtr = jl_string_data(newStr);
    for (size_t i = 0; i < n; i++) {
        // little endianness
        newStrPtr[i] = (hex2uint8(str[2 * i + 1]) << 4) + hex2uint8(str[2 * i]);
    }
    JL_GC_POP();
    return newStr;
}

extern "C" JL_DLLEXPORT jl_value_t *jl_encode_string(jl_value_t *s)
{
    assert(jl_is_string(s));
    size_t n = jl_string_len(s);
    const char *strPtr = jl_string_ptr(s);
    jl_value_t *newStr = nullptr;
    JL_GC_PUSH1(&newStr);
    newStr = jl_alloc_string(2 * n);
    char *newStrPtr = jl_string_data(newStr);
    for (size_t i = 0; i < n; i++) {
        // little endianness
        uint8_t byte = strPtr[i];
        newStrPtr[2 * i] = uint82hex(byte & 0b1111);
        newStrPtr[2 * i + 1] = uint82hex(byte >> 4);
    }
    JL_GC_POP();
    return newStr;
}

extern "C" JL_DLLEXPORT jl_value_t *jl_decodeJuliaValue(jl_value_t *mod, jl_value_t *str)
{
    assert(jl_is_string(str));
    std::string s = jl_string_ptr(str);
    auto v = decodeJuliaValue((jl_module_t *)mod, s);
    return (jl_value_t *)v;
}

extern "C" JL_DLLEXPORT jl_value_t *jl_encodeJuliaValue(jl_value_t *v)
{
    bool needInvalidated = false;
    std::string s = encodeJuliaValue(v, needInvalidated, nullptr);
    assert(!needInvalidated);
    return jl_pchar_to_string(s.data(), s.size());
}

void probeEncodeString(std::string str)
{
    jl_array_t *arr = nullptr;
    JL_GC_PUSH1(&arr);
    // allocate an array here
    arr = jl_alloc_array_1d((jl_value_t *)jl_array_any_type, 0);
    JuliaValueDecoder decoder(jl_main_module, arr, str);
    (void)decoder.decodeJuliaValue();
    jl_(arr);
    JL_GC_POP();
}

extern "C" JL_DLLEXPORT void jl_probeEncodeString(const char *ptr)
{
    probeEncodeString(ptr);
}