#!/bin/bash
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

mkdir -p ms_log
PROJECT_DIR=$(cd "$(dirname "$0")"; pwd)
CUR_DIR=`pwd`
export GLOG_log_dir=${CUR_DIR}/ms_log
export GLOG_logtostderr=0

python ${PROJECT_DIR}/../run_train.py \
    --device_target="Ascend" \
    --device_id=0 \
    --do_eval="true" \
    --epoch_num=3 \
    --task_name="" \
    --do_shuffle="true" \
    --enable_data_sink="true" \
    --data_sink_steps=100 \
    --save_ckpt_step=100 \
    --max_ckpt_num=1 \
    --load_ckpt_path="" \
    --train_data_dir="" \
    --eval_data_dir="" \
    --device_id="" \
    --logging_step=100\
    --do_quant="true" > log.txt 2>&1 &

