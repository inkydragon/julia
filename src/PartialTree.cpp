#include <cassert>
#include <julia.h>
#include <map>
#include <set>
#include <memory>
#include <unordered_set>
#include <vector>
#include <llvm/IR/Module.h>
using std::map;
using std::set;
using std::unordered_map;
using std::vector;
enum NodeState {
    InProcessing, // at least one child is unprocessed
    ChildPending, // all the children are processed, but some of them formed a cycle
    Done // all the children are Done
};
/*
template<typename T>
struct Node {
    set<Node *> children;
    NodeState state;
    T val;
};


// This is a graph where each node represents a pure computation
// So we use a valueMap to avoid redundary computation
// nodePool is used to manage allocation
template<typename T>
class Graph {
    using ActionHandle = void (*)(T);

public:
    Graph<T>(T val)
    {
        root = createRoot(val);
        nodePool.insert({root});
        valueMap.insert({{val, root}});
    };
    // create root node
    Node<T> *createRoot(T val)
    {
        root = new Node<T>();
        root->val = val;
        root->state = InProcessing;
        return root;
    }
    // Fork a child of value `val` at node `n`
    // this child is a cached value and shouldn't have any children!
    Node<T> *forkCached(Node<T> *n, T val)
    {
        Node<T> *node;
        if (valueMap.find(val) != valueMap.end()) {
            node = valueMap[val];
        }
        else {
            node = new Node<T>();
            node->state = Done;
            node->val = val;
            nodePool.insert(node);
            valueMap[val] = node;
        }
    }
    // Fork a child of value `val` at node `n`
    Node<T> *forkChild(Node<T> *n, T val)
    {
        assert(n->state == InProcessing);
        Node<T> *node;
        if (valueMap.find(val) != valueMap.end()) {
            node = valueMap[val];
        }
        else {
            node = new Node<T>();
            node->state = InProcessing;
            node->val = val;
            nodePool.insert(node);
            valueMap[val] = node;
        }
        n->children.insert(node);
        return node;
    }
    bool resolve(Node<T> *n, set<Node<T> *> &hasMeet)
    {
        assert(n->state != Done);
        if (n->state == ChildPending) {
            for (auto i = n->children.begin(); i != n->children.end(); i++) {
                Node<T> *cn = *i;
                if (cn->state == Done) {
                    continue;
                }
                if (hasMeet.find(cn) == hasMeet.end()) {
                    hasMeet.insert(cn);
                    if (!resolve(cn, hasMeet)) {
                        return false;
                    }
                }
            }
            return true;
        }
        return false;
    }
    void setAction(ActionHandle h) { action = h; }
    void takeAction(T val)
    {
        if (action != nullptr) {
            action(val);
        }
    }
    void resolve(Node<T> *n)
    {
        set<Node<T> *> reachableSet = {n};
        bool isAll = resolve(n, reachableSet);
        if (isAll) {
            for (auto i = reachableSet.begin(); i != reachableSet.end(); i++) {
                Node<T> *n = *i;
                assert(n->state != Done);
                n->state = Done;
                takeAction(n->val);
            }
        }
    }
    void setNoMoreChildren(Node<T> *n)
    {
        // n has no more children, we scan through the children of n to see whether n is
        // done
        assert(n->state == InProcessing);
        n->state = ChildPending;
        resolve(n);
    }
    ~Graph<T>()
    {
        for (auto i = nodePool.begin(); i != nodePool.end(); i++) {
            delete *i;
        }
    }
    Node<T> *root;

private:
    set<Node<T> *> nodePool;
    map<T, Node<T> *> valueMap;
    ActionHandle action = nullptr;
};
*/
template<typename T>
class UniqueVector {
public:
    void push_back(T e)
    {
        if (elements.find(e) == elements.end()) {
            queue.push_back(e);
            elements.insert(e);
        }
    }
    bool has(T e) { return elements.find(e) != elements.end(); }
    std::vector<T> queue;
    std::set<T> elements;
};


class CacheGraph {
    struct MethodInstanceNode {
        jl_method_instance_t *mi;
        UniqueVector<MethodInstanceNode *> dependencies;
        NodeState state;
        bool isCached;
        std::unique_ptr<llvm::Module> mod;
        std::unique_ptr<llvm::MemoryBuffer> buffer;
        MethodInstanceNode(jl_method_instance_t *mi, NodeState state, bool isCached)
          : mi(mi), dependencies(), state(state), isCached(isCached){}, mod(nullptr), buffer(nullptr);
        bool operator==(const MethodInstanceNode &other) { return other.mi == mi; }
    };

    struct MethodInstanceNodeHash {
        std::size_t operator()(const MethodInstanceNode &minode)
        {
            return (size_t)minode.mi;
        }
    };

public:
    using MethodInstanceSet = unordered_set<MethodInstanceNode *, MethodInstanceNodeHash>;
    CacheGraph() : {};
    void installCompiledOutput(jl_method_instance_t *mi, std::unique_ptr<llvm::Module> mod, std::unique_ptr<llvm::MemoryBuffer> buffer){
        auto n = getOrCreateUncompiledNode(mi);
        // can't install more than once!
        assert(!(n->mod) && !(n->buffer));
        n->mod = std::move(mod);
        n->buffer = std::move(buffer);
    }
    // instance a MethodInstance node in the graph
    void createUncompiledNode(jl_method_instance_t *mi)
    {
        auto iter = miPool.find(mi);
        MethodInstanceNode *n;
        if (iter == miPool.end()) {
            n = new MethodInstanceNode(mi, InProcessing, false);
            miPool.insert(n);
        }
        else {
            n = *iter;
            assert(n->state == InProcessing && !n->inCached);
        }
    }
    // The method instance is already presented in the binary
    // either already compiled or loaded from file
    void createCachedNode(jl_method_instance_t *mi)
    {
        auto iter = miPool.find(mi);
        MethodInstanceNode *n;
        if (iter == miPool.end()) {
            n = new MethodInstanceNode(mi, Done, true);
            miPool.insert(n);
        }
        else {
            n = *iter;
            assert(n->state == Done);
        }
        return n;
    }
    void forkChild(jl_method_instance_t *parent, jl_method_instance_t *child)
    {
        auto pn = getOrCreateUncompiledNode(parent);
        auto cn = getOrCreateUncompiledNode(child);
        assert(pn->state == InProcessing && !pn->isCached);
        pn->dependencies.push_back(cn);
    }
    // mark the method instance has no more children
    void markNoMoreDeps(jl_method_instance_t *parent)
    {
        auto pn = getOrCreateUncompiledNode(parent);
        assert(pn->state == InProcessing);
        pn->state = ChildPending;
        resolve(pn);
    }
    // void setAction(ActionHandle h) { action = h; }
private:
    void resolve(MethodInstanceNode *n)
    {
        MethodInstanceSet reachableSet = {n};
        bool isAll = resolve(n, reachableSet);
        if (isAll) {
            emitMethodInstances(reachableSet);
        }
    }
    void emitMethodInstances(MethodInstanceSet& reachableSet) {
        for (auto i = reachableSet.begin(); i != reachableSet.end(); i++) {
            MethodInstanceNode *n = *i;
            assert(n->state != Done);
            // must install necessary value
            // n->mod and n->buffer will transfer to llvm's jit
            assert(n->mod && n->buffer);
            n->state = Done;
            //emitMethodInstances(n);
        }
    }
    bool resolve(MethodInstanceNode *n, MethodInstanceSet &hasMeet)
    {
        assert(n->state != Done);
        if (n->state == ChildPending) {
            for (auto cn : n->dependencies.queue) {
                if (cn->state == Done) {
                    continue;
                }
                if (hasMeet.find(cn) == hasMeet.end()) {
                    hasMeet.insert(cn);
                    if (!resolve(cn, hasMeet)) {
                        return false;
                    }
                }
            }
            return true;
        }
        return false;
    }
    // get a node from the graph, if not existed, create an uncompiled one
    MethodInstanceNode *getOrCreateUncompiledNode(jl_method_instance_t *mi)
    {
        auto iter = miPool.find(mi);
        MethodInstanceNode *n;
        if (iter == miPool.end()) {
            n = new MethodInstanceNode(mi, InProcessing, false);
            miPool.insert(n);
        }
        else {
            n = *iter;
        }
        return n;
    }

private:
    // method instance pool, used to manage memory
    MethodInstanceSet miPool;
}
