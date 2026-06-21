# Architecture Diagrams

Here are simple, visual explanations of the architecture we built over the three phases. You can use these to quickly explain how the system works to others.

## 1. Single Node Internal Architecture
This diagram shows what happens inside a single server. Notice how the main `epoll` thread handles the fast work, while the heavy network lifting is pushed to background threads.

```mermaid
graph TD
    Client["Client Request (GET/SET)"]
    Epoll["epoll Event Loop (Main Thread)"]
    DB["Tiered Storage (KVStore & BTree)"]
    RepQ["Replication Queue (Mutex Protected)"]
    RepW["Replication Thread (Background)"]
    GosW["Gossip Thread (Background)"]

    Client -->|"Non-Blocking I/O"| Epoll
    Epoll -->|"Local Read/Write"| DB
    Epoll -->|"Push Backup Tasks"| RepQ
    RepQ -->|"Pops Task"| RepW
    RepW -.->|"INTERNAL_SET"| OtherNodes["Other Cluster Nodes"]
    GosW -.->|"GOSSIP Heartbeats (every 2s)"| OtherNodes
```

---

## 2. Phase 1: Simple Server Proxy (Routing)
Instead of forcing the client to figure out where the data is (Redirects), the server magically fetches it on the client's behalf. 

```mermaid
sequenceDiagram
    participant Client
    participant NodeA as Node 8081
    participant NodeB as Node 8082

    Client->>NodeA: GET user_123
    NodeA-->>NodeA: Hash key "user_123"
    NodeA-->>NodeA: Check Consistent Hash Ring
    note over NodeA: Node 8082 owns this key!
    NodeA->>NodeB: GET user_123 (Secret Proxy Request)
    NodeB-->>NodeB: Data is local!
    NodeB->>NodeA: "OK John Doe"
    NodeA->>Client: "OK John Doe"
```

---

## 3. Phase 2: Asynchronous Replication (N=3)
This diagram illustrates how our system achieves High Availability (the "A" in CAP theorem). The client gets an immediate response without having to wait for the backups to finish saving across the network.

```mermaid
sequenceDiagram
    participant Client
    participant Primary as Node A - Primary
    participant Replica1 as Node B - Backup 1
    participant Replica2 as Node C - Backup 2

    Client->>Primary: SET key1 value1
    Primary->>Primary: Save to local DB
    Primary-->>Primary: Push to Replication Queue
    Primary->>Client: OK (Immediate Response)
    
    note over Primary: Background Thread Wakes Up
    Primary-)Replica1: INTERNAL_SET key1 value1
    Primary-)Replica2: INTERNAL_SET key1 value1
    
    note over Replica1,Replica2: Saves data but does NOT replicate further
```

---

## 4. Phase 3: Gossip Protocol & Failure Detection
Nodes constantly whisper to each other in the background. If a node goes quiet for 10 seconds, it is automatically removed from everyone's Hash Ring.

```mermaid
graph TD
    subgraph "Cluster State"
    Node1["Node 8081 (Heartbeat: 45)"]
    Node2["Node 8082 (Heartbeat: 82)"]
    Node3["Node 8083 (Heartbeat: 19)"]
    Node4["Node 8084 (Heartbeat: 100 - NO RESPONSE)"]
    end

    Node1 -- "GOSSIP (I am 8081, HB: 45)" --> Node2
    Node2 -- "GOSSIP ACK" --> Node1
    
    Node2 -- "GOSSIP (I am 8082, HB: 82)" --> Node3
    Node3 -- "GOSSIP ACK" --> Node2
    
    Node1 -. "Timeout (> 10s)" .-> Node4
    Node1 -- "Marks DEAD & Removes from Ring" --> Node4
```
