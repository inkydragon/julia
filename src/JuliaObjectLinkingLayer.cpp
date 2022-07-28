//===------- ObjectLinkingLayer.cpp - JITLink backed ORC ObjectLayer ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#define private public
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#undef private
#include "JuliaObjectLinkingLayer.h"
#include "llvm/ADT/Optional.h"
#include "julia.h"
#include <vector>

using namespace llvm;
using namespace llvm::jitlink;
using namespace llvm::orc;

std::atomic<uint64_t> ObjectMaterializationUnit::Counter{0};

// Object file ownership finally transfers to JuliaObjectLinkingLayerJITLinkContext
// JuliaObjectLinkingLayerJITLinkContext manages JuliaObjectFile's ownership
class JuliaObjectLinkingLayerJITLinkContext final : public JITLinkContext {
public:
  JuliaObjectLinkingLayerJITLinkContext(
      StaticJuliaJIT* jit,
      JuliaObjectLinkingLayer &Layer,
      std::unique_ptr<MaterializationResponsibility> MR,
      std::unique_ptr<JuliaObjectFile> ObjFile)
      : JITLinkContext(&MR->getTargetJITDylib()), Layer(Layer),
        MR(std::move(MR)), 
        ObjectFile(std::move(ObjFile)), 
        ObjBuffer(std::move(ObjectFile->MemoryBuffer)), 
        JLModule(ObjectFile->JLModule), 
        jit(jit) {}

  ~JuliaObjectLinkingLayerJITLinkContext() {
    // If there is an object buffer return function then use it to
    // return ownership of the buffer.
    if (Layer.ReturnObjectBuffer && ObjBuffer)
      Layer.ReturnObjectBuffer(std::move(ObjBuffer));
  }

  JITLinkMemoryManager &getMemoryManager() override { return Layer.MemMgr; }

  void notifyFailed(Error Err) override {
    for (auto &P : Layer.Plugins)
      Err = joinErrors(std::move(Err), P->notifyFailed(*MR));
    Layer.getExecutionSession().reportError(std::move(Err));
    MR->failMaterialization();
  }

  void lookup(const LookupMap &Symbols,
              std::unique_ptr<JITLinkAsyncLookupContinuation> LC) override {

    JITDylibSearchOrder LinkOrder;
    MR->getTargetJITDylib().withLinkOrderDo(
        [&](const JITDylibSearchOrder &LO) { LinkOrder = LO; });

    auto &ES = Layer.getExecutionSession();

    SymbolLookupSet LookupSet;
    for (auto &KV : Symbols) {
      orc::SymbolLookupFlags LookupFlags;
      switch (KV.second) {
      case jitlink::SymbolLookupFlags::RequiredSymbol:
        LookupFlags = orc::SymbolLookupFlags::RequiredSymbol;
        break;
      case jitlink::SymbolLookupFlags::WeaklyReferencedSymbol:
        LookupFlags = orc::SymbolLookupFlags::WeaklyReferencedSymbol;
        break;
      }
      // llvm::dbgs() << "llvm-internal:" << KV.first.str() << "\n";
      LookupSet.add(ES.intern(KV.first), LookupFlags);
    }

    // OnResolve -- De-intern the symbols and pass the result to the linker.
    auto OnResolve = [LookupContinuation =
                          std::move(LC)](Expected<SymbolMap> Result) mutable {
      if (!Result)
        LookupContinuation->run(Result.takeError());
      else {
        AsyncLookupResult LR;
        for (auto &KV : *Result)
          LR[*KV.first] = KV.second;
        LookupContinuation->run(std::move(LR));
      }
    };

    for (auto &KV : InternalNamedSymbolDeps) {
      SymbolDependenceMap InternalDeps;
      InternalDeps[&MR->getTargetJITDylib()] = std::move(KV.second);
      MR->addDependencies(KV.first, InternalDeps);
    }

    ES.lookup(LookupKind::Static, LinkOrder, std::move(LookupSet),
              SymbolState::Resolved, std::move(OnResolve),
              [this](const SymbolDependenceMap &Deps) {
                registerDependencies(Deps);
              });
  }

  Error notifyResolved(LinkGraph &G) override {
    auto &ES = Layer.getExecutionSession();

    SymbolFlagsMap ExtraSymbolsToClaim;
    bool AutoClaim = Layer.AutoClaimObjectSymbols;

    SymbolMap InternedResult;
    for (auto *Sym : G.defined_symbols())
      if (Sym->hasName() && Sym->getScope() != Scope::Local) {
        auto InternedName = ES.intern(Sym->getName());
        JITSymbolFlags Flags;

        if (Sym->isCallable())
          Flags |= JITSymbolFlags::Callable;
        if (Sym->getScope() == Scope::Default)
          Flags |= JITSymbolFlags::Exported;

        InternedResult[InternedName] =
            JITEvaluatedSymbol(Sym->getAddress(), Flags);
        if (AutoClaim && !MR->getSymbols().count(InternedName)) {
          assert(!ExtraSymbolsToClaim.count(InternedName) &&
                 "Duplicate symbol to claim?");
          ExtraSymbolsToClaim[InternedName] = Flags;
        }
      }

    for (auto *Sym : G.absolute_symbols())
      if (Sym->hasName()) {
        auto InternedName = ES.intern(Sym->getName());
        JITSymbolFlags Flags;
        Flags |= JITSymbolFlags::Absolute;
        if (Sym->isCallable())
          Flags |= JITSymbolFlags::Callable;
        if (Sym->getLinkage() == Linkage::Weak)
          Flags |= JITSymbolFlags::Weak;
        InternedResult[InternedName] =
            JITEvaluatedSymbol(Sym->getAddress(), Flags);
        if (AutoClaim && !MR->getSymbols().count(InternedName)) {
          assert(!ExtraSymbolsToClaim.count(InternedName) &&
                 "Duplicate symbol to claim?");
          ExtraSymbolsToClaim[InternedName] = Flags;
        }
      }

    if (!ExtraSymbolsToClaim.empty())
      if (auto Err = MR->defineMaterializing(ExtraSymbolsToClaim))
        return Err;

    {

      // Check that InternedResult matches up with MR->getSymbols().
      // This guards against faulty transformations / compilers / object caches.

      // First check that there aren't any missing symbols.
      size_t NumMaterializationSideEffectsOnlySymbols = 0;
      SymbolNameVector ExtraSymbols;
      SymbolNameVector MissingSymbols;
      for (auto &KV : MR->getSymbols()) {

        // If this is a materialization-side-effects only symbol then bump
        // the counter and make sure it's *not* defined, otherwise make
        // sure that it is defined.
        if (KV.second.hasMaterializationSideEffectsOnly()) {
          ++NumMaterializationSideEffectsOnlySymbols;
          if (InternedResult.count(KV.first))
            ExtraSymbols.push_back(KV.first);
          continue;
        } else if (!InternedResult.count(KV.first))
          MissingSymbols.push_back(KV.first);
      }

      // If there were missing symbols then report the error.
      if (!MissingSymbols.empty())
        return make_error<MissingSymbolDefinitions>(G.getName(),
                                                    std::move(MissingSymbols));

      // If there are more definitions than expected, add them to the
      // ExtraSymbols vector.
      if (InternedResult.size() >
          MR->getSymbols().size() - NumMaterializationSideEffectsOnlySymbols) {
        for (auto &KV : InternedResult)
          if (!MR->getSymbols().count(KV.first))
            ExtraSymbols.push_back(KV.first);
      }

      // If there were extra definitions then report the error.
      if (!ExtraSymbols.empty())
        return make_error<UnexpectedSymbolDefinitions>(G.getName(),
                                                       std::move(ExtraSymbols));
    }

    if (auto Err = MR->notifyResolved(InternedResult))
      return Err;

    Layer.notifyLoaded(*MR);
    return Error::success();
  }

  void notifyFinalized(
      std::unique_ptr<JITLinkMemoryManager::Allocation> A) override {
    if (auto Err = Layer.notifyEmitted(*MR, std::move(A))) {
      Layer.getExecutionSession().reportError(std::move(Err));
      MR->failMaterialization();
      return;
    }
    if (auto Err = MR->notifyEmitted()) {
      Layer.getExecutionSession().reportError(std::move(Err));
      MR->failMaterialization();
    }
  }

  LinkGraphPassFunction getMarkLivePass(const Triple &TT) const override {
    return [this](LinkGraph &G) { return markResponsibilitySymbolsLive(G); };
  }

  Error modifyPassConfig(const Triple &TT, PassConfiguration &Config) override {
    // Add passes to mark duplicate defs as should-discard, and to walk the
    // link graph to build the symbol dependence graph.
    Config.PrePrunePasses.push_back([this](LinkGraph &G) {
      return claimOrExternalizeWeakAndCommonSymbols(G);
    });

    Layer.modifyPassConfig(*MR, TT, Config);

    Config.PostPrunePasses.push_back(
        [this](LinkGraph &G) { return computeNamedSymbolDependencies(G); });

    return Error::success();
  }

private:
  struct LocalSymbolNamedDependencies {
    SymbolNameSet Internal, External;
  };

  using LocalSymbolNamedDependenciesMap =
      DenseMap<const Symbol *, LocalSymbolNamedDependencies>;

  Error claimOrExternalizeWeakAndCommonSymbols(LinkGraph &G) {
    auto &ES = Layer.getExecutionSession();

    SymbolFlagsMap NewSymbolsToClaim;
    std::vector<std::pair<SymbolStringPtr, Symbol *>> NameToSym;

    auto ProcessSymbol = [&](Symbol *Sym) {
      if (Sym->hasName() && Sym->getLinkage() == Linkage::Weak) {
        auto Name = ES.intern(Sym->getName());
        if (!MR->getSymbols().count(ES.intern(Sym->getName()))) {
          JITSymbolFlags SF = JITSymbolFlags::Weak;
          if (Sym->getScope() == Scope::Default)
            SF |= JITSymbolFlags::Exported;
          NewSymbolsToClaim[Name] = SF;
          NameToSym.push_back(std::make_pair(std::move(Name), Sym));
        }
      }
    };

    for (auto *Sym : G.defined_symbols())
      ProcessSymbol(Sym);
    for (auto *Sym : G.absolute_symbols())
      ProcessSymbol(Sym);

    // Attempt to claim all weak defs that we're not already responsible for.
    // This cannot fail -- any clashes will just result in rejection of our
    // claim, at which point we'll externalize that symbol.
    cantFail(MR->defineMaterializing(std::move(NewSymbolsToClaim)));

    for (auto &KV : NameToSym)
      if (!MR->getSymbols().count(KV.first))
        G.makeExternal(*KV.second);

    return Error::success();
  }

  Error markResponsibilitySymbolsLive(LinkGraph &G) const {
    auto &ES = Layer.getExecutionSession();
    for (auto *Sym : G.defined_symbols())
      if (Sym->hasName() && MR->getSymbols().count(ES.intern(Sym->getName())))
        Sym->setLive(true);
    return Error::success();
  }

  Error computeNamedSymbolDependencies(LinkGraph &G) {
    auto &ES = MR->getTargetJITDylib().getExecutionSession();
    auto LocalDeps = computeLocalDeps(G);

    // Compute dependencies for symbols defined in the JITLink graph.
    for (auto *Sym : G.defined_symbols()) {

      // Skip local symbols: we do not track dependencies for these.
      if (Sym->getScope() == Scope::Local)
        continue;
      assert(Sym->hasName() &&
             "Defined non-local jitlink::Symbol should have a name");

      SymbolNameSet ExternalSymDeps, InternalSymDeps;

      // Find internal and external named symbol dependencies.
      for (auto &E : Sym->getBlock().edges()) {
        auto &TargetSym = E.getTarget();

        if (TargetSym.getScope() != Scope::Local) {
          if (TargetSym.isExternal())
            ExternalSymDeps.insert(ES.intern(TargetSym.getName()));
          else if (&TargetSym != Sym)
            InternalSymDeps.insert(ES.intern(TargetSym.getName()));
        } else {
          assert(TargetSym.isDefined() &&
                 "local symbols must be defined");
          auto I = LocalDeps.find(&TargetSym);
          if (I != LocalDeps.end()) {
            for (auto &S : I->second.External)
              ExternalSymDeps.insert(S);
            for (auto &S : I->second.Internal)
              InternalSymDeps.insert(S);
          }
        }
      }

      if (ExternalSymDeps.empty() && InternalSymDeps.empty())
        continue;

      auto SymName = ES.intern(Sym->getName());
      if (!ExternalSymDeps.empty())
        ExternalNamedSymbolDeps[SymName] = std::move(ExternalSymDeps);
      if (!InternalSymDeps.empty())
        InternalNamedSymbolDeps[SymName] = std::move(InternalSymDeps);
    }

    for (auto &P : Layer.Plugins) {
      auto SyntheticLocalDeps = P->getSyntheticSymbolLocalDependencies(*MR);
      if (SyntheticLocalDeps.empty())
        continue;

      for (auto &KV : SyntheticLocalDeps) {
        auto &Name = KV.first;
        auto &LocalDepsForName = KV.second;
        for (auto *Local : LocalDepsForName) {
          assert(Local->getScope() == Scope::Local &&
                 "Dependence on non-local symbol");
          auto LocalNamedDepsItr = LocalDeps.find(Local);
          if (LocalNamedDepsItr == LocalDeps.end())
            continue;
          for (auto &S : LocalNamedDepsItr->second.Internal)
            InternalNamedSymbolDeps[Name].insert(S);
          for (auto &S : LocalNamedDepsItr->second.External)
            ExternalNamedSymbolDeps[Name].insert(S);
        }
      }
    }

    return Error::success();
  }

  LocalSymbolNamedDependenciesMap computeLocalDeps(LinkGraph &G) {
    DenseMap<jitlink::Symbol *, DenseSet<jitlink::Symbol *>> DepMap;

    // For all local symbols:
    // (1) Add their named dependencies.
    // (2) Add them to the worklist for further iteration if they have any
    //     depend on any other local symbols.
    struct WorklistEntry {
      WorklistEntry(Symbol *Sym, DenseSet<Symbol *> LocalDeps)
          : Sym(Sym), LocalDeps(std::move(LocalDeps)) {}

      Symbol *Sym = nullptr;
      DenseSet<Symbol *> LocalDeps;
    };
    std::vector<WorklistEntry> Worklist;
    for (auto *Sym : G.defined_symbols())
      if (Sym->getScope() == Scope::Local) {
        auto &SymNamedDeps = DepMap[Sym];
        DenseSet<Symbol *> LocalDeps;

        for (auto &E : Sym->getBlock().edges()) {
          auto &TargetSym = E.getTarget();
          if (TargetSym.getScope() != Scope::Local)
            SymNamedDeps.insert(&TargetSym);
          else {
            assert(TargetSym.isDefined() &&
                   "local symbols must be defined");
            LocalDeps.insert(&TargetSym);
          }
        }

        if (!LocalDeps.empty())
          Worklist.push_back(WorklistEntry(Sym, std::move(LocalDeps)));
      }

    // Loop over all local symbols with local dependencies, propagating
    // their respective non-local dependencies. Iterate until we hit a stable
    // state.
    bool Changed;
    do {
      Changed = false;
      for (auto &WLEntry : Worklist) {
        auto *Sym = WLEntry.Sym;
        auto &NamedDeps = DepMap[Sym];
        auto &LocalDeps = WLEntry.LocalDeps;

        for (auto *TargetSym : LocalDeps) {
          auto I = DepMap.find(TargetSym);
          if (I != DepMap.end())
            for (const auto &S : I->second)
              Changed |= NamedDeps.insert(S).second;
        }
      }
    } while (Changed);

    // Intern the results to produce a mapping of jitlink::Symbol* to internal
    // and external symbol names.
    auto &ES = Layer.getExecutionSession();
    LocalSymbolNamedDependenciesMap Result;
    for (auto &KV : DepMap) {
      auto *Local = KV.first;
      assert(Local->getScope() == Scope::Local &&
             "DepMap keys should all be local symbols");
      auto &LocalNamedDeps = Result[Local];
      for (auto *Named : KV.second) {
        assert(Named->getScope() != Scope::Local &&
               "DepMap values should all be non-local symbol sets");
        if (Named->isExternal())
          LocalNamedDeps.External.insert(ES.intern(Named->getName()));
        else
          LocalNamedDeps.Internal.insert(ES.intern(Named->getName()));
      }
    }

    return Result;
  }

  void registerDependencies(const SymbolDependenceMap &QueryDeps) {
    for (auto &NamedDepsEntry : ExternalNamedSymbolDeps) {
      auto &Name = NamedDepsEntry.first;
      auto &NameDeps = NamedDepsEntry.second;
      SymbolDependenceMap SymbolDeps;

      for (const auto &QueryDepsEntry : QueryDeps) {
        JITDylib &SourceJD = *QueryDepsEntry.first;
        const SymbolNameSet &Symbols = QueryDepsEntry.second;
        auto &DepsForJD = SymbolDeps[&SourceJD];

        for (const auto &S : Symbols)
          if (NameDeps.count(S))
            DepsForJD.insert(S);

        if (DepsForJD.empty())
          SymbolDeps.erase(&SourceJD);
      }

      MR->addDependencies(Name, SymbolDeps);
    }
  }

  JuliaObjectLinkingLayer &Layer;
  std::unique_ptr<MaterializationResponsibility> MR;
  std::unique_ptr<JuliaObjectFile> ObjectFile;
  std::unique_ptr<MemoryBuffer> ObjBuffer;
  DenseMap<SymbolStringPtr, SymbolNameSet> ExternalNamedSymbolDeps;
  DenseMap<SymbolStringPtr, SymbolNameSet> InternalNamedSymbolDeps;
  jl_module_t* JLModule;
  StaticJuliaJIT* jit;
};

void JuliaObjectLinkingLayer::emit(std::unique_ptr<MaterializationResponsibility> R,
                              std::unique_ptr<JuliaObjectFile> ObjectFile, 
                              std::unique_ptr<LinkGraph> G) {
  link(std::move(G), std::make_unique<JuliaObjectLinkingLayerJITLinkContext>(
                         jit, *this, std::move(R), std::move(ObjectFile)));
}