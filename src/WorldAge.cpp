#include <julia.h>
#include <unordered_map>
#include <vector>
#include "llvm/ADT/SmallSet.h"
#include "WorldAge.h"
using std::unordered_map;
using std::vector;

class PrimitiveWorld{
    public:
    PrimitiveWorld(jl_module_t* JLModule) : JLModule(JLModule->name) {};
    PrimitiveWorld(jl_sym_t* JLModule) : JLModule(JLModule) {};
    jl_sym_t* JLModule;
    bool operator==(const PrimitiveWorld& w) const{
        return JLModule == w.JLModule;
    }
    bool operator!=(const PrimitiveWorld& w) const{
        return JLModule != w.JLModule;
    }
    bool operator<(const PrimitiveWorld& w) const{
        return JLModule < w.JLModule;
    }
};

namespace std {
  template <> struct hash<PrimitiveWorld>
  {
    size_t operator()(const PrimitiveWorld & x) const
    {
      return hash<void*>()((void*)x.JLModule);
    }
  };
};

class WorldLattice;
using SmallPrimitiveWorldSet = llvm::SmallSet<PrimitiveWorld,10>;
class World{
    public:
    World(SmallPrimitiveWorldSet& components) : components(components) {};
    SmallPrimitiveWorldSet components;
};
enum Cmp{
    Eq,
    Larger,
    Smaller,
    Uncomparable
};
class WorldLattice{
    public:
    WorldLattice() = default;
    friend class World;
    // A larger world contains a small world


    // assuming w1 and w2 is normalized
    World join(World &w1, World& w2){
        SmallPrimitiveWorldSet ws;
        SmallPrimitiveWorldSet w2copy = w2.components;
        for (auto& wc1: w1.components){
            bool discard = false;
            for (auto& wc2 : w2.components){ 
                Cmp c = Ordering(wc1, wc2);
                // wc1 is smaller, discard this world
                if (c == Smaller || c == Eq){
                    discard = true;
                }
                else if (c == Larger){
                    // discard wc2
                    w2copy.erase(wc2);
                }
            }
            if (!discard){
                ws.insert(wc1);
            }
        }
        for (auto& w:w2copy){
            ws.insert(w);
        }
        return ws;
    }
    World joinList(SmallPrimitiveWorldSet& worlds){
        SmallPrimitiveWorldSet result;
        for (auto& w1:worlds){
            bool isSmaller = false;
            for (auto& w2:worlds){
                Cmp c = Ordering(w1, w2);
                if (c == Smaller){
                    isSmaller = true;
                    break;
                }
            }
            if (!isSmaller){
                result.insert(w1);
            }
        }
        return result;
    }
    Cmp Ordering(const World& w1, const World& w2){
        auto& c1 = w1.components;
        auto& c2 = w2.components;
        bool isSmaller = true;
        for (auto& wc1:c1){
            if (!c2.contains(wc1)){
                isSmaller = false;
                break;
            }
        }
        bool isLarger = true;
        for (auto& wc2:c2){
            if (!c1.contains(wc2)){
                isLarger = false;
                break;
            }
        }
        if (isSmaller && isLarger){
            return Eq;
        }
        if (isSmaller){
            return Smaller;
        }
        if (isLarger){
            return Larger;
        }
        return Uncomparable;
    }
    Cmp Ordering(const PrimitiveWorld& w1, const PrimitiveWorld& w2){
        if (worldRelation.find(w1) == worldRelation.end()){
            jl_(w2.JLModule);
            jl_error("World not existed");
        }
        if (worldRelation.find(w2) == worldRelation.end()){
            jl_(w2.JLModule);
            jl_error("World not existed");
        }
        if (w1.JLModule == w2.JLModule){
            return Eq;
        }
        if (!isClosureValid){
            buildTransitiveClosure();
            isClosureValid = true;
        }
        auto& parent1 = transitiveClosure[w1];
        if (parent1.contains(w2)){
            return Larger;
        }
        // w2 depends on w1, so w1 is a smaller world
        auto& parent2 = transitiveClosure[w2];
        if (parent2.contains(w1)){
            return Smaller;
        }
        return Uncomparable;
    }
    
    void init(){
        // init adds top and bottom elements for lattice
        isInit = true;
        worldRelation[jl_main_module] = {};
        worldRelation[jl_main_module].insert(jl_base_module);
        worldRelation[jl_base_module] = {};
        worldRelation[jl_base_module].insert(jl_core_module);
        worldRelation[jl_core_module] = {};
    }

    void addWorld(PrimitiveWorld& w){
        isClosureValid = false;
        if (worldRelation.find(w) == worldRelation.end()){
            worldRelation[PrimitiveWorld(jl_main_module)].insert(w);
            worldRelation[w] = {};
            worldRelation[w].insert(jl_base_module);
        }
    }
    void addDependency(PrimitiveWorld& w1, PrimitiveWorld& w2){
        isClosureValid = false;
        addWorld(w1);
        addWorld(w2);
        worldRelation[w1].insert(w2);
    }

    void visit(const PrimitiveWorld& world){
        // We have visited this node
        if (transitiveClosure.find(world) != transitiveClosure.end()){
            return;
        }
        auto& parents = worldRelation[world];
        if ((parents.size()) == 0){
            transitiveClosure[world] = {};
        }
        else{
            for (auto& parent:parents){
                visit(parent);
            }
            transitiveClosure[world] = {};
            auto& closure = transitiveClosure[world];
            for (auto& parent:parents){
                auto P = transitiveClosure[parent];
                for (auto&i : P){
                    closure.insert(i);
                }
                closure.insert(parent);
            }
        }
    }
    void buildTransitiveClosure(){
        // build the `transitiveClosure` from the `worldRelation`
        // we use a non-incremental version and rebuild closure from scratch
        transitiveClosure.clear();
        for (auto& KV:worldRelation){
            visit(KV.first);
        }
    }
    bool isClosureValid = false;
    bool isInit = false;
    unordered_map<PrimitiveWorld, SmallPrimitiveWorldSet> transitiveClosure;
    unordered_map<PrimitiveWorld, SmallPrimitiveWorldSet> worldRelation;
};

jl_module_t* getRootModule(jl_module_t* mod){
    while (1){
        if (mod == jl_main_module || mod == jl_base_module || mod == jl_core_module){
            return mod;
        }
        if (mod != mod->parent){
            mod = mod->parent;
        }
        else{
            break;
        }
    }
    return mod;
}

class JuliaWorldAgeCalculator{
    public:
    JuliaWorldAgeCalculator(WorldLattice* lattice) : worlds(), lattice(lattice) {
        PrimitiveWorld corew = jl_core_module;
        worlds.insert(corew);
    }
    World calculateMethodInstance(jl_method_instance_t* mi){
        calculateWorld(mi->specTypes);
        calculateWorld(mi->def.method->sig);
        return lattice->joinList(worlds);
    }

    World calculateJuliaValueWorld(jl_value_t* v){
        calculateWorld(v);
        return lattice->joinList(worlds);
    }

    private:
    void calculateModuleWorld(jl_module_t* mod){
        worlds.insert({getRootModule(mod)});
    }

    void calcualteDataTypeWorld(jl_datatype_t* dt){
        calculateModuleWorld(dt->name->module);
        size_t len = jl_svec_len(dt->parameters);
        for (size_t i = 0; i < len; i++) {
            jl_value_t *param = jl_svecref((void *)dt->parameters, i);
            calculateWorld(param);
        }
    }

    void calculateUnionWorld(jl_uniontype_t* u){
        calculateWorld(u->a);
        calculateWorld(u->b);
    }

    void calculateTypeVar(jl_tvar_t* t){
        calculateWorld(t->lb);
        calculateWorld(t->ub);
    }

    void calculateUnionAllWorld(jl_unionall_t* u){
        calculateWorld(u->body);
        calculateTypeVar(u->var);
    }

    void calcualteVararg(jl_vararg_t* vm){
        if (vm->T) {
            calculateWorld(vm->T);
        }
        if (vm->N) {
            calculateWorld(vm->N);
        }
    }

    void calculateWorld(jl_value_t* v){
        if (jl_is_method_instance(v)){
            calculateMethodInstance((jl_method_instance_t*)v);
        }
        else if (jl_is_uniontype(v)){
            calculateUnionWorld((jl_uniontype_t*)v);
        }
        else if (jl_is_unionall(v)){
            calculateUnionAllWorld((jl_unionall_t*)v);
        }
        else if (jl_is_typevar(v)){
            calculateTypeVar((jl_tvar_t*)v);
        }
        else if (jl_is_datatype(v)){
            calcualteDataTypeWorld((jl_datatype_t*)v);
        }
        else if (jl_typeof(v) == (jl_value_t *)jl_vararg_type){
            calcualteVararg((jl_vararg_t*)v);
        }
        else if (v == (jl_value_t *)jl_vararg_type){
        }
        // no need to calculate values defined in Core
        else{
            assert(jl_is_datatype(jl_typeof(v)));
            calcualteDataTypeWorld((jl_datatype_t*)jl_typeof(v));
        }

    }
    SmallPrimitiveWorldSet worlds;
    WorldLattice* lattice;
};

WorldLattice jl_WorldLattice;
extern "C" JL_DLLEXPORT void jl_calculate_world(jl_array_t* root, jl_value_t* v){
    if (!jl_WorldLattice.isInit){
        jl_WorldLattice.init();
        jl_WorldLattice.isInit = true;
    }
    JuliaWorldAgeCalculator cal(&jl_WorldLattice);
    World worlds = cal.calculateJuliaValueWorld(v);
    for (auto& w:worlds.components){
        jl_array_ptr_1d_push(root, (jl_value_t*)w.JLModule);
    }
}

extern "C" JL_DLLEXPORT void jl_freeze_dependency(jl_array_t* dep){
    WorldLattice newLattice;
    newLattice.init();
    newLattice.isInit = true;
    newLattice.isClosureValid = false;
    for (size_t i = 0;i < jl_array_len(dep);i++){
        jl_value_t* pair = jl_arrayref(dep, i);
        assert(jl_is_tuple(pair));
        PrimitiveWorld w1((jl_sym_t*)jl_fieldref(pair, 0));
        PrimitiveWorld w2((jl_sym_t*)jl_fieldref(pair, 1));
        newLattice.addDependency(w1, w2);
    }
    jl_WorldLattice = newLattice;
}

extern "C" JL_DLLEXPORT int jl_compare_world(jl_sym_t* w1, jl_sym_t* w2){
    if (jl_WorldLattice.worldRelation.find(w1) != jl_WorldLattice.worldRelation.end() && jl_WorldLattice.worldRelation.find(w2) != jl_WorldLattice.worldRelation.end()){
        Cmp cmp = jl_WorldLattice.Ordering(w1, w2);
        if (cmp == Larger){
            return 1;
        }
        else if (cmp == Smaller){
            return -1;
        }
        else if (cmp == Eq){
            return 0;
        }
        else{
            return 2;
        }
    }
    return -2;
} 