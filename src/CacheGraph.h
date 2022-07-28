#ifndef JL_CACHEGRAPH_H
#define JL_CACHEGRAPH_H
#include "JITUtil.h"
#include "julia.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/MemoryBuffer.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" const char *jl_name_from_method_instance(jl_method_instance_t *li);
using SymbolTable = std::unordered_map<std::string, std::string>;
class StaticJuliaJIT;
class CacheGraph;
class MethodInstanceNode;
// class ToplevelMethodInstanceNode;
class CachedMethodInstanceNode;
class JITMethodInstanceNode;
class PluginMethodInstanceNode;

// We use LLVM's RTTI system to perform runtime time checking
class MethodInstanceNode {
public:
    enum MethodInstanceNodeKind {
        // SK_ToplevelMethodInstanceNode,
        SK_CachedMethodInstanceNode,
        SK_JITMethodInstanceNode,
        SK_PluginMethodInstanceNode
    };
    MethodInstanceNodeKind getKind() const { return Kind; }
protected:
    MethodInstanceNode(MethodInstanceNodeKind Kind, std::string miName)
      : Kind(Kind), miName(miName){};
private:
    const MethodInstanceNodeKind Kind;
public:
    std::string miName;
};

/*
// This is also generated at runtime, but need some special handle because toplevel code is
// not rooted
class ToplevelMethodInstanceNode final : MethodInstanceNode {
public:
    ToplevelMethodInstanceNode(jl_method_instance_t *mi, jl_module_t *m)
      : MethodInstanceNode(SK_ToplevelMethodInstanceNode, jl_name_from_method_instance(mi)),
mi(mi), m(m), mod() {}; static bool classof(const MethodInstanceNode *n)
      {
          return n->getKind() == SK_ToplevelMethodInstanceNode;
      };
      jl_method_instance_t *mi;
      // current module where method instance is generated

      jl_module_t *m;
      // compiled code
      std::unique_ptr<llvm::Module> mod;
};
*/

/*
    CachedMethodInstanceNode represents a cached method instance which is loaded from a
    binary file. Currently we haven't serialized method instance, so we can only rely on
    MI's name to detect whether we have loaded cache file.
    Invalidated function will be loaded eagerly in our current implementation.
    This is lazy emitted, so we use a flag to indicate whether this function is emitted.


    NONONONONONONO!!!!!!!!!!!!!
    CacheGraph can't be lazy!!! This is because we didn't save edges of CacheGraph
   (crying...) So maybe we can do some work on linker, and make it correlate with our
   CacheGraph (or simply we directly emit it)

    TODO : lazy patched code (non-relocatable code located from cache)
*/
class CachedMethodInstanceNode final : public MethodInstanceNode {
public:
    // Unemitted cached instance node
    CachedMethodInstanceNode(std::string miName, std::string libName, std::string objName,
                             std::unique_ptr<SymbolTable> symbolTable,
                             std::unique_ptr<llvm::MemoryBuffer> buffer)
      : MethodInstanceNode(SK_CachedMethodInstanceNode, miName),
        libName(libName),
        objName(objName),
        symbolTable(std::move(symbolTable)),
        buffer(std::move(buffer)),
        isEmitted(false){};
    // Emitted before this object is constructed !!! (??? TODO : fix the lazy and eager
    // emit)
    CachedMethodInstanceNode(std::string miName, std::string libName, std::string objName, bool isPatched = false)
      : MethodInstanceNode(SK_CachedMethodInstanceNode, miName),
        libName(libName),
        objName(objName),
        isEmitted(!isPatched){
            // If it's patched, then we create a new symbol table just like in JITMethodInstanceNodes
            if (isPatched){
                symbolTable = std::move(std::make_unique<SymbolTable>());
            }
        };
    static bool classof(const MethodInstanceNode *n)
    {
        return n->getKind() == SK_CachedMethodInstanceNode;
    };
    bool hasEmitted()
    {
        if (!isEmitted) {
            // TODO : add test for patched node!
            // assert(buffer);
            assert(symbolTable);
        }
        else {
            assert(!buffer);
            assert(!symbolTable);
        }
        return isEmitted;
    }
    void setEmitted() { isEmitted = true; }
    // the static library that the method instance comes from, might be a path
    std::string libName;
    // name of object file
    std::string objName;
    // serialized symbol tables used for resolution
    // this symbol table will be moved to object linking layer
    // we don't need it for serialized output
    std::unique_ptr<SymbolTable> symbolTable;
    // object code stored in buffer
    std::unique_ptr<llvm::MemoryBuffer> buffer;

    // this field should only be touched by addCachedObject and constructor, don't modify
    // this field at other places
    bool isEmitted;
};

enum NodeState {
    // this method instance is compiled, but at least one child is unprocessed
    InProcessing,
    // all the children are processed, but some of them formed a cycle
    ChildPending,
    // all the children are Done
    Done
};

class JITMethodInstanceNode final : public MethodInstanceNode {
public:
    JITMethodInstanceNode(jl_method_instance_t *mi)
      : MethodInstanceNode(SK_JITMethodInstanceNode, jl_name_from_method_instance(mi)),
        mi(mi),
        state(InProcessing)
    {
        assert(mi);
        isToplevel = !jl_is_method((jl_value_t *)mi->def.method);
        isRelocatable = !isToplevel; // toplevel code can't be rellocated
    };
    static bool classof(const MethodInstanceNode *n)
    {
        return n->getKind() == SK_JITMethodInstanceNode;
    };
    // this method instance must be rooted
    jl_method_instance_t *mi;
    // We use a ordered vector, instead of a set to trace dependencies
    UniqueVector<MethodInstanceNode *> dependencies;
    // state of jit compiling
    NodeState state;
    // whether this node is a toplevel node
    // How to handle toplevel method instance correctly?
    // maybe simply delete it?
    bool isToplevel;
    // whether this method instance is relocatable
    bool isRelocatable;
    // jit result, stored here temporarily and waiting for recursive dependency
    std::unique_ptr<llvm::Module> mod;
    // we need this symbolTable for output, so we can't move this symbolTable to object
    // linking layer generator will copy this symbolTable!
    SymbolTable symbolTable;
    std::unique_ptr<llvm::MemoryBuffer> buffer;
    // used for persistant output, TODO: maybe use optional if we don't want to cache the
    // path?
    std::string unOptIRFilePath;
    std::string optIRFilePath;
    std::string objectFilePath;
};

// Just like a cached MI, but it's isolated with other part of binary
// Currently only used to wrap system image...
// We need a more flexiable API to add our own plugin
// Also eager one, assume the code is already loaded.
class PluginMethodInstanceNode final : public MethodInstanceNode {
    public:
    PluginMethodInstanceNode(std::string miName)
      : MethodInstanceNode(SK_PluginMethodInstanceNode, miName){};
    static bool classof(const MethodInstanceNode *n)
    {
        return n->getKind() == SK_PluginMethodInstanceNode;
    };
    // where this method instance comes from, might be a path
    std::string pluginName;
};

/*
    CacheGraph is used to maintain the call graph of all method instance, cached or
    uncached.
    We ensure the following invariant:
    Every method instance defined in the CacheGraph is in one-to-one corresponding with the
   exported strong function symbols in JITLib.
*/
class CacheGraph {
public:
    CacheGraph(StaticJuliaJIT *jit) : jit(jit){};
    // The method instance is either emitted in memory, or in compiling.
    bool isMethodInstanceCompiled(jl_method_instance_t *mi);
    // Look up the method instance in our CacheGraph
    std::unordered_map<std::string, MethodInstanceNode *>::iterator
    lookup(jl_method_instance_t *mi);
    MethodInstanceNode *lookUpNode(jl_method_instance_t *mi);
    MethodInstanceNode *lookUpNode(std::string miName);
    JITMethodInstanceNode *createJITMethodInstanceNode(jl_method_instance_t *mi);

    CachedMethodInstanceNode *
    createUnemittedCachedMethodInstanceNode(std::string miName, std::string libName,
                                            std::string objName,
                                            std::unique_ptr<SymbolTable> symbolTable,
                                            std::unique_ptr<llvm::MemoryBuffer> buffer);

    CachedMethodInstanceNode *createEmittedCachedMethodInstanceNode(std::string miName,
                                                                    std::string libName,
                                                                    std::string objName);
    CachedMethodInstanceNode *createPatchedMethodInstanceNode(std::string miName,
                                                              std::string libName,
                                                              std::string objName);
    void installCompiledOutput(JITMethodInstanceNode *miNode,
                               std::unique_ptr<llvm::Module> mod);
    void addExternalSymbolDependency(MethodInstanceNode *miNode, std::string gvName,
                                     std::string code);
    std::unordered_map<std::string, std::string> &
    getExternalSymbols(MethodInstanceNode *miNode);

    void addChild(JITMethodInstanceNode *parentNode, MethodInstanceNode *childNode)
    {
        parentNode->dependencies.push_back(childNode);
    }
    using MethodInstanceNodeSet = std::unordered_set<MethodInstanceNode *>;
    // Mark that all the children of this method instance has finished compiling
    void markNoMoreDeps(JITMethodInstanceNode *miNode);
    // Collect all method instance that needs emit
    // Notice that cached object file may also need emit???
    // TODO : make it lazy !!!

    // std::unordered_map<std::string, std::string> &getDependencies(jl_method_instance_t
    // *mi);

    // we have these functions because Julia's compiler doesn't track external dependencies
    // in the ctx so we have to use this to add dependency one by one
    void addDependency(jl_method_instance_t *mi, const std::string &gvName,
                       const std::string &code);
    // void addDependency(JITMethodInstanceNode *jitNode, const std::string &gvName,
    //                    const std::string &code);
    void emitCachedMethodInstanceNode(CachedMethodInstanceNode *cacheNode);
    void markMethodInstanceAsNonRelocatable(jl_method_instance_t *mi);
    // We didn't reclaim memory for toplevel method instance, this is actually quite bad.
    // find a way to do this in the future.
    void detachIfToplevelMethodInstance(jl_method_instance_t *mi);
    std::unordered_map<std::string, MethodInstanceNode *> &getMiNodeMap()
    {
        return miNodeMap;
    }

private:
    void resolve(JITMethodInstanceNode *miNode);
    bool resolve(MethodInstanceNode *miNode, MethodInstanceNodeSet &hasMeet);
    void emitMethodInstanceNode(MethodInstanceNode *miNode);
    void emitJITMethodInstanceNode(JITMethodInstanceNode *miNode);
    void emitMethodInstanceNodes(MethodInstanceNodeSet &reachableSet);
    void addDependency(std::string miName, const std::string &gvName,
                       const std::string &code);
    // jit is used to interact with Julia's jit, mostly to emit object code
    StaticJuliaJIT *jit;
    // Map from method instance name to method instance node
    // We assume that method instance name is unique and we use this to identify different
    // method instance and this mean that we can't redefine function any more, because they
    // have the same name (though different mi). we use mi name instead of pointer, because
    // we don't serialize mi, so we have to identify mi with its name.
    std::unordered_map<std::string, MethodInstanceNode *> miNodeMap;
    // gen is a function with type (jl_method_instance_t*, FileType) -> std::string
};
#endif