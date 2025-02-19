/**
 * Copyright 2020 Huawei Technologies Co., Ltd
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

#ifndef MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_FP32_ADDN_H_
#define MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_FP32_ADDN_H_

#include <vector>
#include "src/inner_kernel.h"
#include "schema/model_generated.h"

namespace mindspore::kernel {
class AddNCPUKernel : public InnerKernel {
 public:
  AddNCPUKernel(OpParameter *parameter, const std::vector<lite::Tensor *> &inputs,
                const std::vector<lite::Tensor *> &outputs, const lite::InnerContext *ctx)
      : InnerKernel(parameter, inputs, outputs, ctx) {}
  ~AddNCPUKernel() = default;

  int Init() override;
  int ReSize() override;
  int Run() override;
  int AddNParallelRun(int thread_id);

 private:
  float *in1_addr_;
  float *in2_addr_;
  float *out_addr_;
  int elements_num_;
};
}  // namespace mindspore::kernel

#endif  // MINDSPORE_LITE_SRC_RUNTIME_KERNEL_ARM_FP32_ADDN_H_
