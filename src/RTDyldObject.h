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
#include <unordered_map>
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "JuliaObjectFile.h"
#include "ObjectFileInterface.h"
class ObjectMaterializationUnit final : public MaterializationUnit {
    public:
    ObjectMaterializationUnit(Interface& I, RTDyldObjectLinkingLayer &ObjLinkingLayer,
                              std::unique_ptr<JuliaObjectFile> ObjectFile)
      : MaterializationUnit(I.SymbolFlags, I.InitSymbol), // we don't use any initialization symbol currently std::move(LGI.SymbolFlags), std::move(LGI.InitSymbol)),
        ObjLinkingLayer(ObjLinkingLayer),
        ObjectFile(std::move(ObjectFile))
    {
    }
    static std::unique_ptr<ObjectMaterializationUnit>
    Create(Interface& I, RTDyldObjectLinkingLayer &ObjLinkingLayer,  std::unique_ptr<JuliaObjectFile> ObjectFile)
    {
        return std::unique_ptr<ObjectMaterializationUnit>(new ObjectMaterializationUnit(I,
                ObjLinkingLayer, std::move(ObjectFile)));
    }
    StringRef getName() const override { return jl_symbol_name_(ObjectFile->JLModule->name); }
    void discard(const JITDylib &JD, const SymbolStringPtr &Name) override
    {
        // TODO : we should we do here ? 
        // we actually cannot do anything here...
    }
    
    void materialize(std::unique_ptr<MaterializationResponsibility> MR) override
    {
        ObjLinkingLayer.emit(std::move(MR), std::move(ObjectFile->MemoryBuffer));
    }

    RTDyldObjectLinkingLayer &ObjLinkingLayer;
    std::unique_ptr<JuliaObjectFile> ObjectFile;
};


