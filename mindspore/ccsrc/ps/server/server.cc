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

#include "ps/server/server.h"
#include <memory>
#include <string>
#include <csignal>
#include "ps/server/round.h"
#include "ps/server/model_store.h"
#include "ps/server/iteration.h"
#include "ps/server/collective_ops_impl.h"
#include "ps/server/distributed_metadata_store.h"
#include "ps/server/distributed_count_service.h"
#include "ps/server/kernel/round/round_kernel_factory.h"

namespace mindspore {
namespace ps {
namespace server {
static std::vector<std::shared_ptr<core::CommunicatorBase>> global_worker_server_comms = {};
// This function is for the exit of server process when an interrupt signal is captured.
void SignalHandler(int signal) {
  MS_LOG(INFO) << "Interrupt signal captured: " << signal;
  std::for_each(global_worker_server_comms.begin(), global_worker_server_comms.end(),
                [](const std::shared_ptr<core::CommunicatorBase> &communicator) { communicator->Stop(); });
  return;
}

void Server::Initialize(bool use_tcp, bool use_http, uint16_t http_port, const std::vector<RoundConfig> &rounds_config,
                        const FuncGraphPtr &func_graph, size_t executor_threshold) {
  MS_EXCEPTION_IF_NULL(func_graph);
  func_graph_ = func_graph;

  if (rounds_config.empty()) {
    MS_LOG(EXCEPTION) << "Rounds are empty.";
    return;
  }
  rounds_config_ = rounds_config;

  use_tcp_ = use_tcp;
  use_http_ = use_http;
  http_port_ = http_port;
  executor_threshold_ = executor_threshold;
  return;
}

// Each step of the server pipeline may have dependency on other steps, which includes:

// InitServerContext must be the first step to set contexts for later steps.

// Server Running relies on URL or Message Type Register:
// StartCommunicator---->InitIteration

// Metadata Register relies on Hash Ring of Servers which relies on Network Building Completion:
// RegisterRoundKernel---->StartCommunicator

// Kernel Initialization relies on Executor Initialization:
// RegisterRoundKernel---->InitExecutor

// Getting Model Size relies on ModelStorage Initialization which relies on Executor Initialization:
// InitCipher---->InitExecutor
void Server::Run() {
  signal(SIGINT, SignalHandler);
  InitServerContext();
  InitCluster();
  InitIteration();
  StartCommunicator();
  InitExecutor();
  RegisterRoundKernel();
  MS_LOG(INFO) << "Server started successfully.";

  // Wait communicators to stop so the main thread is blocked.
  std::for_each(communicators_with_worker_.begin(), communicators_with_worker_.end(),
                [](const std::shared_ptr<core::CommunicatorBase> &communicator) { communicator->Join(); });
  communicator_with_server_->Join();
  MsException::Instance().CheckException();
  return;
}

void Server::InitServerContext() {
  PSContext::instance()->GenerateResetterRound();
  scheduler_ip_ = PSContext::instance()->scheduler_host();
  scheduler_port_ = PSContext::instance()->scheduler_port();
  worker_num_ = PSContext::instance()->initial_worker_num();
  server_num_ = PSContext::instance()->initial_server_num();
  return;
}

void Server::InitCluster() {
  server_node_ = std::make_shared<core::ServerNode>();
  MS_EXCEPTION_IF_NULL(server_node_);
  task_executor_ = std::make_shared<core::TaskExecutor>(32);
  MS_EXCEPTION_IF_NULL(task_executor_);
  if (!InitCommunicatorWithServer()) {
    MS_LOG(EXCEPTION) << "Initializing cross-server communicator failed.";
    return;
  }
  if (!InitCommunicatorWithWorker()) {
    MS_LOG(EXCEPTION) << "Initializing worker-server communicator failed.";
    return;
  }
  global_worker_server_comms = communicators_with_worker_;
  return;
}

bool Server::InitCommunicatorWithServer() {
  MS_EXCEPTION_IF_NULL(task_executor_);
  MS_EXCEPTION_IF_NULL(server_node_);

  communicator_with_server_ =
    server_node_->GetOrCreateTcpComm(scheduler_ip_, scheduler_port_, worker_num_, server_num_, task_executor_);
  MS_EXCEPTION_IF_NULL(communicator_with_server_);

  // Set exception event callbacks for server.
  auto tcp_comm = std::dynamic_pointer_cast<core::TcpCommunicator>(communicator_with_server_);
  MS_EXCEPTION_IF_NULL(tcp_comm);

  tcp_comm->RegisterEventCallback(core::ClusterEvent::CLUSTER_TIMEOUT, [&]() {
    MS_LOG(ERROR) << "Event CLUSTER_TIMEOUT is captured. This is because some nodes(Scheduler/Server/Worker) are not "
                     "started during network building phase.";
    std::for_each(communicators_with_worker_.begin(), communicators_with_worker_.end(),
                  [](const std::shared_ptr<core::CommunicatorBase> &communicator) { communicator->Stop(); });
    communicator_with_server_->Stop();
  });

  tcp_comm->RegisterEventCallback(core::ClusterEvent::SCHEDULER_TIMEOUT, [&]() {
    MS_LOG(ERROR) << "Event SCHEDULER_TIMEOUT is captured. This is because scheduler node is finalized or crashed.";
    std::for_each(communicators_with_worker_.begin(), communicators_with_worker_.end(),
                  [](const std::shared_ptr<core::CommunicatorBase> &communicator) { communicator->Stop(); });
    communicator_with_server_->Stop();
  });

  tcp_comm->RegisterEventCallback(core::ClusterEvent::NODE_TIMEOUT, [&]() {
    MS_LOG(ERROR)
      << "Event NODE_TIMEOUT is captured. This is because some server nodes are finalized or crashed after the "
         "network building phase.";
    std::for_each(communicators_with_worker_.begin(), communicators_with_worker_.end(),
                  [](const std::shared_ptr<core::CommunicatorBase> &communicator) { communicator->Stop(); });
    communicator_with_server_->Stop();
  });
  return true;
}

bool Server::InitCommunicatorWithWorker() {
  MS_EXCEPTION_IF_NULL(server_node_);
  MS_EXCEPTION_IF_NULL(task_executor_);
  if (!use_tcp_ && !use_http_) {
    MS_LOG(EXCEPTION) << "At least one type of protocol should be set.";
    return false;
  }
  if (use_tcp_) {
    auto tcp_comm = communicator_with_server_;
    MS_EXCEPTION_IF_NULL(tcp_comm);
    communicators_with_worker_.push_back(tcp_comm);
  }
  if (use_http_) {
    auto http_comm = server_node_->GetOrCreateHttpComm("0.0.0.0", http_port_, task_executor_);
    MS_EXCEPTION_IF_NULL(http_comm);
    communicators_with_worker_.push_back(http_comm);
  }
  return true;
}

void Server::InitIteration() {
  iteration_ = &Iteration::GetInstance();
  MS_EXCEPTION_IF_NULL(iteration_);

  // 1.Add rounds to the iteration according to the server mode.
  for (const RoundConfig &config : rounds_config_) {
    std::shared_ptr<Round> round = std::make_shared<Round>(config.name, config.check_timeout, config.time_window,
                                                           config.check_count, config.threshold_count);
    MS_LOG(INFO) << "Add round " << config.name << ", check_timeout: " << config.check_timeout
                 << ", time window: " << config.time_window << ", check_count: " << config.check_count
                 << ", threshold: " << config.threshold_count;
    iteration_->AddRound(round);
  }

  // 2.Initialize all the rounds.
  TimeOutCb time_out_cb = std::bind(&Iteration::ProceedToNextIter, iteration_, std::placeholders::_1);
  FinishIterCb finish_iter_cb = std::bind(&Iteration::ProceedToNextIter, iteration_, std::placeholders::_1);
  iteration_->InitRounds(communicators_with_worker_, time_out_cb, finish_iter_cb);
  return;
}

void Server::InitExecutor() {
  if (executor_threshold_ == 0) {
    MS_LOG(EXCEPTION) << "The executor's threshold should greater than 0.";
    return;
  }
  // The train engine instance is used in both push-type and pull-type kernels,
  // so the required_cnt of these kernels must be the same as update_model_threshold_.
  MS_LOG(INFO) << "Required count for push-type and pull-type kernels is " << executor_threshold_;
  Executor::GetInstance().Initialize(func_graph_, executor_threshold_);
  ModelStore::GetInstance().Initialize();
  return;
}

void Server::RegisterRoundKernel() {
  MS_EXCEPTION_IF_NULL(iteration_);
  auto &rounds = iteration_->rounds();
  if (rounds.empty()) {
    MS_LOG(EXCEPTION) << "Server has no round registered.";
    return;
  }

  for (auto &round : rounds) {
    const std::string &name = round->name();
    std::shared_ptr<kernel::RoundKernel> round_kernel = kernel::RoundKernelFactory::GetInstance().Create(name);
    if (round_kernel == nullptr) {
      MS_LOG(EXCEPTION) << "Round kernel for round " << name << " is not registered.";
      return;
    }

    // For some round kernels, the threshold count should be set.
    round_kernel->InitKernel(round->threshold_count());
    round->BindRoundKernel(round_kernel);
  }
  return;
}

void Server::StartCommunicator() {
  MS_EXCEPTION_IF_NULL(communicator_with_server_);
  if (communicators_with_worker_.empty()) {
    MS_LOG(EXCEPTION) << "Communicators for communication with worker is empty.";
    return;
  }

  MS_LOG(INFO) << "Start communicator with server.";
  communicator_with_server_->Start();
  DistributedMetadataStore::GetInstance().Initialize(server_node_);
  CollectiveOpsImpl::GetInstance().Initialize(server_node_);
  DistributedCountService::GetInstance().Initialize(server_node_, kLeaderServerRank);

  MS_LOG(INFO) << "Start communicator with worker.";
  std::for_each(communicators_with_worker_.begin(), communicators_with_worker_.end(),
                [](const std::shared_ptr<core::CommunicatorBase> &communicator) { communicator->Start(); });
}
}  // namespace server
}  // namespace ps
}  // namespace mindspore
