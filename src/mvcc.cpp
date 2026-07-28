#include "raftmvcc/mvcc.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace raftmvcc {

std::optional<std::string> Transaction::get(const MVCCStore& store,
                                            const std::string& key) {
  const auto version = store.visible_version(key, read_timestamp_);
  observations_.emplace(
      key, version ? std::optional<Timestamp>(version->timestamp) : std::nullopt);
  return version ? version->value : std::nullopt;
}

void Transaction::put(std::string key, std::string value) {
  writes_[std::move(key)] = std::move(value);
}

void Transaction::erase(std::string key) {
  writes_[std::move(key)] = std::nullopt;
}

void MVCCStore::apply(const Command& command) {
  if (command.mutations.empty()) {
    return;  // Raft leadership no-op.
  }
  for (const auto& mutation : command.mutations) {
    auto& chain = versions_[mutation.key];
    const Version version{command.commit_timestamp, command.transaction_id,
                          mutation.value};
    const auto position = std::lower_bound(
        chain.begin(), chain.end(), command.commit_timestamp,
        [](const Version& candidate, const Timestamp& timestamp) {
          return candidate.timestamp < timestamp;
        });
    if (position != chain.end() &&
        position->timestamp == command.commit_timestamp) {
      if (position->transaction_id != command.transaction_id ||
          position->value != mutation.value) {
        throw std::logic_error("conflicting MVCC version at one timestamp");
      }
      continue;
    }
    chain.insert(position, version);
  }
}

std::optional<Version> MVCCStore::visible_version(
    const std::string& key, Timestamp timestamp) const {
  const auto found = versions_.find(key);
  if (found == versions_.end()) {
    return std::nullopt;
  }
  const auto& chain = found->second;
  const auto position = std::upper_bound(
      chain.begin(), chain.end(), timestamp,
      [](const Timestamp& ts, const Version& candidate) {
        return ts < candidate.timestamp;
      });
  if (position == chain.begin()) {
    return std::nullopt;
  }
  return *std::prev(position);
}

std::optional<std::string> MVCCStore::read(const std::string& key,
                                           Timestamp timestamp) const {
  const auto version = visible_version(key, timestamp);
  return version ? version->value : std::nullopt;
}

std::map<std::string, std::string> MVCCStore::scan(
    const std::string& begin, const std::string& end,
    Timestamp timestamp) const {
  std::map<std::string, std::string> result;
  for (auto iterator = versions_.lower_bound(begin);
       iterator != versions_.end() && iterator->first < end; ++iterator) {
    const auto value = read(iterator->first, timestamp);
    if (value) {
      result.emplace(iterator->first, *value);
    }
  }
  return result;
}

std::optional<Command> MVCCStore::prepare_commit(
    const Transaction& transaction, Timestamp commit_timestamp) const {
  if (!(transaction.read_timestamp() < commit_timestamp)) {
    return std::nullopt;
  }

  std::map<std::string, std::optional<Timestamp>> expected =
      transaction.observations();
  for (const auto& [key, unused] : transaction.writes()) {
    (void)unused;
    if (expected.find(key) == expected.end()) {
      const auto visible = visible_version(key, transaction.read_timestamp());
      expected.emplace(
          key, visible ? std::optional<Timestamp>(visible->timestamp)
                       : std::nullopt);
    }
  }

  for (const auto& [key, observed] : expected) {
    const auto latest = visible_version(key, Timestamp{
                                                UINT64_MAX, UINT32_MAX});
    const auto current =
        latest ? std::optional<Timestamp>(latest->timestamp) : std::nullopt;
    if (current != observed) {
      return std::nullopt;
    }
  }

  Command command;
  command.transaction_id = transaction.id();
  command.commit_timestamp = commit_timestamp;
  for (const auto& [key, value] : transaction.writes()) {
    command.mutations.push_back(Mutation{key, value});
  }
  if (command.mutations.empty()) {
    return std::nullopt;
  }
  return command;
}

std::size_t MVCCStore::garbage_collect(Timestamp safe_point) {
  std::size_t removed = 0;
  for (auto iterator = versions_.begin(); iterator != versions_.end();) {
    auto& chain = iterator->second;
    const auto first_newer = std::upper_bound(
        chain.begin(), chain.end(), safe_point,
        [](const Timestamp& ts, const Version& version) {
          return ts < version.timestamp;
        });
    if (first_newer != chain.begin()) {
      const auto anchor = std::prev(first_newer);
      removed += static_cast<std::size_t>(std::distance(chain.begin(), anchor));
      chain.erase(chain.begin(), anchor);
    }
    if (chain.size() == 1 && !chain.front().value &&
        !(safe_point < chain.front().timestamp)) {
      chain.clear();
      ++removed;
    }
    if (chain.empty()) {
      iterator = versions_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  return removed;
}

std::size_t MVCCStore::version_count() const noexcept {
  std::size_t count = 0;
  for (const auto& [unused, chain] : versions_) {
    (void)unused;
    count += chain.size();
  }
  return count;
}

std::string MVCCStore::digest(Timestamp timestamp) const {
  std::ostringstream output;
  for (const auto& [key, unused] : versions_) {
    (void)unused;
    const auto value = read(key, timestamp);
    if (value) {
      output << key.size() << ':' << key << '=' << value->size() << ':'
             << *value << ';';
    }
  }
  return output.str();
}

}  // namespace raftmvcc
