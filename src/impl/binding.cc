/*!
 * Copyright (c) 2019 by Contributors
 * \file src/impl/binding.cc
 * \brief Frontend-defined varioble-expression-value bindings
 */
#include "mnm/binding.h"
#include "mnm/registry.h"

namespace mnm {
namespace binding {
namespace {
MNM_REGISTER_OBJECT_NO_REFLECT(GradTapeObj);
MNM_REGISTER_OBJECT_NO_REFLECT(BindingEntryObj);
MNM_REGISTER_OBJECT_NO_REFLECT(NDArrayBindingObj);
MNM_REGISTER_OBJECT_NO_REFLECT(SymbolBindingObj);
}  // namespace

using namespace mnm::ir;
using namespace mnm::value;

class BindingMgr {
 public:
  std::mutex mu;
  std::unordered_map<const VarNode*, BindingEntry> bindings;

  static BindingMgr* Get() {
    static BindingMgr* instance = new BindingMgr();
    return instance;
  }
};

class BoundVarObj : public VarNode {
  // This is basically relay::VarNode, but with a customized callback that
  // deletes the weak reference inside BindingMgr
 public:
  ~BoundVarObj() {
    static BindingMgr* mgr = BindingMgr::Get();
    BindingEntry entry{nullptr};
    {
      std::lock_guard<std::mutex> lock(mgr->mu);
      auto iter = mgr->bindings.find(this);
      CHECK(iter != mgr->bindings.end());
      entry = iter->second;
      mgr->bindings.erase(iter);
    }
    // "entry" is destroyed here, to avoid potential recursive lock
  }
  static Var make(const std::string& name_hint) {
    ObjectPtr<BoundVarObj> n = make_object<BoundVarObj>();
    ObjectPtr<IdNode> id_ptr = make_object<IdNode>();
    id_ptr->name_hint = name_hint;
    n->vid = Id(id_ptr);
    return Var(n);
  }
};

GradTape GradTape::make(Var grad, ClosureValue bp, Array<ObjectRef> prev_tapes) {
  ObjectPtr<GradTapeObj> n = make_object<GradTapeObj>();
  n->grad = std::move(grad);
  n->bp = std::move(bp);
  n->prev_tapes = std::move(prev_tapes);
  return GradTape(n);
}

NDArrayBinding NDArrayBinding::make(Value value, GradTape tape) {
  ObjectPtr<NDArrayBindingObj> n = make_object<NDArrayBindingObj>();
  n->value = std::move(value);
  n->tape = std::move(tape);
  return NDArrayBinding(n);
}

SymbolBinding SymbolBinding::make(Expr expr) {
  ObjectPtr<SymbolBindingObj> n = make_object<SymbolBindingObj>();
  n->expr = std::move(expr);
  return SymbolBinding(n);
}

Var MakeManagedBinding(const BindingEntry& entry, const std::string &name_hint) {
  static BindingMgr* mgr = BindingMgr::Get();
  static auto& bindings = mgr->bindings;
  Var var = BoundVarObj::make(name_hint);
  const VarNode* var_ptr = var.operator->();
  {
    std::lock_guard<std::mutex> lock(mgr->mu);
    bindings.emplace(var_ptr, entry);
  }
  return var;
}

Var BindNDArray(Value value, GradTape tape, std::string name_hint) {
  std::string grad_name_hint = "d" + name_hint;
  return MakeManagedBinding(NDArrayBinding::make(
        /*value=*/std::move(value),
        /*tape=*/tape), name_hint);
}

Var BindSymbol(Expr expr, std::string name_hint) {
  return MakeManagedBinding(SymbolBinding::make(std::move(expr)), name_hint);
}

BindingEntry LookupBinding(const VarNode *var) {
  static BindingMgr* mgr = BindingMgr::Get();
  static const auto& bindings = mgr->bindings;
  {
    std::lock_guard<std::mutex> lock(mgr->mu);
    auto iter = bindings.find(var);
    return iter != bindings.end() ? iter->second : NullValue<BindingEntry>();
  }
}

Value LookupBoundValue(Var var) {
  return Downcast<NDArrayBinding>(LookupBinding(var.operator->()))->value;
}

void SetRequiresGrad(Var var, bool value) {
  GradTape &tape = Downcast<NDArrayBinding>(LookupBinding(var.operator->()))->tape;
  if (tape.defined() != value) {
    if (value) {
      tape = GradTape::make(BindNDArray({}, {}, "d" + var->name_hint()), {}, {});
    } else {
      tape = NullValue<GradTape>();
    }
  }
}

MNM_REGISTER_GLOBAL("mnm.binding.BindNDArray").set_body_typed(BindNDArray);
MNM_REGISTER_GLOBAL("mnm.binding.BindSymbol").set_body_typed(BindSymbol);
MNM_REGISTER_GLOBAL("mnm.binding.LookupBoundValue").set_body_typed(LookupBoundValue);
MNM_REGISTER_GLOBAL("mnm.binding.SetRequiresGrad").set_body_typed(SetRequiresGrad);

}  // namespace binding
}  // namespace mnm