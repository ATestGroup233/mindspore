# Copyright 2020-2021 Huawei Technologies Co., Ltd
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
"""Convert ckpt to air."""
import os
import numpy as np

from mindspore import context
from mindspore import Tensor
from mindspore.train.serialization import export, load_checkpoint, load_param_into_net

from src.FaceDetection.yolov3 import HwYolov3 as backbone_HwYolov3
from model_utils.config import config

def save_air():
    '''save air'''
    print('============= yolov3 start save air ==================')
    devid = int(os.getenv('DEVICE_ID', '0')) if config.run_platform != 'CPU' else 0
    context.set_context(mode=context.GRAPH_MODE, device_target=config.run_platform, save_graphs=False, device_id=devid)

    num_classes = config.num_classes
    anchors_mask = config.anchors_mask
    num_anchors_list = [len(x) for x in anchors_mask]

    network = backbone_HwYolov3(num_classes, num_anchors_list, config)

    if os.path.isfile(config.pretrained):
        param_dict = load_checkpoint(config.pretrained)
        param_dict_new = {}
        for key, values in param_dict.items():
            if key.startswith('moments.'):
                continue
            elif key.startswith('network.'):
                param_dict_new[key[8:]] = values
            else:
                param_dict_new[key] = values
        load_param_into_net(network, param_dict_new)
        print('load model {} success'.format(config.pretrained))

        input_data = np.random.uniform(low=0, high=1.0, size=(config.batch_size, 3, 448, 768)).astype(np.float32)

        tensor_input_data = Tensor(input_data)
        export(network, tensor_input_data,
               file_name=config.pretrained.replace('.ckpt', '_' + str(config.batch_size) + 'b.air'), file_format='AIR')

        print("export model success.")


if __name__ == "__main__":
    save_air()
