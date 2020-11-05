/*!
 * Copyright (c) 2020 by Contributors
 * \file src/op/ty/unary.cc
 * \brief Typing relations of unary operators
 */
#include <tvm/relay/type.h>
#include <tvm/ir/attrs.h>
#include <tvm/node/container.h>
#include <tvm/ir/env_func.h>
#include "mnm/type.h"
#include "../schema/ufunc.h"
#include "./utils.h"

namespace mnm {
namespace op {
namespace type {

using namespace mnm::value;
using schema::UnaryArgs;
using schema::UnaryDxArgs;
using schema::UnaryUfuncArgs;
using tvm::Array;
using tvm::Downcast;
using tvm::relay::TensorType;
using tvm::relay::TupleType;
using tvm::relay::Type;

Type UnaryInfer(const CallValues& value) {
  const auto* args = value->args.as<UnaryArgs>();
  CHECK(args != nullptr);
  // Unary ops' outputs are identical with inputs
  return GetType(args->x);
}

MNM_OP_TYPE("mnm.op.log", "Identity", UnaryInfer);
MNM_OP_TYPE("mnm.op.cos", "Identity", UnaryInfer);
MNM_OP_TYPE("mnm.op.relu", "Identity", UnaryInfer);

MNM_OP_TYPE("mnm.op.tanh", "Identity", UnaryInfer);
MNM_OP_TYPE("mnm.op.sigmoid", "Identity", UnaryInfer);
MNM_OP_TYPE("mnm.op.copy", "Identity", UnaryInfer);
MNM_OP_TYPE("mnm.op.abs", "Identity", UnaryInfer);
MNM_OP_TYPE("mnm.op.ceil", "Identity", UnaryInfer);
MNM_OP_TYPE("mnm.op.floor", "Identity", UnaryInfer);
MNM_OP_TYPE("mnm.op.exp", "Identity", UnaryInfer);
MNM_OP_TYPE("mnm.op.erf", "Identity", UnaryInfer);
MNM_OP_TYPE("mnm.op.sqrt", "Identity", UnaryInfer);
MNM_OP_TYPE("mnm.op.atan", "Identity", UnaryInfer);

Type UnaryDxInfer(const CallValues& value) {
  const auto* args = value->args.as<UnaryDxArgs>();
  CHECK(args != nullptr);
  // Unary ops' outputs are identical with inputs
  return GetType(args->x);
}

MNM_OP_TYPE("mnm.op.relu_dx", "IdentityDx", UnaryDxInfer);
MNM_OP_TYPE("mnm.op.tanh_dx", "IdentityDx", UnaryDxInfer);
MNM_OP_TYPE("mnm.op.sigmoid_dx", "IdentityDx", UnaryDxInfer);
MNM_OP_TYPE("mnm.op.erf_dx", "IdentityDx", UnaryDxInfer);
MNM_OP_TYPE("mnm.op.sqrt_dx", "IdentityDx", UnaryDxInfer);

Type UnaryUfuncInfer(const CallValues& value) {
  const auto* args = value->args.as<UnaryUfuncArgs>();
  CHECK(args != nullptr);
  // UnaryUfunc ops' outputs are identical with inputs
  return GetType(args->x);
}

MNM_OP_TYPE("mnm.op.negative", "IdentityUfunc", UnaryUfuncInfer);
MNM_OP_TYPE("mnm.op.logical_not", "IdentityUfunc", UnaryUfuncInfer);

Type UnaryShapeInfer(const CallValues& value) {
  const auto* args = value->args.as<UnaryArgs>();
  TensorType x = Downcast<TensorType>(GetType(args->x));
  Array<tvm::PrimExpr> shape;
  shape.push_back(tvm::PrimExpr((int32_t)x->shape.size()));
  return TensorType(shape, tvm::runtime::DataType::UInt(32));
}

MNM_OP_TYPE("mnm.op.shape", "Shape", UnaryShapeInfer);

}  // namespace type
}  // namespace op
}  // namespace mnm
