# NitKVStore Architecture

Below are detailed architectural diagrams that visualize exactly how the system is structured and how data flows through it. 

### 1. High-Level Component Architecture
This diagram shows how the different parts of the system interact, from the network layer down to the physical hard drive.

```mermaid
graph TD
    %% Define styles
    classDef client fill:#3498db,stroke:#2980b9,stroke-width:2px,color:white;
    classDef server fill:#2ecc71,stroke:#27ae60,stroke-width:2px,color:white;
    classDef ram fill:#f1c40f,stroke:#f39c12,stroke-width:2px,color:black;
    classDef disk fill:#e74c3c,stroke:#c0392b,stroke-width:2px,color:white;
    classDef file fill:#95a5a6,stroke:#7f8c8d,stroke-width:2px,color:white;

    %% Nodes
    Client1[Client]:::client
    Client2[Client]:::client
    
    Server["Epoll Event Loop<br>server.cpp"]:::server
    KVStore["Tiered KVStore<br>kv_store.cpp"]:::server
    
    subgraph RAM ["RAM Layer - Extremely Fast, Limited Size"]
        LRUCache["LRU Cache"]:::ram
    end
    
    subgraph DISK ["Disk Layer - Slower, Infinite Size"]
        BTree["B-Tree Manager<br>btree.cpp"]:::disk
        DatFile[("database.dat<br>Values")]:::file
        IdxFile[("btree.idx<br>Keys")]:::file
    end

    %% Connections
    Client1 -->|TCP Non-blocking| Server
    Client2 -->|TCP Non-blocking| Server
    Server -->|Parse command| KVStore
    
    KVStore <-->|Fast Lookup| LRUCache
    KVStore -->|Disk Lookup / Sync| BTree
    
    BTree -->|1. Append Value| DatFile
    BTree -->|2. Save Key & Pointer| IdxFile
    
    IdxFile -.->|Read| BTree
    DatFile -.->|Read| BTree
```

---

### 2. Request Flow: The `GET` Operation
This sequence diagram shows the exact step-by-step logic when a client asks for a key. Notice how the database falls back to the hard drive only if the RAM fails (Cache Miss).

```mermaid
sequenceDiagram
    participant Client
    participant Epoll as Server - epoll
    participant KV as KVStore
    participant LRU as RAM Cache
    participant Disk as B-Tree - Disk

    Client->>Epoll: "GET my_key\n"
    Epoll->>KV: get("my_key")
    
    %% RAM Check
    KV->>LRU: Check memory map
    
    alt Cache Hit (Fast Path)
        LRU-->>KV: Returns Value immediately
        KV->>LRU: Move key to front of list (Mark as Recently Used)
    else Cache Miss (Slow Path)
        LRU-->>KV: Not Found (Miss)
        
        %% Disk Check
        KV->>Disk: search("my_key")
        Disk->>Disk: Read btree.idx (Find Offset)
        Disk->>Disk: Read database.dat (Fetch Value)
        Disk-->>KV: Returns Value
        
        %% Bring to RAM
        KV->>LRU: Insert into RAM Cache
        
        opt If Cache is Full
            LRU->>LRU: Evict oldest key from RAM
        end
    end
    
    KV-->>Epoll: Returns string
    Epoll-->>Client: "my_value\n"
```

---

### 3. Request Flow: The `SET` Operation
This sequence diagram shows what happens when data is written. Notice that data is **always** written to the hard drive first to guarantee durability (so we don't lose it if the server crashes).

```mermaid
sequenceDiagram
    participant Client
    participant Epoll as Server - epoll
    participant KV as KVStore
    participant LRU as RAM Cache
    participant Disk as B-Tree - Disk

    Client->>Epoll: "SET my_key my_value\n"
    Epoll->>KV: set("my_key", "my_value")
    
    %% Write to Disk First (Durability)
    KV->>Disk: insert("my_key", "my_value")
    Disk->>Disk: dat_file.write(value) -> flush()
    
    alt B-Tree Root Full
        Disk->>Disk: Split Root Node (Tree grows taller)
    end
    
    Disk->>Disk: Find leaf & insert Key + Offset
    Disk->>Disk: idx_file.write(node) -> flush()
    Disk-->>KV: Insert Success
    
    %% Update RAM Cache
    opt If Cache is Full
        KV->>LRU: Evict oldest key from RAM
    end
    KV->>LRU: Insert/Update "my_key" in RAM
    KV->>LRU: Move to front (Mark as Recently Used)
    
    KV-->>Epoll: Returns "OK"
    Epoll-->>Client: "OK\n"
```
