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

#ifndef MINDSPORE_CCSRC_RUNTIME_FRAMEWORK_ACTOR_SWITCH_ACTOR_H_
#define MINDSPORE_CCSRC_RUNTIME_FRAMEWORK_ACTOR_SWITCH_ACTOR_H_

#include <vector>
#include <string>
#include <memory>
#include <utility>
#include <unordered_map>
#include "runtime/framework/actor/actor_common.h"
#include "runtime/framework/device_tensor_store.h"
#include "mindrt/include/actor/switch_actor.h"
#include "runtime/hardware/device_context.h"

namespace mindspore {
namespace runtime {
using mindspore::device::DeviceContext;
using mindspore::session::KernelWithIndex;

constexpr size_t kSwitchInputNum = 4;
constexpr size_t kSwitchCondPos = 1;
constexpr size_t kSwitchPartialNum = 2;
constexpr size_t kSwitchLayerCondPos = 1;
constexpr size_t kSwitchLayerBranchPos = 2;
constexpr size_t kSwitchLayerInputNum = 3;
constexpr size_t kMaxSwitchCondSize = 8;
constexpr size_t kSwitchTrueBranchPos = 2;
constexpr size_t kSwitchFalseBranchPos = 3;
constexpr size_t kPartialFuncGraphPos = 1;
constexpr size_t kPartialInputStartPos = 2;
constexpr size_t kCallInputStartPos = 1;
constexpr size_t kMakeTupleInputStartPos = 1;

// Switch actor is used to execute the branch according to the input condition.
// Switch and SwitchLayer node will be converted to switch actor.
// The execution process is divided into:
// 1. Put input into the vector.
// 2. Check whether the input condition has been received.
// 3. Check whether all input from the branch corresponding to the index has been received.
// 4. Send the data to the corresponding branch.
// 5. Free Memory
class SwitchActor : public SwitchActorBase<DeviceTensor> {
 public:
  SwitchActor(const std::string &name, const CNodePtr &node) : SwitchActorBase(name), node_(node) {}
  ~SwitchActor() override = default;

  void Init() override;

  // The switch actor run when receive the input data.
  void RunOpData(OpData<DeviceTensor> *input_data, OpContext<DeviceTensor> *context);
  // Initialize the input and output information of the switch actor According to node_.
  void Initialize();
  // Add input for all branches.
  void AddCommonInput(const AnfNodePtr &node);
  // Fetch the input position of the data node.
  size_t FetchDataNodePosition(const AnfNodePtr &data_node) const;

 private:
  friend class GraphScheduler;

  void InitPartial(const AnfNodePtr &node, const size_t branch_id);
  void InitSwitch();
  void InitSwitchLayer();

  // Get index from DeviceTensor.
  size_t GetIndex();
  // Add input for the branch.
  void AddInput(const AnfNodePtr &node, size_t branch);

  // Check whether satisfy the condition for send outputs.
  bool CheckLaunchCondition(OpContext<DeviceTensor> *context) const;
  // Fetch the args of switch branch.
  void FetchInputDeviceTensor(OpContext<DeviceTensor> *context);
  void SendOutput(OpContext<DeviceTensor> *context);
  void SendMemoryFreeReq(OpContext<DeviceTensor> *context);

  // All inputs of the switch actor, excluding weight and tensor.
  // Used to receive input data, the first input is the condition of switch.
  std::vector<AnfNodePtr> input_nodes_;
  // The position of the branch output in the input_nodes_.
  std::vector<std::vector<size_t>> branch_inputs_pos_;

  std::vector<std::vector<AnfNodePtr>> branch_total_inputs_;
  std::vector<FuncGraphPtr> branch_func_graph_;

  std::vector<DeviceTensor *> input_device_tensors_;

  // Save the DeviceContext of input_nodes_, which is used to release the DeviceTensor.
  DeviceContext *device_context_;

  // The id of memory manager actor. Send message to it for alloc and free memory.
  const AID memory_manager_aid_;
  // The dependent input data number.
  size_t input_datas_num_{0};
  CNodePtr node_;

  //  The output_data_ corresponds to the output_data_arrows_ one by one.
  std::vector<std::vector<OpDataUniquePtr<DeviceTensor>>> output_data_;
};

using SwitchActorPtr = std::shared_ptr<SwitchActor>;
}  // namespace runtime
}  // namespace mindspore

#endif  // MINDSPORE_CCSRC_RUNTIME_FRAMEWORK_ACTOR_SWITCH_ACTOR_H_
