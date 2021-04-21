/*!
 * Copyright (c) 2020 by Contributors
 * \file liveness_analysis.h
 * \brief A pass for analyzing tensor liveness.
 */
#pragma once
#include <vector>
#include "mnm/op.h"
#include "mnm/ir.h"
#include "mnm/pass.h"
#include "tvm/ir/type_functor.h"
#include "./let_list.h"
#include "./common.h"

namespace mnm {
namespace pass {
namespace liveness_analysis {

/*
 * Note that plain liveness analysis [1] is not applicable to non-effect IR nor transitive,
 * so we transform the IR in advance:
 *
 * We analyze against (dummy) tensor vars, instead of the original vars
 * in our function. Each tensor var (%t[0...3] in the following example)
 * is the smallest unit for memory allocation. We first obtain the set
 * of tensor var contained by each original var:
 *
 * let %a1 = batch_norm(%x, %mean, %var, %w, %b)    | %a1 = {%t0, %t1, %t2}
 * let %a2 = %a1.0                                  | %a2 = {%t0,}
 * let %a3 = %a1.1                                  | %a3 = {%t1,}
 * let %a4 = %a1.2                                  | %a4 = {%t2,}
 * let %a5 = add(%a3, %a4)                          | %a5 = {%t3,}
 * let %a6 = (%a2, %5)                              | %a6 = {%t0, %t3}
 * %a6                                              |
 *
 * The memory sharing relations over tensor vars are transitive:
 * %tx ~ %ty, %ty ~ %tz => %tx ~ %tz
 *
 * Our algorithm works as follows:
 * 1. obtain the set of tensor var contained by each original var, in ForwardAnalyzer
 * 2. obtain the set of live tensor vars at each line, in BackwardAnalyzer.
 *    Following liveness analysis for registers described in [1], live(l, t) denotes
 *    tensor var t has been defined at line l, and its value will be used at or after
 *    line l. We have rules:
 *    - use(l, x) => live(l, x)
 *    - live(l + 1, x) && !define(l, x) => live(l, x)
 *    where use(l, x) denotes that the computation of line l uses the value of x,
 *    and define(l, x) denotes that line l defines the value of x. x is a tensor var.
 *
 * References:
 * [1] https://www.cs.cmu.edu/~rjsimmon/15411-f15/lec/04-liveness.pdf
 */

using namespace mnm::ir;
using namespace mnm::op;
using tvm::TypeFunctor;
using VSet = std::unordered_set<Var, ObjectPtrHash, ObjectPtrEqual>;

template <typename T>
using StdMap = std::unordered_map<Var, T, ObjectPtrHash, ObjectPtrEqual>;
using MapVar = StdMap<Var>;
using MapVSet = StdMap<VSet>;
using MapFunction = StdMap<Function>;

class LivenessAnalyzer {
 public:
  LivenessAnalyzer(const Function& func) : func_(func) {
  }

  void Run();

  bool IsSuccess() {
    return !failure_;
  }

  MapVSet Results() {
    return inv_live_;
  }

  /*! \brief Get the dummy tensor variable created by CreateTensor.
             Undefined if no 1:1 correspondence */
  Var GetTensorVar(const Var& x) {
    const VSet& vset = vset_.at(x);
    if (vset.size() != 1) {
      return Var();
    }
    return *vset.begin();
  }

  /*! \brief Union-find Forest: Get root in Union-find Forest */
  Var Find(const Var& x) {
    CHECK(union_find_forest_.find(x) != union_find_forest_.end());
    if (x == union_find_forest_.at(x)) {
      return x;
    }
    Var root = Find(union_find_forest_.at(x));
    union_find_forest_[x] = root;
    return root;
  }

  /*! \brief Union-find Forest: Unite two trees in Union-find Forest */
  Var Unite(const Var& x, const Var& y) {
    Var fx = Find(x);
    Var fy = Find(y);
    union_find_forest_[fx] = fy;
    inv_live_[fy].insert(inv_live_.at(fx).begin(), inv_live_.at(fx).end());
    return fy;
  }

  /*! \brief check if inv_live_[x] and inv_live_[y] intersects or not */
  bool Intersect(const Var& x, const Var& y) {
    const VSet& sx = inv_live_.at(x);
    const VSet& sy = inv_live_.at(y);
    for (const auto& v : sx) {
      if (sy.find(v) != sy.end()) {
        return true;
      }
    }
    return false;
  }

  /*! \brief Debug output: vset[x] */
  std::string DebugDump_(MapVSet vset, Var x) {
    std::ostringstream os;
    os << x << ": ";
    if (vset.find(x) != vset.end()) {
      const VSet& vs = vset.at(x);
      for (const auto& v : vs) {
        os << v << ", ";
      }
      os << "\n";
    } else {
      os << "not exsit"
         << "\n";
    }
    return os.str();
  }

  /*! \brief Debug output: vset_ */
  std::string DebugDump(MapVSet vset, Var x = Var()) {
    std::ostringstream os;
    if (x.defined()) {
      return DebugDump_(vset, x);
    }
    for (const auto& kv : vset) {
      Var v = kv.first;
      os << DebugDump_(vset, v);
    }
    return os.str();
  }

 private:
  /*! \brief Create a dummy variable. */
  Var CreateTensorVar(const std::string& name = "t") {
    if (label_.find(name) == label_.end()) {
      label_[name] = 0;
    }
    int label = label_[name]++;
    std::string fullname = name + "_" + std::to_string(label);
    return MakeVar(fullname, {});
  }

  /*! \brief Create a dummy variable, which contains nothing. */
  Var CreateNull(const std::string& name = "n") {
    Var var = CreateTensorVar(name);
    vset_[var] = {};
    return var;
  }

  /*! \brief Create a dummy tensor variable, which contains itself. */
  Var CreateTensor(const std::string& name = "t") {
    Var var = CreateTensorVar(name);
    vset_[var] = {var};
    return var;
  }

  /*! \brief vset1 - vset2 */
  static VSet Remove(const VSet& vset1, const VSet& vset2) {
    VSet ret(vset1);
    for (const auto& var : vset2) {
      ret.erase(var);
    }
    return ret;
  }

  /*! \brief the union of vset1 and vset2 */
  static VSet Merge(const VSet& vset1, const VSet& vset2) {
    VSet ret(vset1);
    ret.insert(vset2.begin(), vset2.end());
    return ret;
  }

  /*! \brief Remove vset_[v2] from vset_[v1] */
  Var Remove(Var v1, Var v2) {
    const VSet& vset1 = vset_.at(v1);
    const VSet& vset2 = vset_.at(v2);
    Var rs = CreateTensorVar("rs");
    vset_[rs] = Remove(vset1, vset2);
    return rs;
  }

  /*! \brief Merge vset_[v1] and vset_[v2] */
  Var Merge(Var v1, Var v2) {
    const VSet& vset1 = vset_.at(v1);
    const VSet& vset2 = vset_.at(v2);
    Var ms = CreateTensorVar("ms");
    vset_[ms] = Merge(vset1, vset2);
    return ms;
  }

  /*! \brief Merge vset_[vars[i]] */
  Var Merge(Array<Var> vars) {
    size_t n = vars.size();
    if (n == 0) {
      return CreateNull();
    } else if (n == 1) {
      CHECK(vset_.find(vars[0]) != vset_.end());
      return vars[0];
    } else {
      Var ret = Merge(vars[0], vars[1]);
      for (size_t i = 2; i < n; ++i) {
        ret = Merge(ret, vars[i]);
      }
      return ret;
    }
  }

  /*! \brief Init vtuple_[to] and vset_[to] with from */
  void Init(Var to, Var from) {
    if (vtuple_.count(from) > 0) {
      CHECK_EQ(vtuple_.count(to), 0);
      vtuple_.Set(to, vtuple_.at(from));
    }
    CHECK(vset_.find(to) == vset_.end());
    vset_[to] = vset_[from];
  }

  /*! \brief Get free variables */
  static Array<Var> FreeVars(Expr e) {
    if (e.as<LetNode>()) {
      Function f({}, e, {}, {});
      Array<Var> free_vars = ::tvm::relay::FreeVars(f);
      return free_vars;
    } else if (e.as<VarNode>()) {
      return {Downcast<Var>(e)};
    } else if (e.as<FunctionNode>()) {
      return ::tvm::relay::FreeVars(Downcast<Function>(e));
    } else {
      LOG(FATAL) << "NotImplementedError: FreeVars for: " << e->GetTypeKey();
    }
  }

 private:
  class ForwardAnalyzer;
  class BackwardAnalyzer;
  class FormChecker;
  class VarCreator;

  /*!
   * \brief invoke ForwardAnalyzer for func:
   *        populate vset_ for all variables in e
   *        populate vtuple_ for all variables of TupleType in e
   * \param e the expression to be analyzed
   * \return the value of e
   * \note vset_ and vtuple_ free variables in e should be available already
   */
  Var Forward(const Expr& e);

  /*!
   * \brief invoke BackwardAnalyzer for func:
   *        populate live_ for each line in e
   * \param e the expression to be analyzed
   * \param next_var live_[next_var] is the live-out variables of e
   * \note vset_ should be available already
   */
  void Backward(const Expr& e, const Var& next_var);

  /*! \brief Check if e contains closure invoke */
  void FormCheck(const Expr& e);

  /*! \brief Create a variable of specified type */
  Var CreateTensorVar(const Type& type);

 private:
  /*! \brief the function to be analyzed */
  const Function& func_;
  /*! \brief whether func_ contains closure invoke */
  bool failure_{false};
  /*! \brief maps a var to the set of real or fake variables which share memory with the key */
  MapVSet vset_;
  /*! \brief maps a variable with TupleType to its constituent (fake) variables */
  Map<Var, Array<Var>> vtuple_;
  /*! \brief the live-in variables at a specific line */
  MapVSet live_;
  /*! \brief count the occurences of a var name, to avoid name collision */
  std::unordered_map<std::string, int> label_;
  /*! \brief mandatory memory sharing between a pair of vars */
  Array<Var> var_out_, var_in_;
  /*! \brief vars that share memory with one another are merged in the union find forest */
  std::unordered_map<Var, Var, ObjectPtrHash, ObjectPtrEqual> union_find_forest_;
  /*! \brief the lines where a variable is live.
             Initially it's the inversion of live_: inv_live_[x] = {y | x \in live_[y]}*/
  MapVSet inv_live_;
};

class LivenessAnalyzer::FormChecker : public ExprVisitor {
 public:
  FormChecker(const Expr& body, LivenessAnalyzer* analyzer) : body_(body), analyzer_(analyzer) {
  }

  void VisitExpr_(const CallNode* node);
  void VisitExpr_(const IfNode* node) override;
  void Run() {
    VisitExpr(body_);
  }

 private:
  /*! \brief the expression to be analyzed */
  const Expr& body_;
  /*! \brief the analyzer it belongs to */
  LivenessAnalyzer* analyzer_;
};

class LivenessAnalyzer::VarCreator : public TypeFunctor<Var(const Type& n)> {
 public:
  VarCreator(LivenessAnalyzer* analyzer) : analyzer_(analyzer) {
  }

  Var VisitType_(const TupleTypeNode* op) override {
    Array<Var> fields;
    for (const auto& field : op->fields) {
      Var var = VisitType(field);
      fields.push_back(var);
    }
    Var tvar = analyzer_->Merge(fields);
    analyzer_->vtuple_.Set(tvar, fields);
    return tvar;
  }

  Var VisitType_(const TensorTypeNode* op) override {
    return analyzer_->CreateTensor();
  }

  Var Run(const Type& type) {
    return VisitType(type);
  }

 private:
  /*! \brief the analyzer it belongs to */
  LivenessAnalyzer* analyzer_;
};

class LivenessAnalyzer::ForwardAnalyzer : public ExprVisitor {
 public:
  ForwardAnalyzer(const Expr& body, LivenessAnalyzer* analyzer)
      : body_(body), ell_(ExplicitLetList::make(body)), analyzer_(analyzer) {
  }

  void VisitExpr_(const FunctionNode* node) override;
  void VisitExpr_(const CallNode* node) override;
  void VisitExpr_(const TupleNode* node) override;
  void VisitExpr_(const TupleGetItemNode* node) override;
  void VisitExpr_(const IfNode* node) override;
  void Match(Var v1, Var v2);
  Var Run();

 private:
  /*! \brief the expression to be analyzed */
  const Expr& body_;
  /*! \brief the explicit let list of func_ */
  std::unique_ptr<ExplicitLetList> ell_{nullptr};
  /*! \brief a variable that is set for each let expr */
  Var let_var_;
  /*! \brief the analyzer it belongs to */
  LivenessAnalyzer* analyzer_;
};

class LivenessAnalyzer::BackwardAnalyzer : public ExprVisitor {
 public:
  BackwardAnalyzer(const Expr& body, LivenessAnalyzer* analyzer)
      : body_(body), ell_(ExplicitLetList::make(body)), analyzer_(analyzer) {
  }

  void VisitExpr_(const FunctionNode* node) override;
  void VisitExpr_(const CallNode* node) override;
  void VisitExpr_(const TupleNode* node) override;
  void VisitExpr_(const TupleGetItemNode* node) override;
  void VisitExpr_(const IfNode* node) override;
  void VisitBranch(const Expr& branch, const Var& def);
  void Run(Var next_var);

 private:
  /*! \brief returns live_[next_var_] - vset_[def] + vset_[cur]
             it's an instantiation of the following rule:
             live(l + 1, x) && !define(l, x) => live(l, x) */
  Var MergeLive(const Var& cur, const Var& def = Var()) {
    Var next_line_var = analyzer_->CreateTensorVar("ml");
    analyzer_->vset_[next_line_var] = analyzer_->live_.at(next_var_);
    Var remain = next_line_var;
    if (def.defined()) {
      remain = analyzer_->Remove(remain, def);
    }
    Var ret = analyzer_->Merge(remain, cur);
    return ret;
  }

 private:
  /*! \brief the expression to be analyzed */
  const Expr& body_;
  /*! \brief the explicit let list of func_ */
  std::unique_ptr<ExplicitLetList> ell_{nullptr};
  /*! \brief a variable that is set for each let expr */
  Var let_var_;
  /*! \brief the variable next to let_var_ */
  Var next_var_;
  /*! \brief the analyzer it belongs to */
  LivenessAnalyzer* analyzer_;
};

}  // namespace liveness_analysis
}  // namespace pass
}  // namespace mnm