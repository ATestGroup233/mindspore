/**
 * Copyright 2019 Huawei Technologies Co., Ltd
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
#ifndef MINDSPORE_CCSRC_BACKEND_OPTIMIZER_GRAPH_KERNEL_PASS_MANAGER_H_
#define MINDSPORE_CCSRC_BACKEND_OPTIMIZER_GRAPH_KERNEL_PASS_MANAGER_H_

#include <utility>
#include <vector>
#include <string>
#include <memory>

#include "utils/context/graph_kernel_flags.h"
#include "backend/optimizer/common/pass_manager.h"

namespace mindspore {
namespace opt {
enum OptLevel {
  OptLevel_0 = 0,  // Disabled
  OptLevel_1,      // Basic functions
  OptLevel_2,      // Default functions
  OptLevel_3,      // Experimental functions
  OptLevel_MAX,
};

class GraphKernelPassManager : public PassManager {
 public:
  GraphKernelPassManager(size_t stage, const std::string &name)
      : PassManager(name, true), stage_(stage), flags_(context::GraphKernelFlags::GetInstance()) {}
  ~GraphKernelPassManager() = default;

  // Add graph pass, the pass object will be freed when pass manager freed.
  virtual void AddPass(const PassPtr &pass, OptLevel level, bool default_enable = true);

  // Run passes on the func_graph
  bool Run(const FuncGraphPtr &func_graph) const override;

 protected:
  std::string GetPassFullname(size_t pass_id, const PassPtr &pass) const override;

  size_t stage_;
  std::vector<bool> enabled_;
  const context::GraphKernelFlags &flags_;
};
using PassManagerPtr = std::shared_ptr<PassManager>;
}  // namespace opt
}  // namespace mindspore

#endif  // MINDSPORE_CCSRC_BACKEND_OPTIMIZER_GRAPH_KERNEL_PASS_MANAGER_H_
