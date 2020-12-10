# pylint: disable=attribute-defined-outside-init,invalid-name,protected-access,too-many-locals,too-many-statements
import pytest
import numpy as np

import mnm
from mnm.model import Conv2d, Linear, BatchNorm
from mnm import distributed as dist

import tvm


def one_hot(batch_size, num_classes, ctx="cuda", dtype="float32"):
    targets = np.random.randint(0, num_classes, size=batch_size)
    m_x = np.zeros([batch_size, num_classes], dtype=dtype)
    m_x[range(batch_size), targets] = 1
    m_x = mnm.array(m_x, ctx=ctx)
    assert list(m_x.shape) == [batch_size, num_classes]
    return m_x


def randn(shape, *, ctx="cuda", dtype="float32", std=1.0, mean=0.0,
          requires_grad=False, positive=False):
    if positive:
        x = np.abs(np.random.randn(*shape)) * std + mean
    else:
        x = np.random.randn(*shape) * std + mean
    if not isinstance(x, np.ndarray):
        x = np.array(x)
    assert list(x.shape) == list(shape)
    x = x.astype(dtype)
    m_x = mnm.array(x, ctx=ctx)
    if requires_grad:
        m_x.requires_grad = True
    return m_x


class MNMTest(mnm.Model):
    # pylint: disable=attribute-defined-outside-init
    def build(self, input_shape=28, num_classes=10):
        self.conv1 = Conv2d(in_channels=3,
                            out_channels=6,
                            kernel_size=5,
                            padding=2,
                            bias=False)
        self.bn1 = BatchNorm(6)
        self.linear1 = Linear((input_shape // 2) ** 2 * 6,
                              num_classes)
    # pylint: enable=attribute-defined-outside-init

    @mnm.model.trace
    def forward(self, x, y_true):
        y_pred = self.forward_infer(x)
        y_pred = mnm.log_softmax(y_pred)
        loss = mnm.nll_loss(y_true=y_true, y_pred=y_pred)
        return loss

    @mnm.model.trace
    def forward_infer(self, x):
        out = self.bn1(self.conv1(x))
        out = mnm.sigmoid(out)
        out = mnm.avg_pool2d(out, (2, 2), (2, 2))
        out = mnm.batch_flatten(out)
        out = self.linear1(out)
        return out


# pylint: disable=unused-variable
@pytest.mark.skipif(not mnm.build.with_cuda(), reason="CUDA is not enabled")
@pytest.mark.parametrize("config", [
    (2, 2, 4),
])
def test_dp(config):
    dctx = dist.get_context()
    dctx.enable_data_parallel = True
    ctx = f"cuda({dctx.local_rank})"
    const = randn([1, 3, config[1], config[1]], ctx=ctx)

    class TestModel(mnm.Model):
        # pylint: disable=attribute-defined-outside-init
        def build(self):
            self.c = const
        # pylint: enable=attribute-defined-outside-init

        @mnm.model.trace
        def forward(self, x, y_true):
            y_pred = self.forward_infer(x)
            loss = mnm.nll_loss(y_true=y_true, y_pred=y_pred)
            return loss

        @mnm.model.trace
        def forward_infer(self, x):
            out = mnm.matmul(x, self.c)
            return out

    def expected():
        shape = [1, 3, config[1], config[1]]

        # Params
        x = tvm.relay.var('x', tvm.relay.TensorType(shape))
        c = tvm.relay.var('c', tvm.relay.TensorType(shape))
        y_true = tvm.relay.var('y_true', tvm.relay.TensorType([1, config[2]]))

        # Forward IR components
        op_matmul = mnm._ffi.op.GetOp('mnm.op.matmul')
        expr_a1 = tvm.relay.Call(op_matmul, [x, c])
        var_a1 = tvm.relay.var('a1')

        op_nll_loss = mnm._ffi.op.GetOp('mnm.op.nll_loss')
        expr_a2 = tvm.relay.Call(op_nll_loss, [y_true, var_a1])
        var_a2 = tvm.relay.var('a2')

        # Backward IR components
        dy = tvm.relay.var('dy')
        var_closure = tvm.relay.var('closure')

        op_nll_loss_dtrue = mnm._ffi.op.GetOp('mnm.op.nll_loss_dtrue')
        expr_x1 = tvm.relay.Call(op_nll_loss_dtrue, [y_true, var_a1])
        var_x1 = tvm.relay.var('x1')

        expr_t0 = tvm.relay.Tuple([var_x1])

        op__allreduce = mnm._ffi.op.GetOp('mnm.op._allreduce')
        expr_g = tvm.relay.Call(op__allreduce, [expr_t0])
        var_g = tvm.relay.var('g')

        op_nll_loss_dpred = mnm._ffi.op.GetOp('mnm.op.nll_loss_dpred')
        expr_x2 = tvm.relay.Call(op_nll_loss_dpred, [y_true, var_a1])
        var_x2 = tvm.relay.var('x2')

        op_matmul_nt = mnm._ffi.op.GetOp('mnm.op.matmul_nt')
        expr_x3 = tvm.relay.Call(op_matmul_nt, [var_x2, c])
        var_x3 = tvm.relay.var('x3')

        expr_t1 = tvm.relay.Tuple([var_x3])

        op__allreduce = mnm._ffi.op.GetOp('mnm.op._allreduce')
        expr_g1 = tvm.relay.Call(op__allreduce, [expr_t1])
        var_g1 = tvm.relay.var('g1')

        op_matmul_tn = mnm._ffi.op.GetOp('mnm.op.matmul_tn')
        expr_x4 = tvm.relay.Call(op_matmul_tn, [x, var_x2])
        var_x4 = tvm.relay.var('x4')

        expr_t2 = tvm.relay.Tuple([var_x4])

        op__allreduce = mnm._ffi.op.GetOp('mnm.op._allreduce')
        expr_g2 = tvm.relay.Call(op__allreduce, [expr_t2])
        var_g2 = tvm.relay.var('g2')

        op_stream_sync = mnm._ffi.op.GetOp('mnm.op.stream_sync')
        const_sream_tag = mnm._ffi.ir._make.Constant(
            mnm._core.value.IntValue(5))
        expr_null = tvm.relay.Call(op_stream_sync, [var_g2, const_sream_tag])
        var_null = tvm.relay.var('null')

        expr_x5 = tvm.relay.Tuple([var_g1, var_g, var_g2])
        var_x5 = tvm.relay.var('x5')

        # Forward IR components
        expr_ret = tvm.relay.Tuple([var_a2, var_closure])
        var_ret = tvm.relay.var('ret')

        # Construct Backward IR as a closure
        let9 = tvm.relay.Let(var_x5, expr_x5, var_x5)
        let8 = tvm.relay.Let(var_null, expr_null, let9)
        let7 = tvm.relay.Let(var_g2, expr_g2, let8)
        let6 = tvm.relay.Let(var_x4, expr_x4, let7)
        let5 = tvm.relay.Let(var_g1, expr_g1, let6)
        let4 = tvm.relay.Let(var_x3, expr_x3, let5)
        let3 = tvm.relay.Let(var_x2, expr_x2, let4)
        let2 = tvm.relay.Let(var_g, expr_g, let3)
        let1 = tvm.relay.Let(var_x1, expr_x1, let2)
        closure_func = tvm.relay.Function([dy], let1)

        # Construct Forward IR
        let10 = tvm.relay.Let(var_ret, expr_ret, var_ret)
        let0 = tvm.relay.Let(var_closure, closure_func, let10)

        let_1 = tvm.relay.Let(var_a2, expr_a2, let0)
        let_2 = tvm.relay.Let(var_a1, expr_a1, let_1)

        return tvm.relay.Function([x, y_true, c], let_2)

    m_model = TestModel()
    m_model.to(ctx=ctx)
    m_model.train_mode()

    m_x = randn([1, 3, config[1], config[1]], ctx=ctx, requires_grad=True)
    m_y = one_hot(batch_size=1, num_classes=config[2], ctx=ctx)

    func_before = m_model._internal(m_x, m_y).func
    func_before = mnm._ffi.pass_.AutoDiff(func_before)
    print("Before auto parallel: ", func_before)
    func_after = mnm._ffi.pass_.AutoDataParallel(func_before)
    print("After auto parallel: ", func_after)

    func_expected = expected()
    print("Expected: ", func_expected())

    text = func_after.astext()
    assert "mnm.op._allreduce" in text
    assert "mnm.op.stream_sync" in text
    assert tvm.ir.structural_equal(func_after, func_expected)

    dctx.enable_data_parallel = False


if __name__ == "__main__":
    pytest.main([__file__])
