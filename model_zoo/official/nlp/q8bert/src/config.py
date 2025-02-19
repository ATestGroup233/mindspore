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
# ===========================================================================

"""config script for task distill"""

import mindspore.common.dtype as mstype
from easydict import EasyDict as edict
from .q8bert_model import BertConfig
train_cfg = edict({
    'batch_size': 16,
    'loss_scale_value': 2 ** 16,
    'scale_factor': 2,
    'scale_window': 50,
    'optimizer_cfg': edict({
        'AdamWeightDecay': edict({
            'learning_rate': 5e-5,
            'end_learning_rate': 1e-14,
            'power': 1.0,
            'weight_decay': 1e-4,
            'eps': 1e-6,
            'decay_filter': lambda x: 'layernorm' not in x.name.lower() and 'bias' not in x.name.lower(),
            'warmup_ratio': 0.1
        }),
    }),
})

eval_cfg = edict({
    'batch_size': 32,
})

model_cfg = BertConfig(
    seq_length=128,
    vocab_size=30522,
    hidden_size=768,
    num_hidden_layers=6,
    num_attention_heads=12,
    intermediate_size=3072,
    hidden_act="gelu",
    hidden_dropout_prob=0.1,
    attention_probs_dropout_prob=0.1,
    max_position_embeddings=512,
    type_vocab_size=2,
    initializer_range=0.02,
    use_relative_positions=False,
    dtype=mstype.float32,
    compute_type=mstype.float32,
)
