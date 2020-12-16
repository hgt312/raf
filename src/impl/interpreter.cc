/*!
 * Copyright (c) 2019 by Contributors
 * \file src/impl/interpreter.cc
 * \brief MNM interpreter, a naive implementation of executor
 */
#include "mnm/executor.h"
#include "mnm/ir.h"
#include "mnm/memory_pool.h"
#include "mnm/op.h"
#include "mnm/pass.h"
#include "mnm/registry.h"
#include "mnm/tensor.h"
#include "mnm/value.h"
#include "mnm/binding.h"
#include "mnm/profiler.h"
#include "mnm/communicator.h"
#include "dmlc/thread_local.h"
#include "../common/shape_utils.h"
#include "../requests.h"

namespace mnm {
namespace executor {
namespace interpreter {

using namespace mnm::ir;
using namespace mnm::value;
using namespace mnm::op;
using binding::BindingEntry;
using binding::BindNDArray;
using binding::DeTuple;
using binding::LookupBinding;
using binding::NDArrayBinding;
using common::shape_utils::BytesCompactTensor;
using memory_pool::Memory;
using requests::Requests;
using stream_pool::Stream;
using tensor::Tensor;

class SymbolTable {
 public:
  std::unordered_map<const VarNode*, std::vector<Value>> tab;

  Value Lookup(const Var& var) {
    auto iter = tab.find(var.operator->());
    if (iter != tab.end() && !iter->second.empty()) {
      return iter->second.back();
    }
    BindingEntry entry = LookupBinding(var.operator->());
    if (!entry.defined()) {
      LOG(FATAL) << "could not find variable binding for " << var->name_hint();
      throw;
    }
    return Downcast<NDArrayBinding>(entry)->value;
  }

  class AddVar {
   public:
    SymbolTable& st;
    Var var;
    explicit AddVar(SymbolTable& st, const Var& var, const Value& value) : st(st), var(var) {
      st.Extend_(var, value);
    }
    ~AddVar() {
      st.Pop_(var);
    }
  };

  class LocalFrame {
   public:
    SymbolTable& st;
    Map<Var, Value> frame;
    explicit LocalFrame(SymbolTable& st, Map<Var, Value>&& frame) : st(st), frame(frame) {
      for (auto iter : frame) {
        st.Extend_(iter.first, iter.second);
      }
    }
    ~LocalFrame() {
      for (auto iter : frame) {
        st.Pop_(iter.first);
      }
    }
  };

 private:
  void Extend_(const Var& var, const Value& value) {
    tab[var.operator->()].push_back(value);
  }

  void Pop_(const Var& var) {
    std::vector<Value>& values = tab.at(var.operator->());
    CHECK(!values.empty());
    values.pop_back();
  }
};

class Interpreter final : public ExprFunctor<Value(const Expr& n)>, public Executor {
 public:
  SymbolTable st;
  Module mod{nullptr};

 public:
  Interpreter() = default;
  ~Interpreter() = default;

  Value Eval(const Expr& expr) {
    return ExprFunctor<Value(const Expr& n)>::VisitExpr(expr);
  }

  Value VisitExpr(const Expr& expr) override {
    return Eval(expr);
  }

  Value VisitExpr_(const VarNode* node) override {
    return st.Lookup(GetRef<Var>(node));
  }

  Value VisitExpr_(const GlobalVarNode* node) override {
    return Eval(mod->Lookup(GetRef<GlobalVar>(node)));
  }

  Value VisitExpr_(const OpNode* node) override {
    // Q: Why not do eta-expansion?
    // A: Sometimes the frontend may be interested in knowning the op.
    return OpValue::make(GetRef<Op>(node));
  }

  Value VisitExpr_(const FunctionNode* node) override {
    const Function& func = GetRef<Function>(node);
    Map<Var, Value> captured_mod;
    Array<Var> free_vars = pass::FreeVars(func);
    for (const auto& var : free_vars) {
      captured_mod.Set(var, Eval(var));
    }
    return ClosureValue::make(captured_mod, func);
  }

  Value VisitExpr_(const CallNode* node) override {
    static auto fschema = Op::GetAttrMap<op::FMNMSchema>("FMNMSchema");
    const Call& call = GetRef<Call>(node);
    Array<Value> args;
    for (auto arg : call->args) {
      args.push_back(Eval(arg));
    }
    CallValues call_values = CallValues::make();
    call_values->callee = Eval(call->op);
    if (call_values->callee->IsInstance<ClosureValueObj>()) {
      call_values->args = MakeListArgs(args);
      return InvokeClosure(call_values);
    } else if (const auto* op = call_values->callee.as<OpValueObj>()) {
      call_values->args = fschema[op->op](args);
      return InvokePrimitive(call_values);
    }
    LOG(FATAL) << "ValueError: type " << call_values->callee->GetTypeKey() << " is not callable";
    throw;
  }

  Value VisitExpr_(const RelayConstantNode* _node) override {
    const ConstantNode* node = static_cast<const ConstantNode*>(_node);
    return node->value.defined() ? Downcast<Value>(node->value) : NullValue<Value>();
  }

  Value VisitExpr_(const LetNode* node) override {
    SymbolTable::AddVar var(st, node->var, Eval(node->value));
    return Eval(node->body);
  }

  Value VisitExpr_(const IfNode* node) override {
    bool result = Downcast<BoolValue>(Eval(node->cond))->data;
    return result ? Eval(node->true_branch) : Eval(node->false_branch);
  }

  Value VisitExpr_(const TupleNode* node) override {
    std::vector<Value> values;
    for (const Expr& field : node->fields) {
      values.push_back(Eval(field));
    }
    return TupleValue::make(values);
  }

  Value VisitExpr_(const TupleGetItemNode* node) override {
    TupleValue tuple = Downcast<TupleValue>(Eval(node->tuple));
    int index = node->index;
    int size = static_cast<int>(tuple->fields.size());
    CHECK(0 <= index && index < size) << "IndexError: tuple index out of range";
    Value sub_value = tuple->fields[index];
    if (sub_value->op_env == nullptr) {
      sub_value->op_env = tuple->op_env;
    }
    return sub_value;
  }

  Value VisitExpr_(const RefCreateNode* node) override {
    return RefValue::make(Eval(node->value));
  }

  Value VisitExpr_(const RefReadNode* node) override {
    return Downcast<RefValue>(Eval(node->ref))->value;
  }

  Value VisitExpr_(const RefWriteNode* node) override {
    Downcast<RefValue>(Eval(node->ref))->value = Eval(node->value);
    return TupleValue::make(tvm::Array<Value>({}));
  }

 public:
  Value InvokePrimitive(const CallValues& call) {
    const Op& op = Downcast<OpValue>(call->callee)->op;
    RunDeclare(call);
    if (!call->callee.defined()) {
      return call->out;
    }
    auto out_buf = AllocOutputBuffer(call->out);
    std::shared_ptr<OpEnv> op_env = OpDispatch::Dispatch(call);
    if (op_env != nullptr) {
      op_env->SetOutputBuffer(std::move(out_buf));
      InvokePrimitiveOpEnv(std::move(op_env), call);
    } else {
      LOG(FATAL) << "ValueError: Cannot dispatch " << op->name << "@" << call->ctx.c_str();
      throw;
    }
    return call->out;
  }

  void InvokePrimitiveOpEnv(std::shared_ptr<OpEnv> op_env, const CallValues& call) {
    const Op& op = Downcast<OpValue>(call->callee)->op;
    std::shared_ptr<Requests> req = op_env->GetRequests();
    {
      // note: Request workspace, workspace is kind of special memory which will be freed once this
      // op is done.
      WITH_BASE_PROFILER(call->ctx, op->name, "WorkspaceRequest",
                         {"Count: " + std::to_string(req->workspace.size())}, {
                           for (int i = 0, n = req->workspace.size(); i < n; ++i) {
                             RequestWorkspace(req.get(), i);
                           }
                         });

      // note: Request stream, every op will run on a given stream. For op that executed on cuda,
      // the default one is cuda DefautlStream. Currently, all ops are running on default stream.
      WITH_BASE_PROFILER(call->ctx, op->name, "StreamRequest",
                         {"Count: " + std::to_string(req->stream.size())}, {
                           for (int i = 0, n = req->stream.size(); i < n; ++i) {
                             RequestStream(req.get(), i);
                           }
                         });

      // note: Request distributed resources, operators like allreduce needs such resources.
      // Currently, the distributed resources only contain a communicator.
      WITH_BASE_PROFILER(call->ctx, op->name, "DistributedRequest",
                         {"Count: " + std::to_string(req->distributed.size())}, {
                           for (int i = 0, n = req->distributed.size(); i < n; ++i) {
                             RequestDistributed(req.get(), i);
                           }
                         });
    }

    // note: Execute the Operator.
    WITH_BASE_PROFILER(call->ctx, op->name, "CUDA_CALL", {}, { op_env->Execute(call); });

    {
      // note: Force op to run synchronously.
      for (int i = 0, n = req->stream.size(); i < n; ++i) {
        req->stream[i].stream->Wait();
      }

      // note: Free the workspace of this op.
      WITH_BASE_PROFILER(call->ctx, op->name, "WorkspaceClear", {}, {
        req->workspace.clear();
        req->workspace.shrink_to_fit();
      });

      req->stream.clear();
      req->stream.shrink_to_fit();
    }

    // note: The next op holds a reference to this op. It will make sure that the memories requested
    // by this op will not be freed after the return of this op.
    call->out->op_env = std::move(op_env);
  }

 public:
  Value InvokeClosure(const CallValues& call) {
    const auto* node = call->callee.as<ClosureValueObj>();
    const Function& func = node->func;
    const Array<Value>& call_args = GetListArgs(call->args);
    Map<Var, Value> locals;
    CHECK_EQ(func->params.size(), call_args.size());
    int n_args = call_args.size();
    for (int i = 0; i < n_args; ++i) {
      locals.Set(func->params[i], call_args[i]);
    }
    for (auto it = node->env.begin(); it != node->env.end(); ++it) {
      locals.Set((*it).first, (*it).second);
    }
    {
      SymbolTable::LocalFrame lf(st, std::move(locals));
      return Eval(func->body);
    }
  }

 public:
  void OnBind(const op::OpEnv* op_env) override {
  }

  void OnDestruct(const op::OpEnv* op_env) override {
  }

  void RequestWorkspace(Requests* req, int index) override {
    Requests::WorkspaceRequest& entry = req->workspace[index];
    CHECK(entry.memory == nullptr);
    std::shared_ptr<Memory> memory = Memory::Alloc(entry.ctx, entry.nbytes);
    *entry.dest = memory->data;
    entry.memory = memory;
  }

  void RequestStream(Requests* req, int index) override {
    Requests::StreamRequest& entry = req->stream[index];
    std::shared_ptr<Stream> stream = Stream::Get(entry.ctx, entry.tag_idx, entry.stream_idx);
    *entry.dest = stream->data();
    entry.stream = stream;
  }

  void RequestDistributed(Requests* req, int index) override {
    Requests::DistributedRequest& entry = req->distributed[index];
    *entry.dest = distributed::communicator::CommunicatorManager::Get()->GetCommunicator();
  }

 private:
  std::vector<std::shared_ptr<Memory>> AllocOutputBuffer(Value& out) {
    std::vector<DLTensor*> out_tensors;
    if (out->IsInstance<TensorValueObj>()) {
      DLTensor* t = out;
      out_tensors.emplace_back(t);
    } else if (const auto* tv = out.as<TupleValueObj>()) {
      for (const auto& v : tv->fields) {
        DLTensor* t = v;
        out_tensors.emplace_back(t);
      }
    } else if (out->IsInstance<VoidValueObj>()) {
      // do nothing.
    } else {
      LOG(FATAL) << "InternalError: Interpreter does not deal with " << out->GetTypeKey();
      throw;
    }
    std::vector<std::shared_ptr<Memory>> out_buf;
    for (auto* dlt : out_tensors) {
      if (dlt->data == nullptr) {
        std::shared_ptr<Memory> memory = Memory::Alloc(dlt->ctx, BytesCompactTensor(*dlt));
        dlt->data = memory->data;
        out_buf.push_back(memory);
      }
    }
    return out_buf;
  }
};

class IntrpThreadEntry {
 public:
  IntrpThreadEntry() = default;

  static Interpreter* ThreadLocal() {
    using TLS = dmlc::ThreadLocalStore<IntrpThreadEntry>;
    return &TLS::Get()->exec;
  }
  Interpreter exec;
};

Value Interpret(Expr expr, Module mod) {
  Interpreter* intrp = IntrpThreadEntry::ThreadLocal();
  intrp->mod = mod.defined() ? mod : Module::Global();
  auto ret = intrp->Eval(expr);
  intrp->mod = {};
  intrp->st.tab = {};
  return ret;
}

Value InvokePrimitive(const CallValues& call) {
  Interpreter* intrp = IntrpThreadEntry::ThreadLocal();
  auto ret = intrp->InvokePrimitive(call);
  intrp->mod = {};
  intrp->st.tab = {};
  return ret;
}

Value InvokeClosure(const CallValues& call) {
  Interpreter* intrp = IntrpThreadEntry::ThreadLocal();
  auto ret = intrp->InvokeClosure(call);
  intrp->mod = {};
  intrp->st.tab = {};
  return ret;
}

ObjectRef _Interpret(Expr expr, Module mod) {
  return DeTuple(Interpret(expr, mod));
}

MNM_REGISTER_GLOBAL("mnm.executor.Interpret").set_body_typed(_Interpret);
}  // namespace interpreter
}  // namespace executor
}  // namespace mnm
