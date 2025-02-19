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
#ifndef MINDSPORE_LITE_TOOLS_OPTIMIZER_FUSION_TFLITE_REL_POS_MULTI_HEAD_ATTENTION_FUSION_H_
#define MINDSPORE_LITE_TOOLS_OPTIMIZER_FUSION_TFLITE_REL_POS_MULTI_HEAD_ATTENTION_FUSION_H_

#include <vector>
#include <memory>
#include <string>
#include "backend/optimizer/common/optimizer.h"
#include "utils/utils.h"
#include "include/errorcode.h"
#include "tools/optimizer/fusion/tf_multi_head_attention_fusion.h"

namespace mindspore::opt {
class TfliteRelPosMultiHeadAttentionFusion : public TfMultiHeadAttentionFusion {
 public:
  explicit TfliteRelPosMultiHeadAttentionFusion(const std::string &name = "tflite_rel_pos_multi_head_attention_fusion",
                                                bool multigraph = true);
  ~TfliteRelPosMultiHeadAttentionFusion() override = default;
  const BaseRef DefinePattern() const override;
  const AnfNodePtr Process(const FuncGraphPtr &, const AnfNodePtr &, const EquivPtr &) const override;

 protected:
  std::shared_ptr<ops::Attention> BuildAttentionPrim(const EquivPtr &equiv) const;

  const VectorRef DefineProcessInputPattern(const BaseRef &input, const BaseRef &weight, const BaseRef &bias,
                                            const std::vector<VarPtr> &stack_params, const VarPtr &full_connect_prim,
                                            bool transpose = false) const;
  const VectorRef DefineProcessOutputPattern(const BaseRef &input, const BaseRef &weight, const BaseRef &bias) const;

  CNodePtr CreateRelPosMultiHeadAttentionNode(const FuncGraphPtr &func_graph, const EquivPtr &equiv,
                                              const std::string &base_name) const;
  const VectorRef DefineRelativeShiftPattern(const BaseRef &input) const;

  VarPtr query_u_;
  VarPtr query_v_;
  VarPtr query_prim_;
  VarPtr key_prim_;
  VarPtr value_prim_;
  VarPtr pos_prim_;
  VarPtr output_prim_;
  VarPtr input_p_;
  VarPtr weight_p_;
  std::vector<VarPtr> query_stack_params_;
  std::vector<VarPtr> key_stack_params_;
  std::vector<VarPtr> value_stack_params_;
  std::vector<VarPtr> pos_stack_params_;
};

}  // namespace mindspore::opt
#endif  // MINDSPORE_LITE_TOOLS_OPTIMIZER_FUSION_TFLITE_REL_POS_MULTI_HEAD_ATTENTION_FUSION_H_
