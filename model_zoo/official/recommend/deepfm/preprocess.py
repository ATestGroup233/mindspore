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
"""preprocess."""
import os
import argparse

from src.config import DataConfig, TrainConfig
from src.dataset import create_dataset, DataType

parser = argparse.ArgumentParser(description='preprocess.')
parser.add_argument('--dataset_path', type=str, default=None, help='Dataset path')
parser.add_argument('--result_path', type=str, default='./preprocess_Result', help='Result path')
args_opt, _ = parser.parse_known_args()

def generate_bin():
    '''generate bin files'''
    data_config = DataConfig()
    train_config = TrainConfig()

    ds = create_dataset(args_opt.dataset_path, train_mode=False,
                        epochs=1, batch_size=train_config.batch_size,
                        data_type=DataType(data_config.data_format))
    batch_ids_path = os.path.join(args_opt.result_path, "00_batch_ids")
    batch_wts_path = os.path.join(args_opt.result_path, "01_batch_wts")
    labels_path = os.path.join(args_opt.result_path, "02_labels")

    os.makedirs(batch_ids_path)
    os.makedirs(batch_wts_path)
    os.makedirs(labels_path)

    for i, data in enumerate(ds.create_dict_iterator(output_numpy=True)):
        file_name = "criteo_bs" + str(train_config.batch_size) + "_" + str(i) + ".bin"
        batch_ids = data['feat_ids']
        batch_ids.tofile(os.path.join(batch_ids_path, file_name))

        batch_wts = data['feat_vals']
        batch_wts.tofile(os.path.join(batch_wts_path, file_name))

        labels = data['label']
        labels.tofile(os.path.join(labels_path, file_name))

    print("=" * 20, "export bin files finished", "=" * 20)

if __name__ == '__main__':
    generate_bin()
