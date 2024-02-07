#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/Layer.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#define private public
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#undef private
#include "julia.h"
#include <unordered_map>
#include "JuliaObjectFile.h"

using namespace llvm;
using namespace llvm::jitlink;
using namespace llvm::orc;

class JuliaObjectLinkingLayerJITLinkContext;
class StaticJuliaJIT;

/// An ObjectLayer implementation built on JITLink.
///
/// Clients can use this class to add relocatable object files to an
/// ExecutionSession, and it typically serves as the base layer (underneath
/// a compiling layer like IRCompileLayer) for the rest of the JIT
class JuliaObjectLinkingLayer : public ObjectLinkingLayer {
    friend class JuliaObjectLinkingLayerJITLinkContext;
public:
    JuliaObjectLinkingLayer(StaticJuliaJIT* jit, ExecutionSession &ES, jitlink::JITLinkMemoryManager &MemMgr)
      : ObjectLinkingLayer(ES, MemMgr){};
    void emit(std::unique_ptr<MaterializationResponsibility> R,
                              std::unique_ptr<JuliaObjectFile> ObjectFile, 
                              std::unique_ptr<LinkGraph> G);
    jitlink::JITLinkMemoryManager &getMemMgr() { return MemMgr; }
    ReturnObjectBufferFunction getReturnObjectBuffer() { return ReturnObjectBuffer; }
    std::vector<std::unique_ptr<Plugin>> &getPlugins() { return Plugins; }
    StaticJuliaJIT* jit;
};

class ObjectMaterializationUnit final : public MaterializationUnit {
private:
    struct LinkGraphInterface {
        SymbolFlagsMap SymbolFlags;
        SymbolStringPtr InitSymbol;
    };

public:
    static std::unique_ptr<ObjectMaterializationUnit>
    Create(JuliaObjectLinkingLayer &ObjLinkingLayer,  std::unique_ptr<JuliaObjectFile> ObjectFile)
    {
        if (auto G = llvm::jitlink::createLinkGraphFromObject(ObjectFile->MemoryBuffer->getMemBufferRef())) {
            auto LGI = scanLinkGraph(ObjLinkingLayer.getExecutionSession(), **G);
            return std::unique_ptr<ObjectMaterializationUnit>(new ObjectMaterializationUnit(
                ObjLinkingLayer, std::move(ObjectFile), std::move(*G), std::move(LGI)));
        }
        else {
            llvm::errs() << G.takeError();
            assert("Failed to create link graph from object! Something is wrong here.");
            return nullptr;
        }
    }

    StringRef getName() const override { return G->getName(); }
    void materialize(std::unique_ptr<MaterializationResponsibility> MR) override
    {
        ObjLinkingLayer.emit(std::move(MR), std::move(ObjectFile), std::move(G));
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

    ObjectMaterializationUnit(JuliaObjectLinkingLayer &ObjLinkingLayer,
                              std::unique_ptr<JuliaObjectFile> ObjectFile,
                              std::unique_ptr<LinkGraph> G, LinkGraphInterface LGI)
      : MaterializationUnit(std::move(LGI.SymbolFlags), std::move(LGI.InitSymbol)),
        ObjLinkingLayer(ObjLinkingLayer),
        ObjectFile(std::move(ObjectFile)),
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
    JuliaObjectLinkingLayer &ObjLinkingLayer;
    std::unique_ptr<JuliaObjectFile> ObjectFile;
    std::unique_ptr<LinkGraph> G;
    static std::atomic<uint64_t> Counter;
};