  // Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/interpreter/proc_interpreter.h"

#include "absl/status/statusor.h"
#include "absl/strings/str_join.h"
#include "xls/common/logging/log_lines.h"
#include "xls/interpreter/ir_interpreter.h"
#include "xls/ir/value_helpers.h"

namespace xls {
namespace {

// A visitor for interpreting procs. Adds handlers for send and receive
// communcate via ChannelQueues.
class ProcIrInterpreter : public IrInterpreter {
 public:
  // Constructor args:
  //   state: is the value to use for the proc state in the tick being
  //     interpreted.
  //   node_values: map from Node to Value for already computed values in this
  //     tick of the proc. Used for continuations.
  //   events: events object to record events in (e.g, traces).
  //   queue_manager: manager for channel queues.
  ProcIrInterpreter(absl::Span<const Value> state,
                    absl::flat_hash_map<Node*, Value>* node_values,
                    InterpreterEvents* events,
                    ChannelQueueManager* queue_manager)
      : IrInterpreter(node_values, events),
        state_(state.begin(), state.end()),
        queue_manager_(queue_manager) {}

  absl::Status HandleReceive(Receive* receive) override {
    XLS_ASSIGN_OR_RETURN(ChannelQueue * queue,
                         queue_manager_->GetQueueById(receive->channel_id()));

    if (receive->predicate().has_value()) {
      const Bits& pred = ResolveAsBits(receive->predicate().value());
      if (pred.IsZero()) {
        // If the predicate is false, nothing is read from the channel.
        // Rather the result of the receive is the zero values of the
        // respective type.
        return SetValueResult(receive, ZeroOfType(receive->GetType()));
      }
    }

    std::optional<Value> value = queue->Read();
    if (!value.has_value()) {
      if (receive->is_blocking()) {
        // Record the channel this receive instruction is blocked on and exit.
        blocked_channel_ = queue->channel();
        return absl::OkStatus();
      }
      // A non-blocking receive returns a zero data value with a zero valid bit
      // if the queue is empty.
      return SetValueResult(receive, ZeroOfType(receive->GetType()));
    }

    if (receive->is_blocking()) {
      return SetValueResult(receive, Value::Tuple({Value::Token(), *value}));
    }

    return SetValueResult(
        receive, Value::Tuple({Value::Token(), *value, Value(UBits(1, 1))}));
  }

  absl::Status HandleSend(Send* send) override {
    XLS_ASSIGN_OR_RETURN(ChannelQueue * queue,
                         queue_manager_->GetQueueById(send->channel_id()));
    if (send->predicate().has_value()) {
      const Bits& pred = ResolveAsBits(send->predicate().value());
      if (pred.IsZero()) {
        return SetValueResult(send, Value::Token());
      }
    }
    // Indicate that data is sent on this channel.
    sent_channel_ = queue->channel();

    XLS_RETURN_IF_ERROR(queue->Write(ResolveAsValue(send->data())));

    // The result of a send is simply a token.
    return SetValueResult(send, Value::Token());
  }

  absl::Status HandleParam(Param* param) override {
    XLS_ASSIGN_OR_RETURN(int64_t index,
                         param->function_base()->GetParamIndex(param));
    if (index == 0) {
      return SetValueResult(param, Value::Token());
    }
    // Params from 1 on are state.
    return SetValueResult(param, state_[index - 1]);
  }

  // Executes a single node and return whether the node is blocked on a channel
  // (for receive nodes) or whether data was sent on a channel (for send nodes).
  struct NodeResult {
    std::optional<Channel*> blocked_channel;
    std::optional<Channel*> sent_channel;
  };
  absl::StatusOr<NodeResult> ExecuteNode(Node* node) {
    // Send/Receive handlers might set these values so clear them before hand.
    blocked_channel_ = std::nullopt;
    sent_channel_ = std::nullopt;
    XLS_RETURN_IF_ERROR(node->VisitSingleNode(this));
    return NodeResult{.blocked_channel = blocked_channel_,
                      .sent_channel = sent_channel_};
  }

 private:
  std::vector<Value> state_;
  ChannelQueueManager* queue_manager_;

  // Ephemeral values set by the send/receive handlers indicating the channel
  // execution is blocked on or the channel on which data was sent.
  std::optional<Channel*> blocked_channel_;
  std::optional<Channel*> sent_channel_;
};

// Computes the node execution order for the interpreter. Due to a bug in the
// way xlscc emits IR, place receives as late as possible in the order to avoid
// deadlocks.
// TODO(https://github.com/google/xls/issues/717): Remove hack for late receive
// ordering when xlscc is fixed.
std::vector<Node*> NodeExecutionOrder(Proc* proc) {
  std::vector<Node*> result;
  std::list<Node*> ready_list;
  absl::flat_hash_map<Node*, int64_t> operands_remaining;
  for (Node* node : proc->nodes()) {
    absl::flat_hash_set<Node*> unique_operands(node->operands().begin(),
                                               node->operands().end());
    operands_remaining[node] = unique_operands.size();
    if (unique_operands.empty()) {
      ready_list.push_back(node);
    }
  }
  while (!ready_list.empty()) {
    auto it = ready_list.begin();
    // Chose the first node on the ready list which is *not* a receive.
    for (; it != ready_list.end(); ++it) {
      if (!(*it)->Is<Receive>()) {
        break;
      }
    }
    // If all nodes on the ready list are receives, then pick the first one.
    it = it == ready_list.end() ? ready_list.begin() : it;
    Node* node = *it;
    ready_list.erase(it);

    result.push_back(node);

    for (Node* user : node->users()) {
      if (--operands_remaining[user] == 0) {
        ready_list.push_back(user);
      }
    }
  }
  XLS_CHECK_EQ(result.size(), proc->node_count());
  return result;
}

}  // namespace

bool TickResult::operator==(const TickResult& other) const {
  return tick_complete == other.tick_complete &&
         progress_made == other.progress_made &&
         blocked_channel == other.blocked_channel &&
         sent_channels == other.sent_channels;
}

bool TickResult::operator!=(const TickResult& other) const {
  return !(*this == other);
}

std::string TickResult::ToString() const {
  return absl::StrFormat(
      "{ tick_complete=%s, progress_made=%s, "
      "blocked_channel=%s, sent_channels={%s} }",
      tick_complete ? "true" : "false", progress_made ? "true" : "false",
      blocked_channel.has_value() ? blocked_channel.value()->ToString()
                                  : "(none)",
      absl::StrJoin(sent_channels, ", ", ChannelFormatter));
}

std::ostream& operator<<(std::ostream& os, const TickResult& result) {
  os << result.ToString();
  return os;
}

ProcInterpreter::ProcInterpreter(Proc* proc, ChannelQueueManager* queue_manager)
    : proc_(proc),
      queue_manager_(queue_manager),
      execution_order_(NodeExecutionOrder(proc)) {}

std::unique_ptr<ProcContinuation> ProcInterpreter::NewContinuation() const {
  return std::make_unique<ProcInterpreterContinuation>(proc());
}

absl::StatusOr<TickResult> ProcInterpreter::Tick(
    ProcContinuation& continuation) const {
  ProcInterpreterContinuation* cont =
      dynamic_cast<ProcInterpreterContinuation*>(&continuation);
  XLS_RET_CHECK_NE(cont, nullptr) << "ProcInterpreter requires a continuation "
                                     "of type ProcInterpreterContinuation";
  std::vector<Channel*> sent_channels;

  ProcIrInterpreter ir_interpreter(cont->GetState(), &cont->GetNodeValues(),
                                   &cont->GetEvents(), queue_manager_);

  // Resume execution at the node indicated in the continuation
  // (NodeExecutionIndex).
  int64_t starting_index = cont->GetNodeExecutionIndex();
  for (int64_t i = starting_index; i < execution_order_.size(); ++i) {
    Node* node = execution_order_[i];
    XLS_ASSIGN_OR_RETURN(ProcIrInterpreter::NodeResult result,
                         ir_interpreter.ExecuteNode(node));
    if (result.sent_channel.has_value()) {
      sent_channels.push_back(result.sent_channel.value());
    }
    if (result.blocked_channel.has_value()) {
      // Proc is blocked at a receive node waiting for data on a channel.
      cont->SetNodeExecutionIndex(i);
      // Raise a status error if interpreter events indicate failure such as a
      // failed assert.
      XLS_RETURN_IF_ERROR(InterpreterEventsToStatus(cont->GetEvents()));
      return TickResult{
          .tick_complete = false,
          .progress_made = cont->GetNodeExecutionIndex() != starting_index,
          .blocked_channel = result.blocked_channel,
          .sent_channels = sent_channels};
    }
  }

  // Proc completed execution of the Tick. Set the next proc state in the
  // continuation.
  std::vector<Value> next_state;
  next_state.reserve(proc()->GetStateElementCount());
  for (Node* next_node : proc()->NextState()) {
    next_state.push_back(ir_interpreter.ResolveAsValue(next_node));
  }
  cont->NextTick(std::move(next_state));

  // Raise a status error if interpreter events indicate failure such as a
  // failed assert.
  XLS_RETURN_IF_ERROR(InterpreterEventsToStatus(cont->GetEvents()));

  return TickResult{.tick_complete = true,
                    .progress_made = true,
                    .blocked_channel = std::nullopt,
                    .sent_channels = sent_channels};
}

}  // namespace xls
