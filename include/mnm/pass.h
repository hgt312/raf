/*!
 * Copyright (c) 2019 by Contributors
 * \file pass.h
 * \brief A compatibility layer for TVM/Relay passes
 */
#pragma once

#include "tvm/ir/transform.h"
#include "tvm/relay/analysis.h"
#include "tvm/relay/dataflow_matcher.h"
#include "tvm/relay/transform.h"
#include "mnm/ir.h"
#include "mnm/ir_ext.h"
#include "mnm/pass_manager.h"

namespace mnm {
namespace pass {
using tvm::AsText;
using tvm::relay::ExpandANormalForm;
using tvm::relay::FreeVars;
using tvm::transform::CreateModulePass;
using tvm::transform::Pass;
using tvm::transform::PassContext;
using tvm::transform::PassInfo;
/*!
 * \brief Automatic Differentiation.
 * \param mod Input module.
 * \param requires_grads If input(s) of function requires gradient. It is in the same order as
 * func->param. If empty, input(s) with float datatype requires gradient.
 * \return Transformed Function.
 */
ir::IRModule AutoDiff(ir::IRModule mod, ir::Array<tvm::Bool> requires_grads = {});
ir::Function AutoDataParallel(ir::Function func);
ir::Expr FoldConstant(ir::Expr expr, ir::IRModule mod);
ir::Expr BindParam(ir::Function func, ir::Array<ir::Expr> args);
ir::IRModule LambdaLift(ir::IRModule mod);
/*!
 * \brief gradient operator input selection.
 * \param func The main Function.
 * \return Transformed Function.
 */
ir::Function GradInputSelect(ir::Function func);
/*!
 * \brief Manifest memory allocation.
 * \param mod The IR module.
 * \return Transformed IR module.
 */
ir::IRModule ManifestAlloc(ir::IRModule mod);
ir::Expr CanonicalizeOps(ir::Expr expr);
/*!
 * \brief Create a type inference pass.
 * \return The created pass.
 */
Pass InferType();
ir::Expr InferType(ir::Expr expr);
/*!
 * \brief Fuse the operators in the expression.
 * \param expr Expression to be fused.
 * \param fuse_opt_level Optimization level. If it is 0, then no operators will be fused.
 * \return Transformed expression.
 */
ir::Expr FuseOps(ir::Expr expr, int fuse_opt_level);

/*!
 * \brief remove unnecessary memory allocation and perform inplace updates.
 * \param mod The IR module.
 * \return Transformed IR module.
 */
ir::IRModule InplaceUpdate(ir::IRModule mod);

/*!
 * \brief Wraps an expr with compiler_begin and compiler_end to indicate that
 * this expr should be handled by the external compiler.
 * \param expr Expression to be annotated.
 * \param target he target backends for annotation.
 * \return Transformed Expression.
 */
ir::Expr AnnotateTarget(ir::Expr expr, ir::Array<ir::String> target);

/*!
 * \brief After operators have been annotated with the targets that support
 * them, this pass creates regions of the operators for each target. It
 * is guaranteed that the regions will have a topological rodering so that
 * no data dependency issue exist.
 *
 * This pass only introduces annotations to indicate the regions.
 * partition_graph must subsequently be called to lift these regions out
 * as external functions.
 * \param expr Expression to be merged.
 * \return Transformed Expression.
 */
ir::Expr MergeCompilerRegions(ir::Expr expr);

/*!
 * \brief Partition an input function into multiple functions according based
 * on the inserted annotation nodes (i.e. compiler_begin and compiler_end).
 * These nodes are used as boundaries to partition the Relay function into
 * multiple regions that can be offloaded to different accelerators/backends.
 *
 * Each of these paritioned functions, a.k.a regions, will be viewed as
 * external functions, and they will use the provided compiler for codegen.
 * \param expr Expression to be partition.
 * \return Parartioned Expression.
 */
ir::Expr PartitionGraph(ir::Expr expr);

/*!
 * \brief Cast input(s) of some operators in the expression.
 * \param expr Expression to be casted.
 * \return Transformed Expression.
 */
ir::Expr AutoCast(ir::Expr func);

/*!
 * \brief Inline the Let stmt that assigns a var to another and TupleGetItem that can be simplified.
 * \param expr Expression to be inlined.
 * \return Transformed Expression.
 */
ir::Expr InlineLet(ir::Expr expr);

/*! \brief Remove expressions which does not effect the program result.
 *
 * It will remove let bindings which are not referenced.
 *
 * For example, this pass should turn `let a = 1 in 2` into `2`,
 * as the value of the expression does not depend on a.
 *
 * As another example, `let a = 1 in a` will be optimized into 1.
 *
 * \param expr Expression to be transformed.
 *
 * \return Transformed Expression.
 */
ir::Expr DeadCodeElimination(const ir::Expr& expr);

/*! \brief Simplifies commonly seen patterns that can be removed at compile time.
 *
 * \param expr Expression to be transformed.
 *
 * \return Transformed Expression.
 */
ir::Expr SimplifyExpr(const ir::Expr& expr);

/*! \brief Convert Relay IR to Meta IR.
 * \param obj tvm::IRModule or ir::Expr
 * \return ir::IRModule or ir::Expr
 */
tvm::ObjectRef FromRelay(tvm::ObjectRef obj);

/*!
 * \brief inline backward function.
 * \param func The IR function.
 * \return inlined function.
 */
ir::Function InlineBackward(ir::Function func);

/*!
 * \brief Substitute variables in expr
 * \param expr The expression
 * \param args_map The substitution rule
 * \return Transformed expression
 */
ir::Expr Substitute(ir::Expr expr, const tvm::Map<ir::Var, ir::Expr>& args_map);

/*!
 * \brief Convert A-normal form to dataflow graph.
 * \param expr The expression
 * \return Transformed expression
 */
ir::Expr ToGraphNormalForm(ir::Expr expr);

/*!
 * \brief Replace init and constant ops with the assigned device.
 * \param expr Expression to be mutated.
 * \param device The target device.
 * \return Transformed expression.
 */
ir::Expr AssignDevice(ir::Expr expr, std::string device);

/*!
 * \brief Lifts if true and false branches to global functions.
 * \param mod The module to be mutated.
 * \return Transformed module.
 */
ir::IRModule LiftBranchBody(ir::IRModule mod);

/*!
 * \brief This is applied after Lambda lifting. Lambda lifting pass lifts the closures to global
 * scope, but the lifted global function still has the closure within. This makes AD harder. This
 * pass flattens the global functions that are marked Closure, and then changes the call sites
 * accordingly. This helps AD pass where it is difficult to handle closures.
 * \param mod The module to be mutated.
 * \return Transformed module.
 */
ir::IRModule FlattenClosure(ir::IRModule mod);

// TODO - Cleanup after pass manager is introduced. These passes are Function passes.
// Once pass manager is introduced, the pass manager can iterate over the functions.
// For now, the overloaded functions are iterating over functions.
ir::IRModule AssignDevice(ir::IRModule mod, std::string device);
ir::IRModule FuseOps(ir::IRModule mod, int fuse_opt_level);
ir::IRModule InlineLet(ir::IRModule mod);
ir::IRModule DeadCodeElimination(ir::IRModule mod);
ir::IRModule SimplifyExpr(ir::IRModule mod);
ir::IRModule ToGraphNormalForm(ir::IRModule mod);

/*!
 * \brief Turn a dataflow graph into Administrative Normal Form, or A-Normal Form (ANF).
 *
 * It will turn an expression that is in a graph form (with sharing implicit),
 * to an expression with explicit sharing (A-Normal Form).
 *
 * The scope of the root expression is the global scope.
 *
 * The scope of any non root expression is the least common ancestor of all it's scope.
 *
 * Values are ordered by post-DFS order in each scope.
 *
 * \param mod The input module.
 * \return Transformed module.
 */
ir::IRModule ToANormalForm(ir::IRModule mod);

/*!
 * \brief Turn an expression to Basic Block Normal Form.
 *
 * We define a block as a group of expressions implied by the scope structure.
 *
 * Each graph node can only belong to a single block.
 *
 * For any value that is being used in multiple blocks, it has to be referred
 * by a Var which is defined in a block, whose scope is the least common ancestor
 * of blocks this value is used.
 *
 * \param mod The input module
 * \return Transformed module.
 */
ir::IRModule ToBasicBlockNormalForm(ir::IRModule mod);

}  // namespace pass
}  // namespace mnm
