#include "raftmvcc/linearizability.h"
#include "raftmvcc/mvcc.h"
#include "raftmvcc/raft.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <string>

namespace {

int failures = 0;
std::uint64_t assertions = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    ++assertions;                                                            \
    if (!(condition)) {                                                      \
      ++failures;                                                            \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: "       \
                << #condition << "\n";                                      \
    }                                                                        \
  } while (false)

raftmvcc::Timestamp ts(std::uint64_t physical,
                       std::uint32_t logical = 0) {
  return raftmvcc::Timestamp{physical, logical};
}

raftmvcc::Command put(std::uint64_t transaction_id,
                      std::uint64_t timestamp, std::string key,
                      std::string value) {
  return raftmvcc::Command{
      transaction_id, ts(timestamp),
      {{std::move(key), std::optional<std::string>(std::move(value))}}};
}

raftmvcc::Command erase(std::uint64_t transaction_id,
                        std::uint64_t timestamp, std::string key) {
  return raftmvcc::Command{transaction_id, ts(timestamp),
                           {{std::move(key), std::nullopt}}};
}

void test_mvcc_time_travel_and_tombstones() {
  raftmvcc::MVCCStore store;
  store.apply(put(1, 10, "alpha", "v1"));
  store.apply(put(2, 30, "alpha", "v3"));
  store.apply(put(3, 20, "alpha", "v2"));  // Deliberately out of order.

  CHECK(!store.read("alpha", ts(9)));
  CHECK(store.read("alpha", ts(10)) == std::optional<std::string>("v1"));
  CHECK(store.read("alpha", ts(29)) == std::optional<std::string>("v2"));
  CHECK(store.read("alpha", ts(30)) == std::optional<std::string>("v3"));

  store.apply(erase(4, 40, "alpha"));
  CHECK(store.read("alpha", ts(39)) == std::optional<std::string>("v3"));
  CHECK(!store.read("alpha", ts(40)));
  CHECK(!store.read("alpha", ts(100)));
}

void test_mvcc_atomic_batch_and_scan() {
  raftmvcc::MVCCStore store;
  raftmvcc::Command batch;
  batch.transaction_id = 7;
  batch.commit_timestamp = ts(50, 2);
  batch.mutations = {{"a", "one"}, {"b", "two"}, {"c", "three"}};
  store.apply(batch);

  const auto snapshot = store.scan("a", "d", ts(50, 2));
  CHECK(snapshot.size() == 3);
  CHECK(snapshot.at("a") == "one");
  CHECK(snapshot.at("b") == "two");
  CHECK(snapshot.at("c") == "three");
  CHECK(store.digest(ts(50, 2)) ==
        "1:a=3:one;1:b=3:two;1:c=5:three;");
}

void test_serializable_optimistic_validation() {
  raftmvcc::MVCCStore store;
  store.apply(put(1, 10, "balance", "100"));

  raftmvcc::Transaction first(2, ts(10));
  raftmvcc::Transaction second(3, ts(10));
  CHECK(first.get(store, "balance") == std::optional<std::string>("100"));
  CHECK(second.get(store, "balance") == std::optional<std::string>("100"));
  first.put("balance", "90");
  second.put("balance", "80");

  const auto first_commit = store.prepare_commit(first, ts(11));
  CHECK(first_commit.has_value());
  store.apply(*first_commit);
  CHECK(store.read("balance", ts(11)) == std::optional<std::string>("90"));
  CHECK(!store.prepare_commit(second, ts(12)).has_value());
}

void test_read_write_conflict_is_detected() {
  raftmvcc::MVCCStore store;
  store.apply(put(1, 1, "left", "L0"));
  store.apply(put(2, 1, "right", "R0"));

  raftmvcc::Transaction transaction(3, ts(1));
  CHECK(transaction.get(store, "left") == std::optional<std::string>("L0"));
  transaction.put("right", "R1");

  store.apply(put(4, 2, "left", "L1"));
  CHECK(!store.prepare_commit(transaction, ts(3)).has_value());
}

void test_gc_preserves_safe_point_anchor() {
  raftmvcc::MVCCStore store;
  for (std::uint64_t time = 1; time <= 6; ++time) {
    store.apply(put(time, time, "key", "v" + std::to_string(time)));
  }
  CHECK(store.version_count() == 6);
  CHECK(store.garbage_collect(ts(4)) == 3);
  CHECK(store.version_count() == 3);
  CHECK(store.read("key", ts(4)) == std::optional<std::string>("v4"));
  CHECK(store.read("key", ts(6)) == std::optional<std::string>("v6"));
}

void test_linearizability_checker_finds_witness() {
  using raftmvcc::HistoryOperation;
  using raftmvcc::OperationType;
  const std::vector<HistoryOperation> history = {
      {1, "register", OperationType::Write, 1, 4, "A", std::nullopt},
      {2, "register", OperationType::Write, 2, 3, "B", std::nullopt},
      {3, "register", OperationType::Read, 5, 6, std::nullopt, "A"},
      {4, "other", OperationType::Read, 1, 2, std::nullopt, std::nullopt},
  };
  const auto result = raftmvcc::check_register_history(history);
  CHECK(result.linearizable);
  CHECK(result.witness.size() == history.size());
  CHECK(result.explored_states >= history.size());
  CHECK(std::find(result.witness.begin(), result.witness.end(), 4) <
        std::find(result.witness.begin(), result.witness.end(), 3));
}

void test_linearizability_checker_rejects_stale_read() {
  using raftmvcc::HistoryOperation;
  using raftmvcc::OperationType;
  const std::vector<HistoryOperation> history = {
      {1, "register", OperationType::Write, 1, 2, "new", std::nullopt},
      {2, "register", OperationType::Read, 3, 4, std::nullopt, std::nullopt},
  };
  const auto result = raftmvcc::check_register_history(history);
  CHECK(!result.linearizable);
  CHECK(result.witness.empty());
}

void test_committed_history_is_linearizable() {
  using raftmvcc::HistoryOperation;
  using raftmvcc::OperationType;
  raftmvcc::Cluster cluster(3);
  CHECK(cluster.campaign(1));
  std::vector<HistoryOperation> history;
  std::uint64_t time = 1;
  for (std::uint64_t index = 1; index <= 20; ++index) {
    const auto value = "value-" + std::to_string(index);
    const auto invoke = time++;
    CHECK(cluster.propose(put(index, index, "register", value)));
    history.push_back(
        {index * 2, "register", OperationType::Write, invoke, time++,
         value, std::nullopt});
    const auto read_invoke = time++;
    const auto observed = cluster.store(1).read("register", ts(index));
    history.push_back({index * 2 + 1, "register", OperationType::Read,
                       read_invoke, time++, std::nullopt, observed});
  }
  const auto result = raftmvcc::check_register_history(history);
  CHECK(result.linearizable);
  CHECK(result.witness.size() == history.size());
}

void test_election_and_replication() {
  raftmvcc::Cluster cluster(3);
  CHECK(cluster.campaign(1));
  CHECK(cluster.leader() == std::optional<raftmvcc::NodeId>(1));
  CHECK(cluster.propose(put(1, 10, "answer", "42")));

  for (int id = 1; id <= 3; ++id) {
    CHECK(cluster.node(id).commit_index() >= 2);
    CHECK(cluster.store(id).read("answer", ts(10)) ==
          std::optional<std::string>("42"));
    CHECK(cluster.node(id).log().size() ==
          cluster.node(1).log().size());
  }
}

void test_minority_leader_cannot_commit() {
  raftmvcc::Cluster cluster(3);
  CHECK(cluster.campaign(1));
  const auto committed_before = cluster.node(1).commit_index();
  cluster.isolate(1);
  CHECK(cluster.node(1).propose(put(1, 10, "unsafe", "minority")));
  cluster.deliver();
  CHECK(cluster.node(1).commit_index() == committed_before);
  CHECK(!cluster.store(1).read("unsafe", ts(100)));

  CHECK(cluster.campaign(2));
  CHECK(cluster.node(2).propose(put(2, 20, "safe", "majority")));
  cluster.deliver();
  CHECK(cluster.store(2).read("safe", ts(20)) ==
        std::optional<std::string>("majority"));
  CHECK(cluster.store(3).read("safe", ts(20)) ==
        std::optional<std::string>("majority"));
  CHECK(!cluster.store(1).read("safe", ts(20)));
}

void test_old_leader_conflict_is_overwritten() {
  raftmvcc::Cluster cluster(3);
  CHECK(cluster.campaign(1));
  CHECK(cluster.propose(put(1, 10, "key", "base")));

  cluster.isolate(1);
  CHECK(cluster.node(1).propose(put(2, 20, "key", "stale")));
  cluster.deliver();
  CHECK(cluster.campaign(2));
  CHECK(cluster.node(2).propose(put(3, 30, "key", "fresh")));
  cluster.deliver();
  CHECK(cluster.store(2).read("key", ts(100)) ==
        std::optional<std::string>("fresh"));

  cluster.heal();
  cluster.tick(2);
  CHECK(cluster.leader() == std::optional<raftmvcc::NodeId>(2));
  for (int id = 1; id <= 3; ++id) {
    CHECK(cluster.store(id).read("key", ts(100)) ==
          std::optional<std::string>("fresh"));
    CHECK(cluster.node(id).log().size() ==
          cluster.node(2).log().size());
    for (std::size_t index = 0; index < cluster.node(2).log().size();
         ++index) {
      CHECK(cluster.node(id).log().at(index).term ==
            cluster.node(2).log().at(index).term);
    }
  }
}

void test_stale_candidate_cannot_win() {
  raftmvcc::Cluster cluster(3);
  CHECK(cluster.campaign(1));
  cluster.isolate(3);
  CHECK(cluster.node(1).propose(put(1, 1, "x", "new")));
  cluster.deliver();
  const auto fresh_last_index = cluster.node(2).last_index();
  CHECK(cluster.node(3).last_index() < fresh_last_index);

  cluster.heal();
  cluster.isolate(1);
  cluster.node(3).campaign();
  cluster.deliver();
  CHECK(cluster.node(3).role() != raftmvcc::Role::Leader);
}

void test_randomized_failover_converges() {
  raftmvcc::Cluster cluster(5);
  CHECK(cluster.campaign(1));
  std::mt19937 generator(53826);
  std::uint64_t transaction = 1;

  for (int round = 0; round < 120; ++round) {
    const auto isolated = 3 + static_cast<int>(generator() % 3);
    cluster.isolate(isolated);
    for (int write = 0; write < 4; ++write) {
      const auto key = "k" + std::to_string(generator() % 17);
      const auto value = "r" + std::to_string(round) + "-" +
                         std::to_string(write);
      CHECK(cluster.propose(
          put(transaction, transaction + 100, key, value)));
      ++transaction;
    }
    cluster.heal();
    cluster.tick(2);
  }

  const auto timestamp = ts(UINT64_MAX, UINT32_MAX);
  const auto reference = cluster.store(1).digest(timestamp);
  for (int id = 1; id <= 5; ++id) {
    CHECK(cluster.store(id).digest(timestamp) == reference);
    CHECK(cluster.node(id).commit_index() ==
          cluster.node(1).commit_index());
  }
  CHECK(cluster.dropped_messages() > 0);
}

}  // namespace

int main() {
  test_mvcc_time_travel_and_tombstones();
  test_mvcc_atomic_batch_and_scan();
  test_serializable_optimistic_validation();
  test_read_write_conflict_is_detected();
  test_gc_preserves_safe_point_anchor();
  test_linearizability_checker_finds_witness();
  test_linearizability_checker_rejects_stale_read();
  test_committed_history_is_linearizable();
  test_election_and_replication();
  test_minority_leader_cannot_commit();
  test_old_leader_conflict_is_overwritten();
  test_stale_candidate_cannot_win();
  test_randomized_failover_converges();

  if (failures != 0) {
    std::cerr << failures << " failures across " << assertions
              << " assertions\n";
    return 1;
  }
  std::cout << "All " << assertions << " assertions passed\n";
  return 0;
}
