# Design notes

## Scope

The project composes three independently testable layers:

1. a Raft state machine;
2. a deterministic transport and failure simulator; and
3. an MVCC state machine applied only through committed log indices.

The separation is intentional. Raft code does not call the MVCC store. The
cluster owns `applied[node]` and advances it monotonically to each node's
`commitIndex`. That makes it possible to test the protocol log independently
from application semantics.

## Raft state

Every node keeps:

```text
currentTerm, votedFor
log[index] = (term, command)
commitIndex
role, leaderId
electionElapsed, randomizedElectionDeadline

leader only:
  nextIndex[peer]
  matchIndex[peer]
```

`log[0]` is a sentinel with term zero. All externally meaningful entries start
at one.

## State transitions

### Follower to candidate

When the election deadline expires, the node:

1. increments `currentTerm`;
2. votes for itself;
3. clears the known leader;
4. resets its deterministic randomized deadline; and
5. sends the last log term/index to every peer.

### Candidate to leader

The candidate becomes leader after a majority of accepted votes in the same
term. It initializes every follower's `nextIndex` to one past its last entry,
appends a no-op in its term, and broadcasts `AppendEntries`.

The no-op gives the leader a current-term entry it can use to commit inherited
entries without waiting for the first client command.

### Any role to follower

Any message with a greater term first moves the receiver to follower, clears
its vote, and resets the election timer. A same-term `AppendEntries` also
demotes a candidate or competing leader.

## Log repair

The follower rejects an append if `prevLogIndex` is absent or its term differs.
The leader decrements `nextIndex` and retries. Once the prefix matches, the
follower scans new entries:

- equal index and equal term: retain the existing entry;
- equal index and different term: truncate from that index;
- missing index: append.

Committed entries are never supposed to be truncated. Raft's election
restriction and quorum intersection are what make that property hold; the
vector operation alone does not.

## Commit rule

For candidate index `N`, the leader counts replicas with `matchIndex >= N`.
It advances `commitIndex` only when:

```text
replicas >= floor(cluster_size / 2) + 1
and log[N].term == currentTerm
```

The current-term predicate is essential. Counting replicas alone can incorrectly
commit an entry from a prior term in the Figure 8 scenario from the extended
Raft paper.

Followers advance to:

```text
min(leaderCommit, followerLastIndex)
```

and the cluster applies entries in increasing index order exactly once.

## MVCC visibility

Versions are ordered lexicographically by `(physical, logical)` timestamp.
For a read at `T`, `upper_bound(T)` finds the first version after the snapshot;
the prior version is visible. A visible tombstone returns absence but remains
part of the version history.

## OCC validation

A transaction records the timestamp of every version it reads. Before creating
a replicated command, validation compares those observations—and the visible
versions of write-only keys—to the store's latest committed versions.

This detects:

- write/write conflicts;
- lost updates;
- a changed predicate input that was directly read; and
- read/write conflicts on point keys.

It does not detect phantoms for arbitrary range predicates. Production
serializable systems need range latches, predicate locks, timestamp caches, or
another mechanism for that case.

## Garbage collection

For each version chain and safe point `S`, GC retains:

- the newest version at or below `S`; and
- every version newer than `S`.

Readers at or after `S` therefore preserve their anchor. If the only retained
version is a tombstone at or below `S`, the key can be removed entirely.

## Linearizability search

For each key, a DFS chooses an unlinearized operation only after every
real-time predecessor has been chosen. Writes replace the register state.
Reads are legal only when their recorded result matches the current state.

Failed states are memoized by:

```text
(bitset of chosen operations, current register value)
```

Partitioning by key is valid for independent read/write registers and reduces
the state space substantially. It is not a generic transaction-history checker.

## Determinism

There are no sleeps in the protocol tests. Election timeout jitter is a pure
function of node ID and term, messages are delivered in stable node order, and
the randomized failure campaign uses a fixed seed. Reproducibility is a design
property, not a best effort.
