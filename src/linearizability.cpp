#include "raftmvcc/linearizability.h"

#include <algorithm>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace raftmvcc {
namespace {

struct ComponentResult {
  bool success = false;
  std::vector<std::uint64_t> witness;
  std::size_t explored = 0;
};

std::string memo_key(std::uint64_t chosen,
                     const std::optional<std::string>& value) {
  std::ostringstream output;
  output << chosen << ':';
  if (value) {
    output << value->size() << ':' << *value;
  } else {
    output << '-';
  }
  return output.str();
}

bool search_component(const std::vector<HistoryOperation>& operations,
                      std::uint64_t chosen,
                      const std::optional<std::string>& value,
                      std::unordered_set<std::string>& dead,
                      std::vector<std::uint64_t>& witness,
                      std::size_t& explored) {
  ++explored;
  const std::uint64_t complete_mask =
      operations.size() == 64
          ? ~std::uint64_t{0}
          : (std::uint64_t{1} << operations.size()) - 1;
  if (chosen == complete_mask) {
    return true;
  }

  const auto state_key = memo_key(chosen, value);
  if (dead.count(state_key) != 0) {
    return false;
  }

  for (std::size_t candidate = 0; candidate < operations.size();
       ++candidate) {
    const auto bit = std::uint64_t{1} << candidate;
    if ((chosen & bit) != 0) {
      continue;
    }
    const auto& operation = operations[candidate];

    bool predecessor_missing = false;
    for (std::size_t other = 0; other < operations.size(); ++other) {
      if ((chosen & (std::uint64_t{1} << other)) == 0 &&
          operations[other].complete_time < operation.invoke_time) {
        predecessor_missing = true;
        break;
      }
    }
    if (predecessor_missing) {
      continue;
    }

    auto next_value = value;
    if (operation.type == OperationType::Write) {
      if (!operation.argument) {
        throw std::invalid_argument("write operation requires an argument");
      }
      next_value = operation.argument;
    } else if (operation.result != value) {
      continue;
    }

    witness.push_back(operation.id);
    if (search_component(operations, chosen | bit, next_value, dead,
                         witness, explored)) {
      return true;
    }
    witness.pop_back();
  }

  dead.insert(state_key);
  return false;
}

ComponentResult check_component(
    std::vector<HistoryOperation> operations) {
  if (operations.size() > 63) {
    throw std::invalid_argument(
        "a register component is limited to 63 operations");
  }
  std::sort(operations.begin(), operations.end(),
            [](const HistoryOperation& left,
               const HistoryOperation& right) {
              return left.id < right.id;
            });
  std::unordered_set<std::string> dead;
  ComponentResult result;
  result.success = search_component(operations, 0, std::nullopt, dead,
                                    result.witness, result.explored);
  return result;
}

}  // namespace

LinearizabilityResult check_register_history(
    const std::vector<HistoryOperation>& history) {
  std::map<std::string, std::vector<HistoryOperation>> components;
  std::set<std::uint64_t> ids;
  for (const auto& operation : history) {
    if (operation.complete_time < operation.invoke_time) {
      throw std::invalid_argument("operation completes before invocation");
    }
    if (!ids.insert(operation.id).second) {
      throw std::invalid_argument("operation ids must be unique");
    }
    components[operation.key].push_back(operation);
  }

  LinearizabilityResult result;
  result.linearizable = true;
  std::vector<std::vector<std::uint64_t>> component_witnesses;
  for (const auto& [unused, component] : components) {
    (void)unused;
    const auto checked = check_component(component);
    result.explored_states += checked.explored;
    if (!checked.success) {
      result.linearizable = false;
      result.witness.clear();
      return result;
    }
    component_witnesses.push_back(checked.witness);
  }

  std::map<std::uint64_t, std::size_t> position;
  for (std::size_t index = 0; index < history.size(); ++index) {
    position.emplace(history[index].id, index);
  }
  std::vector<std::set<std::size_t>> edges(history.size());
  std::vector<std::size_t> indegree(history.size(), 0);
  const auto add_edge = [&](std::size_t from, std::size_t to) {
    if (from != to && edges[from].insert(to).second) {
      ++indegree[to];
    }
  };
  for (std::size_t left = 0; left < history.size(); ++left) {
    for (std::size_t right = 0; right < history.size(); ++right) {
      if (history[left].complete_time < history[right].invoke_time) {
        add_edge(left, right);
      }
    }
  }
  for (const auto& witness : component_witnesses) {
    for (std::size_t index = 1; index < witness.size(); ++index) {
      add_edge(position.at(witness[index - 1]), position.at(witness[index]));
    }
  }

  using Ready = std::pair<std::uint64_t, std::size_t>;
  std::priority_queue<Ready, std::vector<Ready>, std::greater<Ready>> ready;
  for (std::size_t index = 0; index < history.size(); ++index) {
    if (indegree[index] == 0) {
      ready.push({history[index].id, index});
    }
  }
  while (!ready.empty()) {
    const auto [unused, index] = ready.top();
    (void)unused;
    ready.pop();
    result.witness.push_back(history[index].id);
    for (const auto target : edges[index]) {
      if (--indegree[target] == 0) {
        ready.push({history[target].id, target});
      }
    }
  }
  if (result.witness.size() != history.size()) {
    result.linearizable = false;
    result.witness.clear();
  }
  return result;
}

}  // namespace raftmvcc
