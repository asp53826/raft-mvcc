#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace raftmvcc {

struct Timestamp {
  std::uint64_t physical = 0;
  std::uint32_t logical = 0;

  friend bool operator==(const Timestamp& a, const Timestamp& b) {
    return a.physical == b.physical && a.logical == b.logical;
  }
  friend bool operator!=(const Timestamp& a, const Timestamp& b) {
    return !(a == b);
  }
  friend bool operator<(const Timestamp& a, const Timestamp& b) {
    return a.physical < b.physical ||
           (a.physical == b.physical && a.logical < b.logical);
  }
  friend bool operator<=(const Timestamp& a, const Timestamp& b) {
    return a < b || a == b;
  }
  friend bool operator>(const Timestamp& a, const Timestamp& b) {
    return b < a;
  }
};

struct Mutation {
  std::string key;
  std::optional<std::string> value;
};

struct Command {
  std::uint64_t transaction_id = 0;
  Timestamp commit_timestamp;
  std::vector<Mutation> mutations;
};

struct Version {
  Timestamp timestamp;
  std::uint64_t transaction_id = 0;
  std::optional<std::string> value;
};

class MVCCStore;

class Transaction {
 public:
  Transaction(std::uint64_t id, Timestamp read_timestamp)
      : id_(id), read_timestamp_(read_timestamp) {}

  std::optional<std::string> get(const MVCCStore& store,
                                 const std::string& key);
  void put(std::string key, std::string value);
  void erase(std::string key);

  std::uint64_t id() const noexcept { return id_; }
  Timestamp read_timestamp() const noexcept { return read_timestamp_; }
  const std::map<std::string, std::optional<Timestamp>>& observations() const {
    return observations_;
  }
  const std::map<std::string, std::optional<std::string>>& writes() const {
    return writes_;
  }

 private:
  std::uint64_t id_;
  Timestamp read_timestamp_;
  std::map<std::string, std::optional<Timestamp>> observations_;
  std::map<std::string, std::optional<std::string>> writes_;
};

class MVCCStore {
 public:
  void apply(const Command& command);
  std::optional<std::string> read(const std::string& key,
                                  Timestamp timestamp) const;
  std::optional<Version> visible_version(const std::string& key,
                                         Timestamp timestamp) const;
  std::map<std::string, std::string> scan(const std::string& begin,
                                         const std::string& end,
                                         Timestamp timestamp) const;

  std::optional<Command> prepare_commit(const Transaction& transaction,
                                        Timestamp commit_timestamp) const;
  std::size_t garbage_collect(Timestamp safe_point);
  std::size_t version_count() const noexcept;
  std::string digest(Timestamp timestamp) const;

 private:
  std::map<std::string, std::vector<Version>> versions_;
};

}  // namespace raftmvcc
