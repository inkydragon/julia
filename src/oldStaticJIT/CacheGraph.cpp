#include "CacheGraph.h"
#include "StaticJIT.h"
void CacheGraph::addExternalSymbolDependency(MethodInstanceNode *miNode, std::string gvName,
                                             std::string code)
{
    assert(miNode != nullptr);
    assert(miNode->state == InProcessing);
    assert(miNode);
    miNode->externalSymbols.insert({gvName, code});
}
std::unordered_map<std::string, std::string> &
CacheGraph::getExternalSymbols(MethodInstanceNode *miNode)
{
    return miNode->externalSymbols;
}
// Only for compiled mi
MethodInstanceNode *CacheGraph::lookUpCompiledNode(std::string &miName)
{
    auto iter = miNodeMap.find(miName);
    if (iter == miNodeMap.end()){
        return nullptr;
    }
    else{
        auto miNode = iter->second;
        if (miNode->state == Done){
            return miNode;
        }
        else{
            return nullptr;
        }
    }
}
// Only for runtime generated mi
MethodInstanceNode *CacheGraph::lookUpJITNode(jl_method_instance_t *mi)
{
    auto iter = lookup(mi);
    return (iter == miNodeMap.end() ? nullptr : iter->second);
}

bool CacheGraph::isMethodInstanceCompiled(jl_method_instance_t *mi)
{
    auto iter = lookup(mi);
    if (iter == miNodeMap.end()) {
        return false;
    }
    else {
        NodeState s = iter->second->state;
        return s != Uncompiled;
    }
}

bool CacheGraph::isProcessing(MethodInstanceNode *miNode)
{
    return miNode->state == InProcessing;
}


// get a node from the graph, if not existed, create an uncompiled one
MethodInstanceNode *CacheGraph::getOrCreateUncompiledNode(jl_method_instance_t *mi)
{
    auto iter = lookup(mi);
    MethodInstanceNode *n;
    // The child node can't be found, so its state is Uncompiled
    if (iter == miNodeMap.end()) {
        n = new MethodInstanceNode(mi, Uncompiled, false);
        miNodeMap[jl_name_from_method_instance(mi)] = n;
    }
    else {
        n = iter->second;
    }
    return n;
}

// instance a MethodInstance node in the graph
MethodInstanceNode *CacheGraph::createInProcessingNode(jl_method_instance_t *mi)
{
    MethodInstanceNode *n;
    auto iter = lookup(mi);
    if (iter == miNodeMap.end()) {
        n = new MethodInstanceNode(mi, InProcessing, false);
        miNodeMap[jl_name_from_method_instance(mi)] = n;
    }
    else {
        n = iter->second;
        NodeState &s = n->state;
        assert(s == InProcessing || s == Uncompiled);
        s = InProcessing;
    }
    return n;
}

MethodInstanceNode* CacheGraph::createCachedNode(std::string &miName, JuliaLibConfig &config)
{
    auto iter = miNodeMap.find(miName);
    if (iter == miNodeMap.end()) {
        auto n = new MethodInstanceNode(miName, Done, true);
        miNodeMap[miName] = n;
        // Record where the object file comes from
        n->objectFileName = config.objFileName;
        // Record necessary external symbols, we don't need to record this, since cached object will not be cached again
        // n->externalSymbols = config.externalSymbols;
        return n;
    }
    else {
        assert(0);
    }
}
MethodInstanceNode* CacheGraph::createPatchedNode(std::string& miName, JuliaLibConfig& config){
    auto iter = miNodeMap.find(miName);
    if (iter == miNodeMap.end()) {
        auto n = new MethodInstanceNode(miName, Done, false);
        n->isInvalidated = true;
        miNodeMap[miName] = n;
        return n;
    }
    else {
        assert(0);
    }
}
void CacheGraph::installCompiledOutput(MethodInstanceNode* n, 
                                       std::unique_ptr<llvm::Module> mod)
{
    assert(n != nullptr);
    // can't install more than once!
    assert(n->state == InProcessing && !(n->mod));
    n->mod = std::move(mod);
}

void CacheGraph::forkChild(MethodInstanceNode *pn, MethodInstanceNode *cn)
{
    assert(pn->state == InProcessing && !pn->isCached);
    pn->dependencies.push_back(cn);
}

// mark the method instance has no more children
void CacheGraph::markNoMoreDeps(MethodInstanceNode *pn)
{
    assert(pn->state == InProcessing);
    pn->state = ChildPending;
    resolve(pn);
}

std::unordered_map<std::string, MethodInstanceNode *> &CacheGraph::getMiNodeMap()
{
    return miNodeMap;
}

std::unordered_map<std::string, std::string> &
CacheGraph::getDependencies(jl_method_instance_t *mi)
{
    auto iter = lookup(mi);
    if (iter != miNodeMap.end()) {
        assert(iter->second);
        return iter->second->externalSymbols;
    }
    assert(0);
}

void CacheGraph::addDependency(jl_method_instance_t *mi, const std::string &gvName,
                               const std::string &code)
{
    addDependency(lookup(mi)->second, gvName, code);
}

void CacheGraph::addDependency(std::string miName, const std::string &gvName,
                               const std::string &code)
{
    addDependency(miNodeMap[miName], gvName, code);
}

void CacheGraph::addDependency(MethodInstanceNode *miNode, const std::string &gvName,
                               const std::string &code)
{
    miNode->externalSymbols[gvName] = code;
}

void CacheGraph::resolve(MethodInstanceNode *n)
{
    MethodInstanceNodeSet reachableSet = {n};
    bool isAll = resolve(n, reachableSet);
    if (isAll) {
        emitMethodInstances(reachableSet);
    }
}

bool CacheGraph::resolve(MethodInstanceNode *n, MethodInstanceNodeSet &hasMeet)
{
    assert(n->state != Done);
    if (n->state == ChildPending) {
        for (auto cn : n->dependencies.queue) {
            if (cn->state == Done) {
                continue;
            }
            if (hasMeet.find({cn}) == hasMeet.end()) {
                hasMeet.insert({cn});
                if (!resolve(cn, hasMeet)) {
                    return false;
                }
            }
        }
        return true;
    }
    return false;
}

/*
std::unordered_map<std::string, MethodInstanceNode *> &CacheGraph::getMiNodeMap()
{
    return miNodeMap;
}
*/
std::unordered_map<std::string, MethodInstanceNode *>::iterator
CacheGraph::lookup(jl_method_instance_t *mi)
{
    std::string s = jl_name_from_method_instance(mi);
    return miNodeMap.find(s);
}

void CacheGraph::emitMethodInstanceNode(MethodInstanceNode *minode)
{
    std::string& miName = minode->miName;
    jl_method_instance_t* mi = minode->mi;
    assert(mi);
    auto umod = std::move(minode->mod);
    llvm::Module *mod = umod.get();
    jit->removeUnusedExternal(mod);
    bool needInvalidated =
        jit->invalidatedFunctionSet.find(mi) != jit->invalidatedFunctionSet.end();
    if (jl_is_module((jl_value_t*)mi->def.method)){
        needInvalidated = true;
        mi = nullptr;
    }
    minode->isInvalidated = needInvalidated;
    if (needInvalidated) {
        llvm::errs() << "Be invalidated!" << miName << '\n';
    }
    if (!needInvalidated) {
        //minode->unOptIRFilePath = 
        (void)jit->writeOutput(mod, miName, UnoptIR);
    }
    else {
        if (mi != nullptr){
            jit->insertInvalidatedFunction(mi);
        }
    }
    jit->writeOutput(mod, miName, UnoptIR);
    jit->PM.run(*mod);
    jit->removeUnusedExternal(mod);
    minode->optIRFilePath = jit->writeOutput(mod, miName, OptIR);
    if (!needInvalidated) {
        minode->objectFilePath = jit->writeOutput(mod, miName, ObjFile);
    }
    // we don't need the module any more.
    minode->mod.release();
    // now we need to add object buffer
    llvm::SmallVector<char, 0U> emptyBuffer;
    jit->obj_Buffer.swap(emptyBuffer);
    std::unique_ptr<MemoryBuffer> ObjBuffer(
        new SmallVectorMemoryBuffer(std::move(emptyBuffer)));
    auto objorerr = object::ObjectFile::createObjectFile(ObjBuffer->getMemBufferRef());
    if (Error Err = objorerr.takeError()) {
        llvm::errs() << Err;
        jl_error("Unable to create object files!");
    }
    assert(!jit->addJITObject(minode->mi, jit->JuliaFuncJD, std::move(ObjBuffer)));
    minode->mi = mi;
}

void CacheGraph::emitMethodInstances(MethodInstanceNodeSet &reachableSet)
{
    for (auto i = reachableSet.begin(); i != reachableSet.end(); i++) {
        MethodInstanceNode *n = *i;
        assert(n->state != Done);
        assert(n->mod);
        emitMethodInstanceNode(n);
        n->state = Done;
    }
    for (auto i = reachableSet.begin(); i != reachableSet.end(); i++) {
        MethodInstanceNode *n = *i;
        if (n->mi != nullptr){
            assert(jit->getInvokePointer(n->mi, jl_world_counter));
        }
    }
}