/**
 * This is the C++ adaptation and derivative work of Myia (https://github.com/mila-iqia/myia/).
 *
 * Copyright 2019-2021 Huawei Technologies Co., Ltd
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

#include "vm/segment_runner.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <set>
#include <unordered_set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <string>

#include "utils/log_adapter.h"
#include "utils/utils.h"
#include "ir/manager.h"
#include "ir/func_graph_cloner.h"
#include "frontend/operator/ops.h"

namespace mindspore {
namespace compile {

namespace {
// Return the list of nodes whose values are required beyond this segment.
// Arguments:
//   nodes: list of nodes in the segment
//   users: dict mapping each node to its users (globally)
//   seen: set of nodes that are part of the segment
AnfNodePtrList GetOutput(const AnfNodePtrList &nodes, const NodeUsersMap &users,
                         const std::unordered_set<AnfNodePtr> &seen) {
  AnfNodePtrList output;
  if (users.size() == 0) {
    return output;
  }
  for (auto &node : nodes) {
    if (!node->isa<CNode>()) {
      continue;
    }
    auto iter = users.find(node);
    if (iter == users.end()) {
      continue;
    }
    auto &node_users = iter->second;
    const bool has_outer_user = std::any_of(
      std::begin(node_users), std::end(node_users), [&seen](const std::pair<AnfNodePtr, int64_t> &u) -> bool {
        const bool is_outer_user = (seen.find(u.first) == seen.end());
        return is_outer_user && !(IsPrimitiveCNode(u.first, prim::kPrimUpdateState) && u.second > 2);
      });
    if (has_outer_user) {
      output.emplace_back(node);
    }
  }
  return output;
}

AnfNodePtr RefSubGraphNode(const FuncGraphPtr &fg, const AnfNodePtr &node, AnfNodePtrList *const inputs_ptr,
                           AnfNodePtrToAnfNodePtrMap *eqv_ptr) {
  MS_EXCEPTION_IF_NULL(fg);
  MS_EXCEPTION_IF_NULL(inputs_ptr);
  MS_EXCEPTION_IF_NULL(eqv_ptr);
  MS_EXCEPTION_IF_NULL(node);
  auto &inputs = *inputs_ptr;
  auto &eqv = *eqv_ptr;
  if (node->isa<ValueNode>() && !IsValueNode<FuncGraph>(node)) {
    eqv[node] = node;
  } else if (eqv.find(node) == eqv.end()) {
    inputs.push_back(node);
    eqv[node] = fg->add_parameter();
    eqv[node]->set_abstract(node->abstract());
    eqv[node]->set_kernel_info(node->kernel_info_ptr());
  }
  return eqv[node];
}
}  // namespace

std::tuple<FuncGraphPtr, AnfNodePtrList, AnfNodePtrList> TransformSegmentToAnfGraph(const AnfNodePtrList &lst) {
  if (lst.empty()) {
    MS_LOG(EXCEPTION) << "Input anf node list is empty";
  }
  FuncGraphPtr fg = nullptr;
  {
    // limit the lifetime of guard.
    TraceGuard guard(std::make_shared<TraceSegmentTransform>(lst[0]->cast<CNodePtr>()->func_graph()->debug_info()));
    fg = std::make_shared<FuncGraph>();
  }
  AnfNodePtrList inputs;
  AnfNodePtrToAnfNodePtrMap eqv;
  // Merge CNodes into a AnfGraph that represents a linear instruction segment
  for (auto n : lst) {
    if (!n->isa<CNode>()) {
      MS_LOG(EXCEPTION) << "Inst is not CNode";
    }
    auto &inps = n->cast<CNodePtr>()->inputs();
    if (inps.empty()) {
      MS_LOG(EXCEPTION) << "Input is empty";
    }
    if (!IsValueNode<Primitive>(inps[0]) &&
        !(IsValueNode<FuncGraph>(inps[0]) &&
          inps[0]->cast<ValueNodePtr>()->value()->cast<FuncGraphPtr>()->has_attr(FUNC_GRAPH_ATTR_GRAPH_KERNEL))) {
      MS_LOG(EXCEPTION) << "Input[0] Must be a Primitive ValueNode";
    }
    auto fn = inps[0];
    std::vector<AnfNodePtr> args{fn};
    if (IsPrimitive(fn, prim::kPrimDepend) && inps.size() >= 3 && eqv.find(inps[kDependAttachNodeIndex]) == eqv.end()) {
      args.emplace_back(RefSubGraphNode(fg, inps[kRealInputIndexInDepend], &inputs, &eqv));
      for (size_t i = 2; i < inps.size(); ++i) {
        args.emplace_back(NewValueNode(MakeValue(0)));
      }
    } else if (IsPrimitive(fn, prim::kPrimUpdateState)) {
      args.emplace_back(RefSubGraphNode(fg, inps[1], &inputs, &eqv));
      args.emplace_back(RefSubGraphNode(fg, inps[2], &inputs, &eqv));
      for (size_t i = 3; i < inps.size(); ++i) {
        auto &input = inps[i];
        if (eqv.find(input) != eqv.end()) {
          args.emplace_back(RefSubGraphNode(fg, input, &inputs, &eqv));
        }
      }
    } else {
      (void)std::transform(std::begin(inps) + 1, std::end(inps), std::back_inserter(args),
                           [&fg, &inputs, &eqv](const AnfNodePtr &a) { return RefSubGraphNode(fg, a, &inputs, &eqv); });
    }
    TraceGuard tg(std::make_shared<TraceSegmentTransform>(n->debug_info()));
    eqv[n] = fg->NewCNode(args);
    eqv[n]->set_abstract(n->abstract());
    eqv[n]->set_kernel_info(n->kernel_info_ptr());
  }
  std::unordered_set<AnfNodePtr> eqv_keys;
  (void)std::transform(std::begin(eqv), std::end(eqv), std::inserter(eqv_keys, eqv_keys.end()),
                       [](const std::pair<AnfNodePtr, AnfNodePtr> &elem) -> AnfNodePtr { return elem.first; });
  auto outputs = GetOutput(lst, lst[0]->func_graph()->manager()->node_users(), eqv_keys);
  AnfNodePtr fg_output;
  if (outputs.size() > 1) {
    std::vector<AnfNodePtr> output_args;
    output_args.push_back(NewValueNode(prim::kPrimMakeTuple));
    (void)std::transform(std::begin(outputs), std::end(outputs), std::back_inserter(output_args),
                         [&eqv](const AnfNodePtr &o) -> AnfNodePtr { return eqv[o]; });
    // Set output for AnfGraph
    fg_output = fg->NewCNode(output_args);
  } else {
    fg_output = eqv[outputs[0]];
  }
  fg->set_output(fg_output);
  return std::make_tuple(fg, inputs, outputs);
}

// Converts the list of nodes to a runnable form.
// All the nodes in the list must represent linear flow (no calls, branches, ...)
// Returns:
//  (fn, inputs, outputs):
//  - fn: A callable function
//  - inputs: the list of inputs nodes whose values should be
//             provided to the function
//  - outputs: the list of output nodes corresponding to the
//             outputs of the function
// Notes:
//   This implementation will convert the nodes into a subgraph
//   that will run using the MsVM.
template <typename T>
LinConvertResult Convert(const GraphSegmentPtr &segment, const std::string &) {
  MS_EXCEPTION_IF_NULL(segment);
  LinConvertResult result;

  FuncGraphPtr fg = nullptr;
  AnfNodePtrList inputs;
  AnfNodePtrList outputs;

  std::tie(fg, inputs, outputs) = TransformSegmentToAnfGraph(segment->nodes_);

  // Clone in case g contains subgraphs that have a different manager
  fg = BasicClone(fg);

  std::shared_ptr<VMImpl> vm = std::make_shared<T>();

  result.run =
    std::make_shared<RunFunc>([fg, vm](const VectorRef &args) -> VectorRef { return vm->RunGraph(fg, args); });
  result.inputs = inputs;
  result.outputs = outputs;
  result.graph_id = UINT32_MAX;

  return result;
}

LinkFuncType MsVmConvert = Convert<VM>;

std::set<std::string> backend_list = {
  kMsConvert,
  kMsVm,
};
}  // namespace compile
}  // namespace mindspore
