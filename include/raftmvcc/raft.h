#pragma once

#include "raftmvcc/mvcc.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace raftmvcc {

using NodeId = int;

enum class Role { Follower, Candidate, Leader };
enum class MessageType {
  RequestVote,
  VoteResponse,
  AppendEntries,
  AppendResponse,
};

struct LogEntry {
  std::uint64_t index = 0;
  std::uint64_t term = 0;
  Command command;
};

struct Message {
  MessageType type = MessageType::RequestVote;
  NodeId from = 0;
  NodeId to = 0;
  std::uint64_t term = 0;
  bool accepted = false;
  std::uint64_t prev_log_index = 0;
  std::uint64_t prev_log_term = 0;
  std::uint64_t leader_commit = 0;
  std::uint64_t match_index = 0;
  std::vector<LogEntry> entries;
};

class RaftNode {
 public:
  RaftNode(NodeId id, std::vector<NodeId> members,
           std::uint64_t election_timeout = 10,
           std::uint64_t heartbeat_interval = 2);

  void tick();
  void campaign();
  void step(const Message& message);
  bool propose(Command command);
  std::vector<Message> drain_outbox();

  NodeId id() const noexcept { return id_; }
  Role role() const noexcept { return role_; }
  std::uint64_t term() const noexcept { return current_term_; }
  std::uint64_t commit_index() const noexcept { return commit_index_; }
  std::uint64_t last_index() const noexcept;
  std::uint64_t last_term() const noexcept;
  std::optional<NodeId> leader_id() const noexcept { return leader_id_; }
  const std::vector<LogEntry>& log() const noexcept { return log_; }

 private:
  void become_follower(std::uint64_t term, std::optional<NodeId> leader);
  void become_leader();
  void reset_election_timer();
  void send_vote_requests();
  void send_append(NodeId peer);
  void broadcast_append();
  void advance_commit();
  bool candidate_log_is_up_to_date(std::uint64_t index,
                                   std::uint64_t term) const;

  NodeId id_;
  std::vector<NodeId> members_;
  Role role_ = Role::Follower;
  std::uint64_t current_term_ = 0;
  std::optional<NodeId> voted_for_;
  std::optional<NodeId> leader_id_;
  std::vector<LogEntry> log_;
  std::uint64_t commit_index_ = 0;
  std::uint64_t election_elapsed_ = 0;
  std::uint64_t election_timeout_base_;
  std::uint64_t election_timeout_;
  std::uint64_t heartbeat_elapsed_ = 0;
  std::uint64_t heartbeat_interval_;
  std::set<NodeId> votes_;
  std::map<NodeId, std::uint64_t> next_index_;
  std::map<NodeId, std::uint64_t> match_index_;
  std::vector<Message> outbox_;
};

class Cluster {
 public:
  explicit Cluster(std::size_t size);

  void tick(std::size_t count = 1);
  std::size_t deliver();
  void isolate(NodeId node);
  void partition(const std::set<NodeId>& left,
                 const std::set<NodeId>& right);
  void heal();
  bool campaign(NodeId node);
  bool propose(Command command);

  std::optional<NodeId> leader() const;
  RaftNode& node(NodeId id);
  const RaftNode& node(NodeId id) const;
  const MVCCStore& store(NodeId id) const;
  std::size_t dropped_messages() const noexcept { return dropped_messages_; }

 private:
  bool blocked(NodeId from, NodeId to) const;
  void apply_committed();

  std::map<NodeId, RaftNode> nodes_;
  std::map<NodeId, MVCCStore> stores_;
  std::map<NodeId, std::uint64_t> applied_;
  std::set<std::pair<NodeId, NodeId>> blocked_;
  std::size_t dropped_messages_ = 0;
};

}  // namespace raftmvcc
