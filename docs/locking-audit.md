# Link Observatory - Deadlock and Lock Re-entrancy Audit

This document is the explicit audit required before release. It states the lock
order, every acquisition site, why no cycle exists, why no lock is held across a
blocking wait or a thread join, and how the property is proved mechanically
rather than asserted in prose.

## 1. Lock classes and the acquisition order

Every lock in the runtime is a `TrackedLock` or a `LockScope` marked with one
of five classes. The rank is a strict total order:

| Rank | `LockClass` | Guards |
| --- | --- | --- |
| 1 | `RuntimeLifecycle` | the worker pool's running/stopping state |
| 2 | `ObservatoryState` | all mutable runtime state: registry, evidence store, transition log, classifications, statistics |
| 3 | `IngestQueue` | a bounded queue's items, closure flag and counters |
| 4 | `Transport` | the server's statistics counters |
| 5 | `Diagnostics` | the one-shot batch completion latch |

The invariant enforced in tracking builds is:

> a thread may acquire a lock class only if its rank is strictly greater than the
> rank of every class that thread already holds, and a thread may never acquire
> the same class twice.

The rank order was chosen to match the only nesting the design actually needs.
Lifecycle operations close and drain queues, so lifecycle must rank below queue.
The state lock is never taken while a queue lock is held, so ranking it below
queues costs nothing and makes an accidental nesting an immediate report. The
completion latch can be signalled from any context, so it ranks highest.

## 2. Acquisition sites

### 2.1 `ObservatoryState` - `Observatory::state_lock_`

`TrackedLock state_lock_{LockClass::ObservatoryState}` in
`include/linkobs/runtime/observatory.hpp`. It is held for the whole duration of
each operation and released before any callback runs.

There are 28 acquisition sites in `src/runtime/observatory.cpp`. They fall into
four groups:

| Group | Methods |
| --- | --- |
| Lifecycle | `start`, `stop`, `started`, `persist`, `reload` |
| Ingest | `apply_batch`, `submit_batch` (started check), `apply_source`, `apply_link`, `apply_record_locked`'s callers, `refresh_classification_locked` |
| Queries | `links`, `sources`, `find_link`, `classify`, `classify_all`, `transitions`, `recent_transitions`, `evidence_for`, `metrics_for`, `slot_views`, `link_evidence`, `statistics`, `fence_count` |
| Reporting | `restored_from_snapshot`, `loaded_from_backup`, `load_detail`, `now` |

Three facts about this lock matter for the audit:

1. **It is never held together with another tracked lock.** The state lock is the
   only lock taken in the state path, because `LinkRegistry`, `EvidenceStore`
   and `TransitionLog` contain no mutex at all.
2. **It is released before any caller callback.** `apply_batch` applies records
   inside a `std::lock_guard` scope and calls `batch.on_applied()` after that
   scope has ended.
3. **It is never held across a wait or a join.** No code path takes the state lock
   and then waits on a condition variable, submits to a queue, or joins a thread.

### 2.2 `IngestQueue` - `BoundedQueue::lock_`

`TrackedLock lock_{LockClass::IngestQueue}` in
`include/linkobs/ingest/queue.hpp`, with 17 acquisition sites in
`src/ingest/queue.cpp` covering `push`, `try_push`, `pop`, `try_pop`,
`close`, `closed`, `size`, `high_water`, `discard_pending` and the five
counter accessors.

* `push` and `pop` are the only blocking operations. Both use
  `std::unique_lock<TrackedLock>` together with `condition_variable_any`, so
  the queue lock is **released** by the standard library while the thread is
  blocked and reacquired on wake.
* `push` waits for space or closure; `pop` waits for an item or for closure
  with an empty queue. Both loops re-test the predicate, so a spurious wakeup is
  harmless.
* Callers never hold a queue lock while calling into the observatory. The worker
  loop calls `pop`, the lock is released when `pop` returns, and only then is
  the handler invoked.
* `close` and `discard_pending` notify all waiters, which is what makes
  shutdown terminate every blocked producer and consumer.

### 2.3 `Transport` - `TransportServer::stats_lock_`

`TrackedLock stats_lock_{LockClass::Transport}`, 14 acquisition sites in
`src/transport/server.cpp`, every one a `std::lock_guard` around a single
counter update or a statistics snapshot. The lock is never held while a frame is
read, while a batch is submitted, or while the connection thread waits for a push
to be applied.

### 2.4 `Diagnostics` - `BatchCompletion::lock_`

`TrackedLock lock_{LockClass::Diagnostics}` in the one-shot latch. `signal`
takes it for the state flip and notifies after releasing it; `wait` takes it,
and `condition_variable_any::wait` releases it for the duration of the block;
`done` takes it for a single read. Because it ranks highest, acquiring it while
holding anything else would still be legal, which is why it cannot participate in
an order inversion.

### 2.5 `RuntimeLifecycle` - `WorkerPool::lifecycle_lock_`

`TrackedLock lifecycle_lock_{LockClass::RuntimeLifecycle}` guarding
`start`, `running`, `submit`, `try_submit`, `stop` and `cancel`.

This is the only place where two tracked locks are nested, and the nesting is
always `RuntimeLifecycle` (1) then `IngestQueue` (3): closing a queue and
discarding its pending items. It is a single, acyclic edge in the hold graph.

### 2.6 Locks that do not exist

`LinkRegistry`, `EvidenceStore` and `TransitionLog` contain **no**
synchronisation primitive. Their headers state the contract explicitly:

> the observatory owns it and holds the observatory lock for every access.

This is the ownership statement the whole audit rests on. The registry, the
evidence store and the transition log are not thread-safe on their own and are
never used that way: every one of their public methods is called from inside an
`Observatory` method with `state_lock_` held. There is consequently no second
lock in the state path, and therefore no lock that could be acquired in the
opposite order.

## 3. Why no cycle exists

The hold graph is defined by the edges that can actually occur:

    RuntimeLifecycle (1)  ->  IngestQueue (3)          [stop, cancel: close and discard]
    ObservatoryState (2)  ->  (nothing)
    IngestQueue (3)       ->  (nothing)
    Transport (4)         ->  (nothing)
    Diagnostics (5)       ->  (nothing)

There is exactly one edge, and it points from rank 1 to rank 3. No edge points
back towards a lower rank, so no cycle can be formed, regardless of how many
threads run.

Two further observations close the usual escape routes:

* **Re-entrancy.** The same class is never acquired twice on one thread. Queries
  do not call other queries; `apply_record_locked` is only reached from public
  methods that already hold the lock, and it does not re-acquire it. The tracker
  reports re-entrancy explicitly, so a future change that introduces it fails the
  concurrency suite rather than deadlocking a user.
* **Callback inversion.** No lock is held while a caller-supplied callback runs.
  The only callback in the system is `Batch::on_applied`, and both call sites
  (`apply_batch` and the routing path in `submit_batch`) invoke it outside the
  `lock_guard` scope.

## 4. The batch completion latch

The transport must not acknowledge a push before the records are in the store,
and the benchmark and the command line tool need the same signal. The mechanism
is a one-shot latch, `BatchCompletion`, plus a countdown in
`Observatory::submit_batch`.

* The batch is split into one declaration part (applied synchronously by the
  caller) and up to one part per routed source (submitted to workers).
* A shared `std::atomic<std::size_t>` is initialised to the number of parts.
* Each part gets `on_applied = arrive`, where `arrive` decrements the counter
  and, only when it reaches zero, invokes the caller's real completion.
* The worker invokes `on_applied` after its `apply_batch` has released the
  state lock; the caller invokes it after its own `apply_batch` has returned.

Why this cannot invert the lock order:

1. the arriving thread holds **no** lock when it signals, because the state lock
   is released before `on_applied` runs, and the queue lock was released when
   `pop` returned;
2. the waiting thread holds **no** lock while it waits, because `wait` releases
   the latch lock inside `condition_variable_any::wait`, and the server calls
   `completion->wait()` outside every statistics scope;
3. the dependency is one directional and acyclic: a waiter waits for work, and
   the work never waits for the waiter.

The latch has no timeout. Its wait ends when the work ends. Shutdown is still
guaranteed to terminate because every queue is closed and drained, which makes
every worker return, which makes every outstanding count reach zero.

## 5. Waits and joins

| Operation | Waits on | Holds while waiting | Termination |
| --- | --- | --- | --- |
| `BoundedQueue::push` | space or closure | nothing (queue lock released by `wait`) | `close` notifies all |
| `BoundedQueue::pop` | item or closure | nothing | `close` notifies all, so the loop exits when drained |
| `BatchCompletion::wait` | the countdown reaching zero | nothing | all parts always arrive, including on the error paths that signal directly |
| `WorkerPool::stop` | worker threads (join) | `RuntimeLifecycle` | queues closed first, so every worker's `pop` returns false and the handler returns |
| `WorkerPool::cancel` | worker threads (join) | `RuntimeLifecycle` | queues closed and drained first |
| `TransportServer::serve` | `accept()` | nothing | `request_stop` sets a flag and connects to the listener to unblock `accept` |
| `TransportServer::request_stop` | nothing | nothing | it only stores an atomic and performs a loopback connect |

The join in `stop` and `cancel` is safe because a worker never acquires
`RuntimeLifecycle`, so a worker can never be waiting for the lock the joining
thread holds. Closing every queue before joining is what guarantees that each
worker's blocking `pop` returns.

## 6. How the property is proved

The audit is not a reading exercise. Lock tracking is a compile-time feature,
`LINKOBS_LOCK_TRACKING`, defined publicly by the build in the Debug, Release
and AddressSanitizer configurations used for validation.

It detects and reports two conditions:

* **re-entrancy** - the same lock class acquired twice on one thread,
* **ordering** - a lock class acquired while a lower-ranked class is held,
* plus unbalanced release, which would otherwise silently corrupt the stack.

Reports are collected rather than thrown, so a test can run the whole suite and
assert the total. `lock_violation_count()` returns the number of violations,
`lock_violation_reports()` returns the bounded human-readable list, and
`reset_lock_tracking()` clears the count, the reports and the calling thread's
lock stack.

Two test suites use it:

* `tests/test_core.cpp` proves the tracker itself works: after a reset the count
  is zero and the report list is empty; a deliberately re-entrant acquisition
  raises the count; a deliberately out-of-order acquisition raises the count; a
  further reset returns both to zero.
* `tests/test_concurrency.cpp` resets the tracker immediately before driving
  concurrent ingest, concurrent queries, concurrent worker start and stop, and
  cancellation, then reads `lock_violation_reports()` into the failure message
  and asserts `lock_violation_count() == 0`.

A zero count after the concurrency suite, run with tracking compiled in, is the
mechanical evidence for everything argued above.

## 7. Concurrency model in one page

* Declarations (source, link) are applied synchronously by the calling thread.
* Observations are routed to a worker by a deterministic hash of the source name,
  so all records of one source are applied in submission order by one worker.
* Applications take the single state lock; the lock is coarse on purpose. There
  is no lock-free publication, no reader-writer scheme and no advisory protocol,
  because a second mechanism would be a second thing to get wrong.
* Queries take the same lock and return copies. No reference into runtime state
  escapes the lock.
* Cancellation closes every queue, discards and counts the pending batches, and
  joins. Nothing is left running and nothing is silently forgotten.
* There are no timeouts anywhere in the runtime. Waiting is bounded by work
  completion, and every wait ends because some other thread makes progress or
  because shutdown closes the resource.
