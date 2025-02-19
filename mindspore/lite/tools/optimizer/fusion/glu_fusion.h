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
#ifndef MINDSPORE_LITE_TOOLS_OPTIMIZER_FUSION_GLU_FUSION_H_
#define MINDSPORE_LITE_TOOLS_OPTIMIZER_FUSION_GLU_FUSION_H_

#include <vector>
#include <memory>
#include <string>
#include "backend/optimizer/common/optimizer.h"
#include "utils/utils.h"
#include "tools/optimizer/common/gllo_utils.h"

namespace mindspore {
namespace opt {
class GLUFusion : public PatternProcessPass {
 public:
  explicit GLUFusion(const std::string &name = "glu_fusion", bool multigraph = true)
      : PatternProcessPass(name, multigraph), input_(std::make_shared<Var>()) {}

  ~GLUFusion() override = default;
  const AnfNodePtr Process(const FuncGraphPtr &, const AnfNodePtr &, const EquivPtr &) const override;
  const BaseRef DefinePattern() const override;

 protected:
  VarPtr input_ = nullptr;

 private:
  CNodePtr CreateGLUNode(const FuncGraphPtr &func_graph, const AnfNodePtr &node, const EquivPtr &equiv) const;
};
}  // namespace opt
}  // namespace mindspore

#endif  // MINDSPORE_LITE_TOOLS_OPTIMIZER_FUSION_GLU_FUSION_H_
