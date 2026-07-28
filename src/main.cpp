#include "raftmvcc/raft.h"

#include <iostream>
#include <optional>
#include <string>

int main() {
  using namespace raftmvcc;

  Cluster cluster(3);
  if (!cluster.campaign(1)) {
    std::cerr << "leader election failed\n";
    return 1;
  }

  Command transfer;
  transfer.transaction_id = 1;
  transfer.commit_timestamp = Timestamp{100, 0};
  transfer.mutations = {
      {"account/alice", std::optional<std::string>("90")},
      {"account/bob", std::optional<std::string>("110")},
  };
  if (!cluster.propose(std::move(transfer))) {
    std::cerr << "proposal failed\n";
    return 1;
  }

  std::cout << "leader=node-" << *cluster.leader() << "\n";
  for (int node = 1; node <= 3; ++node) {
    std::cout << "node-" << node
              << " term=" << cluster.node(node).term()
              << " commit=" << cluster.node(node).commit_index()
              << " state=\""
              << cluster.store(node).digest(Timestamp{100, 0}) << "\"\n";
  }
  return 0;
}
