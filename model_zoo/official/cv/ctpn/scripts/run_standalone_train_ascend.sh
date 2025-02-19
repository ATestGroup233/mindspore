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
echo "=============================================================================================================="
echo "Please run the script as: "
echo "sh run_standalone_train.sh [TASK_TYPE] [PRETRAINED_PATH] [DEVICE_ID]"
echo "for example: sh run_standalone_train.sh Pretraining /path/vgg16_backbone.ckpt 0"
echo "when device id is occupied, choose for another one"
echo "It is better to use absolute path."
echo "=============================================================================================================="
if [ $# -ne 3 ]
then 
    echo "Usage: sh run_standalone_train_ascend.sh [TASK_TYPE] [PRETRAINED_PATH] [DEVICE_ID]"
exit 1
fi

get_real_path(){
  if [ "${1:0:1}" == "/" ]; then
    echo "$1"
  else
    echo "$(realpath -m $PWD/$1)"
  fi
}

TASK_TYPE=$1
PRETRAINED_PATH=$(get_real_path $2)
echo $PRETRAINED_PATH
if [ ! -f $PRETRAINED_PATH ]
then 
    echo "error: PRETRAINED_PATH=$PRETRAINED_PATH is not a file"
exit 1
fi

ulimit -u unlimited
export DEVICE_NUM=1
export DEVICE_ID=$3
export RANK_ID=0
export RANK_SIZE=1

rm -rf ./train
mkdir ./train
cp ../*.py ./train
cp *.sh ./train
cp -r ../src ./train
cd ./train || exit
echo "start training for device $DEVICE_ID"
env > env.log
python train.py --device_id=$DEVICE_ID --task_type=$TASK_TYPE --pre_trained=$PRETRAINED_PATH &> log &
cd ..
