/**
 * Copyright 2021 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "runtime/framework/actor/kernel_actor.h"
#include "runtime/framework/actor/memory_manager_actor.h"
#include "runtime/framework/actor/output_actor.h"
#include "runtime/framework/actor/recorder_actor.h"
#include "runtime/framework/actor/debug_actor.h"
#include "mindrt/include/async/async.h"
#include "utils/log_adapter.h"

namespace mindspore {
namespace runtime {
void KernelActor::Init() {
  MS_EXCEPTION_IF_NULL(kernel_);
  real_input_num_ = AnfAlgo::GetInputTensorNum(kernel_);
  kernel_info_ = static_cast<KernelInfo *>(kernel_->kernel_info());

  // Init the device tensors and kernel launch info.
  input_device_tensors_.resize(real_input_num_);
  for (auto &input_address : input_device_tensors_) {
    memory_free_list_.emplace_back(input_address);
    launch_info_.inputs_.emplace_back(std::make_shared<Address>());
  }
  MS_EXCEPTION_IF_NULL(kernel_info_);
  for (auto &output_address : kernel_info_->output_address_list()) {
    MS_EXCEPTION_IF_NULL(output_address);
    output_device_tensors_.emplace_back(output_address.get());
    memory_alloc_list_.emplace_back(output_address.get());
    memory_free_list_.emplace_back(output_address.get());
    launch_info_.outputs_.emplace_back(std::make_shared<Address>());
  }
  for (auto &workspace_address : kernel_info_->workspace_address_list()) {
    MS_EXCEPTION_IF_NULL(workspace_address);
    workspace_device_tensors_.emplace_back(workspace_address.get());
    memory_alloc_list_.emplace_back(workspace_address.get());
    memory_free_list_.emplace_back(workspace_address.get());
    launch_info_.workspaces_.emplace_back(std::make_shared<Address>());
  }

  // Init the output data.
  output_data_by_output_index_.resize(output_device_tensors_.size());
  for (auto &data_arrow : output_data_arrows_) {
    MS_EXCEPTION_IF_NULL(data_arrow);
    if (IntToSize(data_arrow->from_output_index_) >= output_device_tensors_.size()) {
      MS_LOG(EXCEPTION) << "The output index is out of range: " << GetAID().Name();
    }
    auto device_address = output_device_tensors_[data_arrow->from_output_index_];
    auto data =
      std::make_unique<OpData<DeviceTensor>>(data_arrow->to_op_id_, device_address, data_arrow->to_input_index_);
    output_data_.emplace_back(data.get());
    output_data_by_output_index_[data_arrow->from_output_index_].emplace_back(std::move(data));
  }
}

void KernelActor::RunOpData(OpData<DeviceTensor> *input_data, OpContext<DeviceTensor> *context) {
  MS_EXCEPTION_IF_NULL(context);
  auto &sequential_num = context->sequential_num_;
  input_op_datas_[sequential_num].emplace_back(input_data);
  // When all the inputs are collected, then allocate memory and callback launch.
  if (CheckLaunchCondition(context)) {
    FetchInputDeviceTensor(context);
    FetchOutputDeviceTensor();
    SendMemoryAllocReq(context);
  }
}

void KernelActor::RunOpControl(AID *input_control, OpContext<DeviceTensor> *context) {
  MS_EXCEPTION_IF_NULL(context);
  auto &sequential_num = context->sequential_num_;
  input_op_controls_[sequential_num].emplace_back(input_control);
  // When all the inputs are collected, then allocate memory and callback launch.
  if (CheckLaunchCondition(context)) {
    FetchInputDeviceTensor(context);
    FetchOutputDeviceTensor();
    SendMemoryAllocReq(context);
  }
}

void KernelActor::RunOpControlWithInputTensor(AID *input_control, OpContext<DeviceTensor> *context,
                                              const std::vector<TensorPtr> *input_tensors) {
  MS_EXCEPTION_IF_NULL(context);
  MS_EXCEPTION_IF_NULL(input_tensors);
  auto &sequential_num = context->sequential_num_;
  input_op_controls_[sequential_num].emplace_back(input_control);
  PushInputDeviceTensor(input_tensors);
  // When all the inputs are collected, then allocate memory and callback launch.
  if (CheckLaunchCondition(context)) {
    FetchInputDeviceTensor(context);
    FetchOutputDeviceTensor();
    SendMemoryAllocReq(context);
  }
}

void KernelActor::SendMemoryAllocReq(OpContext<DeviceTensor> *context) {
  Async(memory_manager_aid_, &MemoryManagerActor::AllocateMemory, &memory_alloc_list_, device_context_, context,
        GetAID());
}

void KernelActor::SendMemoryFreeReq(OpContext<DeviceTensor> *context) {
  Async(memory_manager_aid_, &MemoryManagerActor::FreeMemory, &memory_free_list_, device_context_, context);
}

void KernelActor::OnMemoryAllocFinish(OpContext<DeviceTensor> *context) {
  MS_EXCEPTION_IF_NULL(context);
  MS_EXCEPTION_IF_NULL(kernel_);
  MS_EXCEPTION_IF_NULL(device_context_);

  PreLaunchKernel(context);

  auto ret =
    device_context_->LaunchKernel(kernel_, launch_info_.inputs_, launch_info_.workspaces_, launch_info_.outputs_);
  if (!ret) {
    std::string error_info = "Launch kernel failed: " + kernel_->ToString();
    SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), error_info);
  }

  // Debug actor is blocked, must wait debug actor callback message to process continue.
  if (debug_aid_ != nullptr) {
    SendDebugReq(context);
    return;
  }

  PostLaunchKernel(context);
}

void KernelActor::SendDebugReq(OpContext<DeviceTensor> *context) {
  Async(*debug_aid_, &DebugActor::Debug, kernel_, device_context_, context, &GetAID());
}

void KernelActor::OnDebugFinish(OpContext<DeviceTensor> *context) {
  MS_EXCEPTION_IF_NULL(context);
  PostLaunchKernel(context);
}

bool KernelActor::CheckLaunchCondition(OpContext<DeviceTensor> *context) const {
  MS_EXCEPTION_IF_NULL(context);
  if (input_datas_num_ != 0) {
    const auto &data_iter = input_op_datas_.find(context->sequential_num_);
    if (data_iter == input_op_datas_.end()) {
      return false;
    }
    if (data_iter->second.size() != input_datas_num_) {
      return false;
    }
  }

  if (input_controls_num_ != 0) {
    const auto &control_iter = input_op_controls_.find(context->sequential_num_);
    if (control_iter == input_op_controls_.end()) {
      return false;
    }
    if (control_iter->second.size() != input_controls_num_) {
      return false;
    }
  }
  return true;
}

void KernelActor::PushInputDeviceTensor(const std::vector<TensorPtr> *input_tensors) {
  MS_EXCEPTION_IF_NULL(input_tensors);
  if (input_tensors->size() != real_input_num_) {
    MS_LOG(ERROR) << "Input tensor number: " << input_tensors->size()
                  << " is not equal to kernel's input size: " << real_input_num_;
    return;
  }

  for (size_t input_index = 0; input_index < input_tensors->size(); input_index++) {
    const auto &device_tensor =
      std::dynamic_pointer_cast<DeviceTensor>((*input_tensors)[input_index]->device_address());
    if (device_tensor != nullptr) {
      input_device_tensors_[input_index] = device_tensor.get();
      memory_free_list_[input_index] = device_tensor.get();
    }
  }
}

void KernelActor::FetchInputDeviceTensor(OpContext<DeviceTensor> *context) {
  MS_EXCEPTION_IF_NULL(context);
  MS_EXCEPTION_IF_NULL(device_context_);

  const auto &data_iter = input_op_datas_.find(context->sequential_num_);
  if (data_iter != input_op_datas_.end()) {
    for (auto &input_data : data_iter->second) {
      MS_EXCEPTION_IF_NULL(input_data);
      if (input_device_tensors_[input_data->index_] != input_data->data_) {
        input_device_tensors_[input_data->index_] = input_data->data_;
        memory_free_list_[input_data->index_] = input_data->data_;
      }
    }
  }

  for (auto &device_tensor_store_key : device_tensor_store_keys_) {
    auto device_tensor =
      DeviceTensorStore::GetInstance().Fetch(device_tensor_store_key.second, device_context_->GetDeviceAddressType());
    if (device_tensor == nullptr) {
      std::string error_info =
        GetAID().Name() + " get device tensor store failed: " + device_tensor_store_key.second->fullname_with_scope() +
        ", device type:" + std::to_string(static_cast<int>(device_context_->GetDeviceAddressType()));
      SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), error_info);
    }
    if (input_device_tensors_[device_tensor_store_key.first] != device_tensor) {
      input_device_tensors_[device_tensor_store_key.first] = device_tensor;
      memory_free_list_[device_tensor_store_key.first] = device_tensor;
    }
  }
}

void KernelActor::FetchOutputDeviceTensor() {
  MS_EXCEPTION_IF_NULL(kernel_info_);
  auto &output_addresss = kernel_info_->output_address_list();
  for (size_t i = 0; i < output_addresss.size(); ++i) {
    auto output_address = output_addresss[i].get();
    if (output_device_tensors_[i] != output_address) {
      output_device_tensors_[i] = output_address;
      memory_alloc_list_[i] = output_address;
      memory_free_list_[real_input_num_ + i] = output_address;

      // Update output data.
      for (auto &output_data : output_data_by_output_index_[i]) {
        output_data->data_ = output_address;
      }
    }
  }
}

void KernelActor::PreLaunchKernel(OpContext<DeviceTensor> *) {
  for (size_t i = 0; i < input_device_tensors_.size(); ++i) {
    MS_EXCEPTION_IF_NULL(input_device_tensors_[i]);
    launch_info_.inputs_[i]->addr = input_device_tensors_[i]->GetMutablePtr();
    launch_info_.inputs_[i]->size = input_device_tensors_[i]->GetSize();
  }

  for (size_t i = 0; i < output_device_tensors_.size(); ++i) {
    MS_EXCEPTION_IF_NULL(output_device_tensors_[i]);
    launch_info_.outputs_[i]->addr = output_device_tensors_[i]->GetMutablePtr();
    launch_info_.outputs_[i]->size = output_device_tensors_[i]->GetSize();
  }

  for (size_t i = 0; i < workspace_device_tensors_.size(); ++i) {
    MS_EXCEPTION_IF_NULL(workspace_device_tensors_[i]);
    launch_info_.workspaces_[i]->addr = workspace_device_tensors_[i]->GetMutablePtr();
    launch_info_.workspaces_[i]->size = workspace_device_tensors_[i]->GetSize();
  }
}

void KernelActor::PostLaunchKernel(OpContext<DeviceTensor> *context) {
  // The input is invalid and needs to be erased when finish kernel launch.
  EraseInput(context);

  // Note that SendMemoryFreeReq must be in front of SendOutput, because SendOutput will trigger SendMemoryAllocReq of
  // the next actor and the actor is asynchronous execution. So it is necessary to ensure that SendMemoryFreeReq of the
  // current actor is in front of SendMemoryAllocReq of the next actor.  One is to reuse the memory more fully, the
  // other is to ensure the execution order and avoid the illegal memory timing problem.
  SendMemoryFreeReq(context);
  SendOutput(context);
}

void KernelActor::SendOutput(OpContext<DeviceTensor> *context) const {
  MS_EXCEPTION_IF_NULL(context);
  // Send output data.
  for (auto &output_data : output_data_) {
    MS_EXCEPTION_IF_NULL(output_data);
    Async(output_data->op_id_, &OpActor::RunOpData, output_data, context);
  }

  // Send output control.
  if (output_control_arrows_.size() > 0) {
    auto source_aid = const_cast<AID *>(&GetAID());
    for (auto &output_control : output_control_arrows_) {
      Async(output_control, &OpActor::RunOpControl, source_aid, context);
    }
  }

  // Send graph output result.
  for (const auto &result_arrow : output_result_arrows_) {
    MS_EXCEPTION_IF_NULL(result_arrow);
    Async(result_arrow->to_op_id_, &OutputActor::CollectOutput, kernel_, result_arrow->from_output_index_,
          result_arrow->to_input_index_, context);
  }

  // Send recorder info.
  if (recorder_aid_ != nullptr) {
    Async(*recorder_aid_, &RecorderActor::RecordMemAddressInfo, kernel_.get(), &launch_info_, device_context_, context);
  }

  // No output.
  if ((output_data_arrows_.size() == 0) && (output_control_arrows_.size() == 0) &&
      (output_result_arrows_.size() == 0)) {
    SET_OPCONTEXT_SUCCESS_RET((*context));
  }
}

void KernelActor::EraseInput(OpContext<DeviceTensor> *context) {
  MS_EXCEPTION_IF_NULL(context);
  if (input_datas_num_ != 0) {
    auto ret = input_op_datas_.erase(context->sequential_num_);
    if (ret == 0) {
      std::string error_info = "Erase input data failed: " + GetAID().Name();
      SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), error_info);
    }
  }

  if (input_controls_num_ != 0) {
    auto ret = input_op_controls_.erase(context->sequential_num_);
    if (ret == 0) {
      std::string error_info = "Erase input controls failed: " + GetAID().Name();
      SET_OPCONTEXT_FAIL_RET_WITH_ERROR((*context), error_info);
    }
  }
}

}  // namespace runtime
}  // namespace mindspore
