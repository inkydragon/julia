#ifndef JL_CACHEGRAPH_H
#define JL_CACHEGRAPH_H

#include "llvm/IR/Module.h"
#include "julia.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "JuliaLibConfig.h"
#include "JITUtil.h"
class StaticJuliaJIT;
enum NodeState {
    Uncompiled, // When we firstly create a node, the state of the node is uncompiled, then it
             // will be initialized to corresponding value
    InProcessing, // this method instance is compiled, but at least one child is unprocessed
    ChildPending, // all the children are processed, but some of them formed a cycle
    Done, // all the children are Done
};

class CacheGraph;
// Don't create this node outside a cache graph, which is extremely unsafe.
// Actually I don't want to expose this API, but it seems that this is convienient...
// So we require that the operation must be done by a cache graph
struct MethodInstanceNode {
    friend class CacheGraph;
    public:
    jl_method_instance_t* mi;
    // Name of the method instance
    std::string miName;
    // We use a ordered vector, instead of a set to trace dependencies
    UniqueVector<MethodInstanceNode *> dependencies;
    NodeState state;
    bool isCached;
    bool isInvalidated;
    std::unique_ptr<llvm::Module> mod;
    std::unordered_map<std::string, std::string> externalSymbols;
    // Output file path
    std::string unOptIRFilePath;
    std::string optIRFilePath;
    std::string objectFilePath;
    // Used to match object file in static library
    std::string objectFileName;
    MethodInstanceNode(jl_method_instance_t *mi, NodeState state, bool isCached)
      : mi(mi),
        miName(jl_name_from_method_instance(mi)),
        dependencies(),
        state(state),
        isCached(isCached),
        isInvalidated(false),
        mod(nullptr),
        externalSymbols(),
        unOptIRFilePath(),
        optIRFilePath(),
        objectFilePath(),
        objectFileName(){};
    // Create a cache node, note that method instance is nullptr since we don't output method instance to file
    // mi will be set when we look up method instance in cache graph
    MethodInstanceNode(std::string& miName, NodeState state, bool isCached) :  
        mi(nullptr),
        miName(miName),
        dependencies(),
        state(state),
        isCached(isCached),
        isInvalidated(false),
        mod(nullptr),
        externalSymbols(),
        unOptIRFilePath(),
        optIRFilePath(),
        objectFilePath(),
        objectFileName(){};
};

/*
    Cache graph manages all the dependencies, either emitted in current jit or loaded from
   static libraries. Cache graph also traces dependencies between method instance.

    Cache Graph is a graph of (V, E), where V is the vertex of graph, MethodInstance Node, E
   is the dependency of different MI. There are two kinds of MethodInstance Node: Node
   generated in current jit and Node loaded from cache. The former has associated MI and the
   latter has only text-formed MI.

    So we have to use only text-formed MI to represent MI.
*/

class CacheGraph {
public:
    CacheGraph(StaticJuliaJIT *jit) : jit(jit){};
    void addExternalSymbolDependency(MethodInstanceNode* miNode, std::string gvName, std::string code);
    std::unordered_map<std::string, std::string> &getExternalSymbols(MethodInstanceNode* miNode);
    MethodInstanceNode* lookUpCompiledNode(std::string& miName);
    MethodInstanceNode* lookUpJITNode(jl_method_instance_t* mi);
    // The method instance is either emitted in memory, or in compiling.
    bool isMethodInstanceCompiled(jl_method_instance_t* mi);
    bool isProcessing(MethodInstanceNode* miNode);
    MethodInstanceNode* getOrCreateUncompiledNode(jl_method_instance_t* mi);
    MethodInstanceNode* createInProcessingNode(jl_method_instance_t* mi);
    MethodInstanceNode* createCachedNode(std::string& miName, JuliaLibConfig& config);
    MethodInstanceNode* createPatchedNode(std::string& miName, JuliaLibConfig& config);
    void installCompiledOutput(MethodInstanceNode* miNode, std::unique_ptr<llvm::Module> mod);
    void forkChild(MethodInstanceNode* pn, MethodInstanceNode* cn);
    void markNoMoreDeps(MethodInstanceNode* parent);
    std::unordered_map<std::string, MethodInstanceNode *>& getMiNodeMap();
    std::unordered_map<std::string, std::string> &getDependencies(jl_method_instance_t *mi);
    void addDependency(jl_method_instance_t* mi, const std::string& gvName, const std::string& code);
    void addDependency(std::string miName, const std::string& gvName, const std::string& code);
private:
    using MethodInstanceNodeSet = std::unordered_set<MethodInstanceNode *>;
    void addDependency(MethodInstanceNode* miNode, const std::string& gvName, const std::string& code);
    void resolve(MethodInstanceNode *n);
    bool resolve(MethodInstanceNode *n, MethodInstanceNodeSet &hasMeet);
    void emitMethodInstanceNode(MethodInstanceNode *minode);
    void emitMethodInstances(MethodInstanceNodeSet &reachableSet);
    std::unordered_map<std::string, MethodInstanceNode *>::iterator lookup(jl_method_instance_t *mi);
    StaticJuliaJIT *jit;
    // Map from method instance name to method instance node
    std::unordered_map<std::string, MethodInstanceNode *> miNodeMap;
};

#endif