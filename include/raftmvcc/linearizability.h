#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace raftmvcc {

enum class OperationType { Read, Write };

struct HistoryOperation {
  std::uint64_t id = 0;
  std::string key;
  OperationType type = OperationType::Read;
  std::uint64_t invoke_time = 0;
  std::uint64_t complete_time = 0;
  std::optional<std::string> argument;
  std::optional<std::string> result;
};

struct LinearizabilityResult {
  bool linearizable = false;
  std::vector<std::uint64_t> witness;
  std::size_t explored_states = 0;
};

// Checks independent read/write registers using Herlihy-Wing real-time order.
// Histories are decomposed by key (the register locality property), then each
// component is searched with memoization. A witness is returned on success.
LinearizabilityResult check_register_history(
    const std::vector<HistoryOperation>& history);

}  // namespace raftmvcc
