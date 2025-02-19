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

#include "tools/converter/import/mindspore_importer.h"
#include <memory>
#include <vector>
#include "tools/converter/parser/parser_utils.h"
#include "tools/converter/import/primitive_adjust.h"
#include "tools/converter/import/mindir_adjust.h"
#include "tools/optimizer/common/gllo_utils.h"
#include "tools/common/tensor_util.h"

namespace mindspore::lite {
namespace {
constexpr size_t kConvWeightIndex = 2;
}  // namespace
STATUS MindsporeImporter::AdjustForMindir(const FuncGraphPtr &func_graph, const converter::Flags &flag) {
  auto primitive_adjust_pass = std::make_shared<PrimitiveAdjust>();
  primitive_adjust_pass->SetFmkType(flag.fmk);
  if (!primitive_adjust_pass->Run(func_graph)) {
    MS_LOG(ERROR) << "primitive adjust failed.";
    ReturnCode::GetSingleReturnCode()->UpdateReturnCode(RET_ERROR);
    return RET_ERROR;
  }
  auto mindir_adjust_pass = std::make_shared<MindirAdjust>();
  mindir_adjust_pass->SetFmkType(flag.fmk);
  mindir_adjust_pass->SetQuantType(flag.quantType);
  mindir_adjust_pass->SetTrainFlag(flag.trainModel);
  if (!mindir_adjust_pass->Run(func_graph)) {
    MS_LOG(ERROR) << "mindir adjust failed.";
    ReturnCode::GetSingleReturnCode()->UpdateReturnCode(RET_ERROR);
    return RET_ERROR;
  }
  return RET_OK;
}

STATUS MindsporeImporter::WeightFormatTransform(const FuncGraphPtr &graph) {
  MS_ASSERT(graph != nullptr);
  auto node_list = TopoSort(graph->get_return());
  for (auto &node : node_list) {
    if (!utils::isa<CNodePtr>(node)) {
      continue;
    }
    auto conv_cnode = node->cast<CNodePtr>();
    if (!opt::CheckPrimitiveType(node, prim::kPrimConv2DFusion) &&
        !opt::CheckPrimitiveType(node, opt::kPrimConv2DBackpropInputFusion) &&
        !opt::CheckPrimitiveType(node, prim::kPrimConv2dTransposeFusion)) {
      continue;
    }
    MS_ASSERT(conv_cnode->inputs().size() > kConvWeightIndex);
    int status = HardCodeMindir(conv_cnode, graph);
    if (status != lite::RET_OK) {
      MS_LOG(ERROR) << "Format hard code failed: " << status << ", node: " << node->fullname_with_scope();
      return RET_ERROR;
    }
  }
  return RET_OK;
}

STATUS MindsporeImporter::HardCodeMindir(const CNodePtr &conv_node, const FuncGraphPtr &graph) {
  MS_ASSERT(conv_cnode != nullptr);
  auto prim = GetValueNode<PrimitivePtr>(conv_node->input(0));
  if (prim == nullptr) {
    MS_LOG(ERROR) << "Invalid anfnode, which don't have primitive.";
    return lite::RET_ERROR;
  }
  int64_t format = prim->GetAttr(ops::kFormat) != nullptr ? GetValue<int64_t>(prim->GetAttr(ops::kFormat)) : 0;
  auto weight_node = conv_node->input(kConvWeightIndex);
  schema::Format weight_dst_format = schema::Format::Format_KHWC;
  STATUS status = RET_OK;
  schema::Format weight_src_format = schema::Format::Format_NUM_OF_FORMAT;
  switch (quant_type_) {
    case QuantType_AwareTraining:
    case QuantType_PostTraining:
    case QuantType_WeightQuant:
    case QuantType_QUANT_NONE: {
      if (format == schema::Format::Format_KHWC) {
        weight_src_format = schema::Format::Format_KHWC;
      } else {
        weight_src_format = schema::Format::Format_KCHW;
      }
    } break;
    default: {
      MS_LOG(ERROR) << "Unsupported quantType: " << EnumNameQuantType(quant_type_)
                    << ", node: " << conv_node->fullname_with_scope();
      return RET_ERROR;
    }
  }
  if (utils::isa<CNodePtr>(weight_node)) {
    status = HandleWeightConst(graph, conv_node, weight_node->cast<CNodePtr>(), weight_src_format, weight_dst_format);
    if (status != lite::RET_OK) {
      MS_LOG(ERROR) << "handle weight-const failed.";
      return RET_ERROR;
    }
  }
  weight_node = conv_node->input(kConvWeightIndex);
  auto weight_value = opt::GetTensorInfo(weight_node);
  if (weight_value != nullptr) {
    status = opt::TransFilterFormat(weight_value, weight_src_format, weight_dst_format);
    if (status != RET_OK) {
      MS_LOG(ERROR) << "TransFilter " << EnumNameFormat(schema::EnumValuesFormat()[weight_dst_format]) << "To"
                    << EnumNameFormat(weight_dst_format) << " failed, node : " << conv_node->fullname_with_scope()
                    << "quant type:" << quant_type_;
      return RET_ERROR;
    }
    prim->AddAttr(ops::kFormat, MakeValue<int64_t>(weight_dst_format));
    auto type_id = static_cast<TypeId>(weight_value->data_type());
    auto shape = weight_value->shape();
    std::vector<int64_t> shape_vector(shape.begin(), shape.end());
    auto abstract = lite::CreateTensorAbstract(shape_vector, type_id);
    if (abstract == nullptr) {
      MS_LOG(ERROR) << "Create tensor abstarct failed";
      return RET_ERROR;
    }
    weight_node->set_abstract(abstract);
  }
  if (utils::isa<ParameterPtr>(weight_node)) {
    status = HandleWeightSharing(graph, KHWC, weight_node->cast<ParameterPtr>(), weight_src_format, weight_dst_format);
    if (status != lite::RET_OK) {
      MS_LOG(ERROR) << "handle weight-sharing failed.";
      return RET_ERROR;
    }
  }
  return lite::RET_OK;
}

FuncGraphPtr MindsporeImporter::ImportMindIR(const converter::Flags &flag) {
  quant_type_ = flag.quantType;
  auto func_graph = LoadMindIR(flag.modelFile);
  if (func_graph == nullptr) {
    MS_LOG(ERROR) << "get funcGraph failed for fmk:MINDIR";
    return nullptr;
  }
  func_graph->set_attr("graph_name", MakeValue("main_graph"));
  func_graph->set_attr("fmk", MakeValue(static_cast<int>(converter::FmkType_MS)));
  if (AdjustForMindir(func_graph, flag) != RET_OK) {
    MS_LOG(ERROR) << "AdjustForMindir failed.";
    return nullptr;
  }
  auto status = WeightFormatTransform(func_graph);
  if (status != RET_OK) {
    MS_LOG(ERROR) << "WeightFormatTransform failed.";
    return nullptr;
  }
  return func_graph;
}
}  // namespace mindspore::lite
