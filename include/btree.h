#ifndef BTREE_H
#define BTREE_H

#include <string>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <tuple>

// Order of B-Tree. Let's use a small max keys (e.g. 3) to force splits frequently for demonstration.
// Maximum children = MAX_KEYS + 1
#define MAX_KEYS 3

const int MAX_KEY_LEN = 64;

// BTreeNode represents a block of data stored on disk
struct BTreeNode {
    bool is_leaf;
    int num_keys;
    char keys[MAX_KEYS][MAX_KEY_LEN];
    size_t value_offsets[MAX_KEYS];      // Offset in database.dat
    size_t value_lengths[MAX_KEYS];      // Length of value in database.dat
    bool is_deleted[MAX_KEYS];           // For soft deletes
    uint64_t versions[MAX_KEYS];         // Lamport clock version for each key
    size_t child_pointers[MAX_KEYS + 1]; // File offsets to child nodes in btree.idx
};

struct SearchResult {
    bool found;
    size_t value_offset;
    size_t value_length;
    uint64_t version;
};

class BTree {
private:
    std::fstream idx_file; // btree.idx
    std::fstream dat_file; // database.dat
    size_t root_offset;
    
    size_t _allocate_node();
    void _read_node(size_t offset, BTreeNode& node);
    void _write_node(size_t offset, const BTreeNode& node);
    
    void _split_child(size_t parent_offset, BTreeNode& parent, int i, size_t child_offset, BTreeNode& child);
    void _insert_non_full(size_t node_offset, BTreeNode& node, const std::string& key, size_t val_offset, size_t val_len, uint64_t version);
    
    SearchResult _search(size_t node_offset, const std::string& key);
    void _collect_all(size_t node_offset, std::vector<std::tuple<std::string, std::string, uint64_t>>& results);

public:
    BTree(const std::string& node_prefix = "");
    ~BTree();
    
    void insert(const std::string& key, const std::string& value, uint64_t version = 0);
    std::string search(const std::string& key);
    std::string search(const std::string& key, uint64_t& out_version);
    bool remove(const std::string& key); // Soft delete
    std::vector<std::tuple<std::string, std::string, uint64_t>> get_all_entries();
};

#endif
