/*!
 * Copyright (c) 2020 by Contributors
 * \file type_infer.cc
 * \brief Type inference pass
 */

#include <tvm/ir/module.h>
#include <tvm/ir/type_functor.h>
#include <tvm/tir/op.h>
#include <tvm/relay/transform.h>
#include <tvm/relay/analysis.h>
#include "mnm/op.h"
#include "mnm/ir.h"
#include "mnm/binding.h"
#include "mnm/type.h"
#include "../op/ty/utils.h"

namespace mnm {
namespace pass {
namespace type_infer {

using namespace mnm::ir;
using namespace mnm::op;
using namespace mnm::value;
using namespace mnm::type;
using namespace tvm;
using namespace tvm::relay;
using tvm::TypeFunctor;
using tvm::relay::transform::Pass;

Type Unify(const Type& src, const Type& dst);

Value GetValue(Type type);

Value GetValue(Expr expr);

#define MNM_NODE_NOT_IMPL(NodeType)                     \
  Value VisitExpr_(const NodeType* node) override {     \
    LOG(FATAL) << "NotImplementedError: " << #NodeType; \
    throw;                                              \
  }

class TypeInferencer : public ExprMutator {
 public:
  // MNM_NODE_NOT_IMPL(RefReadNode)
  // MNM_NODE_NOT_IMPL(RefWriteNode)
  // MNM_NODE_NOT_IMPL(RefCreateNode)

 public:
  TypeInferencer(Module mod) : mod_(mod) {
  }

  Type GetValueType(const Value& v) {
    return op::type::GetType(v);
  }

  Expr VisitExpr_(const VarNode* op) override {
    if (op->type_annotation.defined()) {
      op->checked_type_ = op->type_annotation;
    } else if (!op->checked_type_.defined()) {
      op->checked_type_ = IncompleteType(kType);
    }
    return GetRef<Var>(op);
  }

  Expr VisitExpr_(const GlobalVarNode* op) override {
    CHECK(mod_.defined());
    return VisitExpr(mod_->Lookup(GetRef<GlobalVar>(op)));
  }

  Expr VisitExpr_(const CallNode* call) override {
    Array<Expr> args;
    for (const auto& arg : call->args) {
      args.push_back(VisitExpr(arg));
    }
    Expr op = VisitExpr(call->op);
    Call ret = Call(op, args, call->attrs, call->type_args);
    if (const FunctionNode* fn = ret->op.as<FunctionNode>()) {
      ret->checked_type_ = InferClosure(ret, fn);
    } else if (const OpNode* opn = ret->op.as<OpNode>()) {
      ret->checked_type_ = InferPrimitive(ret, opn);
    } else {
      LOG(FATAL) << "Invalid op type: " << call->op->GetTypeKey();
    }
    return ret;
  }

  Type InferPrimitive(const Call& call, const OpNode* op) {
    // Only type inference from leaf to root is supported.
    // Thus incomplete inputs will not be inferred from outputs.
    // Instead, the incompleteness propogates.
    for (const auto& arg : call->args) {
      const Type& type = arg->checked_type();
      if (const auto* itn = type.as<IncompleteTypeNode>()) {
        return IncompleteType(kType);
      }
    }
    // convert to CallValue
    static auto fschema = Op::GetAttrMap<op::FMNMSchema>("FMNMSchema");
    CallValues call_values = CallValues::make();
    Array<Value> arg_values;
    for (const auto& arg : call->args) {
      arg_values.push_back(GetValue(arg));
    }
    call_values->args = fschema[GetRef<Op>(op)](arg_values);
    call_values->callee = OpValue::make(GetRef<Op>(op));
    // invoke type inference
    auto fty = Downcast<FuncType>(op->checked_type());
    CHECK_EQ(fty->type_constraints.size(), 1);
    TypeInference ti = Downcast<TypeInference>(fty->type_constraints[0]);
    return ti->func(call_values);
  }

  Type InferClosure(const Call& call, const FunctionNode* op) {
    // TODO(@hzfan): perform template param deduction to eliminate type_params
    FuncType fty = Downcast<FuncType>(op->checked_type());
    return fty->ret_type;
  }

  Expr VisitExpr_(const RelayConstantNode* op) override {
    using tensor::Tensor;
    op->checked_type_ = GetValueType(TensorValue::make(Tensor::FromDLPack(op->data.ToDLPack())));
    return GetRef<Expr>(op);
  }

  Expr VisitExpr_(const IfNode* node) override {
    Expr cond = VisitExpr(node->cond);
    Expr true_branch = VisitExpr(node->true_branch);
    Expr false_branch = VisitExpr(node->false_branch);
    Expr ret = If(cond, true_branch, false_branch);
    ret->checked_type_ = Unify(true_branch->checked_type(), false_branch->checked_type());
    return ret;
  }

  Expr VisitExpr_(const LetNode* op) override {
    Expr value = VisitExpr(op->value);
    Var var = op->var;
    var->checked_type_ = value->checked_type();
    Expr body = VisitExpr(op->body);
    Let let(var, value, body);
    let->checked_type_ = body->checked_type();
    return let;
  }

  Expr VisitExpr_(const TupleNode* op) override {
    Array<Expr> fields;
    Array<Type> types;
    for (const auto& e : op->fields) {
      auto f = VisitExpr(e);
      fields.push_back(f);
      types.push_back(f->checked_type());
    }
    Tuple ret(fields);
    ret->checked_type_ = TupleType(types);
    return ret;
  }

  Expr VisitExpr_(const TupleGetItemNode* op) override {
    auto tup = VisitExpr(op->tuple);
    TupleGetItem ret(tup, op->index);
    ret->checked_type_ = Downcast<TupleType>(tup->checked_type())->fields[op->index];
    return ret;
  }

  Expr VisitExpr_(const OpNode* op) override {
    static const auto op_type = Op::GetAttrMap<OpType>("OpType");
    op->checked_type_ = op_type[GetRef<Op>(op)];
    return GetRef<Expr>(op);
  }

  Expr VisitExpr_(const FunctionNode* op) override {
    Array<Var> params;
    Array<Type> param_types;
    for (const auto& p : op->params) {
      Var param = Downcast<Var>(VisitExpr(p));
      params.push_back(param);
      param_types.push_back(param->checked_type());
    }
    Expr body = VisitExpr(op->body);
    Type ret_type =
        op->ret_type.defined() ? Unify(body->checked_type(), op->ret_type) : body->checked_type();
    Function func(params, body, ret_type, op->type_params, op->attrs);
    func->checked_type_ = FuncType(param_types, ret_type, op->type_params, {});
    return func;
  }

 private:
  Module mod_;
};

class Unifier : public TypeFunctor<Type(const Type&, const Type&)> {
 public:
  Type Unify(const Type& src, const Type& dst) {
    if (src.as<IncompleteTypeNode>() || !src.defined()) {
      return dst;
    } else if (dst.as<IncompleteTypeNode>() || !dst.defined()) {
      return src;
    } else {
      Type resolved = this->VisitType(src, dst);
      CHECK(resolved.defined()) << "unable to unify: "
                                << "`" << PrettyPrint(src) << "` and `" << PrettyPrint(dst) << "`";
      return resolved;
    }
  }

  // default: unify only if structural-equal
  Type VisitTypeDefault_(const Object* op, const Type& tn) final {
    ObjectRef nr = GetRef<ObjectRef>(op);
    Type t1 = GetRef<Type>(nr.as<tvm::relay::TypeNode>());
    if (!tvm::StructuralEqual()(t1, tn)) {
      return Type(nullptr);
    }
    return t1;
  }

  IndexExpr UnifyDim(const IndexExpr& lhs, const IndexExpr& rhs) {
    if (lhs.same_as(rhs)) {
      return lhs;
    }
    if (lhs.as<AnyNode>() || rhs.as<AnyNode>()) {
      return Any();
    }

    auto left_index0 = lhs.as<tvm::tir::VarNode>();
    auto right_index0 = rhs.as<tvm::IntImmNode>();
    if (left_index0 && right_index0) {
      return rhs;
    }

    auto left_index1 = lhs.as<tvm::IntImmNode>();
    auto right_index1 = rhs.as<tvm::tir::VarNode>();
    if (left_index1 && right_index1) {
      return lhs;
    }

    auto left_index2 = lhs.as<tvm::IntImmNode>();
    auto right_index2 = rhs.as<tvm::IntImmNode>();
    if (left_index2 && right_index2 && left_index2->value == right_index2->value) {
      return lhs;
    }

    return tvm::PrimExpr();
  }

  Type VisitType_(const TensorTypeNode* op, const Type& tn) final {
    const auto* tt_node = tn.as<TensorTypeNode>();
    CHECK(tt_node);
    auto tt1 = GetRef<TensorType>(op);
    auto tt2 = GetRef<TensorType>(tt_node);
    if (tvm::StructuralEqual()(tt1, tt2)) {
      return std::move(tt1);
    }
    CHECK(tt1->dtype == tt2->dtype);

    tvm::Array<IndexExpr> shape;
    CHECK_EQ(tt1->shape.size(), tt2->shape.size())
        << "tensor type `" << PrettyPrint(tt1) << "` has " << tt1->shape.size()
        << " dimensions, while `" << PrettyPrint(tt2) << "` has " << tt2->shape.size()
        << " dimensions";

    CHECK_EQ(tt1->shape.size(), tt2->shape.size());
    for (size_t i = 0; i < tt1->shape.size(); i++) {
      auto dim = UnifyDim(tt1->shape[i], tt2->shape[i]);
      CHECK(dim.defined());
      shape.push_back(dim);
    }
    return TensorType(shape, tt1->dtype);
  }

  Type VisitType_(const TupleTypeNode* op, const Type& tn) final {
    const auto* ttn = tn.as<TupleTypeNode>();
    CHECK(ttn && op->fields.size() == ttn->fields.size());

    TupleType tt1 = GetRef<TupleType>(op);
    TupleType tt2 = GetRef<TupleType>(ttn);

    std::vector<Type> new_fields;
    for (size_t i = 0; i < tt1->fields.size(); i++) {
      Type field = Unify(tt1->fields[i], tt2->fields[i]);
      new_fields.push_back(field);
    }
    return TupleType(new_fields);
  }

  Type VisitType_(const FuncTypeNode* op, const Type& tn) final {
    const auto* ftn = tn.as<FuncTypeNode>();
    CHECK(ftn && op->arg_types.size() == ftn->arg_types.size() &&
          op->type_constraints.size() == ftn->type_constraints.size());

    // without loss of generality, suppose op->type_params.size() >= ftn->type_params.size().
    if (op->type_params.size() < ftn->type_params.size()) {
      return VisitType_(ftn, GetRef<FuncType>(op));
    }

    // remap type vars so they match
    Map<TypeVar, Type> subst_map;
    tvm::Array<TypeVar> ft_type_params;
    for (size_t i = 0; i < ftn->type_params.size(); ++i) {
      subst_map.Set(op->type_params[i], ftn->type_params[i]);
      ft_type_params.push_back(op->type_params[i]);
    }

    for (size_t i = ftn->type_params.size(); i < op->type_params.size(); ++i) {
      subst_map.Set(op->type_params[i], IncompleteType(kType));
    }

    FuncType ft = FuncType(op->arg_types, op->ret_type, ft_type_params, op->type_constraints);
    auto ft1 = Downcast<FuncType>(Bind(ft, subst_map));
    auto ft2 = GetRef<FuncType>(ftn);

    Type ret_type = Unify(ft1->ret_type, ft2->ret_type);

    std::vector<Type> arg_types;
    for (size_t i = 0; i < ft2->arg_types.size(); ++i) {
      Type arg_type = Unify(ft1->arg_types[i], ft2->arg_types[i]);
      arg_types.push_back(arg_type);
    }

    std::vector<TypeConstraint> type_constraints;
    for (size_t i = 0; i < ft1->type_constraints.size(); ++i) {
      Type unified_constraint = Unify(ft1->type_constraints[i], ft2->type_constraints[i]);
      const auto* tcn = unified_constraint.as<TypeConstraintNode>();
      CHECK(tcn) << "Two type constraints unified into a non-constraint?"
                 << ft1->type_constraints[i] << " and " << ft2->type_constraints[i];
      type_constraints.push_back(GetRef<TypeConstraint>(tcn));
    }

    return FuncType(arg_types, ret_type, ft2->type_params, type_constraints);
  }

  Type VisitType_(const TypeCallNode* op, const Type& tn) override {
    const auto* tcn = tn.as<TypeCallNode>();
    if (!tcn || tcn->args.size() != op->args.size()) {
      return Type();
    }

    Type func = Unify(op->func, tcn->func);
    tvm::Array<Type> args;
    for (size_t i = 0; i < op->args.size(); i++) {
      args.push_back(Unify(op->args[i], tcn->args[i]));
    }
    return TypeCall(func, args);
  }
};

class TypeGetter : public TypeFunctor<Value(const Type&)> {
  Value VisitType_(const TensorTypeNode* op) {
    return TensorTypeValue::make(GetRef<TensorType>(op));
  }
  Value VisitType_(const TupleTypeNode* op) {
    Array<Value> ret;
    for (const auto& ty : op->fields) {
      ret.push_back(VisitType(ty));
    }
    return TupleValue::make(ret);
  }
};

class ValueGetter : public ExprFunctor<Value(const Expr&)> {
  Value VisitExpr_(const RelayConstantNode* op) {
    const ConstantNode* node = static_cast<const ConstantNode*>(op);
    return node->value.defined() ? Downcast<Value>(node->value) : NullValue<Value>();
  }

  Value VisitExprDefault_(const Object* op) {
    const auto* e = static_cast<const ExprNode*>(op);
    return GetValue(e->checked_type());
  }
};

Value GetValue(Type type) {
  return TypeGetter()(type);
}

Value GetValue(Expr expr) {
  return ValueGetter()(expr);
}

Type Unify(const Type& src, const Type& dst) {
  Unifier unifier;
  return unifier.Unify(src, dst);
}

}  // namespace type_infer

ir::Module InferType(ir::Module mod) {
  ir::Module updated_mod = ir::Module::make(mod->functions);
  auto ti = type_infer::TypeInferencer(updated_mod);
  std::vector<std::pair<ir::GlobalVar, ir::Function>> updated_funcs;
  for (auto kv : updated_mod->functions) {
    if (kv.second.as<ir::FunctionNode>()) {
      auto func = tvm::runtime::Downcast<ir::Function>(ti.VisitExpr(kv.second));
      updated_funcs.emplace_back(kv.first, func);
    }
  }

  for (const auto& it : updated_funcs) {
    updated_mod->Add(it.first, it.second, true);
  }
  return updated_mod;
}

MNM_REGISTER_GLOBAL("mnm.pass_.InferType").set_body_typed(InferType);

}  // namespace pass
}  // namespace mnm