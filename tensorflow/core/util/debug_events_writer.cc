/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/util/debug_events_writer.h"

#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/lib/strings/stringprintf.h"
#include "tensorflow/core/platform/host_info.h"
#include "tensorflow/core/public/version.h"

namespace tensorflow {
namespace tfdbg {

namespace {
void MaybeSetDebugEventTimestamp(DebugEvent* debug_event, Env* env) {
  if (debug_event->wall_time() == 0) {
    debug_event->set_wall_time(env->NowMicros() / 1e6);
  }
}
}  // namespace

SingleDebugEventFileWriter::SingleDebugEventFileWriter(const string& file_path)
    : env_(Env::Default()),
      file_path_(file_path),
      num_outstanding_events_(0),
      writer_mu_() {}

Status SingleDebugEventFileWriter::Init() {
  if (record_writer_ != nullptr) {
    // TODO(cais): We currently don't check for file deletion. When the need
    // arises, check and fix it.
    return Status::OK();
  }

  // Reset recordio_writer (which has a reference to writable_file_) so final
  // Flush() and Close() call have access to writable_file_.
  record_writer_.reset();

  TF_RETURN_WITH_CONTEXT_IF_ERROR(
      env_->NewWritableFile(file_path_, &writable_file_),
      "Creating writable file ", file_path_);
  record_writer_.reset(new io::RecordWriter(writable_file_.get()));
  if (record_writer_ == nullptr) {
    return errors::Unknown("Could not create record writer at path: ",
                           file_path_);
  }
  num_outstanding_events_.store(0);
  VLOG(1) << "Successfully opened debug events file: " << file_path_;
  return Status::OK();
}

void SingleDebugEventFileWriter::WriteSerializedDebugEvent(
    StringPiece debug_event_str) {
  if (record_writer_ == nullptr) {
    if (!Init().ok()) {
      LOG(ERROR) << "Write failed because file could not be opened.";
      return;
    }
  }
  num_outstanding_events_.fetch_add(1);
  {
    mutex_lock l(writer_mu_);
    record_writer_->WriteRecord(debug_event_str).IgnoreError();
  }
}

Status SingleDebugEventFileWriter::Flush() {
  const int num_outstanding = num_outstanding_events_.load();
  if (num_outstanding == 0) {
    return Status::OK();
  }
  if (writable_file_ == nullptr) {
    return errors::Unknown("Unexpected NULL file for path: ", file_path_);
  }

  {
    mutex_lock l(writer_mu_);
    TF_RETURN_WITH_CONTEXT_IF_ERROR(record_writer_->Flush(), "Failed to flush ",
                                    num_outstanding, " debug events to ",
                                    file_path_);
  }

  TF_RETURN_WITH_CONTEXT_IF_ERROR(writable_file_->Sync(), "Failed to sync ",
                                  num_outstanding, " debug events to ",
                                  file_path_);
  num_outstanding_events_.store(0);
  return Status::OK();
}

Status SingleDebugEventFileWriter::Close() {
  Status status = Flush();
  if (writable_file_ != nullptr) {
    Status close_status = writable_file_->Close();
    if (!close_status.ok()) {
      status = close_status;
    }
    record_writer_.reset(nullptr);
    writable_file_.reset(nullptr);
  }
  num_outstanding_events_ = 0;
  return status;
}

const string SingleDebugEventFileWriter::FileName() { return file_path_; }

mutex DebugEventsWriter::factory_mu_(LINKER_INITIALIZED);

DebugEventsWriter::~DebugEventsWriter() { Close().IgnoreError(); }

// static
DebugEventsWriter* DebugEventsWriter::GetDebugEventsWriter(
    const string& dump_root, int64 cyclic_buffer_size) {
  mutex_lock l(DebugEventsWriter::factory_mu_);
  std::unordered_map<string, std::unique_ptr<DebugEventsWriter>>* writer_pool =
      DebugEventsWriter::GetDebugEventsWriterMap();
  if (writer_pool->find(dump_root) == writer_pool->end()) {
    std::unique_ptr<DebugEventsWriter> writer(
        new DebugEventsWriter(dump_root, cyclic_buffer_size));
    writer_pool->insert(std::make_pair(dump_root, std::move(writer)));
  }
  return (*writer_pool)[dump_root].get();
}

// static
DebugEventsWriter* DebugEventsWriter::GetDebugEventsWriter(
    const string& dump_root) {
  return DebugEventsWriter::GetDebugEventsWriter(dump_root,
                                                 kDefaultCyclicBufferSize);
}

Status DebugEventsWriter::Init() {
  mutex_lock l(initialization_mu_);

  // TODO(cais): We currently don't check for file deletion. When the need
  // arises, check and fix file deletion.
  if (is_initialized_) {
    return Status::OK();
  }

  if (!env_->IsDirectory(dump_root_).ok()) {
    TF_RETURN_WITH_CONTEXT_IF_ERROR(env_->RecursivelyCreateDir(dump_root_),
                                    "Failed to create directory ", dump_root_);
  }

  int64 time_in_seconds = env_->NowMicros() / 1e6;
  file_prefix_ = io::JoinPath(
      dump_root_, strings::Printf("%s.%010lld.%s", kFileNamePrefix,
                                  static_cast<int64>(time_in_seconds),
                                  port::Hostname().c_str()));
  TF_RETURN_IF_ERROR(InitNonMetadataFile(SOURCE_FILES));
  TF_RETURN_IF_ERROR(InitNonMetadataFile(STACK_FRAMES));
  TF_RETURN_IF_ERROR(InitNonMetadataFile(GRAPHS));

  // In case there is one left over from before.
  metadata_writer_.reset();

  // The metadata file should be created.
  string metadata_filename = GetFileNameInternal(METADATA);
  metadata_writer_.reset(new SingleDebugEventFileWriter(metadata_filename));
  if (metadata_writer_ == nullptr) {
    return errors::Unknown("Could not create debug event metadata file writer");
  }

  DebugEvent debug_event;
  DebugMetadata* metadata = debug_event.mutable_debug_metadata();
  metadata->set_tensorflow_version(TF_VERSION_STRING);
  metadata->set_file_version(
      strings::Printf("%s%d", kVersionPrefix, kCurrentFormatVersion));
  SerializeAndWriteDebugEvent(&debug_event, METADATA);
  TF_RETURN_WITH_CONTEXT_IF_ERROR(
      metadata_writer_->Flush(), "Failed to flush debug event metadata writer");

  TF_RETURN_IF_ERROR(InitNonMetadataFile(EXECUTION));
  TF_RETURN_IF_ERROR(InitNonMetadataFile(GRAPH_EXECUTION_TRACES));
  is_initialized_ = true;
  return Status::OK();
}

void DebugEventsWriter::WriteSourceFile(SourceFile* source_file) {
  DebugEvent debug_event;
  debug_event.set_allocated_source_file(source_file);
  SerializeAndWriteDebugEvent(&debug_event, SOURCE_FILES);
}

void DebugEventsWriter::WriteStackFrameWithId(
    StackFrameWithId* stack_frame_with_id) {
  DebugEvent debug_event;
  debug_event.set_allocated_stack_frame_with_id(stack_frame_with_id);
  SerializeAndWriteDebugEvent(&debug_event, STACK_FRAMES);
}

void DebugEventsWriter::WriteGraphOpCreation(
    GraphOpCreation* graph_op_creation) {
  DebugEvent debug_event;
  debug_event.set_allocated_graph_op_creation(graph_op_creation);
  SerializeAndWriteDebugEvent(&debug_event, GRAPHS);
}

void DebugEventsWriter::WriteDebuggedGraph(DebuggedGraph* debugged_graph) {
  DebugEvent debug_event;
  debug_event.set_allocated_debugged_graph(debugged_graph);
  SerializeAndWriteDebugEvent(&debug_event, GRAPHS);
}

void DebugEventsWriter::WriteExecution(Execution* execution) {
  if (cyclic_buffer_size_ <= 0) {
    // No cyclic-buffer behavior.
    DebugEvent debug_event;
    debug_event.set_allocated_execution(execution);
    SerializeAndWriteDebugEvent(&debug_event, EXECUTION);
  } else {
    // Cyclic buffer behavior.
    DebugEvent debug_event;
    MaybeSetDebugEventTimestamp(&debug_event, env_);
    debug_event.set_allocated_execution(execution);
    mutex_lock l(execution_buffer_mu_);
    execution_buffer_.push_back(debug_event);
    if (execution_buffer_.size() > cyclic_buffer_size_) {
      execution_buffer_.pop_front();
    }
  }
}

void DebugEventsWriter::WriteGraphExecutionTrace(
    GraphExecutionTrace* graph_execution_trace) {
  if (cyclic_buffer_size_ <= 0) {
    // No cyclic-buffer behavior.
    DebugEvent debug_event;
    debug_event.set_allocated_graph_execution_trace(graph_execution_trace);
    SerializeAndWriteDebugEvent(&debug_event, GRAPH_EXECUTION_TRACES);
  } else {
    // Cyclic buffer behavior.
    DebugEvent debug_event;
    MaybeSetDebugEventTimestamp(&debug_event, env_);
    debug_event.set_allocated_graph_execution_trace(graph_execution_trace);
    mutex_lock l(graph_execution_trace_buffer_mu_);
    graph_execution_trace_buffer_.push_back(debug_event);
    if (graph_execution_trace_buffer_.size() > cyclic_buffer_size_) {
      graph_execution_trace_buffer_.pop_front();
    }
  }
}

Status DebugEventsWriter::FlushNonExecutionFiles() {
  TF_RETURN_IF_ERROR(Init());
  if (source_files_writer_ != nullptr) {
    TF_RETURN_IF_ERROR(source_files_writer_->Flush());
  }
  if (stack_frames_writer_ != nullptr) {
    TF_RETURN_IF_ERROR(stack_frames_writer_->Flush());
  }
  if (graphs_writer_ != nullptr) {
    TF_RETURN_IF_ERROR(graphs_writer_->Flush());
  }
  return Status::OK();
}

Status DebugEventsWriter::FlushExecutionFiles() {
  TF_RETURN_IF_ERROR(Init());

  if (execution_writer_ != nullptr) {
    if (cyclic_buffer_size_ > 0) {
      // Write out all the content in the cyclic buffers.
      mutex_lock l(execution_buffer_mu_);
      while (!execution_buffer_.empty()) {
        SerializeAndWriteDebugEvent(&execution_buffer_.front(), EXECUTION);
        execution_buffer_.pop_front();
      }
    }
    TF_RETURN_IF_ERROR(execution_writer_->Flush());
  }

  if (graph_execution_traces_writer_ != nullptr) {
    if (cyclic_buffer_size_ > 0) {
      // Write out all the content in the cyclic buffers.
      mutex_lock l(graph_execution_trace_buffer_mu_);
      while (!graph_execution_trace_buffer_.empty()) {
        SerializeAndWriteDebugEvent(&graph_execution_trace_buffer_.front(),
                                    GRAPH_EXECUTION_TRACES);
        graph_execution_trace_buffer_.pop_front();
      }
    }
    TF_RETURN_IF_ERROR(graph_execution_traces_writer_->Flush());
  }

  return Status::OK();
}

string DebugEventsWriter::FileName(DebugEventFileType type) {
  if (file_prefix_.empty()) {
    Init().IgnoreError();
  }
  return GetFileNameInternal(type);
}

Status DebugEventsWriter::Close() {
  std::vector<string> failed_to_close_files;

  if (metadata_writer_ != nullptr) {
    if (!metadata_writer_->Close().ok()) {
      failed_to_close_files.push_back(metadata_writer_->FileName());
    }
    metadata_writer_.reset(nullptr);
  }

  TF_RETURN_IF_ERROR(FlushNonExecutionFiles());
  if (source_files_writer_ != nullptr) {
    if (!source_files_writer_->Close().ok()) {
      failed_to_close_files.push_back(source_files_writer_->FileName());
    }
    source_files_writer_.reset(nullptr);
  }
  if (stack_frames_writer_ != nullptr) {
    if (!stack_frames_writer_->Close().ok()) {
      failed_to_close_files.push_back(stack_frames_writer_->FileName());
    }
    stack_frames_writer_.reset(nullptr);
  }
  if (graphs_writer_ != nullptr) {
    if (!graphs_writer_->Close().ok()) {
      failed_to_close_files.push_back(graphs_writer_->FileName());
    }
    graphs_writer_.reset(nullptr);
  }

  TF_RETURN_IF_ERROR(FlushExecutionFiles());
  if (execution_writer_ != nullptr) {
    if (!execution_writer_->Close().ok()) {
      failed_to_close_files.push_back(execution_writer_->FileName());
    }
    execution_writer_.reset(nullptr);
  }
  if (graph_execution_traces_writer_ != nullptr) {
    if (!graph_execution_traces_writer_->Close().ok()) {
      failed_to_close_files.push_back(
          graph_execution_traces_writer_->FileName());
    }
    graph_execution_traces_writer_.reset(nullptr);
  }

  if (failed_to_close_files.empty()) {
    return Status::OK();
  } else {
    return errors::FailedPrecondition(
        "Failed to close %d debug-events files associated with tfdbg",
        failed_to_close_files.size());
  }
}

// static
std::unordered_map<string, std::unique_ptr<DebugEventsWriter>>*
DebugEventsWriter::GetDebugEventsWriterMap() {
  static std::unordered_map<string, std::unique_ptr<DebugEventsWriter>>*
      writer_pool =
          new std::unordered_map<string, std::unique_ptr<DebugEventsWriter>>();
  return writer_pool;
}

DebugEventsWriter::DebugEventsWriter(const string& dump_root,
                                     int64 cyclic_buffer_size)
    : env_(Env::Default()),
      dump_root_(dump_root),
      is_initialized_(false),
      initialization_mu_(),
      cyclic_buffer_size_(cyclic_buffer_size),
      execution_buffer_(),
      execution_buffer_mu_(),
      graph_execution_trace_buffer_(),
      graph_execution_trace_buffer_mu_() {}

Status DebugEventsWriter::InitNonMetadataFile(DebugEventFileType type) {
  std::unique_ptr<SingleDebugEventFileWriter>* writer = nullptr;
  SelectWriter(type, &writer);
  const string filename = GetFileNameInternal(type);
  writer->reset();

  writer->reset(new SingleDebugEventFileWriter(filename));
  if (*writer == nullptr) {
    return errors::Unknown("Could not create debug event file writer for ",
                           filename);
  }
  TF_RETURN_WITH_CONTEXT_IF_ERROR(
      (*writer)->Init(), "Initializing debug event writer at path ", filename);
  VLOG(1) << "Successfully opened debug event file: " << filename;

  return Status::OK();
}

void DebugEventsWriter::SerializeAndWriteDebugEvent(DebugEvent* debug_event,
                                                    DebugEventFileType type) {
  std::unique_ptr<SingleDebugEventFileWriter>* writer = nullptr;
  SelectWriter(type, &writer);
  if (writer != nullptr) {
    // Timestamp is in seconds, with double precision.
    MaybeSetDebugEventTimestamp(debug_event, env_);
    string str;
    debug_event->AppendToString(&str);
    (*writer)->WriteSerializedDebugEvent(str);
  }
}

void DebugEventsWriter::SelectWriter(
    DebugEventFileType type,
    std::unique_ptr<SingleDebugEventFileWriter>** writer) {
  switch (type) {
    case METADATA:
      *writer = &metadata_writer_;
      break;
    case SOURCE_FILES:
      *writer = &source_files_writer_;
      break;
    case STACK_FRAMES:
      *writer = &stack_frames_writer_;
      break;
    case GRAPHS:
      *writer = &graphs_writer_;
      break;
    case EXECUTION:
      *writer = &execution_writer_;
      break;
    case GRAPH_EXECUTION_TRACES:
      *writer = &graph_execution_traces_writer_;
      break;
  }
}

const string DebugEventsWriter::GetSuffix(DebugEventFileType type) {
  switch (type) {
    case METADATA:
      return kMetadataSuffix;
    case SOURCE_FILES:
      return kSourceFilesSuffix;
    case STACK_FRAMES:
      return kStackFramesSuffix;
    case GRAPHS:
      return kGraphsSuffix;
    case EXECUTION:
      return kExecutionSuffix;
    case GRAPH_EXECUTION_TRACES:
      return kGraphExecutionTracesSuffix;
    default:
      string suffix;
      return suffix;
  }
}

string DebugEventsWriter::GetFileNameInternal(DebugEventFileType type) {
  const string suffix = GetSuffix(type);
  return strings::StrCat(file_prefix_, ".", suffix);
}

}  // namespace tfdbg
}  // namespace tensorflow
