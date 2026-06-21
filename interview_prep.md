# Distributed Key-Value Store: Interview Preparation Guide

This document is a comprehensive summary of the distributed architecture we built. It is designed to serve as a study guide for technical interviews, detailing the "what," "why," and "how" of every major decision, followed by a list of questions an interviewer is highly likely to ask you.

---

## Part 1: Project Architecture & Journey

We transformed a single-node, in-memory key-value store into a **Decentralized, Highly Available, and Partition-Tolerant (AP)** distributed database.

### 1. Data Partitioning (Consistent Hashing)
**The Problem:** A single server can only hold so much data in RAM. We needed to shard (split) the data across multiple servers.
**The Solution:** We implemented a `ConsistentHashRing`. Instead of a simple modulo hash (which breaks if a server is added or removed), we mapped servers to a 64-bit integer ring using `std::hash`. 
*   **Virtual Nodes (v-nodes):** To ensure data was distributed evenly, we assigned 100 virtual nodes to each physical server.
*   **Routing:** When a key is provided, we hash the key and find the next closest server on the ring using `std::map::lower_bound`.

### 2. The Redirect Strategy (vs. Proxy Strategy)
**The Problem:** If Node A receives a request for a key that Node B owns, how do we get the data?
**The Solution:** We chose **Client Redirects** (Option B) over acting as a Proxy (Option A).
*   If Node A acted as a proxy, it would have to open a socket to Node B, wait for the response, and then reply to the client. This blocks the incredibly fast `epoll` event loop.
*   Instead, Node A instantly replies with `REDIRECT <Node B IP:Port>`. This pushes the routing responsibility to the client, keeping the server strictly single-threaded and blazingly fast.

### 3. Asynchronous Replication (Fault Tolerance)
**The Problem:** If a node crashes, all the data on it is permanently lost.
**The Solution:** We implemented a Replication Factor of $N=3$ (1 primary owner, 2 backups). 
*   **Background Threading:** To prevent the main `epoll` loop from blocking during network replication, we spawned a background `std::thread` and a thread-safe `std::queue`. 
*   **Eventual Consistency:** When a client sends a `SET`, the primary node saves it locally, pushes a backup task to the background queue, and instantly returns `OK`. 
*   **Infinite Loop Prevention:** Background threads send `INTERNAL_SET` commands to replica nodes. When a replica sees `INTERNAL_SET`, it saves the data but *does not* replicate it further, breaking the infinite echo loop.

### 4. Gossip Protocol (Auto-Discovery & Self-Healing)
**The Problem:** The cluster was static. If a node died, the Hash Ring didn't know, and clients would still be redirected to a dead node.
**The Solution:** We implemented a decentralized Gossip Protocol.
*   **Heartbeats:** Every 2 seconds, a background thread increments a heartbeat counter.
*   **Gossiping:** The thread randomly picks another node and sends a `GOSSIP <ip> <port> <heartbeat>` message.
*   **Self-Healing:** If a node's heartbeat doesn't update in our registry for 10 seconds, we mark it `DEAD` and dynamically remove it from the Hash Ring.
*   **Auto-Discovery:** You can start a massive cluster by only pointing new nodes to *one* existing node. Through gossip, the nodes share their registries and automatically discover everyone else.

---

## Part 2: Potential Interview Questions & Answers

### System Design & Architecture

**Q1: Why did you choose Consistent Hashing over standard Modulo Hashing?**
> **Answer:** Standard modulo hashing (`hash(key) % N`) is terrible for elasticity. If I have 5 servers and add a 6th, the modulo changes for almost every single key, meaning almost all data has to be moved to different servers. Consistent Hashing maps nodes to a ring. When a node is added or removed, only the keys immediately adjacent to it on the ring are affected. It minimizes data shuffling.

**Q2: What is a "Virtual Node" (v-node) and why did you use it?**
> **Answer:** In a standard Hash Ring with only 3 or 4 physical servers, the spacing between them can be very uneven by pure random chance, meaning one server might end up holding 70% of the data. By creating 100 "Virtual Nodes" for every 1 physical server, we artificially scatter their presence across the ring, guaranteeing a near-perfectly even distribution of data.

**Q3: Where does your system fall on the CAP Theorem?**
> **Answer:** It is an **AP (Available and Partition-Tolerant)** system. Because we use *Asynchronous* Replication, when a user sends a `SET` command, the primary node returns `OK` immediately before the backups are confirmed. If the primary node crashes a millisecond later, the data might not have reached the backups. We sacrificed Strong Consistency (C) in favor of High Availability (A) and blazing fast response times.

**Q4: How does your Gossip Protocol prevent network congestion?**
> **Answer:** Instead of every node broadcasting its status to *every other node* constantly (which requires $O(N^2)$ network traffic), our nodes randomly pick just *one* other node every 2 seconds to gossip with. The information propagates exponentially (like a virus) with extremely low network overhead, keeping the cluster state eventually consistent.

### C++ & Concurrency

**Q5: In your replication thread, you use `std::unique_lock` with a `std::condition_variable`. Why not just use `std::lock_guard`?**
> **Answer:** `std::lock_guard` locks a mutex when created and unlocks it when destroyed. It cannot be unlocked manually. A `std::condition_variable.wait()` requires the thread to unlock the mutex right before it goes to sleep, and re-lock it when it wakes up. `std::unique_lock` supports this temporary unlocking/relocking behavior, which is why it is required for condition variables.

**Q6: What happens if your background replication queue gets too big?**
> **Answer:** Currently, it's an unbounded `std::queue`. If the network to the replica nodes goes down but the client keeps sending `SET` commands, the queue will grow endlessly and eventually cause an Out-Of-Memory (OOM) crash. In a production system, I would implement a bounded queue. If the queue hits a limit (e.g., 10,000 tasks), the main thread would have to drop the backup, or block until space is available.

**Q7: How did you handle thread safety in the `GossipManager`?**
> **Answer:** I used a `std::mutex` to protect the `unordered_map` that stores the cluster state. Because the Gossip background thread is constantly reading and writing to this map, and the main `epoll` loop is also updating it when it receives `GOSSIP` commands from the network, a mutex ensures we don't encounter race conditions or memory corruption.

### Networking & OS (Linux)

**Q8: Why did you use `epoll` instead of spawning a new thread for every client connection?**
> **Answer:** Spawning a thread for every client (Thread-per-connection) doesn't scale. If you have 10,000 concurrent clients, the OS spends all its CPU time just context-switching between 10,000 threads. `epoll` is an event-driven mechanism. It allows a *single* thread to monitor 10,000 sockets simultaneously, sleeping until data arrives on one of them. It is incredibly CPU efficient and is the exact same architecture used by Nginx and Node.js.

**Q9: You mentioned you use non-blocking sockets with `epoll`. Why is that important?**
> **Answer:** If a socket is blocking, a simple `read()` call will freeze the entire thread until the client actually sends data. Because `epoll` runs all 10,000 clients on a single thread, if one client has a slow internet connection and pauses halfway through sending a message, a blocking `read()` would freeze the server for everyone else. Non-blocking sockets guarantee that `read()` returns immediately, either with the data or an `EAGAIN` error indicating we should check back later.

**Q10: Why couldn't you test this natively on Windows?**
> **Answer:** The system relies on `<sys/epoll.h>`, which is a Linux-specific system call. Windows uses a different high-performance I/O model called IOCP (I/O Completion Ports). To test it, I had to compile and run it inside WSL (Windows Subsystem for Linux).
