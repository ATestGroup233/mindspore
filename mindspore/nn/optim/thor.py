# Copyright 2021 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ============================================================================
"""thor"""
import numpy as np
from mindspore.ops import functional as F, composite as C, operations as P
from mindspore.common.initializer import initializer
from mindspore.common.parameter import Parameter, ParameterTuple
from mindspore.common.tensor import Tensor
import mindspore.nn as nn
import mindspore.common.dtype as mstype
from mindspore._checkparam import Validator
from mindspore.nn.optim.optimizer import Optimizer
from mindspore.parallel._utils import _get_device_num, _get_gradients_mean
from mindspore import context
from mindspore.context import ParallelMode
from mindspore.nn.layer import DenseThor, Conv2dThor, EmbeddingThor
from mindspore.nn.wrap import DistributedGradReducer
from mindspore.train.train_thor.convert_utils import ConvertNetUntils
from mindspore.parallel._auto_parallel_context import auto_parallel_context

# Enumerates types of Layer
Other = -1
Conv = 1
FC = 2
Embedding = 3
LayerNorm = 4
BatchNorm = 5

op_add = P.AddN()
apply_decay = C.MultitypeFuncGraph("apply_decay")
_momentum_opt = C.MultitypeFuncGraph("momentum_opt")


@apply_decay.register("Number", "Bool", "Tensor", "Tensor")
def _tensor_apply_decay(weight_decay, if_apply, weight, gradient):
    """Get grad with weight_decay."""
    if if_apply:
        return op_add((weight * weight_decay, gradient))
    return gradient


@_momentum_opt.register("Function", "Tensor", "Tensor", "Tensor", "Tensor", "Tensor")
def _tensor_run_opt_ext(opt, momentum, learning_rate, gradient, weight, moment):
    """Apply momentum optimizer to the weight parameter using Tensor."""
    success = True
    success = F.depend(success, opt(weight, moment, learning_rate, gradient, momentum))
    return success

IS_ENABLE_GLOBAL_NORM = False
GRADIENT_CLIP_TYPE = 1
GRADIENT_CLIP_VALUE = 1.0
clip_grad = C.MultitypeFuncGraph("clip_grad")
hyper_map_op = C.HyperMap()


@clip_grad.register("Number", "Number", "Tensor")
def _clip_grad(clip_type, clip_value, grad):
    """
    Clip gradients.

    Inputs:
        clip_type (int): The way to clip, 0 for 'value', 1 for 'norm'.
        clip_value (float): Specifies how much to clip.
        grad (tuple[Tensor]): Gradients.

    Outputs:
        tuple[Tensor], clipped gradients.
    """
    if clip_type not in [0, 1]:
        return grad
    dt = F.dtype(grad)
    if clip_type == 0:
        new_grad = C.clip_by_value(grad, F.cast(F.tuple_to_array((-clip_value,)), dt),
                                   F.cast(F.tuple_to_array((clip_value,)), dt))
    else:
        new_grad = nn.ClipByNorm()(grad, F.cast(F.tuple_to_array((clip_value,)), dt))
    return new_grad


def clip_gradient(enable_clip_grad, gradients):
    """clip gradients"""
    if enable_clip_grad:
        if IS_ENABLE_GLOBAL_NORM:
            gradients = C.clip_by_global_norm(gradients, GRADIENT_CLIP_VALUE, None)
        else:
            gradients = hyper_map_op(F.partial(clip_grad, GRADIENT_CLIP_TYPE, GRADIENT_CLIP_VALUE), gradients)
    return gradients

C0 = 16


def caculate_device_shape(matrix_dim, channel, is_a):
    ll = (0)
    if is_a:
        if channel // C0 == 0:
            matrix_dim = (matrix_dim / channel) * C0
        ll = (int(matrix_dim // C0), int(matrix_dim // C0), C0, C0), int(matrix_dim)
    else:
        ll = (int(matrix_dim // C0), int(matrix_dim // C0), C0, C0), int(matrix_dim)
    return ll


def caculate_matmul_shape(matrix_a_dim, matrix_g_dim, split_dim):
    """get matmul shape"""
    split_dima = split_dim
    split_dimg = split_dim
    if matrix_a_dim % split_dim == 0:
        batch_w = matrix_a_dim // split_dim
    else:
        if matrix_a_dim < split_dim:
            batch_w = 1
            split_dima = matrix_a_dim
        else:
            batch_w = matrix_a_dim // split_dim + 1

    if matrix_g_dim % split_dim == 0:
        batch_h = matrix_g_dim // split_dim
    else:
        if matrix_g_dim < split_dim:
            batch_h = 1
            split_dimg = matrix_g_dim
        else:
            batch_h = matrix_g_dim // split_dim + 1
    matrix_a_shape = (batch_h, batch_w, split_dima, split_dima)
    matrix_g_shape = (batch_h, split_dimg, split_dimg)
    return matrix_a_shape, matrix_g_shape


def find_net_layertype_recur(net, layertype_map):
    """get net layer type recursively."""
    cells = net.name_cells()
    for name in cells:
        subcell = cells[name]
        if subcell == net:
            continue
        elif isinstance(subcell, Conv2dThor):
            layertype_map.append(Conv)
        elif isinstance(subcell, DenseThor):
            layertype_map.append(FC)
        elif isinstance(subcell, EmbeddingThor):
            layertype_map.append(Embedding)
        elif isinstance(subcell, nn.LayerNorm):
            layertype_map.append(LayerNorm)
        elif isinstance(subcell, nn.BatchNorm2d):
            layertype_map.append(BatchNorm)
        elif isinstance(subcell, (nn.Conv2d, nn.Dense, nn.Embedding, nn.Conv2dTranspose, nn.Conv1d, nn.Conv1dTranspose,
                                  nn.BatchNorm1d, nn.GroupNorm, nn.GlobalBatchNorm)):
            layertype_map.append(Other)
        else:
            find_net_layertype_recur(subcell, layertype_map)


def get_net_layertype_mask(net):
    layertype_map = []
    find_net_layertype_recur(net, layertype_map)
    return layertype_map


def get_layer_counter(layer_type, layer_counter, params, idx):
    """get layer counter"""
    if layer_type in [Conv, FC]:
        if "bias" in params[idx].name.lower():
            layer_counter = layer_counter + 1
        else:
            if idx < len(params) - 1 and "bias" not in params[idx + 1].name.lower():
                layer_counter = layer_counter + 1
    elif layer_type in [LayerNorm, BatchNorm]:
        if "beta" in params[idx].name.lower():
            layer_counter = layer_counter + 1
    else:
        layer_counter = layer_counter + 1
    return layer_counter


def thor(net, learning_rate, damping, momentum, weight_decay=0.0, loss_scale=1.0, batch_size=32,
         use_nesterov=False, decay_filter=lambda x: x.name not in [], split_indices=None, enable_clip_grad=False,
         frequency=100):
    r"""
    Updates gradients by the THOR algorithm.
    Trace-based Hardware-driven layer-ORiented Natural Gradient Descent Computation (THOR) algorithm is proposed in:
    `THOR: Trace-based Hardware-driven layer-ORiented Natural Gradient Descent Computation
    <https://www.aaai.org/AAAI21Papers/AAAI-6611.ChenM.pdf>`_
    """
    context.set_context(max_call_depth=10000)
    ConvertNetUntils().convert_to_thor_net(net)
    if context.get_context("device_target") == "Ascend":
        return ThorAscend(net, learning_rate, damping, momentum, weight_decay, loss_scale, batch_size, decay_filter,
                          split_indices=split_indices, enable_clip_grad=enable_clip_grad, frequency=frequency)
    return ThorGpu(net, learning_rate, damping, momentum, weight_decay, loss_scale, batch_size,
                   use_nesterov, decay_filter, split_indices=split_indices, enable_clip_grad=enable_clip_grad,
                   frequency=frequency)


class ThorGpu(Optimizer):
    """
    ThorGpu
    """
    def __init__(self, net, learning_rate, damping, momentum, weight_decay=0.0, loss_scale=1.0, batch_size=32,
                 use_nesterov=False, decay_filter=lambda x: x.name not in [], split_indices=None,
                 enable_clip_grad=False, frequency=100):
        params = filter(lambda x: x.requires_grad, net.get_parameters())
        super(ThorGpu, self).__init__(learning_rate, params, weight_decay, loss_scale)
        Validator.check_value_type("momentum", momentum, [float], self.cls_name)
        if isinstance(momentum, float) and momentum < 0.0:
            raise ValueError("momentum should be at least 0.0, but got momentum {}".format(momentum))
        self.momentum = Parameter(Tensor(momentum, mstype.float32), name="momentum")
        self.params = self.parameters
        self.use_nesterov = Validator.check_bool(use_nesterov)
        self.moments = self.params.clone(prefix="moments", init='zeros')
        self.hyper_map = C.HyperMap()
        self.opt = P.ApplyMomentum(use_nesterov=self.use_nesterov)
        self.net = net
        self.matrix_a_cov = ParameterTuple(filter(lambda x: 'matrix_a' in x.name, net.get_parameters()))
        self.matrix_g_cov = ParameterTuple(filter(lambda x: 'matrix_g' in x.name, net.get_parameters()))
        self.a_normalizer = ParameterTuple(filter(lambda x: 'a_normalizer' in x.name, net.get_parameters()))
        self.g_normalizer = ParameterTuple(filter(lambda x: 'g_normalizer' in x.name, net.get_parameters()))
        self.batch_size = Tensor(batch_size, mstype.float32)
        self.loss_scale = Tensor(1 / (loss_scale * loss_scale), mstype.float32)
        self.batch_size_scale = Tensor(batch_size * batch_size, mstype.float32)
        self.damping = damping
        self._define_gpu_operator()
        print("matrix_a_cov len is {}".format(len(self.matrix_a_cov)), flush=True)
        self.thor = True
        self.matrix_a = ()
        self.matrix_g = ()
        self.matrix_a_shape = ()
        self.thor_layer_count = 0
        self.conv_layer_count = 0
        self.weight_fim_idx_map = ()
        self.weight_conv_idx_map = ()
        self.weight_layertype_idx_map = ()
        self._process_matrix_init_and_weight_idx_map(self.net)
        self.matrix_a = ParameterTuple(self.matrix_a)
        self.matrix_g = ParameterTuple(self.matrix_g)
        self.weight_decay = weight_decay
        self.decay_flags = tuple(decay_filter(x) for x in self.parameters)
        self.update_gradient = P.UpdateThorGradient(split_dim=split_dim)
        self.enable_clip_grad = enable_clip_grad
        self.frequency = frequency
        self._define_gpu_reducer(split_indices)

    def get_frequency(self):
        """get thor frequency"""
        return self.frequency

    def _define_gpu_operator(self):
        """define gpu operator"""
        self.transpose = P.Transpose()
        self.shape = P.Shape()
        self.reshape = P.Reshape()
        self.matmul = P.MatMul()
        self.assign = P.Assign()
        self.mul = P.Mul()
        self.gather = P.GatherV2()
        self.one = Tensor(1, mstype.int32)
        self.feature_map = Tensor(1.0, mstype.float32)
        self.axis = 0
        self.cov_step = Parameter(initializer(0, [1], mstype.int32), name="cov_step", requires_grad=False)
        self.cast = P.Cast()
        self.sqrt = P.Sqrt()
        self.eye = P.Eye()
        split_dim = 128
        self.embedding_cholesky = P.CholeskyTrsm()
        self.cholesky = P.CholeskyTrsm(split_dim=split_dim)
        self.vector_matmul = P.BatchMatMul(transpose_a=True)
        self.reduce_sum = P.ReduceSum(keep_dims=False)
        self.inv = P.Reciprocal()
        self.square = P.Square()
        self.expand = P.ExpandDims()

    def _define_gpu_reducer(self, split_indices):
        """define gpu reducer"""
        self.parallel_mode = context.get_auto_parallel_context("parallel_mode")
        self.is_distributed = (self.parallel_mode != ParallelMode.STAND_ALONE)
        if self.is_distributed:
            mean = _get_gradients_mean()
            degree = _get_device_num()
            if not split_indices:
                self.split_indices = [len(self.matrix_a_cov) - 1]
            else:
                self.split_indices = split_indices
            auto_parallel_context().set_all_reduce_fusion_split_indices(self.split_indices, "hccl_world_groupsum6")
            auto_parallel_context().set_all_reduce_fusion_split_indices(self.split_indices, "hccl_world_groupsum8")
            self.grad_reducer_a = DistributedGradReducer(self.matrix_a_cov, mean, degree, fusion_type=6)
            self.grad_reducer_g = DistributedGradReducer(self.matrix_a_cov, mean, degree, fusion_type=8)


    def _process_matrix_init_and_weight_idx_map(self, net):
        """for GPU, process matrix init shape, and get weight idx map"""
        layer_type_map = get_net_layertype_mask(net)
        layer_counter = 0
        for idx in range(len(self.params)):
            layer_type = layer_type_map[layer_counter]
            weight = self.params[idx]
            weight_shape = self.shape(weight)
            if layer_type in [Conv, FC] and "bias" not in self.params[idx].name.lower():
                in_channels = weight_shape[1]
                out_channels = weight_shape[0]
                matrix_a_dim = in_channels
                if layer_type == Conv:
                    matrix_a_dim = in_channels * weight_shape[2] * weight_shape[3]
                matrix_g_dim = out_channels
                matrix_a_shape, matrix_g_shape = caculate_matmul_shape(matrix_a_dim, matrix_g_dim, split_dim)
                matrix_a_inv = Parameter(np.zeros(matrix_a_shape).astype(np.float32),
                                         name='matrix_a_inv_' + str(self.thor_layer_count), requires_grad=False)
                matrix_g_inv = Parameter(np.zeros(matrix_g_shape).astype(np.float32),
                                         name="matrix_g_inv_" + str(self.thor_layer_count), requires_grad=False)
                self.matrix_a = self.matrix_a + (matrix_a_inv,)
                self.matrix_g = self.matrix_g + (matrix_g_inv,)
                self.matrix_a_shape = self.matrix_a_shape + (matrix_a_shape,)
            elif layer_type == Embedding:
                vocab_size = weight_shape[0]
                embedding_size = weight_shape[1]
                matrix_a_inv = Parameter(Tensor(np.zeros([vocab_size]).astype(np.float32)),
                                         name='matrix_a_inv_' + str(self.thor_layer_count), requires_grad=False)
                matrix_g_inv = Parameter(Tensor(np.zeros([embedding_size, embedding_size]).astype(np.float32)),
                                         name="matrix_g_inv_" + str(self.thor_layer_count), requires_grad=False)
                self.matrix_a = self.matrix_a + (matrix_a_inv,)
                self.matrix_g = self.matrix_g + (matrix_g_inv,)
                self.matrix_a_shape = self.matrix_a_shape + ((vocab_size,),)

            if layer_type in [Conv, FC, Embedding] and "bias" not in self.params[idx].name.lower():
                self.weight_fim_idx_map = self.weight_fim_idx_map + (self.thor_layer_count,)
                self.thor_layer_count = self.thor_layer_count + 1
                self.weight_layertype_idx_map = self.weight_layertype_idx_map + (layer_type,)
                if layer_type == Conv:
                    self.weight_conv_idx_map = self.weight_conv_idx_map + (self.conv_layer_count,)
                    self.conv_layer_count = self.conv_layer_count + 1
                else:
                    self.weight_conv_idx_map = self.weight_conv_idx_map + (-1,)
            else:
                self.weight_conv_idx_map = self.weight_conv_idx_map + (-1,)
                self.weight_fim_idx_map = self.weight_fim_idx_map + (-1,)
                if layer_type == LayerNorm:
                    self.weight_layertype_idx_map = self.weight_layertype_idx_map + (LayerNorm,)
                else:
                    self.weight_layertype_idx_map = self.weight_layertype_idx_map + (Other,)
            # bert.cls1.output_bias: not a network layer, only a trainable param
            if "output_bias" not in self.params[idx].name.lower():
                layer_counter = get_layer_counter(layer_type, layer_counter, self.params, idx)

    def _get_ainv_ginv_list(self, gradients, damping_step, matrix_a_allreduce, matrix_g_allreduce):
        """get matrixA inverse list and matrix G inverse list"""
        for i in range(len(self.params)):
            thor_layer_count = self.weight_fim_idx_map[i]
            conv_layer_count = self.weight_conv_idx_map[i]
            layer_type = self.weight_layertype_idx_map[i]
            if layer_type in [Conv, FC, Embedding]:
                g = gradients[i]
                matrix_a = self.matrix_a_cov[thor_layer_count]
                matrix_g = self.matrix_g_cov[thor_layer_count]
                matrix_a = F.depend(matrix_a, g)
                matrix_g = F.depend(matrix_g, g)
                damping_a = damping_step
                damping_g = damping_step
                feature_map = self.feature_map
                if layer_type == Conv:
                    a_normalizer = self.a_normalizer[conv_layer_count]
                    g_normalizer = self.g_normalizer[conv_layer_count]
                    a_normalizer = F.depend(a_normalizer, g)
                    g_normalizer = F.depend(g_normalizer, g)
                    damping_a = self.mul(damping_step, 1.0 / a_normalizer)
                    damping_g = self.mul(damping_step, 1.0 / g_normalizer)
                    feature_map = self.sqrt(1.0 / a_normalizer)
                a_shape = self.shape(matrix_a)
                a_eye = self.eye(a_shape[0], a_shape[0], mstype.float32)
                damping_a = self.sqrt(damping_a)
                damping_g = self.sqrt(damping_g)
                g_shape = self.shape(matrix_g)
                g_eye = self.eye(g_shape[0], g_shape[1], mstype.float32)
                matrix_g = self.mul(matrix_g, self.loss_scale)
                matrix_g = self.mul(matrix_g, self.batch_size_scale)
                matrix_g = matrix_g + damping_g * g_eye
                if layer_type == Embedding:
                    a_eye = P.OnesLike()(matrix_a)
                    matrix_a = self.mul(matrix_a, 1.0 / self.batch_size)
                    matrix_a = matrix_a + damping_a * a_eye
                    matrix_a = self.inv(matrix_a)
                    matrix_g = self.embedding_cholesky(matrix_g)
                    matrix_g = self.matmul(matrix_g, matrix_g)
                else:
                    matrix_a = matrix_a + damping_a * a_eye
                    matrix_a = self.cholesky(matrix_a)
                    matrix_a = self.vector_matmul(matrix_a, matrix_a)
                    matrix_a = P.BroadcastTo(self.matrix_a_shape[thor_layer_count])(matrix_a)
                    matrix_g = self.cholesky(matrix_g)
                    matrix_g = self.vector_matmul(matrix_g, matrix_g)
                matrix_a = self.mul(matrix_a, feature_map)
                matrix_g = self.mul(matrix_g, feature_map)
                matrix_a_allreduce = matrix_a_allreduce + (matrix_a,)
                matrix_g_allreduce = matrix_g_allreduce + (matrix_g,)
        return matrix_a_allreduce, matrix_g_allreduce

    def _process_layernorm(self, damping_step, gradient):
        """process layernorm"""
        damping = self.sqrt(damping_step)
        normalizer = self.batch_size
        normalizer = self.cast(normalizer, mstype.float32)
        fim_cov = self.square(gradient)
        fim_cov = self.mul(fim_cov, 1.0 / normalizer)
        fim_cov = fim_cov + damping
        fim_inv = self.inv(fim_cov)
        gradient = self.mul(fim_inv, gradient)
        return gradient

    def _reshape_gradient(self, conv_layer_count, g, g_shape):
        """reshape gradient"""
        if conv_layer_count != -1:
            g = self.reshape(g, g_shape)
        return g

    def construct(self, gradients):
        params = self.params
        moments = self.moments
        gradients = self.scale_grad(gradients)
        damping_step = self.gather(self.damping, self.cov_step, self.axis)
        damping_step = self.cast(damping_step, mstype.float32)
        new_grads = ()
        if self.thor:
            matrix_ainv_list = ()
            matrix_ginv_list = ()
            matrix_a_allreduce, matrix_g_allreduce = self._get_ainv_ginv_list(gradients, damping_step,
                                                                              matrix_ainv_list, matrix_ginv_list)
            if self.is_distributed:
                matrix_a_allreduce = self.grad_reducer_a(matrix_a_allreduce)
                matrix_g_allreduce = self.grad_reducer_g(matrix_g_allreduce)

            for i in range(len(self.params)):
                g = gradients[i]
                thor_layer_count = self.weight_fim_idx_map[i]
                conv_layer_count = self.weight_conv_idx_map[i]
                layer_type = self.weight_layertype_idx_map[i]
                if layer_type in [Conv, FC]:
                    g_shape = self.shape(g)
                    g = self.reshape(g, (g_shape[0], -1))
                    matrix_a = matrix_a_allreduce[thor_layer_count]
                    matrix_g = matrix_g_allreduce[thor_layer_count]
                    g = self.update_gradient(matrix_g, g, matrix_a)
                    fake_a = self.assign(self.matrix_a[thor_layer_count], matrix_a)
                    fake_g = self.assign(self.matrix_g[thor_layer_count], matrix_g)
                    g = F.depend(g, fake_a)
                    g = F.depend(g, fake_g)
                    g = self._reshape_gradient(conv_layer_count, g, g_shape)
                elif layer_type == Embedding:
                    matrix_a = matrix_a_allreduce[thor_layer_count]
                    matrix_g = matrix_g_allreduce[thor_layer_count]
                    fake_a = self.assign(self.matrix_a[thor_layer_count], matrix_a)
                    fake_g = self.assign(self.matrix_g[thor_layer_count], matrix_g)
                    g = F.depend(g, fake_a)
                    g = F.depend(g, fake_g)
                    temp_a = self.expand(matrix_a, 1)
                    g = self.mul(temp_a, g)
                    g = self.matmul(g, matrix_g)
                elif layer_type == LayerNorm:
                    g = self._process_layernorm(damping_step, g)
                new_grads = new_grads + (g,)
        else:
            for j in range(len(self.params)):
                g = gradients[j]
                thor_layer_count = self.weight_fim_idx_map[j]
                conv_layer_count = self.weight_conv_idx_map[j]
                layer_type = self.weight_layertype_idx_map[j]
                if layer_type in [Conv, FC]:
                    g_shape = self.shape(g)
                    g = self.reshape(g, (g_shape[0], -1))
                    matrix_a = self.matrix_a[thor_layer_count]
                    matrix_g = self.matrix_g[thor_layer_count]
                    g = self.update_gradient(matrix_g, g, matrix_a)
                    g = self._reshape_gradient(conv_layer_count, g, g_shape)
                elif layer_type == Embedding:
                    matrix_a = self.matrix_a[thor_layer_count]
                    matrix_g = self.matrix_g[thor_layer_count]
                    g = gradients[j]
                    temp_a = self.expand(matrix_a, 1)
                    g = self.mul(temp_a, g)
                    g = self.matmul(g, matrix_g)
                elif layer_type == LayerNorm:
                    g = self._process_layernorm(damping_step, g)
                new_grads = new_grads + (g,)
        gradients = new_grads

        self.cov_step = self.cov_step + self.one
        if self.weight_decay > 0:
            gradients = self.hyper_map(F.partial(apply_decay, self.weight_decay), self.decay_flags, params, gradients)
        gradients = clip_gradient(self.enable_clip_grad, gradients)
        lr = self.get_lr()
        success = self.hyper_map(F.partial(_momentum_opt, self.opt, self.momentum, lr), gradients, params, moments)
        return success


class ThorAscend(Optimizer):
    """ThorAscend"""

    def __init__(self, net, learning_rate, damping, momentum, weight_decay=0.0, loss_scale=1.0, batch_size=32,
                 decay_filter=lambda x: x.name not in [], split_indices=None, enable_clip_grad=False, frequency=100):
        params = filter(lambda x: x.requires_grad, net.get_parameters())
        super(ThorAscend, self).__init__(learning_rate, params, weight_decay, loss_scale)
        if isinstance(momentum, float) and momentum < 0.0:
            raise ValueError("momentum should be at least 0.0, but got momentum {}".format(momentum))
        self.momentum = Parameter(Tensor(momentum, mstype.float32), name="momentum")
        self.params = self.parameters
        self.moments = self.params.clone(prefix="moments", init='zeros')
        self.hyper_map = C.HyperMap()
        self.opt = P.ApplyMomentum()
        self.net = net
        self.matrix_a_cov = ParameterTuple(filter(lambda x: 'matrix_a' in x.name, net.get_parameters()))
        self.matrix_g_cov = ParameterTuple(filter(lambda x: 'matrix_g' in x.name, net.get_parameters()))
        self.a_normalizer = ParameterTuple(filter(lambda x: 'a_normalizer' in x.name, net.get_parameters()))
        self.g_normalizer = ParameterTuple(filter(lambda x: 'g_normalizer' in x.name, net.get_parameters()))
        print("matrix_a_cov len is {}".format(len(self.matrix_a_cov)), flush=True)
        self._define_ascend_operator()
        self.C0 = 16
        self.matrix_a_dim = ()
        self.pad_a_flag = ()
        self.device_shape_pad_flag = ()
        self.diag_block_dim = 128
        self.matrix_a = ()
        self.matrix_g = ()
        self.thor_layer_count = 0
        self.conv_layer_count = 0
        self.weight_conv_idx_map = ()
        self.weight_fim_idx_map = ()
        self.weight_layertype_idx_map = ()
        self._process_matrix_init_and_weight_idx_map(self.net)
        self.matrix_a = ParameterTuple(self.matrix_a)
        self.matrix_g = ParameterTuple(self.matrix_g)
        self.matrix_max_inv = ()
        for i in range(len(self.matrix_a)):
            self.matrix_max_inv = self.matrix_max_inv + (
                Parameter(initializer(1, [1], mstype.float32), name="matrix_max" + str(i), requires_grad=False),)
        self.matrix_max_inv = ParameterTuple(self.matrix_max_inv)
        self.thor = True
        self.weight_decay = weight_decay
        self.decay_flags = tuple(decay_filter(x) for x in self.parameters)
        self.damping = damping
        self.batch_size = Tensor(batch_size, mstype.float32)
        self.loss_scale = Tensor(1 / (loss_scale * loss_scale), mstype.float32)
        self.batch_size_scale = Tensor(batch_size * batch_size, mstype.float32)
        self.enable_clip_grad = enable_clip_grad
        self.frequency = frequency
        self._define_ascend_reducer(split_indices)


    def get_frequency(self):
        """get thor frequency"""
        return self.frequency

    def _define_ascend_operator(self):
        """define ascend operator"""
        self.cube_matmul_left = P.CusMatMulCubeFraczLeftCast()
        self.cube_matmul_left_fc = P.CusMatMulCubeDenseLeft()
        self.cube_matmul_right_fc = P.CusMatMulCubeDenseRight()
        self.cube_matmul_right_mul = P.CusMatMulCubeFraczRightMul()
        self.transpose = P.Transpose()
        self.shape = P.Shape()
        self.reshape = P.Reshape()
        self.mul = P.Mul()
        self.log = P.Log()
        self.exp = P.Exp()
        self.sqrt = P.Sqrt()
        self.gather = P.GatherV2()
        self.assign = P.Assign()
        self.cast = P.Cast()
        self.eye = P.Eye()
        self.cholesky = P.CusCholeskyTrsm()
        self.vector_matmul = P.CusBatchMatMul()
        self.fused_abs_max2 = P.CusFusedAbsMax1()
        self.matrix_combine = P.CusMatrixCombine()
        self.slice = P.Slice()
        self.expand = P.ExpandDims()
        self.reduce_sum = P.ReduceSum(keep_dims=False)
        self.square = P.Square()
        self.inv = P.Inv()
        self.matmul = P.MatMul()
        self.axis = 0
        self.one = Tensor(1, mstype.int32)
        self.cov_step = Parameter(initializer(0, [1], mstype.int32), name="cov_step", requires_grad=False)

    def _define_ascend_reducer(self, split_indices):
        """define ascend reducer"""
        self.parallel_mode = context.get_auto_parallel_context("parallel_mode")
        self.is_distributed = (self.parallel_mode != ParallelMode.STAND_ALONE)
        if self.is_distributed:
            mean = _get_gradients_mean()
            degree = _get_device_num()
            if not split_indices:
                self.split_indices = [len(self.matrix_a_cov) - 1]
            else:
                self.split_indices = split_indices
            if self.conv_layer_count > 0:
                auto_parallel_context().set_all_reduce_fusion_split_indices(self.split_indices, "hccl_world_groupsum2")
                auto_parallel_context().set_all_reduce_fusion_split_indices(self.split_indices, "hccl_world_groupsum4")
                self.grad_reducer_amax = DistributedGradReducer(self.matrix_a_cov, mean, degree, fusion_type=2)
                self.grad_reducer_gmax = DistributedGradReducer(self.matrix_a_cov, mean, degree, fusion_type=4)

            auto_parallel_context().set_all_reduce_fusion_split_indices(self.split_indices, "hccl_world_groupsum6")
            auto_parallel_context().set_all_reduce_fusion_split_indices(self.split_indices, "hccl_world_groupsum8")
            self.grad_reducer_a = DistributedGradReducer(self.matrix_a_cov, mean, degree, fusion_type=6)
            self.grad_reducer_g = DistributedGradReducer(self.matrix_a_cov, mean, degree, fusion_type=8)

    def _process_matrix_init_and_weight_idx_map(self, net):
        """for Ascend, process matrix init shape, and get weight idx map"""
        layer_counter = 0
        layer_type_map = get_net_layertype_mask(net)
        for idx in range(len(self.params)):
            layer_type = layer_type_map[layer_counter]
            weight = self.params[idx]
            weight_shape = self.shape(weight)
            if layer_type == Conv and "bias" not in self.params[idx].name.lower():
                in_channels = weight_shape[1]
                out_channels = weight_shape[0]
                matrix_a_dim = in_channels * weight_shape[2] * weight_shape[3]
                matrix_g_dim = out_channels
                matrix_a_device_shape, matrix_a_device_dim = caculate_device_shape(matrix_a_dim, in_channels, True)
                matrix_g_device_shape, matrix_g_device_dim = caculate_device_shape(matrix_g_dim, in_channels, False)
                matrix_a_inv = Parameter(
                    Tensor(np.reshape(np.identity(matrix_a_device_dim).astype(np.float16), matrix_a_device_shape)),
                    name='matrix_a_inv_' + str(self.thor_layer_count), requires_grad=False)
                matrix_g_inv = Parameter(
                    Tensor(np.reshape(np.identity(matrix_g_device_dim).astype(np.float16), matrix_g_device_shape)),
                    name="matrix_g_inv_" + str(self.thor_layer_count), requires_grad=False)
                self.matrix_a = self.matrix_a + (matrix_a_inv,)
                self.matrix_g = self.matrix_g + (matrix_g_inv,)
                self.matrix_a_dim = self.matrix_a_dim + (matrix_a_dim,)
                pad_a_flag = False
                if (matrix_a_dim // self.diag_block_dim) * self.diag_block_dim != matrix_a_dim \
                        and matrix_a_dim > self.diag_block_dim:
                    pad_a_flag = True
                self.pad_a_flag = self.pad_a_flag + (pad_a_flag,)
                device_shape_pad_flag = False
                if matrix_a_dim != matrix_a_device_dim:
                    device_shape_pad_flag = True
                self.device_shape_pad_flag = self.device_shape_pad_flag + (device_shape_pad_flag,)
            elif layer_type == FC and "bias" not in self.params[idx].name.lower():
                out_channels = weight_shape[0]
                if out_channels == 1001:
                    fc_matrix_a = Parameter(Tensor(np.zeros([128, 128, 16, 16]).astype(np.float16)),
                                            name='matrix_a_inv_' + str(self.thor_layer_count),
                                            requires_grad=False)
                    fc_matrix_g = Parameter(Tensor(np.zeros([63, 63, 16, 16]).astype(np.float16)),
                                            name="matrix_g_inv_" + str(self.thor_layer_count),
                                            requires_grad=False)
                    self.matrix_a = self.matrix_a + (fc_matrix_a,)
                    self.matrix_g = self.matrix_g + (fc_matrix_g,)

            if layer_type in [Conv, FC, Embedding] and "bias" not in self.params[idx].name.lower():
                self.weight_fim_idx_map = self.weight_fim_idx_map + (self.thor_layer_count,)
                self.weight_layertype_idx_map = self.weight_layertype_idx_map + (layer_type,)
                self.thor_layer_count = self.thor_layer_count + 1
                if layer_type == Conv:
                    self.weight_conv_idx_map = self.weight_conv_idx_map + (self.conv_layer_count,)
                    self.conv_layer_count = self.conv_layer_count + 1
                else:
                    self.weight_conv_idx_map = self.weight_conv_idx_map + (-1,)
            else:
                self.weight_fim_idx_map = self.weight_fim_idx_map + (-1,)
                self.weight_conv_idx_map = self.weight_conv_idx_map + (-1,)
                if layer_type == LayerNorm:
                    self.weight_layertype_idx_map = self.weight_layertype_idx_map + (LayerNorm,)
                else:
                    self.weight_layertype_idx_map = self.weight_layertype_idx_map + (Other,)

            # bert.cls1.output_bias: not a network layer, only a trainable param
            if "output_bias" not in self.params[idx].name.lower():
                layer_counter = get_layer_counter(layer_type, layer_counter, self.params, idx)

    def _get_fc_ainv_ginv(self, index, damping_step, gradients, matrix_a_allreduce, matrix_g_allreduce,
                          matrix_a_max_allreduce, matrix_g_max_allreduce):
        """get fc layer ainv and ginv"""
        thor_layer_count = self.weight_fim_idx_map[index]
        g = gradients[index]
        matrix_a = self.matrix_a_cov[thor_layer_count]
        matrix_g = self.matrix_g_cov[thor_layer_count]
        matrix_a = F.depend(matrix_a, g)
        matrix_g = F.depend(matrix_g, g)
        a_shape = self.shape(matrix_a)
        a_eye = self.eye(a_shape[0], a_shape[0], mstype.float32)
        g_shape = self.shape(matrix_g)
        g_eye = self.eye(g_shape[0], g_shape[0], mstype.float32)
        damping = self.sqrt(damping_step)
        matrix_a = matrix_a + damping * a_eye
        matrix_a_inv = self.cholesky(matrix_a)
        matrix_a_inv = self.vector_matmul(matrix_a_inv, matrix_a_inv)
        weight_shape = self.shape(self.params[index])
        out_channels = weight_shape[0]
        if out_channels == 2:
            matrix_a_inv = self.matrix_combine(matrix_a_inv)
            matrix_g_inv = g_eye
        else:
            matrix_g = self.mul(matrix_g, self.loss_scale)
            matrix_g = self.mul(matrix_g, self.batch_size_scale)
            matrix_g = matrix_g + damping * g_eye
            matrix_g_inv = self.cholesky(matrix_g)
            matrix_g_inv = self.vector_matmul(matrix_g_inv, matrix_g_inv)
            if out_channels == 1001:
                matrix_a_inv_max = self.fused_abs_max2(matrix_a_inv)
                a_max = self.fused_abs_max2(matrix_a_inv_max)
                matrix_a_inv = self.matrix_combine(matrix_a_inv)
                matrix_a_inv_shape = self.shape(matrix_a_inv)
                matrix_a_inv = self.reshape(matrix_a_inv,
                                            (matrix_a_inv_shape[0] / 16, 16,
                                             matrix_a_inv_shape[0] / 16, 16))
                matrix_a_inv = self.transpose(matrix_a_inv, (2, 0, 1, 3))
                matrix_g_inv_max = P.CusFusedAbsMax1([1001, 1001])(matrix_g_inv)
                g_max = self.fused_abs_max2(matrix_g_inv_max)
                matrix_g_inv = self.matrix_combine(matrix_g_inv)
                matrix_g_inv = self.slice(matrix_g_inv, (0, 0), (1001, 1001))
                matrix_g_inv = P.Pad(((0, 7), (0, 7)))(matrix_g_inv)
                matrix_g_inv_shape = self.shape(matrix_g_inv)
                matrix_g_inv = self.reshape(matrix_g_inv,
                                            (matrix_g_inv_shape[0] / 16, 16,
                                             matrix_g_inv_shape[0] / 16, 16))
                matrix_g_inv = self.transpose(matrix_g_inv, (2, 0, 1, 3))
                a_max = F.depend(a_max, g)
                g_max = F.depend(g_max, g)
                matrix_a_max_allreduce = matrix_a_max_allreduce + (a_max,)
                matrix_g_max_allreduce = matrix_g_max_allreduce + (g_max,)
            else:
                matrix_a_inv = self.matrix_combine(matrix_a_inv)
                matrix_g_inv = self.matrix_combine(matrix_g_inv)
        matrix_a_allreduce = matrix_a_allreduce + (matrix_a_inv,)
        matrix_g_allreduce = matrix_g_allreduce + (matrix_g_inv,)
        return matrix_a_allreduce, matrix_g_allreduce, matrix_a_max_allreduce, matrix_g_max_allreduce

    def _get_ainv_ginv_amax_gmax_list(self, gradients, damping_step, matrix_a_allreduce, matrix_g_allreduce,
                                      matrix_a_max_allreduce, matrix_g_max_allreduce):
        """get matrixA inverse list, matrixG inverse list, matrixA_max list, matrixG_max list"""
        for i in range(len(self.params)):
            thor_layer_count = self.weight_fim_idx_map[i]
            conv_layer_count = self.weight_conv_idx_map[i]
            layer_type = self.weight_layertype_idx_map[i]
            if layer_type == Conv:
                g = gradients[i]
                matrix_a = self.matrix_a_cov[thor_layer_count]
                matrix_g = self.matrix_g_cov[thor_layer_count]
                matrix_a = F.depend(matrix_a, g)
                matrix_g = F.depend(matrix_g, g)
                a_shape = self.shape(matrix_a)
                a_eye = self.eye(a_shape[0], a_shape[0], mstype.float32)
                g_shape = self.shape(matrix_g)
                g_eye = self.eye(g_shape[0], g_shape[0], mstype.float32)
                a_normalizer = self.a_normalizer[conv_layer_count]
                g_normalizer = self.g_normalizer[conv_layer_count]
                a_normalizer = F.depend(a_normalizer, g)
                g_normalizer = F.depend(g_normalizer, g)
                damping_a = self.mul(damping_step, self.batch_size / a_normalizer)
                damping_g = self.mul(damping_step, self.batch_size / g_normalizer)
                damping_a = self.sqrt(damping_a)
                matrix_a = matrix_a + damping_a * a_eye
                matrix_a_inv = self.cholesky(matrix_a)
                matrix_a_inv = self.vector_matmul(matrix_a_inv, matrix_a_inv)
                a_max = P.CusFusedAbsMax1([self.matrix_a_dim[conv_layer_count],
                                           self.matrix_a_dim[conv_layer_count]])(matrix_a_inv)
                a_max = self.fused_abs_max2(a_max)
                matrix_a_inv = self.matrix_combine(matrix_a_inv)
                if self.pad_a_flag[conv_layer_count]:
                    matrix_a_inv = self.slice(matrix_a_inv, (0, 0), (self.matrix_a_dim[conv_layer_count],
                                                                     self.matrix_a_dim[conv_layer_count]))
                if self.device_shape_pad_flag[conv_layer_count]:
                    weight = self.params[i]
                    weight_shape = self.shape(weight)
                    kernel_hw = weight_shape[2] * weight_shape[3]
                    in_channels = weight_shape[1]
                    matrix_a_inv = self.reshape(matrix_a_inv, (kernel_hw, in_channels, kernel_hw, in_channels))
                    matrix_a_inv = P.Pad(((0, 0), (0, self.C0 - in_channels), (0, 0),
                                          (0, self.C0 - in_channels)))(matrix_a_inv)
                matrix_a_inv_shape = self.shape(self.matrix_a[thor_layer_count])
                matrix_a_device_temp_shape = (matrix_a_inv_shape[0], matrix_a_inv_shape[2],
                                              matrix_a_inv_shape[1], matrix_a_inv_shape[3])
                matrix_a_inv = self.reshape(matrix_a_inv, matrix_a_device_temp_shape)
                matrix_a_inv = self.transpose(matrix_a_inv, (2, 0, 1, 3))

                damping_g = self.sqrt(damping_g)
                matrix_g = self.mul(matrix_g, self.loss_scale)
                matrix_g = self.mul(matrix_g, self.batch_size_scale)
                matrix_g = matrix_g + damping_g * g_eye
                matrix_g_inv = self.cholesky(matrix_g)
                matrix_g_inv = self.vector_matmul(matrix_g_inv, matrix_g_inv)
                g_max = self.fused_abs_max2(matrix_g_inv)
                g_max = self.fused_abs_max2(g_max)
                matrix_g_inv = self.matrix_combine(matrix_g_inv)
                matrix_g_inv_shape = self.shape(self.matrix_g[thor_layer_count])
                matrix_g_device_temp_shape = (matrix_g_inv_shape[0], matrix_g_inv_shape[2],
                                              matrix_g_inv_shape[1], matrix_g_inv_shape[3])
                matrix_g_inv = self.reshape(matrix_g_inv, matrix_g_device_temp_shape)
                matrix_g_inv = self.transpose(matrix_g_inv, (2, 0, 1, 3))

                a_max = F.depend(a_max, g)
                g_max = F.depend(g_max, g)
                matrix_a_allreduce = matrix_a_allreduce + (matrix_a_inv,)
                matrix_g_allreduce = matrix_g_allreduce + (matrix_g_inv,)
                matrix_a_max_allreduce = matrix_a_max_allreduce + (a_max,)
                matrix_g_max_allreduce = matrix_g_max_allreduce + (g_max,)
            elif layer_type == FC:
                matrix_a_allreduce, matrix_g_allreduce, matrix_a_max_allreduce, matrix_g_max_allreduce = \
                    self._get_fc_ainv_ginv(i, damping_step, gradients, matrix_a_allreduce, matrix_g_allreduce,
                                           matrix_a_max_allreduce, matrix_g_max_allreduce)
            elif layer_type == Embedding:
                g = gradients[i]
                matrix_a = self.matrix_a_cov[thor_layer_count]
                matrix_g = self.matrix_g_cov[thor_layer_count]
                matrix_a = F.depend(matrix_a, g)
                matrix_g = F.depend(matrix_g, g)
                g_shape = self.shape(matrix_g)
                g_eye = self.eye(g_shape[0], g_shape[0], mstype.float32)
                damping = self.sqrt(damping_step)
                a_eye = P.OnesLike()(matrix_a)
                matrix_a = self.mul(matrix_a, 1.0 / self.batch_size)
                matrix_a = matrix_a + damping * a_eye
                matrix_a_inv = self.inv(matrix_a)
                matrix_g = self.mul(matrix_g, self.loss_scale)
                matrix_g = self.mul(matrix_g, self.batch_size_scale)
                matrix_g = matrix_g + damping * g_eye
                matrix_g_inv = self.cholesky(matrix_g)
                matrix_g_inv = self.vector_matmul(matrix_g_inv, matrix_g_inv)
                matrix_g_inv = self.matrix_combine(matrix_g_inv)
                matrix_a_allreduce = matrix_a_allreduce + (matrix_a_inv,)
                matrix_g_allreduce = matrix_g_allreduce + (matrix_g_inv,)
        return matrix_a_allreduce, matrix_g_allreduce, matrix_a_max_allreduce, matrix_g_max_allreduce

    def _process_layernorm(self, damping_step, gradient):
        """process layernorm layer for thor"""
        damping = self.sqrt(damping_step)
        normalizer = self.cast(self.batch_size, mstype.float32)
        fim_cov = self.square(gradient)
        fim_cov = self.mul(fim_cov, 1.0 / normalizer)
        fim_cov = fim_cov + damping
        fim_inv = self.inv(fim_cov)
        gradient = self.mul(fim_inv, gradient)
        return gradient

    def _process_thor_fc(self, thor_layer_count, matrix_a_allreduce, matrix_g_allreduce, g):
        """process thor graph fc layer"""
        temp_a = matrix_a_allreduce[thor_layer_count]
        temp_g = matrix_g_allreduce[thor_layer_count]
        fake_a = self.assign(self.matrix_a_cov[thor_layer_count], temp_a)
        fake_g = self.assign(self.matrix_g_cov[thor_layer_count], temp_g)
        g = F.depend(g, fake_a)
        g = F.depend(g, fake_g)
        temp_a = self.cast(temp_a, mstype.float16)
        temp_g = self.cast(temp_g, mstype.float16)
        g = self.cast(g, mstype.float16)
        g = self.matmul(temp_g, g)
        g = self.matmul(g, temp_a)
        g = self.cast(g, mstype.float32)
        return g

    def _get_second_gradients(self, new_grads, damping_step, gradients):
        """get second gradients for thor"""
        params_len = len(self.params)
        if self.conv_layer_count > 0:
            for i in range(params_len):
                g = gradients[i]
                thor_layer_count = self.weight_fim_idx_map[i]
                layer_type = self.weight_layertype_idx_map[i]
                matrix_a = self.matrix_a[thor_layer_count]
                matrix_g = self.matrix_g[thor_layer_count]
                matrix_max = self.matrix_max_inv[thor_layer_count]
                if layer_type == FC:
                    g = self.cube_matmul_left_fc(matrix_g, g)
                    g = self.cube_matmul_right_fc(g, matrix_a, matrix_max)
                elif layer_type == Conv:
                    g = self.cube_matmul_left(matrix_g, g)
                    g = self.cube_matmul_right_mul(g, matrix_a, matrix_max)
                new_grads = new_grads + (g,)
        else:
            for i in range(params_len):
                g = gradients[i]
                thor_layer_count = self.weight_fim_idx_map[i]
                layer_type = self.weight_layertype_idx_map[i]
                if layer_type == Embedding:
                    temp_a_ori = self.matrix_a_cov[thor_layer_count]
                    temp_g = self.matrix_g_cov[thor_layer_count]
                    temp_a = self.expand(temp_a_ori, 1)
                    g = self.mul(temp_a, g)
                    temp_g = self.cast(temp_g, mstype.float16)
                    g = self.cast(g, mstype.float16)
                    g = self.matmul(g, temp_g)
                    g = self.cast(g, mstype.float32)
                elif layer_type == FC:
                    temp_a = self.matrix_a_cov[thor_layer_count]
                    temp_g = self.matrix_g_cov[thor_layer_count]
                    temp_a = self.cast(temp_a, mstype.float16)
                    temp_g = self.cast(temp_g, mstype.float16)
                    g = self.cast(g, mstype.float16)
                    g = self.matmul(temp_g, g)
                    g = self.matmul(g, temp_a)
                    g = self.cast(g, mstype.float32)
                elif layer_type == LayerNorm:
                    g = self._process_layernorm(damping_step, g)
                new_grads = new_grads + (g,)
        return new_grads

    def _get_second_grad_by_matmul(self, index, temp_a, temp_g, g, temp_max):
        """get second gradient by matmul"""
        conv_layer_count = self.weight_conv_idx_map[index]
        layer_type = self.weight_layertype_idx_map[index]
        if layer_type == FC:
            g = self.cube_matmul_left_fc(temp_g, g)
            g = self.cube_matmul_right_fc(g, temp_a, temp_max)
        elif layer_type == Conv:
            a_normalizer = self.a_normalizer[conv_layer_count]
            a_normalizer = F.depend(a_normalizer, g)
            temp_max = self.mul(temp_max, self.batch_size / a_normalizer)
            g = self.cube_matmul_left(temp_g, g)
            g = self.cube_matmul_right_mul(g, temp_a, temp_max)
        return g, temp_max

    def _get_second_grad_by_layertype(self, index, matrix_a_allreduce, matrix_g_allreduce, g, damping_step):
        """get second gradient by layertype"""
        thor_layer_count = self.weight_fim_idx_map[index]
        layer_type = self.weight_layertype_idx_map[index]
        if layer_type == Embedding:
            temp_a_ori = matrix_a_allreduce[thor_layer_count]
            temp_g = matrix_g_allreduce[thor_layer_count]
            fake_a = self.assign(self.matrix_a_cov[thor_layer_count], temp_a_ori)
            fake_g = self.assign(self.matrix_g_cov[thor_layer_count], temp_g)
            g = F.depend(g, fake_a)
            g = F.depend(g, fake_g)
            temp_a = self.expand(temp_a_ori, 1)
            g = self.mul(temp_a, g)
            temp_g = self.cast(temp_g, mstype.float16)
            g = self.cast(g, mstype.float16)
            g = self.matmul(g, temp_g)
            g = self.cast(g, mstype.float32)
        elif layer_type == FC:
            g = self._process_thor_fc(thor_layer_count, matrix_a_allreduce, matrix_g_allreduce, g)
        elif layer_type == LayerNorm:
            g = self._process_layernorm(damping_step, g)
        return g

    def construct(self, gradients):
        params = self.params
        moments = self.moments
        gradients = self.scale_grad(gradients)
        damping_step = self.gather(self.damping, self.cov_step, self.axis)
        damping_step = self.cast(damping_step, mstype.float32)
        if self.thor:
            matrix_a_allreduce = ()
            matrix_g_allreduce = ()
            matrix_a_max_allreduce = ()
            matrix_g_max_allreduce = ()
            matrix_a_allreduce, matrix_g_allreduce, matrix_a_max_allreduce, matrix_g_max_allreduce = \
                self._get_ainv_ginv_amax_gmax_list(gradients, damping_step, matrix_a_allreduce, matrix_g_allreduce,
                                                   matrix_a_max_allreduce, matrix_g_max_allreduce)
            if self.is_distributed:
                matrix_a_allreduce = self.grad_reducer_a(matrix_a_allreduce)
                matrix_g_allreduce = self.grad_reducer_g(matrix_g_allreduce)
                if self.conv_layer_count > 0:
                    matrix_a_max_allreduce = self.grad_reducer_amax(matrix_a_max_allreduce)
                    matrix_g_max_allreduce = self.grad_reducer_gmax(matrix_g_max_allreduce)

            new_grads = ()
            if self.conv_layer_count > 0:
                for i in range(len(self.params)):
                    g = gradients[i]
                    thor_layer_count = self.weight_fim_idx_map[i]
                    temp_a = matrix_a_allreduce[thor_layer_count]
                    temp_g = matrix_g_allreduce[thor_layer_count]
                    matrix_a_inv_max = self.log(matrix_a_max_allreduce[thor_layer_count])
                    matrix_a_inv_max = self.mul(matrix_a_inv_max, -1)
                    matrix_a_inv_max = self.exp(matrix_a_inv_max)
                    temp_a = self.mul(temp_a, matrix_a_inv_max)
                    matrix_g_inv_max = self.log(matrix_g_max_allreduce[thor_layer_count])
                    matrix_g_inv_max = self.mul(matrix_g_inv_max, -1)
                    matrix_g_inv_max = self.exp(matrix_g_inv_max)
                    temp_g = self.mul(temp_g, matrix_g_inv_max)
                    temp_max = self.mul(matrix_g_max_allreduce[thor_layer_count],
                                        matrix_g_max_allreduce[thor_layer_count])
                    temp_a = self.cast(temp_a, mstype.float16)
                    temp_g = self.cast(temp_g, mstype.float16)
                    g, temp_max = self._get_second_grad_by_matmul(i, temp_a, temp_g, g, temp_max)
                    fake_a = self.assign(self.matrix_a[thor_layer_count], temp_a)
                    fake_g = self.assign(self.matrix_g[thor_layer_count], temp_g)
                    fake_max = self.assign(self.matrix_max_inv[thor_layer_count], temp_max)
                    g = F.depend(g, fake_a)
                    g = F.depend(g, fake_g)
                    g = F.depend(g, fake_max)
                    new_grads = new_grads + (g,)
                gradients = new_grads
            else:
                for i in range(len(self.params)):
                    g = gradients[i]
                    g = self._get_second_grad_by_layertype(i, matrix_a_allreduce, matrix_g_allreduce, g, damping_step)
                    new_grads = new_grads + (g,)
                gradients = new_grads
        else:
            new_grads = ()
            gradients = self._get_second_gradients(new_grads, damping_step, gradients)

        self.cov_step = self.cov_step + self.one
        if self.weight_decay > 0:
            gradients = self.hyper_map(F.partial(apply_decay, self.weight_decay), self.decay_flags, params, gradients)
        gradients = clip_gradient(self.enable_clip_grad, gradients)
        lr = self.get_lr()
        success = self.hyper_map(F.partial(_momentum_opt, self.opt, self.momentum, lr), gradients, params, moments)
        return success
