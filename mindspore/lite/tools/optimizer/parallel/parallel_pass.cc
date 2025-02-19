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

#include "tools/optimizer/parallel/parallel_pass.h"
#include "include/errorcode.h"
#include "ir/tensor.h"
#include "tools/optimizer/parallel/operator_info_register.h"

namespace mindspore {
namespace opt {
bool ParallelPass::IsParallelCareNode(const AnfNodePtr &node) {
  type_name_.clear();
  return std::any_of(kParallelSet.begin(), kParallelSet.end(), [this, &node](auto &prim) {
    if (CheckPrimitiveType(node, prim)) {
      type_name_ = PrimToString(prim);
    }
    return !type_name_.empty();
  });
}

std::string ParallelPass::PrimToString(const PrimitivePtr &prim) {
  std::string parallel_name;
  if (kParallelOpNames.find(prim) == kParallelOpNames.end()) {
    MS_LOG(ERROR) << "String of the type not registered";
    return parallel_name;
  }
  return kParallelOpNames.at(prim);
}

bool ParallelPass::SetParallelOpName(const AnfNodePtr &node, std::string *parallel_name) {
  if (!utils::isa<CNode>(node)) {
    return false;
  }
  auto cnode = node->cast<CNodePtr>();
  std::string cnode_name = cnode->fullname_with_scope();
  if (cnode_name.find(PARALLEL_NAME_SUFFIX) != std::string::npos) {
    MS_LOG(DEBUG) << " : Skip splited cnode " << cnode_name;
    return false;
  }

  // find operator name first, then operator type name.
  if (split_strategys_.find(*parallel_name) == split_strategys_.end()) {
    *parallel_name = type_name_;
  }

  MS_LOG(DEBUG) << " : Reached a parallel care node: " << cnode_name;
  if (split_strategys_.find(*parallel_name) == split_strategys_.end()) {
    MS_LOG(DEBUG) << *parallel_name << " : No split strategy for the current CNode.";
    return false;
  }
  cnode->set_fullname_with_scope(cnode_name + PARALLEL_NAME_SUFFIX);
  return true;
}

OperatorInfoPtr ParallelPass::CreateParallelOperator(const AnfNodePtr &node, const std::string &scope_name,
                                                     const std::string &parallel_op_name) {
  // foreach kernel_list && data_type
  auto cnode = node->cast<CNodePtr>();
  auto node_prim = cnode->input(kParallelPrimitiveIndex);
  auto prim = GetValueNode<PrimitivePtr>(node_prim);
  auto split_key_pair = kParallelSchemaId.find(prim);

  if (split_key_pair == kParallelSchemaId.end()) {
    return nullptr;
  }
  auto split_schema_id = split_key_pair->second.first;
  auto split_type_id = split_key_pair->second.second;
  SplitOpKey op_key = SplitOpKey(split_schema_id, split_type_id);

  auto op_create_func = OperatorInfoFactory::GeInstance()->FindOperatorInfo(op_key);
  if (op_create_func == nullptr) {
    return nullptr;
  }
  OperatorInfoPtr op = op_create_func(scope_name, split_strategys_[parallel_op_name]);
  return op;
}

AnfNodePtr ParallelPass::Run(const FuncGraphPtr &func_graph, const AnfNodePtr &node) {
  if (CheckIfFuncGraphIsNull(func_graph) != lite::RET_OK || CheckIfAnfNodeIsNull(node) != lite::RET_OK) {
    return node;
  }
  if (!utils::isa<CNode>(node)) {
    return node;
  }

  if (!IsParallelCareNode(node)) {
    return node;
  }

  auto cnode = node->cast<CNodePtr>();
  std::string parallel_op_name = cnode->fullname_with_scope();
  if (CheckIfCNodeIsNull(cnode) != lite::RET_OK) {
    return nullptr;
  }

  if (!SetParallelOpName(node, &parallel_op_name)) {
    return node;
  }

  std::string cnode_name = cnode->fullname_with_scope();
  OperatorInfoPtr parallel_operator = CreateParallelOperator(node, cnode_name, parallel_op_name);
  if (parallel_operator == nullptr) {
    MS_LOG(ERROR) << "Failure: Create " << parallel_op_name << " OperatorInstance failed";
    return node;
  }
  parallel_operator->set_cnode(cnode);
  parallel_operator->set_func_graph(func_graph);
  parallel_operator->set_fmk(fmk_type_);
  if (parallel_operator->Init() == RET_ERROR) {
    MS_LOG(ERROR) << "Failure: operator " << parallel_op_name << " init failed";
    return node;
  }
  return parallel_operator->replace_op();
}

}  // namespace opt
}  // namespace mindspore
