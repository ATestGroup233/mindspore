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

#ifndef MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_BASE_SPLIT_WITH_OVER_LAP_BASE_H_
#define MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_BASE_SPLIT_WITH_OVER_LAP_BASE_H_

#include <vector>
#include "include/errorcode.h"
#include "include/context.h"
#include "src/lite_kernel.h"
#include "nnacl/split_parameter.h"
#include "nnacl/base/split_with_over_lap_base.h"

namespace mindspore::kernel {
class SplitWithOverlapBaseCPUKernel : public InnerKernel {
 public:
  SplitWithOverlapBaseCPUKernel(OpParameter *parameter, const std::vector<lite::Tensor *> &inputs,
                                const std::vector<lite::Tensor *> &outputs, const lite::InnerContext *ctx)
      : InnerKernel(parameter, inputs, outputs, ctx) {
    param_ = reinterpret_cast<SplitWithOverlapParameter *>(op_parameter_);
  }
  ~SplitWithOverlapBaseCPUKernel() override = default;
  void CalculateSplitedShapes(const SplitWithOverlapParameter *param, const std::vector<int> &shape);
  int Init() override;
  int ReSize() override;
  int Run() override;
  int Split(int task_id);

 protected:
  // range: [start, end)
  std::vector<int> start_indices_;
  std::vector<int> end_indices_;

  int outer_total_dim_{0};
  int inner_stride_{0};
  int element_bytes_{0};
  int split_dim_size_{0};
  SplitWithOverlapParameter *param_ = nullptr;
  char *input_ptr_{nullptr};
  std::vector<char *> output_ptr_;
};
}  // namespace mindspore::kernel

#endif  // MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_BASE_SPLIT_WITH_OVER_LAP_BASE_H_
