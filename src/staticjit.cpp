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
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/IR/LegacyPassManagers.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Support/DynamicLibrary.h>
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
#include "julia.h"
#include "julia_assert.h"
#include "julia_internal.h"
#include <iostream>
#include <jitlayers.h>
#include <string>
class StaticJuliaJIT;
extern StaticJuliaJIT *jl_StaticJuliaJIT;
extern TargetMachine *jl_TargetMachine;

// Currently LLVM doesn't provide MaterializtionUnit for object, so we use a modified
// version of LightMaterializtionUnit. The only difference is the `Create` method. But Since
// LightMaterializtionUnit is defined in an anonymous namespace, we can't wrap that class.
// We need to redefine the whole MU.

class ObjectMaterializationUnit : public MaterializationUnit {
private:
    struct LinkGraphInterface {
        SymbolFlagsMap SymbolFlags;
        SymbolStringPtr InitSymbol;
    };

public:
    static std::unique_ptr<ObjectMaterializationUnit>
    Create(ObjectLinkingLayer &ObjLinkingLayer,
           std::unique_ptr<llvm::object::ObjectFile> objfile,
           std::unique_ptr<llvm::MemoryBuffer> membuf)
    {
        if (auto G = llvm::jitlink::createLinkGraphFromObject(membuf->getMemBufferRef())) {
            auto LGI = scanLinkGraph(ObjLinkingLayer.getExecutionSession(), **G);
            return std::unique_ptr<ObjectMaterializationUnit>(new ObjectMaterializationUnit(
                ObjLinkingLayer, std::move(objfile), std::move(membuf), std::move(*G),
                std::move(LGI)));
        }
        else {
            llvm::errs() << G.takeError();
            jl_error("shouldn't be here");
            return nullptr;
        }
    }

    StringRef getName() const override { return G->getName(); }
    void materialize(std::unique_ptr<MaterializationResponsibility> MR) override
    {
        ObjLinkingLayer.emit(std::move(MR), std::move(G));
    }

private:
    static LinkGraphInterface scanLinkGraph(ExecutionSession &ES, LinkGraph &G)
    {
        LinkGraphInterface LGI;

        for (auto *Sym : G.defined_symbols()) {
            // Skip local symbols.
            if (Sym->getScope() == Scope::Local)
                continue;
            assert(Sym->hasName() && "Anonymous non-local symbol?");

            JITSymbolFlags Flags;
            if (Sym->getScope() == Scope::Default)
                Flags |= JITSymbolFlags::Exported;

            if (Sym->isCallable())
                Flags |= JITSymbolFlags::Callable;

            LGI.SymbolFlags[ES.intern(Sym->getName())] = Flags;
        }

        if ((G.getTargetTriple().isOSBinFormatMachO() && hasMachOInitSection(G)) ||
            (G.getTargetTriple().isOSBinFormatELF() && hasELFInitSection(G)))
            LGI.InitSymbol = makeInitSymbol(ES, G);

        return LGI;
    }

    static bool hasMachOInitSection(LinkGraph &G)
    {
        for (auto &Sec : G.sections())
            if (Sec.getName() == "__DATA,__obj_selrefs" ||
                Sec.getName() == "__DATA,__objc_classlist" ||
                Sec.getName() == "__TEXT,__swift5_protos" ||
                Sec.getName() == "__TEXT,__swift5_proto" ||
                Sec.getName() == "__TEXT,__swift5_types" ||
                Sec.getName() == "__DATA,__mod_init_func")
                return true;
        return false;
    }

    static bool hasELFInitSection(LinkGraph &G)
    {
        for (auto &Sec : G.sections())
            if (Sec.getName() == ".init_array")
                return true;
        return false;
    }

    static SymbolStringPtr makeInitSymbol(ExecutionSession &ES, LinkGraph &G)
    {
        std::string InitSymString;
        raw_string_ostream(InitSymString) << "$." << G.getName() << ".__inits" << Counter++;
        return ES.intern(InitSymString);
    }

    ObjectMaterializationUnit(ObjectLinkingLayer &ObjLinkingLayer,
                              std::unique_ptr<llvm::object::ObjectFile> objfile,
                              std::unique_ptr<llvm::MemoryBuffer> membuff,
                              std::unique_ptr<LinkGraph> G, LinkGraphInterface LGI)
      : MaterializationUnit(std::move(LGI.SymbolFlags), std::move(LGI.InitSymbol)),
        objfile(std::move(objfile)),
        membuff(std::move(membuff)),
        ObjLinkingLayer(ObjLinkingLayer),
        G(std::move(G))
    {
    }

    void discard(const JITDylib &JD, const SymbolStringPtr &Name) override
    {
        for (auto *Sym : G->defined_symbols())
            if (Sym->getName() == *Name) {
                assert(Sym->getLinkage() == Linkage::Weak &&
                       "Discarding non-weak definition");
                G->makeExternal(*Sym);
                break;
            }
    }
    std::unique_ptr<llvm::object::ObjectFile> objfile;
    std::unique_ptr<llvm::MemoryBuffer> membuff;
    ObjectLinkingLayer &ObjLinkingLayer;
    std::unique_ptr<LinkGraph> G;
    static std::atomic<uint64_t> Counter;
};
std::atomic<uint64_t> ObjectMaterializationUnit::Counter{0};

// Add an object to ObjectLinkingLayer. This is lazy, and ownership is transfered to ObjectMaterializationUnit.
// Materizalition happens when we look up symbols. Once materialization is done, the memory is released.
Error addobject(ObjectLinkingLayer &OL, JITDylib &JD,
                std::unique_ptr<llvm::object::ObjectFile> objfile,
                std::unique_ptr<llvm::MemoryBuffer> membuf)
{
    auto RT = JD.getDefaultResourceTracker();
    return JD.define(ObjectMaterializationUnit::Create(OL, std::move(objfile),
                                                       std::move(membuf)),
                     std::move(RT));
};

// A materializtion unit used for resolving external variables. They are mainly produced by literal_pointer_val and ccall.
// Currently this is unused,
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


bool startsWith(std::string mainStr, std::string toMatch)
{
    // std::string::find returns 0 if toMatch is found at starting
    if (mainStr.find(toMatch) == 0)
        return true;
    else
        return false;
};

// provide address for some special builtin value, like true and false.
std::vector<std::pair<std::string, uint64_t>> *SpecialJuliaRuntimeValue;
class JuliaRuntimeSymbolGenerator : public DefinitionGenerator {
public:
    Error tryToGenerate(LookupState &LS, LookupKind K, JITDylib &JD,
                        JITDylibLookupFlags JDLookupFlags,
                        const SymbolLookupSet &LookupSet) override
    {
        llvm::errs() << "Generate symbol for: ";
        llvm::errs() << JD.getName() << "\n";
        SymbolMap smap;
        SymbolNameVector v = LookupSet.getSymbolNames();
        for (auto i = v.begin(); i != v.end(); i++) {
            std::string s((**i).str().c_str());
            uint64_t addr = get_extern_name_addr(s);
            if (addr <= 0x100) {
                llvm::errs() << "Unresolved : Symbol " << s << "\n";
            }
            else {
                llvm::errs() << "Resolved   : Symbol " << s
                             << " with address : " << format("%p", (void *)addr) << "\n";
            }
            smap[*i] = JITEvaluatedSymbol(addr, JITSymbolFlags::FlagNames::Exported);
        }
        return JD.define(
            std::make_unique<AbsoluteSymbolsMaterializationUnit>(std::move(smap)));
    };
    // working horse to resolve symbols
    uint64_t get_extern_name_addr(std::string &s);
};

class StaticJuliaJIT {
public:
    enum ConstantType { String, Symbol, BitValue };
    llvm::TargetMachine *TM;

    SmallVector<char, 0> obj_Buffer;
    raw_svector_ostream obj_OS;
    llvm::legacy::PassManager PM;

    llvm::orc::ExecutionSession ES;
    /*(cantFail(llvm::orc::SelfExecutorProcessControl::Create()))*/
    llvm::jitlink::InProcessMemoryManager Memgr;
    llvm::orc::ObjectLinkingLayer ObjectLayer;
    JITDylib &JuliaBuiltinJD; // resolve builtin function name
    JITDylib &JuliaRuntimeJD; // resolve runtime julia value
    JITDylib &finalJD; // contain all the loaded object code
    std::map<StaticJuliaJIT::ConstantType, jl_value_t *> constpool;
    StaticJuliaJIT();
    void init_constpool();
};

StaticJuliaJIT::StaticJuliaJIT()
  : obj_Buffer(),
    obj_OS(obj_Buffer),
    PM(),
    ES(),
    Memgr(),
    ObjectLayer(ES, Memgr),
    // ijl_xxx and some builtin like julia.typeof.xxx
    JuliaBuiltinJD(ES.createBareJITDylib("JuliaBuiltin")),
    // deal with external name of julia.XXX.XXX
    JuliaRuntimeJD(ES.createBareJITDylib("JuliaRuntime")),
    // collect all the symbols of added object file
    finalJD(ES.createBareJITDylib("finalJD")),
    constpool()
{
    init_constpool();
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
    // JuliaRuntimeJD.addGenerator(std::make_unique<JuliaRuntimeSymbolGenerator>());
    std::string ErrorStr;
    if (sys::DynamicLibrary::LoadLibraryPermanently(nullptr, &ErrorStr)) {
        report_fatal_error("FATAL: unable to dlopen self\n" + ErrorStr);
    }
    // Fallback to resolve builtin symbols. In future it would resolve "julia::internal::XXX" symbol. 
    // But currently this is unused
    JuliaBuiltinJD.addGenerator(cantFail(
        orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(DL.getGlobalPrefix())));
    finalJD.addGenerator(std::make_unique<JuliaRuntimeSymbolGenerator>());
    JITDylibSearchOrder emptyorder;
    JuliaRuntimeJD.setLinkOrder(emptyorder, false);
    JuliaBuiltinJD.setLinkOrder(emptyorder, false);
    JITDylibSearchOrder linkorder;
    linkorder.push_back(
        {&JuliaBuiltinJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly});
    linkorder.push_back({&finalJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly});
    linkorder.push_back(
        {&JuliaRuntimeJD, orc::JITDylibLookupFlags::MatchExportedSymbolsOnly});
    finalJD.setLinkOrder(linkorder, false);
}
static void verify_root_type(jl_binding_t *b, jl_value_t *ty)
{
    assert(b->constp);
    assert(jl_isa(b->value, ty));
}


// root some constant value
void StaticJuliaJIT::init_constpool()
{
    jl_binding_t *stringpool_binding =
        jl_get_binding(jl_main_module, jl_symbol("StringPool"));
    assert(stringpool_binding != NULL);
    // TODO: verify the string pool type...
    // since we only use global rooted type, there is no need to create GC frame.
    // jl_box_int64(1);
    verify_root_type(stringpool_binding, (jl_value_t *)jl_array_type);
    constpool[StaticJuliaJIT::ConstantType::String] =
        jl_atomic_load_relaxed(&(stringpool_binding->value));

    jl_binding_t *symbolpool_binding =
        jl_get_binding(jl_main_module, jl_symbol("SymbolPool"));
    assert(symbolpool_binding != NULL);
    // TODO: verify the string pool type...
    // since we only use global rooted type, there is no need to create GC frame.
    // jl_box_int64(1);
    verify_root_type(symbolpool_binding, (jl_value_t *)jl_array_type);
    constpool[StaticJuliaJIT::ConstantType::Symbol] =
        jl_atomic_load_relaxed(&(symbolpool_binding->value));


    jl_binding_t *bitvalue_pool = jl_get_binding(jl_main_module, jl_symbol("BitValuePool"));
    assert(bitvalue_pool != NULL);
    // TODO: verify the string pool type...
    // since we only use global rooted type, there is no need to create GC frame.
    // jl_box_int64(1);
    verify_root_type(bitvalue_pool, (jl_value_t *)jl_array_type);
    constpool[StaticJuliaJIT::ConstantType::BitValue] =
        jl_atomic_load_relaxed(&(bitvalue_pool->value));
}


uint8_t decodeChar(char c)
{
    assert((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    else {
        return (uint8_t)(c - 'A' + 10);
    }
}
void decodeBytes(uint8_t *buffer, const char *c, size_t len)
{
    assert(strlen(c) >= 2 * len);
    for (size_t i = 0; i < len; i++) {
        buffer[i] = (decodeChar(c[2 * i]) << 4) + decodeChar(c[2 * i + 1]);
    }
}

const char *decodeImmutableValueInternal(jl_value_t *_dt, const char *bytes,
                                         uint8_t *buffer, jl_array_t *root)
{
    assert(jl_is_immutable_datatype(_dt) && ((jl_datatype_t *)_dt)->isconcretetype);
    jl_datatype_t *dt = (jl_datatype_t *)_dt;
    size_t nfields = jl_datatype_nfields(dt);
    // if dt is a primitive type, we directly copy the value to the buffer
    if (dt->layout && nfields == 0 && dt != jl_string_type && dt != jl_symbol_type) {
        size_t byte_num = jl_datatype_size(dt);
        assert(byte_num > 0);
        decodeBytes(buffer, bytes, byte_num);
        return bytes + 2 * byte_num;
    }
    else {
        for (size_t i = 0; i < nfields; i++) {
            jl_value_t *_fieldtype = jl_svec_ref(dt->types, i);
            // TODO: what about abstract field type
            assert(jl_is_datatype(_fieldtype));
            jl_datatype_t *fieldtype = (jl_datatype_t *)_fieldtype;
            size_t offset = jl_field_offset(dt, i);
            if (fieldtype == jl_string_type || fieldtype == jl_symbol_type) {
                assert(bytes[0] == 'c' || bytes[0] == 's');
                size_t start = 1;
                size_t len = 0;
                char endc;
                while (1) {
                    endc = bytes[start];
                    if (endc != 's' && endc != 'c') {
                        assert(endc >= '0' && endc <= '9');
                        len = len * 10 + (endc - '0');
                    }
                    else {
                        break;
                    }
                    start += 1;
                }
                start += 1;
                std::vector<uint8_t> sbuffer(len);
                // decode the string to a buffer
                decodeBytes(sbuffer.data(), &bytes[start], len);
                uint64_t ptr;
                if (endc == 's') {
                    jl_sym_t *sym = NULL;
                    JL_GC_PUSH1(&sym);
                    sym = jl_symbol_n((const char *)sbuffer.data(), len);
                    jl_array_ptr_1d_push(root, (jl_value_t *)sym);
                    JL_GC_POP();
                    ptr = (uint64_t)sym;
                }
                else {
                    jl_value_t *str = NULL;
                    JL_GC_PUSH1(&str);
                    str = jl_pchar_to_string((const char *)sbuffer.data(), len);
                    jl_array_ptr_1d_push(root, str);
                    JL_GC_POP();
                    ptr = (uint64_t)str;
                }
                // write the pointer to the buffer
                *(uint64_t *)(buffer + offset) = ptr;
                bytes = &(bytes[start + 2 * len]);
                // buffer += 8;
            }
            else if (fieldtype->layout && jl_datatype_nfields(fieldtype) == 0) {
                size_t byte_num = jl_datatype_size(fieldtype);
                if (byte_num == 0) {
                    continue;
                }
                decodeBytes(&buffer[offset], bytes, byte_num);
                // consume 2 * bytenum input
                bytes += byte_num * 2;
            }
            else if (jl_is_immutable_datatype(_fieldtype) &&
                     ((jl_datatype_t *)_fieldtype)->isconcretetype) {
                bytes =
                    decodeImmutableValueInternal(_fieldtype, bytes, &buffer[offset], root);
            }
            else {
                // jl_safe_printf("abstract value is not supported!");
                // 16 is a pointer size;
                jl_binding_t *bnd =
                    jl_get_binding(jl_main_module, jl_symbol("SharedString"));
                *(uint64_t *)(buffer + offset) =
                    (uint64_t)jl_atomic_load_relaxed(&bnd->value);
                // decodeBytes(&buffer[offset], bytes, 8);
                bytes += 16;
                // jl_error("abstract value is not supported!");
            }
        }
    }
    return bytes;
}
// need to take alignment into consider...
// maybe just copy code from staticdata.c ?
// decode value from bytes and write the data to buffer
jl_value_t *decodeImmutableValue(jl_value_t *_dt, const char *bytes)
{
    jl_array_t *root = NULL;
    JL_GC_PUSH1(&root);
    root = jl_alloc_array_1d(jl_array_any_type, 0);
    assert(jl_is_immutable_datatype(_dt) && ((jl_datatype_t *)_dt)->isconcretetype);
    jl_datatype_t *dt = (jl_datatype_t *)_dt;
    std::vector<uint8_t> buffer(dt->size);
    for (size_t i = 0; i < buffer.size(); i++) {
        buffer[i] = 0;
    }
    bytes = decodeImmutableValueInternal(_dt, bytes, (uint8_t *)buffer.data(), root);
    assert(*bytes == '\0');
    // jl_value_t* jl_new_v = NULL;
    jl_value_t *jl_new_v = jl_new_bits(_dt, buffer.data());
    JL_GC_POP();
    return jl_new_v;
}
uint64_t JuliaRuntimeSymbolGenerator::get_extern_name_addr(std::string &s)
{
    // This is corresponds to cgutils.cpp's definition
    // auto iter = SpecialJuliaRuntimeValue.find(s);
    // llvm::errs() << "Input: " << s << " " << s.size() << "\n";
    for (auto i = SpecialJuliaRuntimeValue->begin(); i != SpecialJuliaRuntimeValue->end();
         i++) {
        // llvm::errs() << "Check: " << i->first << " " << i->first.size() << "\n";
        // llvm::errs() << "Result" << i->first.compare(s) << "\n";
        if (i->first.compare(s) == 0) {
            return i->second;
        }
    }
    uint64_t offset = 0;
    const char *dataptr = nullptr;
    if (startsWith(s, "julia::const::module::")) {
        offset = strlen("julia::const::module::");
    }
    else if (startsWith(s, "julia::const::datatype::")) {
        offset = strlen("julia::const::datatype::");
    }
    else if (startsWith(s, "julia::const::typename::")) {
        offset = strlen("julia::const::typename::");
    }
    else if (startsWith(s, "julia::const::generic_function::")) {
        offset = strlen("julia::const::generic_function::");
    }
    else if (startsWith(s, "julia::const::methodinstance::ROOT")) {
        /*
        jl_value_t* comp_module_v = jl_get_binding(jl_core_module, jl_symbol("Compiler"));
        assert(jl_is_module(comp_module_v));
        jl_module_t* comp_module = (jl_module_t*)comp_module_v;
        jl_value_t* root = jl_get_binding(comp_module, jl_symbol("ROOT"));
        */
        return 0x5;
    }
    else if (startsWith(s, "julia::const::symbol::")) {
        offset = strlen("julia::const::symbol::");
        dataptr = s.c_str() + offset;
        jl_sym_t *newsym;
        JL_GC_PUSH1(&newsym);
        newsym = jl_symbol(dataptr);
        jl_value_t *jl_symbol_pool =
            jl_StaticJuliaJIT->constpool[StaticJuliaJIT::ConstantType::Symbol];
        jl_array_ptr_1d_push((jl_array_t *)jl_symbol_pool, (jl_value_t *)newsym);
        JL_GC_POP();
        return (uint64_t)newsym;
    }
    else if (startsWith(s, "julia::const::singleton::")) {
        offset = strlen("julia::const::singleton::");
    }
    else if (startsWith(s, "julia::const::bitvalue::String::")) {
        offset = strlen("julia::const::bitvalue::String::");
        size_t slen = (s.size() - offset) / 2 + 1;
        dataptr = s.c_str() + offset;
        std::vector<char> v(slen);
        for (size_t i = 0; i < slen; i += 1) {
#define decode(c) ((c) >= 'A' ? (c) - 'A' + 10 : (c) - '0')
            char c = (decode(dataptr[2 * i]) << 4) + decode(dataptr[2 * i + 1]);
            v[i] = c;
#undef decode
        }
        v[slen - 1] = '\0';
        // not quite correct, we need to root the value!
        // jl_value_t* val = jl_cstr_to_string((const char*)v.data());
        // jl_cstr_to_string will copy the data, so we don't need to worry about
        // ownership here
        jl_value_t *js = NULL;
        JL_GC_PUSH1(&js);
        js = jl_cstr_to_string(v.data());
        jl_value_t *jl_string_pool =
            jl_StaticJuliaJIT->constpool[StaticJuliaJIT::ConstantType::String];
        jl_array_ptr_1d_push((jl_array_t *)jl_string_pool, js);
        JL_GC_POP();
        return (uint64_t)js;
    }
    else if (startsWith(s, "julia::const::bitvalue::")) {
        std::string remains = s.substr(strlen("julia::const::bitvalue::"));
        size_t index = remains.find("::");
        jl_value_t *dt = jl_eval_string(remains.substr(0, index).c_str());
        // assert(dt != NULL);
        // offset by "::"
        jl_value_t *bitv = NULL;
        JL_GC_PUSH1(&bitv);
        // not a bitvalue
        // if (dt == (jl_value_t*)jl_unionall_type || !(jl_is_immutable_datatype(dt) &&
        // ((jl_datatype_t*)dt)->isconcretetype)){
        //    return (uint64_t)0x3;
        //}
        if (dt == (jl_value_t *)jl_uint64_type) {
            bitv = decodeImmutableValue(dt, remains.c_str() + index + 2);
        }
        else {
            bitv = decodeImmutableValue(dt, remains.c_str() + index + 2);
        }
        if (bitv != nullptr) {
            jl_array_ptr_1d_push((jl_array_t *)jl_StaticJuliaJIT
                                     ->constpool[StaticJuliaJIT::ConstantType::BitValue],
                                 bitv);
        }
        JL_GC_POP();
        return (uint64_t)bitv;
    }
    else if (startsWith(s, "julia::const_global::mutable::")) {
        offset = strlen("julia::const_global::mutable::");
    }
    else if (startsWith(s, "julia::globalslot::")) {
        offset = strlen("julia::globalslot::");
        size_t modnameend = s.find("::", offset);
        if (modnameend == s.npos) {
            jl_printf(JL_STDERR, s.c_str());
            jl_printf(JL_STDERR, "module is not existed!");
            assert(0);
            return (uint64_t)0x3;
        }
        std::string modname = s.substr(offset, modnameend - offset);
        std::string symname = s.substr(modnameend + 2);
        jl_value_t *v = jl_eval_string(modname.c_str());
        assert(jl_is_module(v));
        jl_binding_t *b =
            jl_get_module_binding((jl_module_t *)v, jl_symbol(symname.c_str()));
        return (uint64_t)(&(b->value));
    }
    else if (startsWith(s, "julia::internal::")) {
        void *symaddr;
        const char *libsym = s.c_str() + strlen("julia::internal::");
        if (!jl_dlsym(jl_libjulia_internal_handle, libsym, &symaddr, 0)) {
            jl_printf(JL_STDERR, s.c_str());
            jl_printf(JL_STDERR, "internal symbol not existed!");
            assert(0);
        }
        else {
            return (uint64_t)(symaddr);
        }
    }
    else if (startsWith(s, "julia::dylib::")) {
        // external symbol
        offset = strlen("julia::dylib::");
        size_t libnameend = s.find("::", offset);
        if (libnameend == s.npos) {
            assert(0);
            return (uint64_t)0x3;
        }
        std::string libname_s = s.substr(offset, libnameend - offset);
        const char *libname = libname_s.c_str();
        const char *libsym = s.c_str() + libnameend + 2;
        void *symaddr;
        void *libhandle = jl_get_library_(libname, 0);
        if (!libhandle || !jl_dlsym(libhandle, libsym, &symaddr, 0)) {
            assert(0);
            return (uint64_t)0x3;
        }
        else {
            return (uint64_t)(symaddr);
        }
    }
    dataptr = s.c_str() + offset;
    jl_value_t *v = jl_eval_string(dataptr);
    if (startsWith(s, "julia::const::singleton::")) {
        // we need to check whether this has instance
        if (jl_is_datatype(v)) {
            return (uint64_t)(((jl_datatype_t *)v)->instance);
        }
        else {
            jl_(v);
            jl_safe_printf("This is incorrect for singelton type!");
            return 0;
        }
    }
    return (uint64_t)v;
}
StaticJuliaJIT *jl_StaticJuliaJIT = nullptr;
extern "C" JL_DLLEXPORT JITTargetAddress jl_lookup(jl_value_t *symbol)
{
    assert(jl_isa(symbol, (jl_value_t *)jl_symbol_type));
    if (jl_StaticJuliaJIT == nullptr) {
        jl_StaticJuliaJIT = new StaticJuliaJIT();
    }
    SymbolStringPtr namerefptr =
        jl_StaticJuliaJIT->ES.intern(StringRef(jl_symbol_name((jl_sym_t *)symbol)));
    auto symOrErr = jl_StaticJuliaJIT->ES.lookup(
        {/*&jl_StaticJuliaJIT->JuliaBuiltinJD,*/
         &jl_StaticJuliaJIT->finalJD,
         /*&jl_StaticJuliaJIT->JuliaRuntimeJD*/},
        namerefptr);
    if (auto Err = symOrErr.takeError()) {
        llvm::errs() << Err;
        jl_error("shouldn't be here");
        return 0;
    }
    else {
        // auto table = jl_StaticJuliaJIT->JuliaRuntimeJD.Symbols;
        /*
        for (auto i = table.begin();i != table.end();i++){
            std::cout << *(*i->first).str();
            llvm::orc::JITDylib::Sym entry = (*i->second);
            std::cout << (int)(getState());
            std::cout << i->
        }
        */
        return symOrErr.get().getAddress();
    }
}

extern void jl_init_staticjit()
{
    if (jl_StaticJuliaJIT == nullptr) {
        jl_StaticJuliaJIT = new StaticJuliaJIT();
        SpecialJuliaRuntimeValue = new std::vector<std::pair<std::string, uint64_t>>(
            {{"julia::const::bool::true", (uint64_t)jl_true},
             {"julia::const::bool::false", (uint64_t)jl_false}});
    }
}
extern "C" JL_DLLEXPORT void jl_compile_objects(jl_value_t *objectfilearray)
{
    jl_init_staticjit();
    assert(jl_subtype(jl_typeof(objectfilearray), (jl_value_t *)jl_array_type));
    std::vector<std::string> objectfiles;
    for (unsigned int i = 0; i < jl_array_len(objectfilearray); i++) {
        jl_value_t *item = jl_arrayref((jl_array_t *)objectfilearray, i);
        assert(jl_typeof(item) == (jl_value_t *)jl_string_type);
        const char *ptr = jl_string_data(item);
        std::string s(ptr);
        objectfiles.push_back(std::move(s));
    }
    // std::make_unique<llvm::orc::IRCompileLayer>(this) is used to call this() to compile
    // IR code;
    // llvm::orc::IRCompileLayer CompileLayer(
    //    ES, ObjectLayer, std::make_unique<llvm::orc::IRCompileLayer>(this));
    // we can directly construct a JITDylib by ES and name!
    // but the link order of this JITDylib is default to itself. And we need to manage the
    // memory by ourself.

    // finalJD.setLinkOrder(std::move(JITDylibSearchOrder),true);
    // if we know the link order of the object files, we can build a JITDylib for each
    // object and link them manually
    /*
        Consider this case:
            Suppose that we have object file one with `f` call `g`, f defined, and object
       file two with `g` call `f`, g defined. We have one JITDylib and two materialization
       unit. When we materialize first object file, it will resolve address of f and then
       look up symbol g in the second object file, The lookup will trigger materialization
       of second object file, which produce the address of f and it will look up the address
       of symbol f in first object file, which we already have.
    */
    for (unsigned int i = 0; i < objectfiles.size(); i++) {
        std::string &filename = objectfiles[i];
        StringRef sref(filename);
        auto objorerr = object::ObjectFile::createObjectFile(sref);
        if (Error Err = objorerr.takeError()) {
            // printf("Encounter error while loading object file!");
            llvm::errs() << Err;
            jl_error("shouldn't be here");
        }
        auto obj = objorerr.get().takeBinary();
        std::unique_ptr<llvm::object::ObjectFile> objfile = std::move(std::get<0>(obj));
        std::unique_ptr<llvm::MemoryBuffer> membuf = std::move(std::get<1>(obj));
        // MemoryBufferRef ObjBuffer = membuf->getMemBufferRef();
        addobject(jl_StaticJuliaJIT->ObjectLayer, jl_StaticJuliaJIT->finalJD,
                  std::move(objfile), std::move(membuf));
        // then we load objfile into memory by ObjectLayer
        // it seems that we can only add LLVM IR into IRCompilerLayer
        // or add LinkGraph into ObjectLinkLayer, or directly emit an object file.
        // What about a RtDyldObjectLinkingLayer
        // It seems that it's designed for object with relocatable section?
        // auto Ctx =
        // std::make_unique<llvm::orc::ObjectLinkingLayerJITLinkContext>(ObjectLayer,
        // std::move(R), std::move(O)); if (auto G = createLinkGraphFromObject(ObjBuffer)) {
        // Ctx->notifyMaterializing(**G);
        //}
    }
}


char *outputdir = NULL;
extern "C" JL_DLLEXPORT void jl_setoutput_dir(jl_value_t *dir)
{
    assert(jl_is_string(dir));
    outputdir = (char *)jl_string_ptr(dir);
}
extern std::pair<std::unique_ptr<Module>, jl_llvm_functions_t>
emit_function(jl_method_instance_t *lam, jl_code_info_t *src, jl_value_t *jlrettype,
              jl_codegen_params_t &params, bool vaOverride = false);
extern int8_t jl_force_object_gen;
std::map<jl_method_instance_t *, std::tuple<std::vector<jl_method_instance_t *>, uint64_t>>
    has_emitted;

// Reuse optimizer from jitlayer.cpp
static int UniqueID = 0;
extern void addTargetPasses(legacy::PassManagerBase *PM, TargetMachine *TM);
extern void addMachinePasses(legacy::PassManagerBase *PM, TargetMachine *TM);
extern void addOptimizationPasses(legacy::PassManagerBase *PM, int opt_level,
                                  bool lower_intrinsics, bool dump_native);

/*
    Find the unique cached code instance for a method instance. If there are more than one, raise a warning.
    This is to prevent bad cache and we only use method instance for codegen instead of code instance.
    So a one-to-one corresponding is importance here.
    We must be careful about Julia's GC and ensure all the data are correctly rooted.
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
                        "Warning: More than one matched code instance! (maybe some bootstraped code?)");
                    //jl_(ci);
                    //jl_(codeinst);
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
    assert(jl_is_method(mi->def.method) && jl_symbol_name(mi->def.method->name)[0] != '@');
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

void removeUnusedExternal(llvm::Module *M)
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

// Currently this function is not used
// Since we haven't bundle object files into static libraries.
/*
static object::Archive::Kind getDefaultForHost(Triple &triple)
{
    if (triple.isOSDarwin())
        return object::Archive::K_DARWIN;
    return object::Archive::K_GNU;
}
*/
extern "C" JL_DLLEXPORT void jl_compile_methodinst_recursively(jl_method_instance_t *mi,
                                                               char *dir, size_t world,
                                                               bool recur)
{
    jl_init_staticjit();
    // store all the object files into a directory
    const char *dirname = dir;
    std::vector<jl_code_instance_t *> workqueue;
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
    workqueue.push_back(first_codeinst);
    llvm::legacy::PassManager &PM = jl_StaticJuliaJIT->PM;
    // step 2: work on workqueue
    while (!workqueue.empty()) {
        jl_code_instance_t *curr_codeinst = workqueue.back();
        workqueue.pop_back();
        jl_code_instance_t *new_curr_codeinst =
            prepare_code_instance(curr_codeinst->def, world);
        // In some rare cases this will happen for bootstrap code. Raise a warning here.
        if (new_curr_codeinst != curr_codeinst) {
            //jl_(new_curr_codeinst);
            //jl_(curr_codeinst);
            jl_safe_printf("Two different code instance, Shouldn't be uninferred!");
            curr_codeinst = new_curr_codeinst;
        }
        jl_method_instance_t *curr_methinst = curr_codeinst->def;
        // if we have emitted the code, skip this method instance;
        if (has_emitted.find(curr_methinst) != has_emitted.end()) {
            continue;
        }
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
        std::vector<jl_method_instance_t *> call_targets;
        while (!params.workqueue.empty()) {
            jl_method_instance_t *target = std::get<0>(params.workqueue.back())->def;
            workqueue.push_back(std::get<0>(params.workqueue.back()));
            call_targets.push_back(target);
            params.workqueue.pop_back();
        }
        // emitted[curr_meth_inst] = call_targets;
        // emit module!
        std::unique_ptr<llvm::Module> mod = std::move(std::get<0>(result));
        removeUnusedExternal(mod.get());
        std::string s = "";
        // trunc the long filename. We maybe need to use hash (MD5) to store file.
        // long symbol names with special symbols in object file and LLVM module are fine.
        llvm::raw_string_ostream(s)
            << dirname << "/" << UniqueID << std::get<1>(result).specFunctionObject;
        UniqueID += 1;
        std::string s1 = s.substr(0, 200);
        s1 += ".ll";
        llvm::StringRef sref1(s1);
        std::error_code ec;
        llvm::raw_fd_ostream fos1(sref1, ec);
        mod->print(fos1, nullptr);
        fos1.close();
        // run pass can trigger undefine symbol error, so we need to emit external
        // symbol correctly this will also emit object file to objOS
        /*
            We should add a IR verification pass here, since sometimes imagecodegen will produce incorrect IR.
            But verifyModule doesn't work, it seems that strict check can only happen if we try to parse LLVM IR (or use a debug version of LLVM).
        }
        */
        PM.run(*mod);
        removeUnusedExternal(mod.get());
        std::string s2 = s.substr(0, 200) + "-optimize.ll";
        llvm::StringRef sref2(s2);
        llvm::raw_fd_ostream fos2(sref2, ec);
        mod->print(fos2, nullptr);
        fos2.close();
        llvm::SmallVector<char, 0U> emptyBuffer;
        jl_StaticJuliaJIT->obj_Buffer.swap(emptyBuffer);
        {
            std::string s3 = s.substr(0, 200) + ".o";
            std::ofstream file(s3, std::ios::binary);
            file.write(emptyBuffer.data(), emptyBuffer.size());
        }
        std::unique_ptr<MemoryBuffer> ObjBuffer(
            new SmallVectorMemoryBuffer(std::move(emptyBuffer)));
        auto objorerr = object::ObjectFile::createObjectFile(ObjBuffer->getMemBufferRef());
        if (Error Err = objorerr.takeError()) {
            llvm::errs() << Err;
            jl_error("shouldn't be here");
        }
        std::unique_ptr<llvm::object::ObjectFile> objfile = std::move(objorerr.get());
        // MemoryBufferRef ObjBuffer = membuf->getMemBufferRef();
        /*
        llvm::MemoryBufferRef mbref(objfile.get()->getData(), name_from_method_instance());
        llvm::NewArchiveMember mem(mbref);
        llvm::ArrayRef<llvm::NewArchiveMember> ref(men);
        llvm::object::Archive::Kind Kind =
        getDefaultForHost(jl_StaticJuliaJIT->TM->getTargetTriple());
        writeArchive(StringRef(s3),ref, true, Kind, true, false);
        */
        addobject(jl_StaticJuliaJIT->ObjectLayer, jl_StaticJuliaJIT->finalJD,
                  std::move(objfile), std::move(ObjBuffer));
        has_emitted[curr_methinst] =
            std::make_tuple<std::vector<jl_method_instance_t *>, uint64_t>(
                std::move(call_targets), 0);
        if (!recur) {
            break;
        }
    }
    return;
}
class AdressSort {
public:
    bool operator()(std::pair<std::string, uint64_t> &v1,
                    std::pair<std::string, uint64_t> &v2) const
    {
        return v1.second < v2.second;
    };
};

// A cheap way to debug assembly code, we have all the information for emitted symbol.
void debug_address()
{
    std::vector<std::pair<std::string, uint64_t>> queue;
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
    std::sort(queue.begin(), queue.end(), AdressSort());
    llvm::errs() << "\n==========\nAddress :\n";
    for (auto i = queue.begin(); i != queue.end(); i++) {
        llvm::errs() << i->first << " : " << format("%p", (void *)i->second) << "\n";
    }
    llvm::errs() << "\n==========\nEnd of Address :\n";
}

extern "C" JL_DLLEXPORT void *jl_compile_one_methodinst(jl_method_instance_t *mi,
                                                        size_t world)
{
    jl_init_staticjit();
    if (outputdir != NULL) {
        auto iter = has_emitted.find(mi);
        // assert(src!= NULL && jl_is_code_info(src));
        void *codeinst;
        assert(outputdir != NULL);
        if (iter == has_emitted.end()) {
            std::string s;
            llvm::raw_string_ostream(s)
                << "julia::method::invoke::" << name_from_method_instance(mi);
            SymbolStringPtr namerefptr = jl_StaticJuliaJIT->ES.intern(StringRef(s));
            auto symOrErr = jl_StaticJuliaJIT->ES.lookup(
                {/*&jl_StaticJuliaJIT->JuliaBuiltinJD,*/
                 &jl_StaticJuliaJIT->finalJD,
                 /*&jl_StaticJuliaJIT->JuliaRuntimeJD*/},
                namerefptr);
            // we can't find the symbol
            auto Err = symOrErr.takeError();
            if (!Err) {
                auto addr = symOrErr.get().getAddress();
                // A bad symbol, discard this symbol.
                if (addr < 0x100) {
                    llvm::orc::SymbolNameSet set({namerefptr});
                    jl_StaticJuliaJIT->finalJD.remove(set);
                }
                else {
                    return (void *)addr;
                }
            }
            jl_compile_methodinst_recursively(mi, outputdir, world, true);
            iter = has_emitted.find(mi);
            assert(iter != has_emitted.end());
        }
        uint64_t &invokeptr = std::get<1>(iter->second);
        if (invokeptr == 0) {
            codeinst = NULL;
            std::string s;
            llvm::raw_string_ostream(s)
                << "julia::method::invoke::" << name_from_method_instance(mi);
            auto addr = jl_lookup((jl_value_t *)jl_symbol(s.c_str()));
            llvm::errs() << "julia::method::invoke::" << name_from_method_instance(mi)
                         << " with address : " << format("%p", (void *)addr) << "\n";
            std::string s1;
            llvm::raw_string_ostream(s1)
                << "julia::method::specfunc::" << name_from_method_instance(mi);
            auto newaddr = jl_lookup((jl_value_t *)jl_symbol(s1.c_str()));
            llvm::errs() << "julia::method::specfunc::" << name_from_method_instance(mi)
                         << " with address : " << format("%p", (void *)newaddr) << "\n";
            invokeptr = addr;
            //debug_address();
        }
        codeinst = (void *)invokeptr;
        return codeinst;
    }
    jl_error("Dir is unset or in a wrong mode!");
    return NULL;
}