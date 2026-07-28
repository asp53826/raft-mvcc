#include "raftmvcc/raft.h"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

raftmvcc::Command command(std::uint64_t id) {
  return raftmvcc::Command{
      id, raftmvcc::Timestamp{id + 1, 0},
      {{"key-" + std::to_string(id % 256),
        std::optional<std::string>("value-" + std::to_string(id))}}};
}

double replication_benchmark(std::size_t nodes, std::uint64_t operations) {
  raftmvcc::Cluster cluster(nodes);
  if (!cluster.campaign(1)) {
    throw std::runtime_error("failed to elect benchmark leader");
  }
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t id = 1; id <= operations; ++id) {
    if (!cluster.propose(command(id))) {
      throw std::runtime_error("benchmark proposal failed");
    }
  }
  const auto elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start);
  return static_cast<double>(operations) / elapsed.count();
}

std::size_t failover_ticks() {
  raftmvcc::Cluster cluster(5);
  cluster.campaign(1);
  cluster.isolate(1);
  for (std::size_t ticks = 1; ticks <= 100; ++ticks) {
    cluster.tick();
    for (int id = 2; id <= 5; ++id) {
      if (cluster.node(id).role() == raftmvcc::Role::Leader) {
        return ticks;
      }
    }
  }
  throw std::runtime_error("failover did not elect a majority leader");
}

double snapshot_read_benchmark(std::uint64_t reads) {
  raftmvcc::MVCCStore store;
  for (std::uint64_t version = 1; version <= 10000; ++version) {
    store.apply(command(version));
  }
  std::uint64_t checksum = 0;
  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < reads; ++index) {
    const auto value = store.read("key-" + std::to_string(index % 256),
                                  raftmvcc::Timestamp{10001, 0});
    checksum += value ? value->size() : 0;
  }
  const auto elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start);
  if (checksum == 0) {
    throw std::runtime_error("read benchmark was optimized away");
  }
  return static_cast<double>(reads) / elapsed.count();
}

template <typename Function>
double median_of_five(Function&& function) {
  std::vector<double> samples;
  for (int trial = 0; trial < 5; ++trial) {
    samples.push_back(function());
  }
  std::sort(samples.begin(), samples.end());
  return samples[2];
}

}  // namespace

int main() {
  std::cout << std::fixed << std::setprecision(1);
  std::cout << "raft_3_node_ops_s="
            << median_of_five(
                   [] { return replication_benchmark(3, 20000); })
            << "\n";
  std::cout << "raft_5_node_ops_s="
            << median_of_five(
                   [] { return replication_benchmark(5, 20000); })
            << "\n";
  std::cout << "mvcc_snapshot_reads_s="
            << median_of_five(
                   [] { return snapshot_read_benchmark(1000000); })
            << "\n";
  std::cout << "five_node_failover_ticks=" << failover_ticks() << "\n";
  return 0;
}
