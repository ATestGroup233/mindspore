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
"""preprocess"""
import os
import argparse
import json
import numpy as np
from src.dataset import vgg_create_dataset

def create_label(result_path, dir_path):
    print("[WARNING] Create imagenet label. Currently only use for Imagenet2012!")
    dirs = os.listdir(dir_path)
    file_list = []
    for file in dirs:
        file_list.append(file)
    file_list = sorted(file_list)

    total = 0
    img_label = {}
    for i, file_dir in enumerate(file_list):
        files = os.listdir(os.path.join(dir_path, file_dir))
        for f in files:
            img_label[f] = i
        total += len(files)

    json_file = os.path.join(result_path, "imagenet_label.json")
    with open(json_file, "w+") as label:
        json.dump(img_label, label)

    print("[INFO] Completed! Total {} data.".format(total))

parser = argparse.ArgumentParser('preprocess')
parser.add_argument('--dataset', type=str, choices=["cifar10", "imagenet2012"], default="cifar10")
parser.add_argument('--data_path', type=str, default='', help='eval data dir')
parser.add_argument('--result_path', type=str, default='./preprocess_Result/', help='result path')
args = parser.parse_args()

if args.dataset == "cifar10":
    from src.config import cifar_cfg as cfg
else:
    from src.config import imagenet_cfg as cfg

args.per_batch_size = cfg.batch_size
args.image_size = list(map(int, cfg.image_size.split(',')))


if __name__ == "__main__":
    if args.dataset == "cifar10":
        dataset = vgg_create_dataset(args.data_path, args.image_size, args.per_batch_size, training=False)
        img_path = os.path.join(args.result_path, "00_data")
        os.makedirs(img_path)
        label_list = []
        for idx, data in enumerate(dataset.create_dict_iterator(output_numpy=True)):
            file_name = "VGG16_data_bs" + str(args.per_batch_size) + "_" + str(idx) + ".bin"
            file_path = os.path.join(img_path, file_name)
            data["image"].tofile(file_path)
            label_list.append(data["label"])
        np.save(os.path.join(args.result_path, "cifar10_label_ids.npy"), label_list)
        print("=" * 20, "export bin files finished", "=" * 20)
    else:
        create_label(args.result_path, args.data_path)
