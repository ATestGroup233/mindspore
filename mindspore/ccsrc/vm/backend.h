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
#ifndef MINDSPORE_CCSRC_VM_BACKEND_H_
#define MINDSPORE_CCSRC_VM_BACKEND_H_

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utils/contract.h"
#include "ir/anf.h"
#include "vm/segment_runner.h"
#include "vm/graph_partition.h"
#include "vm/vm.h"
#include "backend/session/session_basic.h"
#include "runtime/hardware/device_context.h"
#include "runtime/framework/graph_scheduler.h"

namespace mindspore {
namespace compile {
using OpRunInfo = session::OpRunInfo;
using DeviceContext = device::DeviceContext;
using ActorInfo = runtime::ActorInfo;
using GraphCompilerInfo = runtime::GraphCompilerInfo;
using ControlNodeParser = runtime::ControlNodeParser;
using FrontToBackendNodeWithContext = runtime::FrontToBackendNodeWithContext;
enum SwitchCondStatus {
  kCondOk = 0,
  kCondAlreadyRun,
};

class Backend {
 public:
  explicit Backend(const std::string &name);

  virtual ~Backend() = default;

  LinkFuncType convert_fn() { return convert_fn_; }
  std::string name() { return name_; }
  virtual bool GetCond(const BaseRef &c, bool *value);
  virtual bool GetIndex(const BaseRef &c, int64_t *value);
  virtual GraphId CompileGraph(NotNull<FuncGraphPtr> fg) { return kInvalidGraphId; }
  virtual void Link(GraphId) {}
  virtual void SetDebugger() {}

  bool is_multi_graph_sink() const { return is_multi_graph_sink_; }
  void set_is_multi_graph_sink(bool flag) { is_multi_graph_sink_ = flag; }

 protected:
  std::string name_;
  LinkFuncType convert_fn_;
  bool is_multi_graph_sink_;
};

class MsBackend : public Backend {
 public:
  MsBackend(const std::string &name, const std::string &target, uint32_t device_id);
  ~MsBackend() override = default;

  LinConvertResult MsConvert(const GraphSegmentPtr &segment, const std::string &target = "");
  VectorRef MsRunGraph(const GraphId &g, const VectorRef &args, const std::string &target = "");

  VectorRef MsSimuRunGraph(const GraphId &g, const VectorRef &args);
  void Link(GraphId) override;
  GraphId CompileGraph(NotNull<FuncGraphPtr> fg) override;
  VectorRef RunGraph(GraphId graph_id, const VectorRef &args);
  void ClearSessionGraphs();
  void CreateOtherSession(const std::string &target);

#ifdef ENABLE_DEBUGGER
  void SetDebugger() override;
#endif

 private:
  session::SessionPtr target_sess_;
  session::SessionPtr other_sess_;
  std::string target_device_;
  std::string other_device_;
  std::unordered_map<GraphId, LinConvertResult> graph_id_map_;
};

class MindRTBackend : public Backend {
 public:
  MindRTBackend(const std::string &backend_name, const std::string &device_name, uint32_t device_id);
  ~MindRTBackend() override = default;

  // The parameter root_graph is a root graph, and the root graph maybe contain multiple sub graphs, It will traverse
  // all sub graphs to call CompileGraph.
  const ActorInfo &CompileGraphs(const FuncGraphPtr &root_graph);

  // Compile single op kernel graph in the pyNative mode.
  const ActorInfo &CompileGraph(const OpRunInfo &op_run_info, const GraphInfo &graph_info,
                                const std::vector<int64_t> *tensors_mask,
                                std::vector<tensor::TensorPtr> *input_tensors);

  // Run Graph in the graph mode.
  VectorRef RunGraph(const ActorInfo &actor_info, const VectorRef &args);

  // Run Graph in the pyNative mode.
  VectorRef RunGraph(const ActorInfo &actor_info, const std::vector<int64_t> *tensors_mask,
                     const std::vector<tensor::TensorPtr> *input_tensors);

 private:
  // The parameter func_graph is a graph, it can be either a root graph or a sub graph,
  // The result of graph compiler is stored in graph_id_to_device_context_ and control_nodes_.
  void CompileGraph(const FuncGraphPtr &func_graph);

  // Construct the GraphCompilerInfo by the compilation results of graph, used in Graph mode.
  std::unique_ptr<GraphCompilerInfo> ConstructGraphCompilerInfo(const FuncGraphPtr &root_graph);

  // Construct the GraphCompilerInfo by the compilation results of graph, used in PyNative mode.
  std::unique_ptr<GraphCompilerInfo> ConstructGraphCompilerInfo(const std::vector<int64_t> *tensors_mask,
                                                                const std::vector<tensor::TensorPtr> *input_tensors);

  // Split complete kernel graph to single op graph in PyNative back
  // propagation, then compile and run single op graph.
  void RunGraphBySingleOp(const std::vector<KernelGraphPtr> &graphs,
                          const std::vector<std::vector<tensor::TensorPtr>> &inputs, VectorRef *outputs);

  // When compiling FuncGraph, it is divided according to the control nodes, and obtain the control nodes and several
  // node segments. Node segments will be compiled into kernelGraphs which are expressed as GraphId and bound to
  // the corresponding device_context.
  std::unordered_map<GraphId, DeviceContext *> graph_id_to_device_context_;
  std::unordered_map<GraphInfo, DeviceContext *> graph_info_to_device_context_;
  std::vector<AnfNodePtr> control_nodes_;

  std::unordered_map<ActorInfo, std::unique_ptr<GraphCompilerInfo>> actor_to_graph_compiler_info_;

  GraphPartitionPtr graph_partition_;
  std::string device_name_;
  uint32_t device_id_;
};
}  // namespace compile
}  // namespace mindspore
#endif
