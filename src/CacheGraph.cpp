#include "CacheGraph.h"
#include "StaticJIT.h"

/*
void debugMethodInstanceNode(MethodInstanceNode* miNode){
    if (auto jitNode = dyn_cast<JITMethodInstanceNode>(miNode)){
        llvm::errs() << "JITMethodInstanceNode with state : ";
    }
}
*/

std::unordered_map<std::string, MethodInstanceNode *>::iterator
CacheGraph::lookup(jl_method_instance_t *mi)
{
    std::string s = jl_name_from_method_instance(mi);
    return miNodeMap.find(s);
}

bool CacheGraph::isMethodInstanceCompiled(jl_method_instance_t *mi)
{
    auto iter = lookup(mi);
    if (iter == miNodeMap.end()) {
        return false;
    }
    return true;
}

MethodInstanceNode *CacheGraph::lookUpNode(jl_method_instance_t *mi)
{
    auto iter = lookup(mi);
    if (iter != miNodeMap.end()) {
        return iter->second;
    }
    return nullptr;
}

MethodInstanceNode *CacheGraph::lookUpNode(std::string miName)
{
    auto iter = miNodeMap.find(miName);
    if (iter != miNodeMap.end()) {
        return iter->second;
    }
    return nullptr;
}

JITMethodInstanceNode *CacheGraph::createJITMethodInstanceNode(jl_method_instance_t *mi)
{
    auto iter = lookup(mi);
    // disallow repetive create
    assert(iter == miNodeMap.end());
    auto n = new JITMethodInstanceNode(mi);
    miNodeMap[jl_name_from_method_instance(mi)] = (MethodInstanceNode*)n;
    return n;
}

CachedMethodInstanceNode *CacheGraph::createUnemittedCachedMethodInstanceNode(
    std::string miName, std::string libName, std::string objName,
    std::unique_ptr<SymbolTable> symbolTable, std::unique_ptr<llvm::MemoryBuffer> buffer)
{
    auto iter = miNodeMap.find(miName);
    // this is important, otherwise we may define two same symbols
    if (iter == miNodeMap.end()) {
        auto n = new CachedMethodInstanceNode(miName, libName, objName,
                                              std::move(symbolTable), std::move(buffer));
        miNodeMap[miName] = (MethodInstanceNode*)n;
        return n;
    }
    else {
        llvm::errs() << "Conflicting load of object :" << miName << " from " << libName
                     << '\n';
        llvm::errs() << "Previous defintion : ";
        // TODO : make this robust !!! 
        return nullptr;
    }
}

CachedMethodInstanceNode *
CacheGraph::createEmittedCachedMethodInstanceNode(std::string miName, std::string libName,
                                                  std::string objName)
{
    auto iter = miNodeMap.find(miName);
    // this is important, otherwise we may define two same symbols
    if (iter == miNodeMap.end()) {
        auto n = new CachedMethodInstanceNode(miName, libName, objName);
        miNodeMap[miName] = (MethodInstanceNode*)n;
        return n;
    }
    else {
        llvm::errs() << "Conflicting load of object :" << miName << " from " << libName
                     << '\n';
        llvm::errs() << "Previous defintion : ";
        assert(0);
        return nullptr;
    }
}
CachedMethodInstanceNode *CacheGraph::createPatchedMethodInstanceNode(std::string miName,
                                                          std::string libName,
                                                          std::string objName){
    auto iter = miNodeMap.find(miName);
    // this is important, otherwise we may define two same symbols
    // todo : this hack is bad, maybe use a patched node...
    if (iter == miNodeMap.end()) {
        auto n = new CachedMethodInstanceNode(miName, libName, objName, /*isPatched = */ true);
        miNodeMap[miName] = (MethodInstanceNode*)n;
        return n;
    }
    else{
        // This should be checked before
        llvm::errs() << "Cached node is already existed" << miName;
        exit(1);
    }
}

// mark that the method instance has no more method instance
void CacheGraph::markNoMoreDeps(JITMethodInstanceNode *miNode)
{
    assert(miNode->state == InProcessing);
    miNode->state = ChildPending;
    resolve(miNode);
}

void CacheGraph::resolve(JITMethodInstanceNode *miNode)
{
    MethodInstanceNodeSet reachableSet = {};
    bool isAll = resolve((MethodInstanceNode *)miNode, reachableSet);
    if (isAll) {
        emitMethodInstanceNodes(reachableSet);
    }
}


bool CacheGraph::resolve(MethodInstanceNode *miNode, MethodInstanceNodeSet &hasMeet)
{
    // Collect all reachable node from miNode and test whether these miNodes are already
    // compiled or still in processing
    if (hasMeet.find({miNode}) != hasMeet.end()) {
        return true;
    }
    if (auto jitNode = dyn_cast<JITMethodInstanceNode>(miNode)) {
        if (jitNode->state == Done) {
            return true;
        }
        else if (jitNode->state == ChildPending) {
            hasMeet.insert({miNode});
            for (auto cn : jitNode->dependencies.queue) {
                if (!resolve(cn, hasMeet)) {
                    return false;
                }
            }
        }
        else {
            return false;
        }
    }
    else if (auto cacheNode = dyn_cast<CachedMethodInstanceNode>(miNode)) {
        if (!cacheNode->hasEmitted()) {
            hasMeet.insert({miNode});
        }
    }
    else {
        // TODO : load plugin lazily
    }
    return true;
}


void CacheGraph::emitJITMethodInstanceNode(JITMethodInstanceNode *jitNode)
{
    std::string &miName = jitNode->miName;
    llvm::Module *mod = jitNode->mod.get();
    bool isRelocatable = jitNode->isRelocatable;
    jit->removeUnusedExternal(mod);
    if (!isRelocatable && jl_is_method(jitNode->mi->def.value)) {
        // llvm::errs() << "Non relocatable function:" << miName << '\n';
    }
    if (!jitNode->isToplevel){
        jitNode->unOptIRFilePath = jit->writeOutput(mod, miName, UnoptIR);
    }
    jit->PM.run(*mod);
    jit->removeUnusedExternal(mod);
    if (!jitNode->isToplevel){
        jitNode->optIRFilePath = jit->writeOutput(mod, miName, OptIR);
        jitNode->objectFilePath = jit->writeOutput(mod, miName, ObjFile);
    }
    // we don't need the module any more.
    jitNode->mod.release();
    // now we need to add object buffer
    // TODO : why we swap here... I copy this code from Julia, but this is really bad
    // change it one day
    llvm::SmallVector<char, 0U> emptyBuffer;
    jit->obj_Buffer.swap(emptyBuffer);
    std::unique_ptr<MemoryBuffer> ObjBuffer(
        new SmallVectorMemoryBuffer(std::move(emptyBuffer)));
    auto objorerr = object::ObjectFile::createObjectFile(ObjBuffer->getMemBufferRef());
    if (Error Err = objorerr.takeError()) {
        llvm::errs() << Err;
        jl_error("Unable to create object files!");
    }
    jitNode->buffer = std::move(ObjBuffer);
    llvm::Error err = jit->addJITObject(jitNode, jit->JuliaFuncJD);
    if (err){
        llvm::errs() << "Failed to add JIT Object";
    }
    // TODO : we should set it at some other place...
    jitNode->state = Done;
}

void CacheGraph::emitCachedMethodInstanceNode(CachedMethodInstanceNode *cacheNode)
{
    // Currently we disallow lazy loading, so this function should never be called
    assert(0);
    assert(!cacheNode->hasEmitted());
    // what about patched code?
    // TODO : make this work!!!
    // assert(!jit->addCachedObject(cacheNode, jit->JuliaFuncJD, std::move(ObjBuffer)));
}

void CacheGraph::emitMethodInstanceNode(MethodInstanceNode *miNode)
{
    if (auto jitNode = dyn_cast<JITMethodInstanceNode>(miNode)) {
        emitJITMethodInstanceNode(jitNode);
    }
    else if (auto cacheNode = dyn_cast<CachedMethodInstanceNode>(miNode)) {
        emitCachedMethodInstanceNode(cacheNode);
    }
    else{
        assert(0);
    }
}

void CacheGraph::emitMethodInstanceNodes(MethodInstanceNodeSet &reachableSet)
{
    for (auto i = reachableSet.begin(); i != reachableSet.end(); i++) {
        MethodInstanceNode *n = *i;
        emitMethodInstanceNode(n);
    }
    for (auto i = reachableSet.begin(); i != reachableSet.end(); i++) {
        MethodInstanceNode *n = *i;
        std::string fname = "julia::method::invoke::";
        llvm::raw_string_ostream(fname) << n->miName;
        llvm::cantFail(jit->tryLookupSymbol(fname));
    }
}

void CacheGraph::addDependency(jl_method_instance_t *mi, const std::string &gvName,
                               const std::string &code)
{
    addDependency(jl_name_from_method_instance(mi), gvName, code);
}

void CacheGraph::addDependency(std::string miName, const std::string &gvName,
                               const std::string &code)
{
    auto iter = miNodeMap.find(miName);
    // Since we support loading of invalidated function, so it's possible that we can add symbol to unemitted cache object
    // TODO : merge the code into one ??
    if (iter == miNodeMap.end()){
        llvm::errs() << "Method instance is not cached!\n" << miName << '\n';
        abort();
    }
    if (JITMethodInstanceNode *jitNode = dyn_cast<JITMethodInstanceNode>(iter->second)) {
        auto iter2 = jitNode->symbolTable.find(gvName);
        if (iter2 != jitNode->symbolTable.end()) {
            if (iter2->second != code) {
                /*
                TODO : we currently disable it because it cause too many errors
                assert(0);
                llvm::errs() << "Inconsistent code for the same symbols" << gvName << '\n';
                llvm::errs() << "\tPrevious value: " << iter2->second << '\n';
                llvm::errs() << "\tCurrent value: " << code << '\n';
                */
            }
        }
        else{
            jitNode->symbolTable[gvName] = code;
        }
    }
    else if (CachedMethodInstanceNode *cacheNode = dyn_cast<CachedMethodInstanceNode>(iter->second)){
        // This should never happened;
        assert(!cacheNode->hasEmitted());
        auto iter2 = cacheNode->symbolTable->find(gvName);
        if (iter2 != cacheNode->symbolTable->end()) {
            if (iter2->second != code) {
                assert(0);
                llvm::errs() << "Inconsistent code for the same symbols" << gvName << '\n';
                llvm::errs() << "\tPrevious value: " << iter2->second << '\n';
                llvm::errs() << "\tCurrent value: " << code << '\n';
            }
        }
        else{
            (*(cacheNode->symbolTable))[gvName] = code;
        }
    }
    
}

void CacheGraph::markMethodInstanceAsNonRelocatable(jl_method_instance_t *mi)
{
    auto miNode = lookUpNode(mi);
    if (JITMethodInstanceNode *jitNode = dyn_cast<JITMethodInstanceNode>(miNode)){
        assert(jitNode);
        jitNode->isRelocatable = false;
    }
    else if (CachedMethodInstanceNode *cacheNode = dyn_cast<CachedMethodInstanceNode>(miNode)){
        if (cacheNode->hasEmitted()){
            llvm::errs() << "This cached node should not have emitted already" << jl_name_from_method_instance(mi);
            exit(1);
        }
    }
    else{
        exit(1);
        assert(0);
    }
}

void CacheGraph::detachIfToplevelMethodInstance(jl_method_instance_t *mi)
{
    auto iter = lookup(mi);
    assert(iter != miNodeMap.end());
    auto miNode = iter->second;
    JITMethodInstanceNode *jitNode = dyn_cast<JITMethodInstanceNode>(miNode);
    assert(jitNode != nullptr);
    if (jitNode->isToplevel) {
        // Check that the memory is reclaimed
        assert(!jitNode->mod);
        miNodeMap.erase(iter);
        delete jitNode;
    }
    else {
        // assert(0);
        // llvm::errs() << "Don't call detachIfToplevelMethodInstance on non-toplevel function";
    }
}

void CacheGraph::installCompiledOutput(JITMethodInstanceNode *miNode,
                                       std::unique_ptr<llvm::Module> mod)
{
    assert(!miNode->mod);
    miNode->mod = std::move(mod);
}