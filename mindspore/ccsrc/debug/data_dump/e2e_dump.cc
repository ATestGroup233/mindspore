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

#include "debug/data_dump/e2e_dump.h"

#include <algorithm>
#include <map>
#include <vector>

#include "debug/data_dump/dump_json_parser.h"
#include "common/trans.h"
#include "debug/common.h"
#include "backend/session/anf_runtime_algorithm.h"
#include "utils/ms_context.h"
#include "runtime/device/kernel_runtime_manager.h"
#ifdef ENABLE_DEBUGGER
#include "debug/debug_services.h"
#include "debug/tensor_load.h"
#include "debug/debugger/debugger.h"
#endif

namespace mindspore {
bool E2eDump::IsDeviceTargetGPU() {
  auto context = MsContext::GetInstance();
  MS_EXCEPTION_IF_NULL(context);
  return context->get_param<std::string>(MS_CTX_DEVICE_TARGET) == kGPUDevice;
}

void E2eDump::DumpGPUMemToFile(const std::string &file_path, const std::string &original_kernel_name,
                               NotNull<const device::DeviceAddress *> addr, const ShapeVector &int_shapes,
                               const TypeId &host_type, const TypeId &device_type, bool trans_flag, size_t slot,
                               const Debugger *debugger) {
#ifdef ENABLE_DEBUGGER
  auto format = kOpFormat_DEFAULT;
  MS_EXCEPTION_IF_NULL(debugger);
  auto ret = debugger->DumpTensorToFile(original_kernel_name, trans_flag, file_path, format, int_shapes, host_type,
                                        device_type, addr->format(), slot);
  if (!ret) {
    MS_LOG(ERROR) << "DumpTensorToFile Failed: flag:" << std::to_string(trans_flag) << ", path:" << file_path
                  << ", host_format:" << format;
  }
#endif
}

void E2eDump::DumpOutput(const session::KernelGraph *graph, const std::string &dump_path, const Debugger *debugger) {
  MS_EXCEPTION_IF_NULL(graph);
  auto &dump_json_parser = DumpJsonParser::GetInstance();
  if (!dump_json_parser.OutputNeedDump()) {
    return;
  }
  MS_LOG(INFO) << "Start e2e dump output";
  bool trans_flag = dump_json_parser.trans_flag();
  const auto &apply_kernels = graph->execution_order();
  for (const auto &node : apply_kernels) {
    MS_EXCEPTION_IF_NULL(node);
    std::string kernel_name = node->fullname_with_scope();
    if (!dump_json_parser.NeedDump(kernel_name)) {
      continue;
    }
    DumpJsonParser::GetInstance().MatchKernel(kernel_name);
    DumpOutputImpl(node, trans_flag, dump_path, &kernel_name, debugger);
  }
}

void E2eDump::DumpOutputImpl(const CNodePtr &node, bool trans_flag, const std::string &dump_path,
                             std::string *kernel_name, const Debugger *debugger) {
  MS_EXCEPTION_IF_NULL(node);
  GetFileKernelName(NOT_NULL(kernel_name));
  auto output_size = AnfAlgo::GetOutputTensorNum(node);
  for (size_t j = 0; j < output_size; ++j) {
    if (!AnfAlgo::OutputAddrExist(node, j)) {
      continue;
    }
    auto addr = AnfAlgo::GetOutputAddr(node, j);
    ShapeVector int_shapes;
    GetDumpIntShape(node, j, NOT_NULL(&int_shapes), trans_flag);
    auto type = AnfAlgo::GetOutputInferDataType(node, j);
    auto device_type = AnfAlgo::GetOutputDeviceDataType(node, j);
    std::string op_type = AnfAlgo::GetCNodeName(node);
    uint32_t task_id = 0;
    uint32_t stream_id = 0;
    uint64_t timestamp = GetTimeStamp();
    std::string file_path = dump_path + '/' + op_type + '.' + *kernel_name + '.' + std::to_string(task_id) + '.' +
                            std::to_string(stream_id) + '.' + std::to_string(timestamp) + ".output." +
                            std::to_string(j);
    if (IsDeviceTargetGPU()) {
      DumpGPUMemToFile(file_path, node->fullname_with_scope(), NOT_NULL(addr), int_shapes, type, device_type,
                       trans_flag, j, debugger);
    } else {
      DumpMemToFile(file_path, NOT_NULL(addr), int_shapes, type, trans_flag);
    }
  }
}

uint64_t E2eDump::GetTimeStamp() {
  auto cur_sys_time = std::chrono::system_clock::now();
  uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(cur_sys_time.time_since_epoch()).count();
  return timestamp;
}

void E2eDump::DumpInput(const session::KernelGraph *graph, const std::string &dump_path, const Debugger *debugger) {
  MS_EXCEPTION_IF_NULL(graph);
  auto &dump_json_parser = DumpJsonParser::GetInstance();
  if (!dump_json_parser.InputNeedDump()) {
    return;
  }
  MS_LOG(INFO) << "Start e2e dump input";
  bool trans_flag = dump_json_parser.trans_flag();
  const auto &apply_kernels = graph->execution_order();
  for (const auto &node : apply_kernels) {
    MS_EXCEPTION_IF_NULL(node);
    std::string kernel_name = node->fullname_with_scope();
    if (!dump_json_parser.NeedDump(kernel_name)) {
      continue;
    }
    DumpJsonParser::GetInstance().MatchKernel(kernel_name);
    DumpInputImpl(node, trans_flag, dump_path, &kernel_name, debugger);
  }
}

void E2eDump::DumpInputImpl(const CNodePtr &node, bool trans_flag, const std::string &dump_path,
                            std::string *kernel_name, const Debugger *debugger) {
  MS_EXCEPTION_IF_NULL(node);
  GetFileKernelName(NOT_NULL(kernel_name));
  auto input_size = AnfAlgo::GetInputTensorNum(node);
  for (size_t j = 0; j < input_size; ++j) {
    auto kernel_with_index = AnfAlgo::GetPrevNodeOutput(node, j);
    auto input = kernel_with_index.first;
    auto index = kernel_with_index.second;
    if (!AnfAlgo::OutputAddrExist(input, index)) {
      continue;
    }
    auto addr = AnfAlgo::GetOutputAddr(input, index);

    std::string tensor_name;
    size_t slot;
    if (IsDeviceTargetGPU()) {
      auto input_kernel = node->input(j + 1);
      std::string input_kernel_name = input_kernel->fullname_with_scope();
      tensor_name = input_kernel_name;
      slot = 0;
    } else {
      tensor_name = node->fullname_with_scope();
      slot = j;
    }
    ShapeVector int_shapes;
    GetDumpIntShape(input, index, NOT_NULL(&int_shapes), trans_flag);
    auto type = AnfAlgo::GetOutputInferDataType(input, index);
    auto device_type = AnfAlgo::GetOutputDeviceDataType(input, index);
    std::string op_type = AnfAlgo::GetCNodeName(node);
    uint64_t timestamp = GetTimeStamp();
    uint32_t task_id = 0;
    uint32_t stream_id = 0;
    std::string file_path = dump_path + '/' + op_type + '.' + *kernel_name + '.' + std::to_string(task_id) + '.' +
                            std::to_string(stream_id) + '.' + std::to_string(timestamp) + ".input." + std::to_string(j);
    if (IsDeviceTargetGPU()) {
      DumpGPUMemToFile(file_path, tensor_name, NOT_NULL(addr), int_shapes, type, device_type, trans_flag, slot,
                       debugger);
    } else {
      DumpMemToFile(file_path, NOT_NULL(addr), int_shapes, type, trans_flag);
    }
  }
}

void E2eDump::DumpSingleAnfNode(const AnfNodePtr &anf_node, const size_t output_index, const std::string &dump_path,
                                bool trans_flag, std::map<std::string, size_t> *const_map, const Debugger *debugger) {
  MS_EXCEPTION_IF_NULL(anf_node);
  auto &dump_json_parser = DumpJsonParser::GetInstance();
  if (!anf_node->isa<Parameter>() && !anf_node->isa<ValueNode>()) {
    return;
  }
  std::string node_name = anf_node->fullname_with_scope();
  std::string dump_name = node_name;
  if (anf_node->isa<ValueNode>()) {
    auto iter = const_map->find(node_name);
    if (iter == const_map->end()) {
      return;
    }
    dump_name = std::string("cst") + std::to_string(iter->second);
  }

  if (!dump_json_parser.NeedDump(node_name)) {
    return;
  }
  DumpJsonParser::GetInstance().MatchKernel(node_name);
  GetFileKernelName(NOT_NULL(&node_name));
  // check if output address exists, if not, return;
  if (!AnfAlgo::OutputAddrExist(anf_node, output_index)) {
    return;
  }
  auto addr = AnfAlgo::GetOutputAddr(anf_node, output_index);
  MS_EXCEPTION_IF_NULL(addr);
  ShapeVector int_shapes;
  GetDumpIntShape(anf_node, output_index, NOT_NULL(&int_shapes), trans_flag);
  auto type = AnfAlgo::GetOutputInferDataType(anf_node, output_index);
  auto device_type = AnfAlgo::GetOutputDeviceDataType(anf_node, output_index);
  std::string file_path = dump_path + '/' + dump_name + "_output_0";
  if (IsDeviceTargetGPU()) {
    DumpGPUMemToFile(file_path, node_name, NOT_NULL(addr), int_shapes, type, device_type, trans_flag, 0, debugger);
  } else {
    DumpMemToFile(file_path, NOT_NULL(addr), int_shapes, type, trans_flag);
  }
}

void E2eDump::DumpParametersAndConst(const session::KernelGraph *graph, const std::string &dump_path,
                                     const Debugger *debugger) {
  MS_EXCEPTION_IF_NULL(graph);
  auto &dump_json_parser = DumpJsonParser::GetInstance();
  MS_LOG(INFO) << "Start e2e dump parameters and Const values";
  bool trans_flag = dump_json_parser.trans_flag();
  std::map<std::string, size_t> const_map;
  GetConstantId(graph, &const_map);

  // dump parameters
  const auto &parameters = graph->inputs();
  for (auto &item : parameters) {
    DumpSingleAnfNode(item, PARAMETER_OUTPUT_INDEX, dump_path, trans_flag, &const_map, debugger);
  }
  // dump const values
  auto value_nodes = graph->graph_value_nodes();
  for (const auto &value_node : value_nodes) {
    DumpSingleAnfNode(value_node, VALUE_NODE_OUTPUT_INDEX, dump_path, trans_flag, &const_map, debugger);
  }
}

void E2eDump::DumpSetup(const session::KernelGraph *graph, uint32_t device_id) {
  auto &dump_json_parser = DumpJsonParser::GetInstance();
  uint32_t cur_iter = dump_json_parser.cur_dump_iter();
  if (dump_json_parser.AsyncDumpEnabled() && dump_json_parser.IsDumpIter(cur_iter)) {
    auto zero_dir_dump_path =
      dump_json_parser.path() + "/rank_" + std::to_string(device_id) + "/_/" + std::to_string(graph->graph_id()) + "/0";

    auto root_cur_iter_dump_path = dump_json_parser.path() + "/rank_" + std::to_string(device_id) + "/" +
                                   dump_json_parser.net_name() + "/" + std::to_string(graph->graph_id());

    auto cur_iter_dump_path = root_cur_iter_dump_path + "/" + std::to_string(cur_iter);

    MS_LOG(INFO) << "zero_dir_dump_path: " << zero_dir_dump_path;
    MS_LOG(INFO) << "root_cur_iter_dump_path: " << root_cur_iter_dump_path;
    MS_LOG(INFO) << "cur_iter_dump_path: " << cur_iter_dump_path;

    // create cur_iter_dump_path dirs
    bool status = Common::CreateNotExistDirs(root_cur_iter_dump_path);
    if (!status) {
      MS_LOG(EXCEPTION) << "Failed at CreateNotExistDirs for " << root_cur_iter_dump_path;
      return;
    }

    // test if cur_iter_dump_path dir already exists
    std::string command = "test -d " + cur_iter_dump_path;
    MS_LOG(INFO) << "test command: " << command;
    if (system(command.c_str())) {
      // create symlink to active dump dir for the iteration in final dump dir
      command = "ln -fs " + zero_dir_dump_path + " " + cur_iter_dump_path;
      MS_LOG(INFO) << "ln command: " << command;
      if (system(command.c_str())) {
        MS_LOG(EXCEPTION) << "did not create symlink to active dump dir for the iteration in final dump dir.";
      }
    } else {
      MS_LOG(INFO) << "final dump dir already exists";
    }
  }
}

bool E2eDump::DumpData(const session::KernelGraph *graph, uint32_t device_id, const Debugger *debugger) {
  MS_EXCEPTION_IF_NULL(graph);
  auto &dump_json_parser = DumpJsonParser::GetInstance();
  uint32_t graph_id = graph->graph_id();
  if (starting_graph_id == INT32_MAX) {
    starting_graph_id = graph_id;
  }
  if (starting_graph_id == graph_id) {
    dump_json_parser.UpdateDumpIter();
  }

  if (dump_json_parser.GetIterDumpFlag()) {
    MS_LOG(INFO) << "Start e2e dump. Current iteration is " << dump_json_parser.cur_dump_iter();
    MS_LOG(INFO) << "Current graph id is " << graph_id;
    std::string dump_path = GenerateDumpPath(graph_id, &device_id);

    DumpInput(graph, dump_path, debugger);
    DumpOutput(graph, dump_path, debugger);
    DumpParametersAndConst(graph, dump_path, debugger);
    return true;
  } else if (dump_json_parser.AsyncDumpEnabled()) {
    uint32_t prev_dump_iter = dump_json_parser.cur_dump_iter() - 1;

    auto zero_dir_dump_path =
      dump_json_parser.path() + "/rank_" + std::to_string(device_id) + "/_/" + std::to_string(graph->graph_id()) + "/0";

    auto cur_iter_dump_path = dump_json_parser.path() + "/rank_" + std::to_string(device_id) + "/" +
                              dump_json_parser.net_name() + "/" + std::to_string(graph->graph_id()) + "/" +
                              std::to_string(prev_dump_iter);

    MS_LOG(INFO) << "zero_dir_dump_path: " << zero_dir_dump_path;
    MS_LOG(INFO) << "cur_iter_dump_path: " << cur_iter_dump_path;

    if (dump_json_parser.IsDumpIter(prev_dump_iter)) {
      // remove symlink to active dump dir
      std::string command = "rm -f " + cur_iter_dump_path;
      MS_LOG(INFO) << "rm command: " << command;
      if (system(command.c_str())) {
        MS_LOG(WARNING) << "did not remove symlink to active dump dir, likely an actual dir";
      }

      // create actual dir for iteration in final dump dir
      bool status = Common::CreateNotExistDirs(cur_iter_dump_path);
      if (!status) {
        MS_LOG(EXCEPTION) << "failed at CreateNotExistDirs for " << cur_iter_dump_path;
      }

      // move contents from active dump dir to final dump dir
      command = "mv " + zero_dir_dump_path + "/* " + cur_iter_dump_path + "/.";
      MS_LOG(INFO) << "mv command: " << command;
      if (system(command.c_str())) {
        MS_LOG(EXCEPTION) << "Ascend runtime has changed the dump dir structure!!!";
      }
    } else {
      // delete contents from active dump dir
      std::string command = "rm -f " + zero_dir_dump_path + "/*";
      MS_LOG(INFO) << "rm command: " << command;
      if (system(command.c_str())) {
        MS_LOG(EXCEPTION) << "Ascend runtime has changed the dump dir structure!!!";
      }
    }

    return true;
  }
  return false;
}
}  // namespace mindspore
