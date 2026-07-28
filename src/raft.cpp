#include "raftmvcc/raft.h"

#include <algorithm>
#include <stdexcept>

namespace raftmvcc {
namespace {

std::size_t quorum(std::size_t members) {
  return members / 2 + 1;
}

}  // namespace

RaftNode::RaftNode(NodeId id, std::vector<NodeId> members,
                   std::uint64_t election_timeout,
                   std::uint64_t heartbeat_interval)
    : id_(id),
      members_(std::move(members)),
      election_timeout_base_(election_timeout),
      election_timeout_(election_timeout),
      heartbeat_interval_(heartbeat_interval) {
  if (members_.empty() ||
      std::find(members_.begin(), members_.end(), id_) == members_.end()) {
    throw std::invalid_argument("Raft membership must contain this node");
  }
  if (election_timeout <= heartbeat_interval || heartbeat_interval == 0) {
    throw std::invalid_argument(
        "election timeout must exceed heartbeat interval");
  }
  std::sort(members_.begin(), members_.end());
  members_.erase(std::unique(members_.begin(), members_.end()),
                 members_.end());
  log_.push_back(LogEntry{});  // Sentinel at index zero.
  reset_election_timer();
}

std::uint64_t RaftNode::last_index() const noexcept {
  return log_.back().index;
}

std::uint64_t RaftNode::last_term() const noexcept {
  return log_.back().term;
}

void RaftNode::reset_election_timer() {
  election_elapsed_ = 0;
  const auto spread = std::max<std::uint64_t>(1, election_timeout_base_);
  election_timeout_ =
      election_timeout_base_ +
      (static_cast<std::uint64_t>(id_ * 17) + current_term_ * 7) % spread;
}

void RaftNode::tick() {
  if (role_ == Role::Leader) {
    ++heartbeat_elapsed_;
    if (heartbeat_elapsed_ >= heartbeat_interval_) {
      heartbeat_elapsed_ = 0;
      broadcast_append();
    }
    return;
  }
  ++election_elapsed_;
  if (election_elapsed_ >= election_timeout_) {
    campaign();
  }
}

void RaftNode::become_follower(std::uint64_t term,
                               std::optional<NodeId> leader) {
  role_ = Role::Follower;
  if (term > current_term_) {
    current_term_ = term;
    voted_for_.reset();
  }
  leader_id_ = leader;
  votes_.clear();
  heartbeat_elapsed_ = 0;
  reset_election_timer();
}

void RaftNode::campaign() {
  role_ = Role::Candidate;
  ++current_term_;
  leader_id_.reset();
  voted_for_ = id_;
  votes_ = {id_};
  reset_election_timer();
  if (votes_.size() >= quorum(members_.size())) {
    become_leader();
  } else {
    send_vote_requests();
  }
}

void RaftNode::send_vote_requests() {
  for (const auto peer : members_) {
    if (peer == id_) {
      continue;
    }
    Message request;
    request.type = MessageType::RequestVote;
    request.from = id_;
    request.to = peer;
    request.term = current_term_;
    request.prev_log_index = last_index();
    request.prev_log_term = last_term();
    outbox_.push_back(std::move(request));
  }
}

bool RaftNode::candidate_log_is_up_to_date(std::uint64_t index,
                                           std::uint64_t term) const {
  return term > last_term() || (term == last_term() && index >= last_index());
}

void RaftNode::become_leader() {
  role_ = Role::Leader;
  leader_id_ = id_;
  votes_.clear();
  heartbeat_elapsed_ = 0;

  const auto next = last_index() + 1;
  for (const auto peer : members_) {
    next_index_[peer] = next;
    match_index_[peer] = 0;
  }

  log_.push_back(
      LogEntry{last_index() + 1, current_term_, Command{}});
  match_index_[id_] = last_index();
  next_index_[id_] = last_index() + 1;
  advance_commit();
  broadcast_append();
}

void RaftNode::send_append(NodeId peer) {
  auto next = next_index_[peer];
  next = std::max<std::uint64_t>(1, next);
  next = std::min(next, last_index() + 1);

  Message append;
  append.type = MessageType::AppendEntries;
  append.from = id_;
  append.to = peer;
  append.term = current_term_;
  append.prev_log_index = next - 1;
  append.prev_log_term = log_.at(append.prev_log_index).term;
  append.leader_commit = commit_index_;
  for (auto index = next; index <= last_index(); ++index) {
    append.entries.push_back(log_.at(index));
  }
  outbox_.push_back(std::move(append));
}

void RaftNode::broadcast_append() {
  for (const auto peer : members_) {
    if (peer != id_) {
      send_append(peer);
    }
  }
}

void RaftNode::advance_commit() {
  const auto old_commit = commit_index_;
  for (auto candidate = last_index(); candidate > commit_index_;
       --candidate) {
    if (log_.at(candidate).term != current_term_) {
      continue;
    }
    std::size_t replicated = 0;
    for (const auto member : members_) {
      if (member == id_ || match_index_[member] >= candidate) {
        ++replicated;
      }
    }
    if (replicated >= quorum(members_.size())) {
      commit_index_ = candidate;
      break;
    }
  }
  if (commit_index_ != old_commit && role_ == Role::Leader) {
    broadcast_append();
  }
}

bool RaftNode::propose(Command command) {
  if (role_ != Role::Leader) {
    return false;
  }
  log_.push_back(LogEntry{last_index() + 1, current_term_,
                          std::move(command)});
  match_index_[id_] = last_index();
  next_index_[id_] = last_index() + 1;
  advance_commit();
  broadcast_append();
  return true;
}

void RaftNode::step(const Message& message) {
  if (message.term > current_term_) {
    become_follower(message.term, std::nullopt);
  }

  if (message.type == MessageType::RequestVote) {
    Message response;
    response.type = MessageType::VoteResponse;
    response.from = id_;
    response.to = message.from;
    response.term = current_term_;
    if (message.term == current_term_ &&
        (!voted_for_ || *voted_for_ == message.from) &&
        candidate_log_is_up_to_date(message.prev_log_index,
                                    message.prev_log_term)) {
      voted_for_ = message.from;
      response.accepted = true;
      reset_election_timer();
    }
    outbox_.push_back(std::move(response));
    return;
  }

  if (message.term < current_term_) {
    if (message.type == MessageType::AppendEntries) {
      Message response;
      response.type = MessageType::AppendResponse;
      response.from = id_;
      response.to = message.from;
      response.term = current_term_;
      response.accepted = false;
      response.match_index = last_index();
      outbox_.push_back(std::move(response));
    }
    return;
  }

  if (message.type == MessageType::VoteResponse) {
    if (role_ == Role::Candidate && message.term == current_term_ &&
        message.accepted) {
      votes_.insert(message.from);
      if (votes_.size() >= quorum(members_.size())) {
        become_leader();
      }
    }
    return;
  }

  if (message.type == MessageType::AppendEntries) {
    if (role_ != Role::Follower || leader_id_ != message.from) {
      become_follower(message.term, message.from);
    } else {
      leader_id_ = message.from;
      reset_election_timer();
    }

    Message response;
    response.type = MessageType::AppendResponse;
    response.from = id_;
    response.to = message.from;
    response.term = current_term_;
    if (message.prev_log_index > last_index() ||
        log_.at(message.prev_log_index).term != message.prev_log_term) {
      response.accepted = false;
      response.match_index =
          std::min(message.prev_log_index, last_index());
      outbox_.push_back(std::move(response));
      return;
    }

    for (const auto& entry : message.entries) {
      if (entry.index <= last_index() &&
          log_.at(entry.index).term != entry.term) {
        log_.resize(entry.index);
      }
      if (entry.index > last_index()) {
        log_.push_back(entry);
      }
    }
    commit_index_ = std::min(message.leader_commit, last_index());
    response.accepted = true;
    response.match_index = last_index();
    outbox_.push_back(std::move(response));
    return;
  }

  if (message.type == MessageType::AppendResponse &&
      role_ == Role::Leader) {
    if (message.accepted) {
      match_index_[message.from] =
          std::max(match_index_[message.from], message.match_index);
      next_index_[message.from] = match_index_[message.from] + 1;
      advance_commit();
    } else {
      auto& next = next_index_[message.from];
      next = std::max<std::uint64_t>(1, next - 1);
      send_append(message.from);
    }
  }
}

std::vector<Message> RaftNode::drain_outbox() {
  std::vector<Message> messages;
  messages.swap(outbox_);
  return messages;
}

Cluster::Cluster(std::size_t size) {
  if (size == 0) {
    throw std::invalid_argument("cluster size must be positive");
  }
  std::vector<NodeId> members;
  for (std::size_t index = 1; index <= size; ++index) {
    members.push_back(static_cast<NodeId>(index));
  }
  for (const auto id : members) {
    nodes_.emplace(id, RaftNode{id, members});
    stores_.emplace(id, MVCCStore{});
    applied_[id] = 0;
  }
}

bool Cluster::blocked(NodeId from, NodeId to) const {
  return blocked_.count({from, to}) != 0;
}

std::size_t Cluster::deliver() {
  std::size_t delivered = 0;
  for (std::size_t round = 0; round < 10000; ++round) {
    std::vector<Message> messages;
    for (auto& [unused, node] : nodes_) {
      (void)unused;
      auto pending = node.drain_outbox();
      messages.insert(messages.end(),
                      std::make_move_iterator(pending.begin()),
                      std::make_move_iterator(pending.end()));
    }
    if (messages.empty()) {
      apply_committed();
      return delivered;
    }
    for (const auto& message : messages) {
      if (blocked(message.from, message.to)) {
        ++dropped_messages_;
        continue;
      }
      nodes_.at(message.to).step(message);
      ++delivered;
    }
    apply_committed();
  }
  throw std::logic_error("message delivery failed to quiesce");
}

void Cluster::apply_committed() {
  for (auto& [id, node] : nodes_) {
    while (applied_[id] < node.commit_index()) {
      ++applied_[id];
      stores_.at(id).apply(node.log().at(applied_[id]).command);
    }
  }
}

void Cluster::tick(std::size_t count) {
  for (std::size_t iteration = 0; iteration < count; ++iteration) {
    for (auto& [unused, node] : nodes_) {
      (void)unused;
      node.tick();
    }
    deliver();
  }
}

void Cluster::isolate(NodeId node) {
  for (const auto& [peer, unused] : nodes_) {
    (void)unused;
    if (peer != node) {
      blocked_.insert({node, peer});
      blocked_.insert({peer, node});
    }
  }
}

void Cluster::partition(const std::set<NodeId>& left,
                        const std::set<NodeId>& right) {
  for (const auto source : left) {
    for (const auto target : right) {
      blocked_.insert({source, target});
      blocked_.insert({target, source});
    }
  }
}

void Cluster::heal() {
  blocked_.clear();
}

bool Cluster::campaign(NodeId node_id) {
  nodes_.at(node_id).campaign();
  deliver();
  return nodes_.at(node_id).role() == Role::Leader;
}

bool Cluster::propose(Command command) {
  const auto elected = leader();
  if (!elected) {
    return false;
  }
  const auto accepted = nodes_.at(*elected).propose(std::move(command));
  deliver();
  return accepted;
}

std::optional<NodeId> Cluster::leader() const {
  std::optional<NodeId> result;
  for (const auto& [id, node] : nodes_) {
    if (node.role() != Role::Leader) {
      continue;
    }
    if (result) {
      return std::nullopt;
    }
    result = id;
  }
  return result;
}

RaftNode& Cluster::node(NodeId id) {
  return nodes_.at(id);
}

const RaftNode& Cluster::node(NodeId id) const {
  return nodes_.at(id);
}

const MVCCStore& Cluster::store(NodeId id) const {
  return stores_.at(id);
}

}  // namespace raftmvcc
