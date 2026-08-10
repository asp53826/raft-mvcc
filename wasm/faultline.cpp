#include "raftmvcc/linearizability.h"
#include "raftmvcc/raft.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define FAULTLINE_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define FAULTLINE_EXPORT
#endif

namespace {

using raftmvcc::Cluster;
using raftmvcc::Command;
using raftmvcc::HistoryOperation;
using raftmvcc::OperationType;
using raftmvcc::Role;
using raftmvcc::Timestamp;

std::unique_ptr<Cluster> cluster;
std::string response;
std::string last_event = "Cluster initialized. No leader has been elected.";
std::uint64_t logical_tick = 0;
int isolated_node = 0;

const char* role_name(Role role) {
  switch (role) {
    case Role::Follower:
      return "follower";
    case Role::Candidate:
      return "candidate";
    case Role::Leader:
      return "leader";
  }
  return "unknown";
}

std::string json_escape(const std::string& input) {
  std::ostringstream out;
  for (const char character : input) {
    switch (character) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << character;
    }
  }
  return out.str();
}

Cluster& current_cluster() {
  if (!cluster) {
    cluster = std::make_unique<Cluster>(5);
  }
  return *cluster;
}

const char* snapshot() {
  auto& active = current_cluster();
  std::ostringstream out;
  out << "{\"engine\":\"C++17/WASM\",\"tick\":" << logical_tick
      << ",\"dropped\":" << active.dropped_messages()
      << ",\"isolated\":" << isolated_node
      << ",\"event\":\"" << json_escape(last_event) << "\",\"nodes\":[";
  for (int id = 1; id <= 5; ++id) {
    const auto& node = active.node(id);
    if (id > 1) out << ',';
    out << "{\"id\":" << id
        << ",\"role\":\"" << role_name(node.role()) << "\""
        << ",\"term\":" << node.term()
        << ",\"commit\":" << node.commit_index()
        << ",\"lastIndex\":" << node.last_index()
        << ",\"leader\":";
    if (node.leader_id()) out << *node.leader_id(); else out << "null";
    out << ",\"digest\":\""
        << json_escape(active.store(id).digest(Timestamp{logical_tick + 1000, 0}))
        << "\"}";
  }
  out << "]}";
  response = out.str();
  return response.c_str();
}

}  // namespace

extern "C" {

FAULTLINE_EXPORT const char* faultline_reset() {
  cluster = std::make_unique<Cluster>(5);
  logical_tick = 0;
  isolated_node = 0;
  last_event = "Cluster reset to five followers with deterministic election timers.";
  return snapshot();
}

FAULTLINE_EXPORT const char* faultline_snapshot() {
  return snapshot();
}

FAULTLINE_EXPORT const char* faultline_campaign(int node) {
  auto& active = current_cluster();
  const bool elected = active.campaign(node);
  last_event = elected
      ? "Node " + std::to_string(node) + " won a majority and became leader."
      : "Node " + std::to_string(node) + " campaigned without a majority.";
  return snapshot();
}

FAULTLINE_EXPORT const char* faultline_tick(int count) {
  const auto steps = count < 1 ? 1 : static_cast<std::size_t>(count);
  current_cluster().tick(steps);
  logical_tick += steps;
  last_event = "Advanced " + std::to_string(steps) +
               " deterministic logical tick" + (steps == 1 ? "." : "s.");
  return snapshot();
}

FAULTLINE_EXPORT const char* faultline_isolate(int node) {
  current_cluster().isolate(node);
  isolated_node = node;
  last_event = "Bidirectional partition isolated node " + std::to_string(node) + ".";
  return snapshot();
}

FAULTLINE_EXPORT const char* faultline_heal() {
  current_cluster().heal();
  isolated_node = 0;
  last_event = "Network healed; queued state converges through fresh Raft messages.";
  return snapshot();
}

FAULTLINE_EXPORT const char* faultline_propose(unsigned int transaction_id,
                                                const char* key,
                                                const char* value) {
  auto& active = current_cluster();
  const auto leader_before = active.leader();
  const auto commit_before = leader_before ? active.node(*leader_before).commit_index() : 0;
  Command command;
  command.transaction_id = transaction_id;
  command.commit_timestamp = Timestamp{logical_tick + transaction_id + 1, 0};
  command.mutations = {{key ? key : "key", std::optional<std::string>(value ? value : "value")}};
  const bool accepted = active.propose(std::move(command));
  const bool committed = accepted && leader_before &&
      active.node(*leader_before).commit_index() > commit_before;
  if (committed) {
    last_event = "Leader committed transaction " + std::to_string(transaction_id) + " through a majority.";
  } else if (accepted) {
    last_event = "Leader appended transaction " + std::to_string(transaction_id) + " locally, but no quorum committed it.";
  } else {
    last_event = "Transaction rejected because the cluster has no unique leader.";
  }
  return snapshot();
}

FAULTLINE_EXPORT const char* faultline_check_history(int stale_read) {
  const std::vector<HistoryOperation> history = stale_read
      ? std::vector<HistoryOperation>{
            {1, "register", OperationType::Write, 1, 2, "new", std::nullopt},
            {2, "register", OperationType::Read, 3, 4, std::nullopt, "old"},
        }
      : std::vector<HistoryOperation>{
            {1, "register", OperationType::Write, 1, 4, "A", std::nullopt},
            {2, "register", OperationType::Write, 2, 3, "B", std::nullopt},
            {3, "register", OperationType::Read, 5, 6, std::nullopt, "A"},
        };
  const auto result = raftmvcc::check_register_history(history);
  std::ostringstream out;
  out << "{\"linearizable\":" << (result.linearizable ? "true" : "false")
      << ",\"exploredStates\":" << result.explored_states << ",\"witness\":[";
  for (std::size_t index = 0; index < result.witness.size(); ++index) {
    if (index) out << ',';
    out << result.witness[index];
  }
  out << "]}";
  response = out.str();
  return response.c_str();
}

}  // extern "C"
