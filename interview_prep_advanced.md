# Distributed Key-Value Store: Advanced Interview Preparation (100 Questions)

This document is a comprehensive deep-dive into every aspect of the distributed key-value store we built, covering the original architecture **plus** the advanced features: **Key Migration**, **Read Repair**, **Anti-Entropy**, and **Lamport Clock Versioning**. Designed for senior-level distributed systems interviews.

---

## Section 1: Data Structures & Storage Engine (Q1–Q15)

**Q1: Why did you choose a B-Tree for on-disk storage instead of an LSM-Tree (like LevelDB/RocksDB)?**
> **Answer:** A B-Tree is optimized for **read-heavy** workloads because lookups are O(log N) with minimal disk seeks — you walk the tree directly to the key. An LSM-Tree is optimized for **write-heavy** workloads because writes go to a sorted in-memory buffer (memtable) and only flush to disk periodically. We chose the B-Tree because our system is a cache-first KV store where most reads hit the LRU cache, and the B-Tree is simpler to implement correctly for a learning project. In production, Redis itself uses a hash table (in-memory), and systems like Cassandra use LSM-Trees.

**Q2: What is the time complexity of your B-Tree operations?**
> **Answer:** For a B-Tree of order `t` (our `MAX_KEYS = 3`, so `t = 2`), with `N` keys:
> - **Search:** O(t · log_t(N)) — at each node, we do a linear scan of up to `t-1` keys, and the tree has height log_t(N).
> - **Insert:** O(t · log_t(N)) — same traversal, plus possible splits which are O(t) each.
> - **Delete (soft):** O(t · log_t(N)) — same traversal, we just flip `is_deleted`.
> - **`get_all_entries()`:** O(N) — in-order traversal visits every non-deleted key exactly once.

**Q3: Your B-Tree uses soft deletes (`is_deleted` flag). What are the trade-offs vs. hard deletes?**
> **Answer:** **Soft deletes** are simple — you just mark a key as deleted without restructuring the tree. This avoids the complex merge/rebalance operations that B-Tree hard deletes require (borrowing from siblings, merging nodes). The downside is **space waste** — deleted keys still occupy slots in the node, and their values still exist in `database.dat`. Over time, this causes "bloat." In production, you'd run periodic compaction to reclaim space.

**Q4: Explain how `_split_child` works and why it's necessary.**
> **Answer:** When a B-Tree node reaches `MAX_KEYS` (3 keys), it can't accept another key. `_split_child` takes a full child node, splits it into two halves, and promotes the **median key** up to the parent. Specifically: (1) The right half of the child's keys are moved to a new node. (2) The median key is inserted into the parent at position `i`. (3) The parent's child pointer at `i+1` is set to the new node. This maintains the B-Tree invariant that all nodes have between `t-1` and `2t-1` keys (except the root). With `MAX_KEYS=3`, splits happen frequently, which is great for testing correctness.

**Q5: How does your B-Tree handle duplicate keys?**
> **Answer:** In `_insert_non_full`, before inserting, we scan the node for an existing key match. If found, we **update in place** — overwriting the `value_offset`, `value_length`, and `version`. With Lamport clock versioning, we only overwrite if the incoming version >= the existing version. The old value bytes in `database.dat` become dead space (append-only file).

**Q6: How does the LRU cache work? What data structures back it?**
> **Answer:** The LRU cache uses two data structures working together:
> 1. **`std::list<std::string>` (doubly-linked list):** Maintains access order. The front is most-recently-used, the back is least-recently-used.
> 2. **`std::unordered_map`:** Maps key → `{Entry, list::iterator}`. The iterator points directly into the list, enabling O(1) promotion.
>
> On **GET**: splice the key's list node to the front → O(1).
> On **SET**: if key exists, splice to front. If not, push_front. If cache is full, evict from back (pop_back + erase from map) → O(1).
> On **Cache Miss**: read from B-Tree on disk, then insert into LRU cache.

**Q7: Why do you use `std::list::splice` instead of erase + push_front?**
> **Answer:** `splice` is O(1) and **moves** a node within the list without allocating or deallocating memory. `erase + push_front` would destroy the node and create a new one, which is slower and invalidates all stored iterators pointing to that node. Since we store the iterator in the map, invalidation would be catastrophic.

**Q8: Your `database.dat` file is append-only. How would you implement compaction?**
> **Answer:** A background compaction thread would: (1) Lock the database (or use a snapshot). (2) Create a new `database_compacted.dat` file. (3) Traverse the entire B-Tree via `get_all_entries()`. (4) For each live key, write its value to the new file and record the new offset. (5) Update all `value_offset` pointers in the B-Tree nodes. (6) Atomically rename the compacted file to `database.dat`. (7) Delete the old file. This reclaims all dead space from updates and deletes.

**Q9: What happens if the server crashes mid-write to `btree.idx`?**
> **Answer:** Currently, the B-Tree index can be **corrupted** — a partial node write leaves the file in an inconsistent state. For durability, we should implement a **Write-Ahead Log (WAL/AOF)**: before any B-Tree mutation, append the operation to a log file and `fsync()`. On crash recovery, replay the log to rebuild state. Alternatively, use copy-on-write B-Trees (like LMDB) where new nodes are written to new locations and the root pointer is updated atomically.

**Q10: Why is `BTreeNode` a fixed-size struct written directly with `write()`?**
> **Answer:** Fixed-size structs allow **direct memory-mapped I/O** — we can `read(offset)` to load any node in one syscall because we know exactly how many bytes to read (`sizeof(BTreeNode)`). This also makes `_allocate_node()` trivial: seek to end, write a zeroed struct, return the offset. The trade-off is wasted space (fixed `char[64]` for keys, even short ones) and no portability across architectures (padding, endianness).

**Q11: How does `_collect_all()` traverse the B-Tree?**
> **Answer:** It performs an **in-order traversal**: for each internal node with `n` keys, it recursively visits child[0], then key[0], then child[1], then key[1], ..., then child[n]. For leaf nodes, it just visits all keys. It skips `is_deleted` entries. For each live key, it seeks to `value_offset` in `dat_file` and reads `value_length` bytes. This produces all entries in sorted key order.

**Q12: What is the disk layout of your storage engine?**
> **Answer:** Two files per node:
> - **`node_XXXX_btree.idx`:** First 8 bytes = root offset pointer. Then a sequence of `BTreeNode` structs, each containing keys, value pointers, child pointers, versions, and delete flags.
> - **`node_XXXX_database.dat`:** Append-only stream of raw value bytes. Values are referenced by (offset, length) pairs stored in the B-Tree nodes.

**Q13: How much memory overhead does your LRU cache have per key?**
> **Answer:** Per key, the cache stores: (1) the key string in the `unordered_map` (~32 bytes for small string + hash bucket), (2) the `Entry` struct (string value + time_t + uint64_t version), (3) a `list::iterator` (pointer, 8 bytes), (4) a `std::list` node (key string + two pointers for doubly-linked list, ~48 bytes). Total: roughly 100-200 bytes per entry depending on key/value sizes, plus hash table overhead.

**Q14: Why does `get_with_version()` use a unique_lock instead of shared_lock?**
> **Answer:** Because of **lazy TTL deletion**. When we access a key and discover it's expired, we need to erase it from both the LRU list and the map — these are write operations. If we held a shared_lock, we couldn't safely mutate the data structures. So every `get()` takes a unique (write) lock preemptively. This is a trade-off: we sacrifice read concurrency for correctness.

**Q15: Your B-Tree has `MAX_KEYS = 3`. What would you change for production?**
> **Answer:** I'd increase `MAX_KEYS` to match the disk block size (e.g., 4KB). With `MAX_KEY_LEN = 64` and associated metadata per key (~100 bytes), we could fit ~30-40 keys per node in a 4KB block. This minimizes the number of disk seeks because each I/O reads a full block. The tree becomes wider and shorter, reducing lookup time. I'd also consider variable-length keys with prefix compression.

---

## Section 2: Networking & I/O (Q16–Q25)

**Q16: Why did you use `epoll` instead of `select` or `poll`?**
> **Answer:** `select` is limited to 1024 file descriptors (FD_SETSIZE) and does an O(N) scan every time. `poll` removes the FD limit but still scans O(N). `epoll` uses a kernel-level event queue — it only returns the file descriptors that are **actually ready**, making it O(1) per ready FD. For 10,000 concurrent connections, `select`/`poll` waste CPU scanning 10,000 FDs each time, while `epoll` only processes the handful that have data.

**Q17: Explain how your epoll event loop works step by step.**
> **Answer:** (1) `epoll_create1(0)` creates an epoll instance. (2) We add the server socket with `EPOLLIN` (readable) event. (3) `epoll_wait()` blocks until events arrive. (4) When the server socket is readable, we `accept()` a new client, set it non-blocking, and add it to epoll. (5) When a client socket is readable, we `read()` data and process the command. (6) If `read()` returns 0 or negative, the client disconnected — we remove it from epoll and `close()` it.

**Q18: Why must client sockets be set to non-blocking mode?**
> **Answer:** If a socket is blocking, `read()` will freeze the entire thread until the client sends data. Since `epoll` runs all clients on a single thread, one slow client would freeze the server for everyone. Non-blocking sockets guarantee `read()` returns immediately with either data or `EAGAIN` (no data available yet). This lets the epoll loop continue processing other clients.

**Q19: What is the `SIGPIPE` issue and why do you ignore it?**
> **Answer:** When you `send()` data to a client that has already closed its connection, the OS sends a `SIGPIPE` signal to the process. The default handler **terminates the process**. By calling `signal(SIGPIPE, SIG_IGN)`, we tell the OS to silently return an error from `send()` instead of killing the server. Without this, any client disconnect during a response could crash the entire server.

**Q20: Your proxy forwarding blocks the epoll loop. How would you fix this?**
> **Answer:** The proxy opens a blocking TCP connection to the owner node with a 2-second timeout. During this time, the entire epoll loop is frozen. Fix options: (1) **Thread pool:** offload proxy requests to a worker thread pool, freeing the epoll loop. (2) **Async proxy:** use non-blocking connect + epoll to manage the proxy socket alongside client sockets. (3) **Client redirect:** instead of proxying, reply `REDIRECT <ip:port>` and let the client reconnect directly (original Redis approach).

**Q21: What is the TCP message framing problem in your server?**
> **Answer:** TCP is a byte stream, not a message protocol. A single `read()` call might return: (a) **Partial message** — only half of `SET key value\n` arrived. (b) **Multiple messages** — `SET a 1\nGET b\n` arrives in one TCP segment. Our server assumes one `read()` = one complete command, which silently drops/corrupts data under load. Fix: maintain a per-client read buffer, accumulate bytes, and process complete lines (delimited by `\n`).

**Q22: Why do you use `SO_REUSEADDR` on the server socket?**
> **Answer:** After the server shuts down, the OS keeps the port in `TIME_WAIT` state for ~60 seconds to handle straggling TCP segments. Without `SO_REUSEADDR`, attempting to restart the server during this window fails with "Address already in use." `SO_REUSEADDR` tells the OS to allow rebinding to the port immediately, which is essential during development and quick restarts.

**Q23: Why can't this project run natively on Windows?**
> **Answer:** The system relies on `<sys/epoll.h>`, which is a **Linux-specific** system call. Windows uses a different high-performance I/O model called **IOCP (I/O Completion Ports)**. Also, POSIX socket functions like `read()/write()` on sockets, `fcntl()` for non-blocking mode, and `signal(SIGPIPE)` are all Unix-specific. The project must be compiled and run in WSL (Windows Subsystem for Linux) or a Docker container.

**Q24: How does the gossip thread avoid blocking the main epoll loop?**
> **Answer:** The gossip thread runs as a separate `std::thread` that is `detach()`ed from the main thread. It sleeps for 2 seconds, then opens its own blocking socket connections to other nodes. These blocking operations happen on the gossip thread's stack, completely independent of the epoll event loop. The only shared state is `gossip_mgr` (protected by `gossip_mutex`) and `ring` (protected by `ring_mutex`).

**Q25: What happens if `send()` doesn't send all the bytes?**
> **Answer:** `send()` can return fewer bytes than requested (short write), especially under heavy load. Our code doesn't check the return value — we assume the entire message was sent. In production, you'd loop: `while (total_sent < msg_size) { sent = send(sock, buf + total_sent, remaining, 0); total_sent += sent; }`. For our small command messages (<1KB), short writes are extremely unlikely but technically possible.

---

## Section 3: Distributed Systems — Consistent Hashing & CAP (Q26–Q40)

**Q26: Why Consistent Hashing over simple Modulo Hashing?**
> **Answer:** Modulo hashing (`hash(key) % N`) requires **rehashing almost every key** when N changes. If you have 5 servers and add a 6th, `hash(key) % 5` vs `hash(key) % 6` differ for ~80% of keys, causing massive data migration. Consistent hashing maps both keys and servers to a ring (0 to 2^64). Only keys in the arc between the old and new server positions need to move — roughly `K/N` keys, where K is total keys and N is total servers.

**Q27: What are Virtual Nodes (v-nodes) and why do you use 100 per physical node?**
> **Answer:** With only 3 physical servers on a hash ring, random placement can cause one server to own 50-70% of the ring by pure chance. Virtual nodes spread each server across 100 points on the ring, creating 300 total positions. By the law of large numbers, this guarantees near-uniform distribution (~33% each). The number 100 is chosen to balance even distribution against memory overhead (100 × 3 = 300 map entries, which is trivial).

**Q28: How does `get_node_for_key()` work?**
> **Answer:** (1) Hash the key to a 64-bit integer using `std::hash`. (2) Call `ring.lower_bound(hash)` on the `std::map` — this finds the first server position on the ring that is ≥ the key's hash, in O(log V) time where V is total virtual nodes. (3) If we hit `end()`, wrap around to `begin()` (the ring is circular). (4) Return the ClusterNode associated with that position.

**Q29: How does `get_replica_nodes()` find N backup nodes?**
> **Answer:** Starting from the primary owner's position on the ring, walk clockwise through the map. Skip any virtual node that belongs to a physical server we've already seen (tracked with a `std::set<string>`). Collect unique physical servers until we have `count` replicas. If we wrap all the way around and return to the start, the cluster is too small for the requested replication factor — we return what we have.

**Q30: Where does your system fall on the CAP Theorem? Explain.**
> **Answer:** It's an **AP (Available, Partition-Tolerant)** system. During a network partition, each partition continues to serve reads and writes independently (Available). Because replication is asynchronous, writes on one side of the partition won't reach the other side, so they may diverge (not Consistent). When the partition heals, read repair and anti-entropy gradually reconcile the differences. We sacrifice **Strong Consistency** for **High Availability** and **low latency**.

**Q31: What is the difference between Strong Consistency, Eventual Consistency, and Causal Consistency?**
> **Answer:**
> - **Strong Consistency:** Every read returns the most recent write. Requires synchronous replication (all replicas acknowledge before returning OK). High latency.
> - **Eventual Consistency:** If no new writes occur, all replicas will eventually converge to the same value. Our system uses this model.
> - **Causal Consistency:** If operation A causally precedes B (e.g., B reads A's result), then all nodes see A before B. Our Lamport clocks provide **partial causal ordering** — they track happens-before relationships through message passing.

**Q32: How would you make the system Strongly Consistent?**
> **Answer:** Use **synchronous quorum writes**. On `SET`, wait for W replicas to acknowledge (where W + R > N, e.g., W=2, R=2, N=3). On `GET`, read from R replicas and return the latest version. This is the approach used by Cassandra (`QUORUM` consistency level) and DynamoDB. The trade-off: much higher write latency (must wait for network round-trips to replicas) and reduced availability (if W replicas are down, writes fail).

**Q33: What happens to data when a node is removed from the hash ring?**
> **Answer:** The keys that were owned by the removed node are now mapped to the **next node clockwise** on the ring. Our `migration_worker` detects the ring change (via `ring_version`), scans all local keys, and pushes any keys that are now owned by a different node. The old node keeps its copy as a potential replica. Eventually, anti-entropy ensures all replicas converge.

**Q34: How does your system handle "hot keys" (keys accessed much more frequently)?**
> **Answer:** Currently, it doesn't handle hot keys specially. All requests for a key are routed to its primary owner, which could become a bottleneck. Solutions: (1) **Read replicas:** allow GET from any replica, not just the primary (our read repair ensures replicas are fresh). (2) **Client-side caching:** let clients cache frequently-read keys with TTL. (3) **Key splitting:** split a hot key across multiple sub-keys (e.g., `user:123:shard_0`, `user:123:shard_1`).

**Q35: What is the "hash ring thread safety" bug you fixed?**
> **Answer:** The `ConsistentHashRing` was a `std::map` accessed concurrently by the main epoll thread (via `get_node_for_key()`) and the gossip background thread (via `add_node()`/`remove_node()`). `std::map` is not thread-safe — concurrent read+write causes undefined behavior (corrupted internal tree, crash). We added a `std::shared_mutex`: readers take a shared_lock (multiple concurrent reads OK), writers take a unique_lock (exclusive access for mutations).

**Q36: Why use `std::shared_mutex` instead of `std::mutex` for the hash ring?**
> **Answer:** The ring is read-heavy (every GET/SET calls `get_node_for_key()`) and write-rare (only on node join/leave). A `std::mutex` would serialize all operations. A `std::shared_mutex` allows **concurrent reads** (shared_lock) while still providing exclusive access for writes (unique_lock). This is a classic readers-writer lock pattern that maximizes throughput for read-dominated workloads.

**Q37: How does your system handle network partitions between cluster nodes?**
> **Answer:** If nodes in partition A can't reach nodes in partition B: (1) After 10 seconds, gossip marks unreachable nodes as DEAD and removes them from the ring. (2) The migration worker scans keys and re-routes ownership within each partition. (3) Both partitions continue serving requests independently (AP behavior). When the partition heals, gossip rediscovers nodes, the ring is rebuilt, migration pushes keys back, and anti-entropy reconciles version conflicts using Lamport clocks (last-write-wins).

**Q38: What is the "split-brain" problem and does your system have it?**
> **Answer:** Split-brain occurs when a network partition causes two sub-clusters to both believe they are the "master" and independently accept writes to the same keys. **Yes, our system has this.** During a partition, both sides accept writes and assign independent Lamport clock versions. When the partition heals, conflicts are resolved via last-write-wins (higher version wins). This can cause data loss — if both sides wrote different values to the same key, the lower-version write is silently discarded.

**Q39: How would you add data center awareness to your hash ring?**
> **Answer:** Modify `get_replica_nodes()` to prefer replicas in **different data centers**. Each `ClusterNode` would have a `datacenter` field. When walking the ring, instead of just skipping duplicate physical servers, also try to place replicas in different DCs. This ensures that if an entire DC goes down, replicas in other DCs still have the data.

**Q40: What is the difference between a Proxy strategy and a Redirect strategy?**
> **Answer:** **Proxy:** Node A receives a request for Node B's key, opens a connection to Node B, forwards the request, gets the response, and relays it to the client. The client is unaware of the topology. **Redirect:** Node A replies with `REDIRECT B_address`, and the client reconnects to Node B directly. Our system uses Proxy (server-side forwarding) for transparency, but it blocks the epoll loop during the proxy call. Redis Cluster uses Redirect (`-MOVED` responses).

---

## Section 4: Replication & Versioning (Q41–Q55)

**Q41: Why is your replication asynchronous?**
> **Answer:** Synchronous replication would require the primary to wait for all replicas to acknowledge before returning `OK` to the client. This adds network round-trip latency to every write. With async replication, the primary writes locally, pushes a task to the background replication queue, and returns `OK` immediately. The client gets sub-millisecond responses. The trade-off: if the primary crashes before replication completes, the write is lost on the replicas.

**Q42: Explain the replication flow for a `SET key value` command.**
> **Answer:** (1) Client sends `SET key value` to primary. (2) Primary calls `db->set(key, value)`, which increments the Lamport clock to version V and writes to disk. (3) Primary calls `ring.get_replica_nodes(key, 2)` to find 2 backup nodes. (4) Primary pushes `INTERNAL_SET key value V` tasks to the replication queue. (5) Primary returns `OK` to client. (6) Background `replication_worker` pops tasks, opens TCP connections to replicas, and sends the commands. (7) Replicas call `db->set_versioned(key, value, V)`.

**Q43: What is a Lamport Clock and why do you use it?**
> **Answer:** A Lamport Clock is a **logical timestamp** that establishes a partial ordering of events across distributed nodes without requiring synchronized physical clocks. Rules: (1) Before every local event, increment the counter. (2) When sending a message, include the current counter value. (3) When receiving a message with counter V, set `my_counter = max(my_counter, V)`. This ensures that if event A causally precedes event B, then `clock(A) < clock(B)`. We use it for version conflict resolution — the higher version wins.

**Q44: Is the Lamport Clock a single point of failure?**
> **Answer:** **No.** Each node has its own independent `std::atomic<uint64_t> version_counter`. There is no centralized counter. The Lamport clock **advances** when a node receives a version from another node (rule 3), ensuring counters across the cluster roughly converge. The only risk is version **ties** (two nodes independently assign the same counter value), which we handle with last-write-wins semantics.

**Q45: How do you handle version ties between nodes?**
> **Answer:** In practice, ties are extremely rare because: (1) Lamport clock advancement on message receipt causes counters to diverge. (2) The primary owner increments before replication, so replicas always receive a higher value. If a tie does occur, `set_versioned` uses `>=` comparison — the last write to arrive wins. For guaranteed uniqueness, you could use a composite `(counter, node_id)` tuple as the version, with node_id as tiebreaker.

**Q46: What is the `INTERNAL_SET` command and how does it differ from `SET`?**
> **Answer:** `SET` is for external clients — it increments the Lamport clock, writes to disk, and triggers replication to backup nodes. `INTERNAL_SET` is for node-to-node replication — it accepts an explicit version, calls `set_versioned()` (which only writes if the version is newer), and does **NOT** trigger further replication. This prevents infinite replication loops (A→B→A→B...).

**Q47: What is `set_versioned()` and why doesn't it always update the cache?**
> **Answer:** `set_versioned()` is used by `INTERNAL_SET` and `MIGRATE` to accept a specific version from another node. It always attempts the disk write (BTree handles version comparison). But for the LRU cache, it **only updates existing cached entries** — it never adds new ones. Why? If the key isn't cached but is on disk with version 5, and we receive version 3 from a stale replica, adding version 3 to the cache would cause future GETs to return stale data. By only updating (not inserting), we let the cache be populated lazily from disk on the next GET, which always returns the latest disk version.

**Q48: What is the "infinite replication loop" problem?**
> **Answer:** Without the `INTERNAL_SET` distinction: Node A receives `SET key val`, replicates to B. Node B receives it and replicates to A. Node A receives it and replicates to B. Infinite loop. Solution: `INTERNAL_SET` commands are never re-replicated. Only client-facing `SET` commands trigger replication.

**Q49: What happens if a replication message is lost?**
> **Answer:** The replica will have stale data. Three mechanisms fix this: (1) **Read Repair:** When any node GETs the key, it queries replicas in the background and pushes the latest version to stale ones. (2) **Anti-Entropy:** Every 30 seconds, the primary owner scans all its keys and ensures replicas are up-to-date. (3) **Next SET:** The next write to the key will replicate normally and overwrite the stale value.

**Q50: How does your replication queue work internally?**
> **Answer:** It's a `std::queue<ReplicationTask>` protected by a `std::mutex` and a `std::condition_variable`. The main thread pushes tasks and calls `notify_one()`. The background `replication_worker` thread blocks on `cv.wait()` until the queue is non-empty, pops a task, opens a TCP connection to the target node, sends the command, and reads the response. This is a standard producer-consumer pattern.

**Q51: What happens if the replication queue grows unbounded?**
> **Answer:** If replicas are down but the client keeps sending writes, tasks pile up in memory and eventually cause an **OOM (Out-Of-Memory) crash**. Fix: implement a **bounded queue**. When it hits a limit (e.g., 10,000 tasks), either: (a) block the main thread until space is available (back-pressure, sacrifices availability), or (b) drop the oldest tasks (sacrifices durability). Alternatively, persist the queue to disk.

**Q52: Why do you use `std::unique_lock` with `condition_variable` instead of `lock_guard`?**
> **Answer:** `condition_variable::wait()` requires the thread to **unlock the mutex** before sleeping and **relock it** upon waking. `std::lock_guard` can't do this — it locks in the constructor and unlocks in the destructor, with no manual control. `std::unique_lock` supports manual `lock()`/`unlock()` and the internal unlock/relock mechanism that `cv.wait()` needs.

**Q53: How does the Lamport clock handle a rejoining node?**
> **Answer:** When a node restarts, its `version_counter` starts at 0. But the first time it receives a message from another node (via gossip, `INTERNAL_SET`, `MIGRATE`, etc.), the Lamport clock rule advances its counter: `version_counter = max(0, received_version)`. This instantly catches the counter up to the cluster's current level. Additionally, its existing on-disk B-Tree entries retain their old versions, so it doesn't overwrite newer data.

**Q54: What is the replication factor and how is it configured?**
> **Answer:** The replication factor is N=3 (1 primary + 2 backups), hardcoded in `ring.get_replica_nodes(key, 2)`. In production, this should be configurable via a CLI flag (`--replication-factor 3`). Higher replication factors increase durability (survive more node failures) but increase write amplification (each SET generates more network traffic).

**Q55: Could you implement synchronous replication alongside async? When would you use each?**
> **Answer:** Yes. Add a `--sync-replication` flag. On `SET`, instead of pushing to the background queue, directly send `INTERNAL_SET` to all replicas and wait for acknowledgments before returning `OK`. Use sync for critical data (financial transactions) and async for non-critical data (session caches). Cassandra supports this via per-query consistency levels (`ONE`, `QUORUM`, `ALL`).

---

## Section 5: Key Migration & Node Rejoin (Q56–Q70)

**Q56: What is key migration and why is it needed?**
> **Answer:** When the hash ring changes (node joins or leaves), some keys' ownership shifts. Key migration is the process of physically moving data from the old owner to the new owner. Without it, the new owner would receive requests for keys it doesn't have, and the old owner would waste storage on keys it no longer owns.

**Q57: How does the `migration_worker` detect ring changes?**
> **Answer:** The `ConsistentHashRing` has an `std::atomic<uint64_t> ring_version` counter that increments on every `add_node()` or `remove_node()`. The migration worker sleeps for 1 second, then checks if `ring.get_ring_version() != last_seen_ring_version`. If it changed, a migration scan is triggered. This is a simple polling mechanism that avoids the complexity of callbacks or observer patterns.

**Q58: Walk through what happens when a new node joins the cluster.**
> **Answer:** (1) New node starts with seed node addresses. (2) Gossip discovers the new node and calls `ring.add_node()`, incrementing `ring_version`. (3) Every node's `migration_worker` detects the ring change. (4) Each node calls `db->get_all_entries()` to scan all its local keys. (5) For each key, `ring.get_node_for_key()` is called — if the owner is now the new node, a `MIGRATE key value version` command is pushed to the replication queue. (6) The replication worker sends the MIGRATE command to the new node. (7) The new node's `set_versioned()` stores the data.

**Q59: What happens when a node crashes and then comes back?**
> **Answer:** (1) After 10 seconds of no heartbeats, gossip marks the node DEAD and removes it from the ring. Keys shift to the next node on the ring. (2) The surviving nodes' migration workers push keys to the new owners. (3) When the node restarts, gossip rediscovers it and calls `ring.add_node()`. (4) Keys shift again — some go back to the restarted node. (5) Migration workers push these keys back. (6) The restarted node's B-Tree may still have old data from before the crash — `set_versioned()` only accepts newer versions, so stale data is naturally overwritten.

**Q60: Why do you keep the local copy after migrating a key?**
> **Answer:** Safety and simplicity. The migrated key may be a valid **replica** — if this node is one of the 2 backup nodes for that key on the new ring, keeping it avoids a redundant re-replication. Even if it's not a replica, the key will eventually be evicted from the LRU cache and the disk space is reclaimed only via compaction. Deleting immediately risks data loss if the migration fails (network error).

**Q61: What is the `MIGRATE` command and how does it differ from `INTERNAL_SET`?**
> **Answer:** Functionally, `MIGRATE` and `INTERNAL_SET` do the same thing — they call `set_versioned()` and don't trigger further replication. The separate command name exists for **clarity in logging and debugging**. When you see `[MIGRATE]` in the logs, you know it's a ring-change migration. When you see `INTERNAL_SET`, you know it's a normal replication or read-repair fix.

**Q62: What if two nodes simultaneously try to migrate the same key to each other?**
> **Answer:** This can happen during rapid ring changes. Both migrations call `set_versioned()`, which uses **Lamport clock version comparison** — only the write with the higher version wins. Since both nodes have the same key with the same version (they got it from the same source), the second migration is a no-op (`version >= existing` → overwrites with same data, which is harmless).

**Q63: How much network traffic does a migration generate?**
> **Answer:** In the worst case (adding a node to a 2-node cluster), roughly 1/3 of all keys migrate. For K total keys with average value size V bytes, the migration traffic is approximately `K/3 × (key_size + V + overhead)` bytes. With 100K keys × 100 bytes average → ~3.3 MB of migration traffic. This happens once per ring change and is spread over time by the replication queue.

**Q64: What happens if a migration fails (network error)?**
> **Answer:** The replication worker's `connect()` fails, and the task is silently dropped. The key remains on the old node. Mechanisms that recover from this: (1) **Anti-entropy** (every 30s) will detect that the primary owner doesn't have the key and push it again. (2) **Read repair** on GET will detect the version mismatch and fix it. (3) The next ring change triggers another migration scan.

**Q65: Could the migration worker overwhelm the replication queue?**
> **Answer:** Yes. If 100K keys need migration, all 100K tasks are pushed to the queue at once. This could cause: (1) Memory pressure if the queue grows large. (2) Starvation of normal replication tasks. Fix: implement **rate limiting** — migrate at most N keys per second, or use a separate dedicated queue for migration tasks with lower priority than replication.

**Q66: How does migration interact with concurrent client writes?**
> **Answer:** A client's `SET key value` goes to the current primary (after hash ring lookup). If a migration is in progress for that key: (1) The client's write has a higher Lamport version (freshly incremented). (2) When the migration's `MIGRATE` command arrives at the new owner, `set_versioned` compares versions — the client's newer write wins. No data is lost.

**Q67: What is the latency impact of a ring change on client requests?**
> **Answer:** Immediately after a ring change: (1) Some GETs might return `NULL` for keys that haven't migrated yet. (2) Some SETs might go to the old owner (the ring is updated, but migration takes time). The migration worker checks every 1 second, and each key migration takes ~1ms (TCP connection + send). For a moderate number of keys, convergence takes a few seconds.

**Q68: How would you implement zero-downtime migration?**
> **Answer:** During migration, run a **dual-read** protocol: on GET, check local first, and if `NULL`, proxy to the old owner. This ensures no reads fail during migration. On SET, write to the new owner (per the updated ring). The migration worker pushes remaining keys in the background. Once complete, disable the dual-read fallback.

**Q69: How do you ensure the ring version doesn't overflow?**
> **Answer:** `ring_version` is a `uint64_t` (18.4 quintillion). Even if the ring changes once per second, it would take 584 billion years to overflow. In practice, ring changes happen at most a few times per day. Overflow is not a concern.

**Q70: Could you batch migration commands instead of one TCP connection per key?**
> **Answer:** Yes, and you should for production. Instead of `MIGRATE key1 val1 v1\n` per connection, send `BULK_MIGRATE\nkey1 val1 v1\nkey2 val2 v2\n...\nEND\n` over a single persistent connection. This amortizes TCP handshake overhead and is ~100x faster for large migrations.

---

## Section 6: Read Repair & Anti-Entropy (Q71–Q85)

**Q71: What is Read Repair?**
> **Answer:** Read Repair is a consistency mechanism where, after serving a GET request, the server **asynchronously queries all replicas** for the same key, compares versions, and fixes any stale copies. If a replica has an older version, the server pushes its newer version. If a replica has a newer version, the server updates its own copy. This ensures that frequently-read keys converge quickly.

**Q72: Why is Read Repair asynchronous in your system?**
> **Answer:** The client gets the local value **immediately** — the repair happens in the background. Synchronous read repair (querying all replicas before responding) would add 1-2 network round-trips of latency to every GET. Since this is an AP system optimized for speed, we trade momentary staleness for sub-millisecond GETs.

**Q73: Walk through a Read Repair scenario.**
> **Answer:** (1) Client sends `GET key1` to Node A (primary owner). (2) Node A returns `value_v5` (version 5) immediately. (3) Node A pushes `{key1, value_v5, 5}` to the read repair queue. (4) Background `read_repair_worker` pops the task. (5) Worker sends `INTERNAL_GET key1` to Replica B → gets `value_v3` (version 3). (6) Worker detects version 3 < 5 → pushes `INTERNAL_SET key1 value_v5 5` to Replica B via the replication queue. (7) Replica B now has version 5. Log: `[READ_REPAIR] Fixed stale key "key1" on B (v3 -> v5)`.

**Q74: What if a replica has a NEWER version during Read Repair?**
> **Answer:** This can happen if the primary crashed and came back with stale data, or during a partition. The read repair worker detects `replica_version > local_version`, calls `db->set_versioned(key, replica_value, replica_version)` to update the local copy, and updates the task's local version for subsequent replica checks. This ensures the primary always converges to the latest version.

**Q75: What is Anti-Entropy and how does it differ from Read Repair?**
> **Answer:** Read Repair only triggers on **read operations** — keys that are never read are never repaired. Anti-Entropy is a **periodic background process** (every 30 seconds) that scans ALL keys owned by this node and proactively checks replicas. It catches stale replicas for cold (rarely-read) keys. Together, they provide a comprehensive consistency mechanism: read repair for hot keys (fast convergence), anti-entropy for cold keys (eventual convergence).

**Q76: How does the Anti-Entropy scan work?**
> **Answer:** (1) Call `db->get_all_entries()` — full B-Tree traversal. (2) For each key where `ring.get_node_for_key(key) == my_node_id` (we're the primary): (3) Get the 2 replica nodes. (4) For each replica, send `INTERNAL_GET key` and compare versions. (5) If replica is stale, push `INTERNAL_SET` with our version. (6) If we're stale (shouldn't happen often), update local. (7) Log the total number of repairs.

**Q77: Why does Anti-Entropy only scan keys this node is the PRIMARY owner of?**
> **Answer:** If every node scanned every key, you'd get O(N²) redundant checks and network traffic. By limiting to primary-owned keys, each key is checked by exactly one node, reducing traffic to O(K/N) per node where K is total keys. The primary is the authoritative source, so it pushes corrections to replicas.

**Q78: What is the network cost of Anti-Entropy?**
> **Answer:** For K keys owned by this node with R replicas: K × R × 2 messages per scan (one `INTERNAL_GET` + response, and optionally one `INTERNAL_SET`). With K=1000, R=2: 4000 small messages every 30 seconds. Each message is <200 bytes, so ~800KB/30s ≈ 27KB/s. Very manageable. For millions of keys, you'd need optimization (Merkle trees, sampling).

**Q79: What is a Merkle Tree and how would it optimize Anti-Entropy?**
> **Answer:** A Merkle Tree is a binary tree of hashes. Each leaf is the hash of a key-value pair. Each internal node is the hash of its children. Two nodes can compare their **root hashes** in O(1). If they differ, they descend the tree to find the exact keys that differ — O(log K) messages instead of O(K). Cassandra uses Merkle trees for anti-entropy. Our simple approach (scan all keys) works for small datasets but doesn't scale beyond ~100K keys.

**Q80: Could Read Repair cause a write amplification storm?**
> **Answer:** Yes. If a key is extremely popular (1000 GETs/second), each GET queues a read repair task. The repair worker queries 2 replicas per task = 2000 TCP connections/second just for one key. Fix: (1) **Debounce:** track recently-repaired keys and skip repair if repaired within the last N seconds. (2) **Sample:** only trigger repair for a random subset of GETs (e.g., 1 in 100). (3) **Bounded queue:** cap the repair queue size.

**Q81: What happens if `INTERNAL_GET` returns `NULL` during Read Repair?**
> **Answer:** The replica doesn't have the key at all. The read repair worker detects `replica_value == "NULL"` and skips that replica. It doesn't push the key because the replica might not be responsible for it (e.g., the ring changed). Anti-entropy handles this case more carefully — it only scans keys for which this node is the primary owner.

**Q82: How do Read Repair and Anti-Entropy interact with TTL expiry?**
> **Answer:** Currently, TTL expiry is implemented via lazy deletion — expired keys are only removed when accessed. Read Repair could accidentally "revive" an expired key by receiving it from a replica that hasn't checked it yet. To fix this: `INTERNAL_GET` should check TTL before responding, and `set_versioned` should respect TTL. Alternatively, include `expiry_time` in the replication protocol.

**Q83: What consistency level does your system provide with Read Repair enabled?**
> **Answer:** **Eventual consistency with read-your-writes guarantee** (on the primary). After a write, the primary immediately has the latest version. GETs to the primary always return the latest. GETs to replicas may return stale data until read repair or anti-entropy fixes them. The convergence time depends on read frequency (repair triggers on GET) and the 30-second anti-entropy interval.

**Q84: How would you implement "read repair at write time" (Hinted Handoff)?**
> **Answer:** When a replica is unreachable during replication, instead of dropping the task, store a "hint" locally: `{target_node, key, value, version}`. Run a background thread that periodically retries sending hints. When the target node comes back online (detected via gossip), flush all hints to it. Cassandra uses this extensively. It's more reliable than relying on anti-entropy alone.

**Q85: What is the `INTERNAL_GET` command and why did you add it?**
> **Answer:** `INTERNAL_GET key` returns `value version\n` (or `NULL 0\n`). Standard `GET key` only returns the value. Read repair and anti-entropy need the **version** to compare against the local copy. `INTERNAL_GET` bypasses the hash ring redirect (it's an internal protocol), so it always queries the local node's data regardless of key ownership.

---

## Section 7: Concurrency & Thread Safety (Q86–Q95)

**Q86: List all the threads in your server and their roles.**
> **Answer:**
> 1. **Main thread (epoll):** Accepts connections, reads commands, processes requests.
> 2. **Replication worker:** Sends `INTERNAL_SET`/`MIGRATE`/`INTERNAL_DEL` to other nodes.
> 3. **Gossip worker:** Sends heartbeats, detects dead nodes, discovers new nodes.
> 4. **Migration worker:** Detects ring changes, scans keys, pushes migrations.
> 5. **Read Repair worker:** Queries replicas after GET, fixes stale copies.
> 6. **Anti-Entropy worker:** Periodic full scan, ensures replica consistency.

**Q87: What shared state exists between threads and how is it protected?**
> **Answer:**
> | Shared State | Threads | Protection |
> |---|---|---|
> | `ConsistentHashRing ring` | All threads | `std::shared_mutex ring_mutex` |
> | `KVStore* db` | Main, migration, repair, anti-entropy | `std::shared_mutex rw_lock` (inside KVStore) |
> | `GossipManager gossip_mgr` | Main, gossip | `std::mutex gossip_mutex` (inside GossipManager) |
> | `replication_queue` | Main, replication, repair, anti-entropy, migration | `std::mutex rep_mutex` + `condition_variable` |
> | `read_repair_queue` | Main, read_repair | `std::mutex rr_mutex` + `condition_variable` |
> | `my_heartbeat` | Main, gossip | `std::mutex hb_mutex` |

**Q88: Why do you use `std::atomic` for `version_counter` and `ring_version`?**
> **Answer:** Atomics provide **lock-free** thread-safe access for simple operations (increment, load, compare-and-swap). `version_counter` is incremented on every SET and read during `set_versioned` — a mutex would serialize all writes unnecessarily. `ring_version` is incremented on ring changes and polled by the migration worker — an atomic load is ~1 nanosecond vs ~25 nanoseconds for a mutex lock/unlock.

**Q89: Explain the `compare_exchange_weak` loop in `set_versioned`.**
> **Answer:** We need to atomically set `version_counter = max(version_counter, incoming_version)`. There's no atomic `max` operation, so we use a CAS (Compare-And-Swap) loop: (1) Load current value. (2) If incoming > current, try to swap. (3) If another thread changed the value between steps 1 and 2, CAS fails and `current` is updated to the new value. (4) Retry. `compare_exchange_weak` can spuriously fail (for performance), so we use a `while` loop. This is the standard lock-free pattern for atomic max.

**Q90: Could there be a deadlock between `rep_mutex` and `rr_mutex`?**
> **Answer:** No, because no thread ever holds both locks simultaneously. The main thread locks `rep_mutex` or `rr_mutex` independently (in separate `if` blocks). The read repair worker locks `rr_mutex` (to pop), then later locks `rep_mutex` (to push fixes). But it releases `rr_mutex` before locking `rep_mutex`, so there's no circular dependency. Deadlocks require a cycle in the lock acquisition order — we don't have one.

**Q91: Why are all background threads `detach()`ed instead of `join()`ed?**
> **Answer:** `detach()` lets the thread run independently until the process exits. `join()` would block `main()` until the thread completes — but our threads run infinite loops, so `join()` would block forever. We detach because: (1) The threads should outlive `main()`'s setup code. (2) They terminate automatically when the process exits. In production, you'd use a shutdown flag (`std::atomic<bool> running`) and `join()` for clean shutdown.

**Q92: What is a data race and how does your `ring_mutex` prevent one?**
> **Answer:** A data race occurs when two threads access the same memory location concurrently, at least one is a write, and there's no synchronization. Before the fix: the gossip thread called `ring.remove_node()` (write) while the epoll thread called `ring.get_node_for_key()` (read) simultaneously. The `std::map`'s internal red-black tree could be mid-rotation during the read, causing corrupted pointers and a crash. The `shared_mutex` ensures writes are exclusive and reads wait for writes to finish.

**Q93: How does `condition_variable::wait` prevent busy-waiting?**
> **Answer:** Without a CV, the replication worker would loop: `while (queue.empty()) { sleep(1ms); }` — wasting CPU on constant lock+check+unlock cycles. `cv.wait(lock, predicate)` puts the thread to sleep at the OS level (no CPU usage) and wakes it **only** when `notify_one()` is called. This is essentially interrupt-driven instead of poll-driven. The predicate `[]{ return !queue.empty(); }` prevents spurious wakeups.

**Q94: What is a spurious wakeup and how do you handle it?**
> **Answer:** A spurious wakeup is when `cv.wait()` returns even though `notify_one()` wasn't called. This is allowed by the C++ standard for implementation efficiency. We handle it by passing a **predicate** to `wait()`: `cv.wait(lock, []{ return !queue.empty(); })`. The CV internally checks the predicate after every wakeup — if the queue is still empty (spurious), it goes back to sleep.

**Q95: What is the difference between `notify_one()` and `notify_all()`?**
> **Answer:** `notify_one()` wakes exactly one waiting thread. `notify_all()` wakes all waiting threads. We use `notify_one()` because we have a single consumer per queue (one replication worker, one read repair worker). Waking all threads would be wasteful if multiple workers competed for the same queue — only one would get the task, others would check the predicate, find it false, and go back to sleep.

---

## Section 8: Gossip Protocol & System Design (Q96–Q100)

**Q96: Why Gossip instead of a centralized coordinator (like ZooKeeper)?**
> **Answer:** Gossip is **fully decentralized** — no single point of failure. ZooKeeper requires a separate cluster of 3-5 coordinator nodes, adds operational complexity, and introduces a dependency. Gossip's tradeoffs: slower convergence (information spreads exponentially, not instantly) and higher bandwidth (O(N) messages per round). For our use case (small clusters, 3-5 nodes), gossip is simpler and sufficient. For large clusters (1000+ nodes), consider SWIM protocol or a coordinator.

**Q97: How does gossip achieve auto-discovery of new nodes?**
> **Answer:** A new node only needs to know **one** existing node (seed node). On startup, it sends a `GOSSIP` message to the seed. The seed adds the new node to its state and responds with `GOSSIP_ACK`. On the next gossip round, the seed might gossip with Node C and include the new node's info. Node C adds it. Within a few rounds (O(log N) time), every node in the cluster knows about the new node.

**Q98: What is the time complexity of gossip convergence?**
> **Answer:** With N nodes gossiping to 1 random peer every 2 seconds, information spreads like an epidemic. After round 1, 2 nodes know. After round 2, ~4 nodes. After round k, ~2^k nodes. Full convergence takes O(log₂ N) rounds = O(log N × 2s). For 8 nodes: ~3 rounds = 6 seconds. For 1024 nodes: ~10 rounds = 20 seconds.

**Q99: If you were building this for production, what are the top 5 changes you'd make?**
> **Answer:**
> 1. **TCP message framing** — per-client read buffers with `\n` delimiter parsing.
> 2. **Bounded queues** — cap replication and repair queues to prevent OOM.
> 3. **RESP protocol** — binary-safe Redis protocol for values with spaces/newlines.
> 4. **Merkle tree anti-entropy** — O(log K) comparison instead of O(K) full scan.
> 5. **Hinted handoff** — persist failed replication tasks to disk instead of dropping them.

**Q100: How would you benchmark this system and what metrics would you track?**
> **Answer:** Use the existing `benchmark.py` with multiple configurations:
> - **Throughput:** Requests per second (ops/s) at varying concurrency (10, 50, 100 clients).
> - **Latency:** p50, p95, p99 response time per operation.
> - **Cache hit rate:** `cache_hits / (cache_hits + cache_misses)` — exposed via an `INFO` command.
> - **Replication lag:** Time from SET on primary to replication on replica (add timestamps to logs).
> - **Migration speed:** Keys migrated per second during ring changes.
> - **Convergence time:** Time from a SET on primary until all replicas have the latest version (measure with `INTERNAL_GET` polling).
> - **Recovery time:** Time from node crash to full data availability on the replacement node.
