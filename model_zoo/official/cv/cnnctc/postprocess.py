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
"""post process for 310 inference"""
import os
import argparse
import numpy as np
from src.config import Config_CNNCTC
from src.util import CTCLabelConverter

parser = argparse.ArgumentParser(description="cnnctc acc calculation")
parser.add_argument("--result_path", type=str, required=True, help="result files path.")
parser.add_argument("--label_path", type=str, required=True, help="label path.")
args = parser.parse_args()


def calcul_acc(labels, preds):
    return sum(1 for x, y in zip(labels, preds) if x == y) / len(labels)


def get_result(result_path, label_path):
    config = Config_CNNCTC()
    converter = CTCLabelConverter(config.CHARACTER)
    files = os.listdir(result_path)
    preds = []
    labels = []
    label_dict = {}
    with open(label_path, 'r') as f:
        lines = f.readlines()
        for line in lines:
            label_dict[line.split(',')[0]] = line.split(',')[1].replace('\n', '')
    for file in files:
        file_name = file.split('.')[0]
        label = label_dict[file_name + '.png']
        labels.append(label)
        resultPath = os.path.join(result_path, file)
        output = np.fromfile(resultPath, dtype=np.float32)
        output = np.reshape(output, (config.FINAL_FEATURE_WIDTH, config.NUM_CLASS))
        model_predict = np.squeeze(output)
        preds_size = np.array([model_predict.shape[0]] * 1)
        preds_index = np.argmax(model_predict, axis=1)
        preds_str = converter.decode(preds_index, preds_size)
        preds.append(preds_str[0].upper())
    acc = calcul_acc(labels, preds)
    print("TOtal data: {}, accuracy: {}".format(len(labels), acc))


if __name__ == '__main__':
    get_result(args.result_path, args.label_path)
