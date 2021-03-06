/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/common_runtime/eager/context.h"

#include <vector>

// clang-format off
// Required for IS_MOBILE_PLATFORM
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/process_function_library_runtime.h"
#include "tensorflow/core/lib/gtl/map_util.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/platform.h"
// clang-format on

#include "tensorflow/core/common_runtime/collective_executor_mgr.h"
#include "tensorflow/core/common_runtime/collective_param_resolver_local.h"
#include "tensorflow/core/common_runtime/device_resolver_local.h"
#include "tensorflow/core/common_runtime/device_set.h"
#include "tensorflow/core/common_runtime/process_util.h"
#include "tensorflow/core/framework/graph_def_util.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/public/version.h"
#include "tensorflow/core/util/device_name_utils.h"
#if !defined(IS_MOBILE_PLATFORM)
#include "tensorflow/core/distributed_runtime/cluster_function_library_runtime.h"
#include "tensorflow/core/distributed_runtime/collective_param_resolver_distributed.h"
#include "tensorflow/core/distributed_runtime/device_resolver_distributed.h"
#include "tensorflow/core/distributed_runtime/rpc_collective_executor_mgr.h"
#endif  // !IS_MOBILE_PLATFORM
#include "tensorflow/core/framework/resource_mgr.h"
#include "tensorflow/core/lib/core/blocking_counter.h"
#include "tensorflow/core/lib/monitoring/gauge.h"
#include "tensorflow/core/platform/monitoring.h"
#include "tensorflow/core/util/env_var.h"

namespace tensorflow {
namespace {

bool ReadBoolFromEnvVar(StringPiece env_var_name, bool default_val) {
  bool val;
  if (tensorflow::ReadBoolFromEnvVar(env_var_name, default_val, &val).ok()) {
    return val;
  }
  return default_val;
}

auto* eager_context_created =
    monitoring::Gauge<bool, 0>::New("/tensorflow/core/eager_context_created",
                                    "True if an eager context was created.");

}  // namespace

EagerContext::EagerContext(
    const SessionOptions& opts,
    ContextDevicePlacementPolicy default_device_placement_policy,
    ContextMirroringPolicy default_mirroring_policy, bool async,
    const DeviceMgr* device_mgr, bool device_mgr_owned, Rendezvous* rendezvous,
    const CustomKernelCreator* custom_kernel_creator,
    DistributedFunctionLibraryRuntime* cluster_flr)
    : default_device_placement_policy_(default_device_placement_policy),
      default_mirroring_policy_(default_mirroring_policy),
      devices_(device_mgr->ListDevices()),
      rendezvous_(rendezvous),
      thread_pool_(NewThreadPoolFromSessionOptions(opts)),
      custom_kernel_creator_(custom_kernel_creator),
      pflr_(new ProcessFunctionLibraryRuntime(
          device_mgr, opts.env, TF_GRAPH_DEF_VERSION, &func_lib_def_,
          opts.config.graph_options().optimizer_options(), thread_pool_.get(),
          cluster_flr, custom_kernel_creator_)),
      log_device_placement_(opts.config.log_device_placement()),
      allow_soft_placement_(opts.config.allow_soft_placement()),
      num_active_steps_(0),
      default_executor_(async),
      log_memory_(LogMemory::IsEnabled()),
      env_(opts.env),
      use_send_tensor_rpc_(false),
      pin_small_ops_to_cpu_(ReadBoolFromEnvVar(
          "TF_EAGER_ENABLE_SMALL_TENSOR_CPU_PINNING", false)) {
  // Starts exporting metrics through a platform-specific monitoring API (if
  // provided). For builds using "tensorflow/core/platform/default", this is
  // currently a no-op.
  eager_context_created->GetCell()->Set(true);
  monitoring::StartExporter();
  if (device_mgr_owned) {
    local_device_manager_.reset(device_mgr);
    local_unowned_device_manager_ = nullptr;
  } else {
    local_unowned_device_manager_ = device_mgr;
  }
  InitDeviceMapAndAsync();
  runner_ = [this](std::function<void()> closure) {
    this->thread_pool_->Schedule(std::move(closure));
  };

#if !defined(IS_MOBILE_PLATFORM)
  context_id_ = kInvalidContextId;
#endif  // IS_MOBILE_PLATFORM

  std::unique_ptr<DeviceResolverInterface> drl(
      new DeviceResolverLocal(local_device_mgr()));
  std::unique_ptr<ParamResolverInterface> cprl(new CollectiveParamResolverLocal(
      opts.config, local_device_mgr(), drl.get(),
      "/job:localhost/replica:0/task:0"));
  collective_executor_mgr_.reset(new CollectiveExecutorMgr(
      opts.config, local_device_mgr(), std::move(drl), std::move(cprl)));
}

void EagerContext::InitDeviceMapAndAsync() {
  for (auto* device : devices_) {
    devices_map_[device->name()] = device;
  }

  if (remote_device_mgr() != nullptr) {
    for (auto* device : remote_device_mgr()->ListDevices()) {
      if (devices_map_.find(device->name()) == devices_map_.end()) {
        devices_map_[device->name()] = device;
        devices_.push_back(device);
      }
    }
  }

  DeviceSet ds;
  for (Device* d : devices_) {
    ds.AddDevice(d);
  }
  prioritized_device_type_list_ = ds.PrioritizedDeviceTypeList();
}

EagerExecutor& EagerContext::Executor() {
  tf_shared_lock l(executor_map_mu_);
  return *gtl::FindWithDefault(thread_local_executor_,
                               std::this_thread::get_id(), &default_executor_);
}

void EagerContext::SetExecutorForThread(EagerExecutor* executor) {
  tensorflow::mutex_lock l(executor_map_mu_);
  if (executor == &default_executor_) {
    thread_local_executor_.erase(std::this_thread::get_id());
  } else {
    thread_local_executor_[std::this_thread::get_id()] = executor;
  }
}

void EagerContext::ClearCaches() {
  {
    mutex_lock ml(executor_map_mu_);
    for (auto& entry : thread_local_executor_) {
      entry.second->WaitForAllPendingNodes().IgnoreError();
    }
  }
  {
    // The executor stores pointers to kernels, so we need to make sure that no
    // async eager ops are still executing. We lock the cache during this time
    // as well.
    mutex_lock ml(cache_mu_);
    default_executor_.WaitForAllPendingNodes().IgnoreError();
    kernel_cache_.clear();
    for (auto& entry : registered_functions_) {
      entry.second->cached_kernel_keys->clear();
    }
  }
}

void EagerContext::SetThreadLocalDevicePlacementPolicy(
    ContextDevicePlacementPolicy policy) {
  mutex_lock ml(policy_map_mu_);
  device_placement_policy_[std::this_thread::get_id()] = policy;
}

ContextDevicePlacementPolicy EagerContext::GetDevicePlacementPolicy() const {
  tf_shared_lock l(policy_map_mu_);
  auto policy_map_it =
      device_placement_policy_.find(std::this_thread::get_id());
  if (policy_map_it != device_placement_policy_.end()) {
    return policy_map_it->second;
  }
  return default_device_placement_policy_;
}

void EagerContext::SetThreadLocalMirroringPolicy(
    ContextMirroringPolicy policy) {
  mutex_lock ml(policy_map_mu_);
  mirroring_policy_[std::this_thread::get_id()] = policy;
}

ContextMirroringPolicy EagerContext::GetMirroringPolicy() const {
  tf_shared_lock l(policy_map_mu_);
  auto policy_map_it = mirroring_policy_.find(std::this_thread::get_id());
  if (policy_map_it != mirroring_policy_.end()) {
    return policy_map_it->second;
  }
  return default_mirroring_policy_;
}

bool EagerContext::MirrorTensors() const {
  return GetMirroringPolicy() == MIRRORING_ALL;
}

#if !defined(IS_MOBILE_PLATFORM)
void EagerContext::CloseAndClearAllRemoteContexts() {
  CloseRemoteContexts(remote_contexts_);
  remote_contexts_.clear();
}

void EagerContext::CloseRemoteContexts(
    const std::vector<string>& remote_contexts) {
  // Close all remote contexts.
  eager::CloseContextRequest request;
  uint64 context_id;
  {
    mutex_lock l(remote_state_mu_);
    if (!is_master_) return;
    context_id = context_id_;
    context_id_ = kInvalidContextId;
  }
  request.set_context_id(context_id);
  // Setting context_id to a new value can avoid us issuing DestroyTensorHandle
  // request to closed remote workers.
  std::vector<eager::CloseContextResponse> responses(remote_contexts.size());
  BlockingCounter counter(static_cast<int>(remote_contexts.size()));

  int i = 0;
  for (const auto& worker : remote_contexts) {
    eager::EagerClient* client;
    Status s = remote_eager_workers_->GetClient(worker, &client);

    client->CloseContextAsync(
        &request, &responses[i],
        [&worker, &counter, context_id](const Status& s) {
          if (!s.ok()) {
            LOG(ERROR) << "Unable to close remote context with ID "
                       << context_id << " for worker: " << worker << " due to "
                       << s.error_message();
          }
          counter.DecrementCount();
        });
    i++;
  }

  counter.Wait();
}

#endif  // !IS_MOBILE_PLATFORM

void EagerContext::WaitForAndCloseRemoteContexts() {
  ClearCaches();

#if !defined(IS_MOBILE_PLATFORM)
  {
    mutex_lock l(keep_alive_thread_shutdown_mu_);
    shutting_down_ = true;
    keep_alive_thread_cv_.notify_all();
  }
  keep_alive_thread_.reset();

  if (!remote_contexts_.empty()) {
    CloseAndClearAllRemoteContexts();
  }

  {
    mutex_lock l(remote_state_mu_);

    default_executor_.ShutDown().IgnoreError();
    std::unordered_map<std::thread::id, EagerExecutor*> executors_copy;
    {
      mutex_lock l(executor_map_mu_);
      executors_copy = thread_local_executor_;
    }
    for (const auto& it : executors_copy) {
      it.second->ShutDown().IgnoreError();
    }
  }

  // This shuts down the completion queue and joins the thread polling it.
  // The thread exits only after the completion queue has been drained of all
  // the events. These events' completion should invoke all remaining RPC
  // callbacks.
  // This also deletes all EagerClient instances. There should not be any
  // references to EagerClients left after all RPCs and async ops have been
  // finished.
  remote_eager_workers_ = nullptr;
#endif  // !IS_MOBILE_PLATFORM
}

EagerContext::~EagerContext() {
  ClearCaches();
  for (auto& entry : registered_functions_) {
    while (!entry.second->Unref()) {
      // remove all references.
    }
  }
  registered_functions_.clear();

#if !defined(IS_MOBILE_PLATFORM)
  if (server_) {
    // TODO(b/136478427): Fix this.
    LOG(WARNING) << "Unable to destroy server_ object, so releasing instead. "
                    "Servers don't support clean shutdown.";
    server_.release();
  }

  {
    mutex_lock l(keep_alive_thread_shutdown_mu_);
    shutting_down_ = true;
    keep_alive_thread_cv_.notify_all();
  }
  keep_alive_thread_.reset();
  if (!remote_contexts_.empty()) {
    CloseAndClearAllRemoteContexts();
  }
#endif  // !IS_MOBILE_PLATFORM

  rendezvous_->Unref();
}

bool EagerContext::FindFunctionByName(const string& name) {
  return func_lib_def_.Find(name) != nullptr;
}

Status EagerContext::FindFunctionOpData(
    const string& name, const tensorflow::OpRegistrationData** op_data) {
  return func_lib_def_.LookUp(name, op_data);
}

const FunctionDef* EagerContext::FindFunctionDef(const string& name) {
  return func_lib_def_.Find(name);
}

std::vector<const FunctionDef*> EagerContext::ListRegisteredFunctions() {
  std::vector<const FunctionDef*> result;
  std::vector<string> function_names = func_lib_def_.ListFunctionNames();
  result.reserve(function_names.size());
  for (const string& fn : function_names) {
    result.emplace_back(func_lib_def_.Find(fn));
  }
  return result;
}

// TODO(gjn): Delete in favour of FindDeviceFromName
Status EagerContext::FindDeviceByName(const string& name,
                                      Device** result) const {
  auto it = devices_map_.find(name);
  if (it == devices_map_.end()) {
    return errors::InvalidArgument(name, " unknown device.");
  }
  *result = it->second;
  return Status::OK();
}

void EagerContext::ClearRunMetadata() { run_metadata_.Clear(); }

void EagerContext::StartStep() {
  mutex_lock ml(metadata_mu_);
  num_active_steps_++;
  if (step_container_ == nullptr) {
    step_container_.reset(
        new ScopedStepContainer(0, [this](const string& name) {
          auto local_devices = local_device_mgr()->ListDevices();
          for (Device* device : local_devices) {
            device->resource_manager()->Cleanup(name).IgnoreError();
          }
        }));
  }
}

void EagerContext::EndStep() {
  mutex_lock ml(metadata_mu_);
  num_active_steps_--;
  if (num_active_steps_ == 0) {
    step_container_.reset();
  }
}

ScopedStepContainer* EagerContext::StepContainer() {
  if (num_active_steps_.load() == 0) {
    return nullptr;
  }
  mutex_lock ml(metadata_mu_);
  return step_container_.get();
}

Status EagerContext::MaybeRegisterFunctionRemotely(const FunctionDef& fdef) {
  // Only client context can register function on remote worker context.
  if (remote_device_manager_ == nullptr) return Status::OK();
#if !defined(IS_MOBILE_PLATFORM)
  BlockingCounter blocking_counter(static_cast<int>(remote_contexts_.size()));

  eager::RegisterFunctionRequest request;
  request.set_context_id(GetContextId());
  *request.mutable_function_def() = fdef;
  StripDefaultAttributes(*OpRegistry::Global(),
                         request.mutable_function_def()->mutable_node_def());
  std::vector<eager::RegisterFunctionResponse> responses(
      remote_contexts_.size());
  std::vector<Status> statuses(remote_contexts_.size());

  int i = 0;
  for (const auto& target : remote_contexts_) {
    eager::EagerClient* eager_client;
    statuses[i] = remote_eager_workers_->GetClient(target, &eager_client);
    if (!statuses[i].ok()) {
      blocking_counter.DecrementCount();
      continue;
    }

    eager_client->RegisterFunctionAsync(
        &request, &responses[i],
        [i, &statuses, &blocking_counter](const Status& status) {
          statuses[i] = status;
          blocking_counter.DecrementCount();
        });

    i++;
  }
  blocking_counter.Wait();

  for (int i = 0; i < remote_contexts_.size(); i++) {
    TF_RETURN_IF_ERROR(statuses[i]);
  }
#endif  // !IS_MOBILE_PLATFORM
  return Status::OK();
}

Status EagerContext::RegisterExistingFunctionsOnRemoteWorkers(
    const std::vector<const FunctionDef*>& function_defs,
    const std::vector<string>& remote_workers) {
#if !defined(IS_MOBILE_PLATFORM)
  // Register multiple functions on selected remote workers.
  int num_requests = function_defs.size() * remote_workers.size();
  BlockingCounter counter(num_requests);
  std::vector<Status> statuses(num_requests);
  uint64 context_id = GetContextId();
  for (int i = 0; i < remote_workers.size(); i++) {
    eager::EagerClient* eager_client;
    Status s =
        remote_eager_workers_->GetClient(remote_workers[i], &eager_client);
    if (!s.ok()) {
      for (int j = 0; j < function_defs.size(); j++) {
        statuses[i * function_defs.size() + j] = s;
        counter.DecrementCount();
      }
      continue;
    }
    for (int j = 0; j < function_defs.size(); j++) {
      eager::RegisterFunctionRequest request;
      request.set_context_id(context_id);
      *request.mutable_function_def() = *function_defs[j];
      auto* response = new eager::RegisterFunctionResponse();
      int request_idx = i * function_defs.size() + j;
      eager_client->RegisterFunctionAsync(
          &request, response,
          [request_idx, &statuses, response, &counter](const Status& s) {
            statuses[request_idx] = s;
            delete response;
            counter.DecrementCount();
          });
    }
  }
  counter.Wait();
  for (int i = 0; i < num_requests; i++) {
    TF_RETURN_IF_ERROR(statuses[i]);
  }
#endif  // !IS_MOBILE_PLATFORM
  return Status::OK();
}

Status EagerContext::AddFunctionDef(const FunctionDef& fdef,
                                    const bool add_to_local_only) {
  bool is_first_ref = false;
  {
    mutex_lock l(cache_mu_);
    auto* registered_function =
        gtl::FindPtrOrNull(registered_functions_, fdef.signature().name());
    if (registered_function == nullptr) {
      registered_function = new RegisteredFunction;
      registered_function->cached_kernel_keys =
          absl::make_unique<std::vector<Fprint128>>();
      gtl::InsertOrUpdate(&registered_functions_, fdef.signature().name(),
                          registered_function);
    } else {
      registered_function->Ref();
    }
    is_first_ref = registered_function->RefCountIsOne();
  }
  if (is_first_ref) {
    TF_RETURN_IF_ERROR(func_lib_def_.AddFunctionDef(fdef));
    if (!add_to_local_only) {
      return MaybeRegisterFunctionRemotely(fdef);
    }
  }
  return Status::OK();
}

Status EagerContext::RemoveFunction(const string& func) {
  bool is_last_ref = false;
  {
    mutex_lock l(cache_mu_);
    auto* registered_function = gtl::FindPtrOrNull(registered_functions_, func);
    if (registered_function == nullptr) {
      return errors::InvalidArgument("Tried to remove non-existent function '",
                                     func, "'.");
    }
    is_last_ref = registered_function->RefCountIsOne();
    if (is_last_ref) {
      for (auto& key : *registered_function->cached_kernel_keys) {
        kernel_cache_.erase(key);
      }
      registered_functions_.erase(func);
    }
    registered_function->Unref();
  }
  if (is_last_ref) {
    // TODO(fishx): Remove remote function as well.
    return func_lib_def_.RemoveFunction(func);
  }
  return Status::OK();
}

core::RefCountPtr<KernelAndDevice> EagerContext::GetCachedKernel(
    Fprint128 cache_key) {
  tf_shared_lock l(cache_mu_);
  auto iter = kernel_cache_.find(cache_key);
  if (iter == kernel_cache_.end()) {
    return nullptr;
  }
  core::RefCountPtr<KernelAndDevice> new_ref(iter->second.get());
  new_ref->Ref();
  return new_ref;
}

void EagerContext::AddKernelToCache(Fprint128 cache_key,
                                    KernelAndDevice* kernel) {
  mutex_lock ml(cache_mu_);
  core::RefCountPtr<KernelAndDevice> new_ref(kernel);
  new_ref->Ref();
  kernel_cache_[cache_key] = std::move(new_ref);
  auto* registered_function =
      gtl::FindPtrOrNull(registered_functions_, kernel->name());
  // The kernel name can be either a primitive op or a function.
  if (registered_function != nullptr) {
    registered_function->cached_kernel_keys->emplace_back(cache_key);
  }
}

bool EagerContext::ShouldStoreGraphs() { return should_store_graphs_.load(); }

void EagerContext::SetShouldStoreGraphs(bool value) {
  mutex_lock ml(metadata_mu_);
  should_store_graphs_.store(value);
  if (!value) {
    run_metadata_.Clear();
  }
}

Status EagerContext::FindDeviceFromName(const char* device_name,
                                        Device** device) const {
  *device = HostCPU();
  if (device_name == nullptr || strlen(device_name) == 0) {
    return Status::OK();
  }

  auto status = local_device_mgr()->LookupDevice(device_name, device);
  if (status.ok()) {
    return status;
  }

  if (remote_device_mgr() != nullptr) {
    return remote_device_mgr()->LookupDevice(device_name, device);
  }

  return status;
}

bool EagerContext::OnSameTask(const Device* first, const Device* second) const {
  if (first == nullptr) first = HostCPU();
  if (second == nullptr) second = HostCPU();
  return first->parsed_name().job == second->parsed_name().job &&
         first->parsed_name().replica == second->parsed_name().replica &&
         first->parsed_name().task == second->parsed_name().task;
}

// Gets the CPU device on the task of device.
Status EagerContext::CPUDeviceOnTask(const Device* device,
                                     Device** cpu_device) const {
  string cpu_device_name;
  TF_RETURN_IF_ERROR(DeviceNameUtils::DeviceNameToCpuDeviceName(
      device->name(), &cpu_device_name));

  return FindDeviceByName(cpu_device_name, cpu_device);
}

namespace {
Status GetTaskName(Device* d, string* task_name) {
  string ignored;
  if (!DeviceNameUtils::SplitDeviceName(d->name(), task_name, &ignored)) {
    return errors::InvalidArgument("Unable to parse device name: ", d->name());
  }

  return Status::OK();
}
}  // namespace

bool EagerContext::IsLocalDeviceName(
    const DeviceNameUtils::ParsedName& device_name) const {
  if ((!device_name.has_job && !device_name.has_replica &&
       !device_name.has_task) ||
      remote_device_mgr() == nullptr)
    return true;
  auto& host_cpu_name = HostCPU()->parsed_name();
  return device_name.job == host_cpu_name.job &&
         device_name.replica == host_cpu_name.replica &&
         device_name.task == host_cpu_name.task;
}

#if !defined(IS_MOBILE_PLATFORM)
Status EagerContext::GetClient(Device* device, eager::EagerClient** client) {
  return GetClient(device->parsed_name(), client);
}

Status EagerContext::GetClient(const DeviceNameUtils::ParsedName& device_name,
                               eager::EagerClient** client) {
  if (remote_eager_workers_ == nullptr) {
    return errors::Internal(
        "Haven't set up remote eager worker in this eager context yet.");
  }
  string device_task_name;
  if (!DeviceNameUtils::GetTaskName(device_name, &device_task_name)) {
    return errors::InvalidArgument(
        "Task is not fully specified in device name: ",
        DeviceNameUtils::ParsedNameToString(device_name));
  }

  TF_RETURN_IF_ERROR(
      remote_eager_workers_->GetClient(device_task_name, client));

  if (*client == nullptr) {
    return errors::InvalidArgument(
        "Unable to find eager client corresponding to device ",
        DeviceNameUtils::ParsedNameToString(device_name));
  }

  if (std::find(remote_contexts_.begin(), remote_contexts_.end(),
                device_task_name) == remote_contexts_.end()) {
    return errors::Internal("Unable to find a context for handle on task: ",
                            device_task_name, ". This should not be possible");
  }

  return Status::OK();
}

Status EagerContext::GetClient(const string& remote_task,
                               eager::EagerClient** client) {
  if (remote_eager_workers_ == nullptr) {
    return errors::Internal(
        "Haven't set up remote eager worker in this eager context yet.");
  }
  TF_RETURN_IF_ERROR(remote_eager_workers_->GetClient(remote_task, client));

  if (*client == nullptr) {
    return errors::InvalidArgument(
        "Unable to find eager client corresponding to target ", remote_task);
  }
  return Status::OK();
}

uint64 EagerContext::GetContextId() {
  tf_shared_lock l(remote_state_mu_);
  return context_id_;
}

uint64 EagerContext::GetContextViewId() {
  tf_shared_lock l(remote_state_mu_);
  return context_view_id_;
}

Status EagerContext::StoreCollectiveOpsServer(
    std::unique_ptr<ServerInterface> server, DeviceMgr* device_mgr,
    CollectiveExecutorMgrInterface* rpc_collective_executor_mgr) {
  collective_executor_mgr_.reset(nullptr);
  unowned_collective_executor_mgr_ = rpc_collective_executor_mgr;

  local_device_manager_.reset(nullptr);
  local_unowned_device_manager_ = device_mgr;

  devices_ = local_unowned_device_manager_->ListDevices();
  devices_map_.clear();

  InitDeviceMapAndAsync();
  ClearCaches();
  default_executor_.ClearError();
  {
    tensorflow::mutex_lock l(executor_map_mu_);
    for (auto& entry : thread_local_executor_) {
      entry.second->ClearError();
    }
  }

  pflr_.reset(new ProcessFunctionLibraryRuntime(
      local_unowned_device_manager_, env_, TF_GRAPH_DEF_VERSION, &func_lib_def_,
      {}, thread_pool_.get()));

  // Memory leak!
  if (server_ != nullptr) {
    LOG(WARNING) << "Unable to destroy server_ object, so releasing instead. "
                    "Servers don't support clean shutdown.";
    server_.release();
  }
  server_ = std::move(server);

  return Status::OK();
}

Status EagerContext::InitializeRemoteMaster(
    std::unique_ptr<ServerInterface> server, WorkerEnv* worker_env,
    std::shared_ptr<WorkerSession> worker_session,
    std::unique_ptr<eager::EagerClientCache> remote_eager_workers,
    std::unique_ptr<DynamicDeviceMgr> remote_device_manager,
    const std::vector<string>& remote_contexts, uint64 context_id,
    Rendezvous* r, DeviceMgr* local_device_mgr, int keep_alive_secs,
    DistributedFunctionLibraryRuntime* cluster_flr,
    std::unique_ptr<eager::RemoteMgr, std::function<void(eager::RemoteMgr*)>>
        remote_mgr) {
  if (context_id == kInvalidContextId) {
    return errors::InvalidArgument(
        "Failed to initialize remote for master context due to invalid ",
        "context id");
  }

  if (!remote_contexts_.empty()) {
    CloseAndClearAllRemoteContexts();
  }
  remote_contexts_ = remote_contexts;

  return SetMasterContextState(
      std::move(server), worker_env, std::move(worker_session),
      std::move(remote_eager_workers), std::move(remote_device_manager),
      context_id, 0, r, local_device_mgr, keep_alive_secs, cluster_flr,
      std::move(remote_mgr));
}

Status EagerContext::UpdateRemoteMaster(
    std::unique_ptr<ServerInterface> server, WorkerEnv* worker_env,
    std::shared_ptr<WorkerSession> worker_session,
    std::unique_ptr<eager::EagerClientCache> remote_eager_workers,
    std::unique_ptr<DynamicDeviceMgr> remote_device_manager,
    const std::vector<string>& add_remote_contexts,
    const std::vector<string>& remove_remote_contexts, uint64 context_id,
    Rendezvous* r, DeviceMgr* local_device_mgr, int keep_alive_secs,
    DistributedFunctionLibraryRuntime* cluster_flr) {
  {
    tf_shared_lock l(remote_state_mu_);
    if (context_id != context_id_) {
      return errors::InvalidArgument(
          "Failed to update remote remote master context due to invalid ",
          "context id. Request id = ", context_id,
          " but current id = ", context_id_);
    }
  }

  if (!remove_remote_contexts.empty()) {
    CloseRemoteContexts(remove_remote_contexts);
    for (const string& remote_context : remove_remote_contexts) {
      remote_contexts_.erase(
          std::remove(remote_contexts_.begin(), remote_contexts_.end(),
                      remote_context),
          remote_contexts_.end());
    }
  } else if (!add_remote_contexts.empty()) {
    remote_contexts_.insert(std::end(remote_contexts_),
                            std::begin(add_remote_contexts),
                            std::end(add_remote_contexts));
  }
  std::vector<const FunctionDef*> function_defs = ListRegisteredFunctions();
  TF_RETURN_IF_ERROR(SetMasterContextState(
      std::move(server), worker_env, std::move(worker_session),
      std::move(remote_eager_workers), std::move(remote_device_manager),
      context_id, GetContextViewId() + 1, r, local_device_mgr, keep_alive_secs,
      cluster_flr, nullptr));

  // Register existing functions to the newly added remote workers. Note that
  // this should happen only after updating `remote_contexts_` because new
  // functions might be registered while we update the context. When that
  // happens, this ordering ensures that `MaybeRegisterFunctionRemotely` will
  // register the new functions on all remote workers (including the newly added
  // ones), and `RegisterExistingFunctionsOnRemoteWorkers` will take care of
  // registering existing functions, where duplicate registrations will be
  // ignored by the remote workers.
  TF_RETURN_IF_ERROR(RegisterExistingFunctionsOnRemoteWorkers(
      function_defs, add_remote_contexts));
  return Status::OK();
}

Status EagerContext::SetMasterContextState(
    std::unique_ptr<ServerInterface> server, WorkerEnv* worker_env,
    std::shared_ptr<WorkerSession> worker_session,
    std::unique_ptr<eager::EagerClientCache> remote_eager_workers,
    std::unique_ptr<DynamicDeviceMgr> remote_device_manager, uint64 context_id,
    uint64 context_view_id, Rendezvous* r, DeviceMgr* local_device_mgr,
    int keep_alive_secs, DistributedFunctionLibraryRuntime* cluster_flr,
    std::unique_ptr<eager::RemoteMgr, std::function<void(eager::RemoteMgr*)>>
        remote_mgr) {
  mutex_lock l(remote_state_mu_);
  is_master_ = true;
  context_id_ = context_id;
  context_view_id_ = context_view_id;

  use_send_tensor_rpc_ =
      ReadBoolFromEnvVar("TF_EAGER_REMOTE_USE_SEND_TENSOR_RPC", true);

  local_unowned_device_manager_ = local_device_mgr;
  local_device_manager_ = nullptr;

  devices_ = local_unowned_device_manager_->ListDevices();
  devices_map_.clear();

  if (rendezvous_ != nullptr) rendezvous_->Unref();
  rendezvous_ = r;

  // Memory leak!
  if (server_ != nullptr) {
    LOG(WARNING) << "Unable to destroy server_ object, so releasing instead. "
                    "Servers don't support clean shutdown.";
    server_.release();
  }

  server_ = std::move(server);
  if (remote_mgr != nullptr) {
    remote_mgr_ = std::move(remote_mgr);
  }
  worker_env_ = worker_env;
  worker_session_ = worker_session;
  remote_eager_workers_ = std::move(remote_eager_workers);

  remote_device_manager_ = std::move(remote_device_manager);

  InitDeviceMapAndAsync();

  ClearCaches();
  default_executor_.ClearError();
  {
    tensorflow::mutex_lock l(executor_map_mu_);
    for (auto& entry : thread_local_executor_) {
      entry.second->ClearError();
    }
  }
  pflr_.reset(new ProcessFunctionLibraryRuntime(
      local_unowned_device_manager_, env_, TF_GRAPH_DEF_VERSION, &func_lib_def_,
      {}, thread_pool_.get(), cluster_flr, custom_kernel_creator_));

  keep_alive_secs_ = keep_alive_secs;
  sleep_for_secs_ = std::max(1, keep_alive_secs_ / 2);
  // Only schedule a single closure.
  if (keep_alive_thread_ == nullptr) {
    keep_alive_thread_.reset(
        env_->StartThread({}, "EagerKeepAliveThread", [this]() {
          while (true) {
            {
              {
                mutex_lock l(keep_alive_thread_shutdown_mu_);

                if (shutting_down_) {
                  return;
                }

                keep_alive_thread_cv_.wait_for(
                    l, std::chrono::seconds(sleep_for_secs_));

                if (shutting_down_) {
                  return;
                }
              }
              {
                mutex_lock l(remote_state_mu_);
                if (keep_alive_secs_ > 0) {
                  {
                    for (const auto& worker : remote_contexts_) {
                      eager::EagerClient* client;
                      Status s =
                          remote_eager_workers_->GetClient(worker, &client);

                      if (!s.ok()) {
                        LOG(WARNING) << "Keep-alive thread was unable to find "
                                        "a client for target "
                                     << worker << ". Got error: " << s;
                        continue;
                      }

                      eager::KeepAliveRequest* request =
                          new eager::KeepAliveRequest;
                      eager::KeepAliveResponse* response =
                          new eager::KeepAliveResponse;

                      request->set_context_id(context_id_);
                      client->KeepAliveAsync(
                          request, response,
                          [request, response](const Status& s) {
                            delete request;
                            delete response;
                          });
                    }
                  }
                }
              }
            }
          }
        }));
  }
  return Status::OK();
}

Status EagerContext::InitializeRemoteWorker(
    std::unique_ptr<eager::EagerClientCache> remote_eager_workers,
    const DynamicDeviceMgr* remote_device_mgr,
    const std::vector<string>& remote_contexts, uint64 context_id,
    std::function<Rendezvous*(const int64)> rendezvous_creator,
    std::unique_ptr<eager::RemoteMgr, std::function<void(eager::RemoteMgr*)>>
        remote_mgr) {
  if (context_id == kInvalidContextId) {
    return errors::InvalidArgument(
        "Failed to initialize remote for worker context due to invalid ",
        "context id");
  }
  mutex_lock l(remote_state_mu_);

  if (remote_device_manager_ != nullptr || server_ != nullptr ||
      keep_alive_thread_ != nullptr) {
    return errors::FailedPrecondition(
        "EagerContext::InitializeRemoteWorker Failed. ",
        "Already initialized remote as a master context.");
  }
  is_master_ = false;

  remote_contexts_ = remote_contexts;
  context_id_ = context_id;
  context_view_id_ = 0;

  rendezvous_creator_ = std::move(rendezvous_creator);
  remote_eager_workers_ = std::move(remote_eager_workers);
  remote_mgr_ = std::move(remote_mgr);

  remote_unowned_device_manager_ = remote_device_mgr;
  InitDeviceMapAndAsync();

  ClearCaches();
  default_executor_.ClearError();
  {
    tensorflow::mutex_lock l(executor_map_mu_);
    for (auto& entry : thread_local_executor_) {
      entry.second->ClearError();
    }
  }

  return Status::OK();
}

Status EagerContext::UpdateRemoteWorker(
    const DeviceMgr* worker_session_device_mgr,
    std::unique_ptr<eager::EagerClientCache> remote_eager_workers,
    const DynamicDeviceMgr* remote_device_mgr,
    const std::vector<string>& remote_contexts, uint64 context_id,
    DistributedFunctionLibraryRuntime* cluster_flr) {
  {
    mutex_lock l(remote_state_mu_);
    if (context_id != context_id_) {
      return errors::InvalidArgument(
          "Failed to update remote for worker context due to invalid ",
          "context id. Request id = ", context_id,
          " but current id = ", context_id_);
    }
    context_view_id_++;
  }

  remote_contexts_ = remote_contexts;

  remote_eager_workers_ = std::move(remote_eager_workers);

  remote_unowned_device_manager_ = remote_device_mgr;
  devices_ = worker_session_device_mgr->ListDevices();
  devices_map_.clear();
  InitDeviceMapAndAsync();

  ClearCaches();
  default_executor_.ClearError();
  {
    tensorflow::mutex_lock l(executor_map_mu_);
    for (auto& entry : thread_local_executor_) {
      entry.second->ClearError();
    }
  }

  SessionOptions options = SessionOptions();
  pflr_.reset(new ProcessFunctionLibraryRuntime(
      worker_session_device_mgr, options.env, TF_GRAPH_DEF_VERSION,
      FuncLibDef(), options.config.graph_options().optimizer_options(),
      thread_pool_.get(), cluster_flr, custom_kernel_creator_));
  return Status::OK();
}
#endif  // !IS_MOBILE_PLATFORM

}  // namespace tensorflow
