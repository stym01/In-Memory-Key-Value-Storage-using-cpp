#include "../include/btree.h"
#include <iostream>

const size_t ROOT_POINTER_OFFSET = 0;
const size_t DATA_START_OFFSET = sizeof(size_t);

BTree::BTree(const std::string& node_prefix) {
    std::string idx_filename = node_prefix.empty() ? "btree.idx" : "node_" + node_prefix + "_btree.idx";
    std::string dat_filename = node_prefix.empty() ? "database.dat" : "node_" + node_prefix + "_database.dat";

    idx_file.open(idx_filename, std::ios::in | std::ios::out | std::ios::binary);
    if (!idx_file.is_open()) {
        idx_file.open(idx_filename, std::ios::out | std::ios::binary);
        idx_file.close();
        idx_file.open(idx_filename, std::ios::in | std::ios::out | std::ios::binary);
        
        root_offset = DATA_START_OFFSET;
        idx_file.seekp(ROOT_POINTER_OFFSET);
        idx_file.write(reinterpret_cast<const char*>(&root_offset), sizeof(size_t));
        
        BTreeNode root;
        root.is_leaf = true;
        root.num_keys = 0;
        for(int i=0; i<MAX_KEYS; i++) {
            root.is_deleted[i] = false;
            root.versions[i] = 0;
        }
        _write_node(root_offset, root);
    } else {
        idx_file.seekg(ROOT_POINTER_OFFSET);
        idx_file.read(reinterpret_cast<char*>(&root_offset), sizeof(size_t));
    }
    
    dat_file.open(dat_filename, std::ios::in | std::ios::out | std::ios::binary);
    if (!dat_file.is_open()) {
        dat_file.open(dat_filename, std::ios::out | std::ios::binary);
        dat_file.close();
        dat_file.open(dat_filename, std::ios::in | std::ios::out | std::ios::binary);
    }
}

BTree::~BTree() {
    if (idx_file.is_open()) idx_file.close();
    if (dat_file.is_open()) dat_file.close();
}

size_t BTree::_allocate_node() {
    idx_file.seekp(0, std::ios::end);
    size_t offset = idx_file.tellp();
    if (offset < DATA_START_OFFSET) offset = DATA_START_OFFSET;
    
    // Reserve the space on disk so subsequent allocations don't return the same offset
    BTreeNode dummy;
    memset(&dummy, 0, sizeof(BTreeNode));
    idx_file.seekp(offset);
    idx_file.write(reinterpret_cast<const char*>(&dummy), sizeof(BTreeNode));
    idx_file.flush();
    
    return offset;
}

void BTree::_read_node(size_t offset, BTreeNode& node) {
    idx_file.seekg(offset);
    idx_file.read(reinterpret_cast<char*>(&node), sizeof(BTreeNode));
}

void BTree::_write_node(size_t offset, const BTreeNode& node) {
    idx_file.seekp(offset);
    idx_file.write(reinterpret_cast<const char*>(&node), sizeof(BTreeNode));
}

SearchResult BTree::_search(size_t node_offset, const std::string& key) {
    BTreeNode node;
    _read_node(node_offset, node);
    
    int i = 0;
    while (i < node.num_keys && key > node.keys[i]) {
        i++;
    }
    
    if (i < node.num_keys && key == node.keys[i]) {
        if (!node.is_deleted[i]) {
            return {true, node.value_offsets[i], node.value_lengths[i], node.versions[i]};
        }
        return {false, 0, 0, 0};
    }
    
    if (node.is_leaf) {
        return {false, 0, 0, 0};
    }
    
    return _search(node.child_pointers[i], key);
}

std::string BTree::search(const std::string& key) {
    uint64_t dummy;
    return search(key, dummy);
}

std::string BTree::search(const std::string& key, uint64_t& out_version) {
    SearchResult res = _search(root_offset, key);
    if (res.found) {
        dat_file.seekg(res.value_offset);
        std::string value(res.value_length, '\0');
        dat_file.read(&value[0], res.value_length);
        out_version = res.version;
        return value;
    }
    out_version = 0;
    return "NULL";
}

void BTree::_split_child(size_t parent_offset, BTreeNode& parent, int i, size_t child_offset, BTreeNode& child) {
    size_t new_child_offset = _allocate_node();
    BTreeNode new_child;
    new_child.is_leaf = child.is_leaf;
    
    int t = (MAX_KEYS + 1) / 2;
    new_child.num_keys = MAX_KEYS - t; 
    
    for (int j = 0; j < new_child.num_keys; j++) {
        strncpy(new_child.keys[j], child.keys[j + t], MAX_KEY_LEN);
        new_child.value_offsets[j] = child.value_offsets[j + t];
        new_child.value_lengths[j] = child.value_lengths[j + t];
        new_child.is_deleted[j] = child.is_deleted[j + t];
        new_child.versions[j] = child.versions[j + t];
    }
    
    if (!child.is_leaf) {
        for (int j = 0; j <= new_child.num_keys; j++) {
            new_child.child_pointers[j] = child.child_pointers[j + t];
        }
    }
    
    child.num_keys = t - 1;
    
    for (int j = parent.num_keys; j >= i + 1; j--) {
        parent.child_pointers[j + 1] = parent.child_pointers[j];
    }
    parent.child_pointers[i + 1] = new_child_offset;
    
    for (int j = parent.num_keys - 1; j >= i; j--) {
        strncpy(parent.keys[j + 1], parent.keys[j], MAX_KEY_LEN);
        parent.value_offsets[j + 1] = parent.value_offsets[j];
        parent.value_lengths[j + 1] = parent.value_lengths[j];
        parent.is_deleted[j + 1] = parent.is_deleted[j];
        parent.versions[j + 1] = parent.versions[j];
    }
    
    strncpy(parent.keys[i], child.keys[t - 1], MAX_KEY_LEN);
    parent.value_offsets[i] = child.value_offsets[t - 1];
    parent.value_lengths[i] = child.value_lengths[t - 1];
    parent.is_deleted[i] = child.is_deleted[t - 1];
    parent.versions[i] = child.versions[t - 1];
    parent.num_keys++;
    
    _write_node(child_offset, child);
    _write_node(new_child_offset, new_child);
    _write_node(parent_offset, parent);
}

void BTree::_insert_non_full(size_t node_offset, BTreeNode& node, const std::string& key, size_t val_offset, size_t val_len, uint64_t version) {
    int i = node.num_keys - 1;
    
    // Check for duplicate key — only update if incoming version >= existing version
    for (int j = 0; j < node.num_keys; j++) {
        if (key == node.keys[j]) {
            if (version >= node.versions[j]) {
                node.value_offsets[j] = val_offset;
                node.value_lengths[j] = val_len;
                node.is_deleted[j] = false;
                node.versions[j] = version;
                _write_node(node_offset, node);
            }
            return;
        }
    }

    if (node.is_leaf) {
        while (i >= 0 && key < node.keys[i]) {
            strncpy(node.keys[i + 1], node.keys[i], MAX_KEY_LEN);
            node.value_offsets[i + 1] = node.value_offsets[i];
            node.value_lengths[i + 1] = node.value_lengths[i];
            node.is_deleted[i + 1] = node.is_deleted[i];
            node.versions[i + 1] = node.versions[i];
            i--;
        }
        
        strncpy(node.keys[i + 1], key.c_str(), MAX_KEY_LEN);
        node.value_offsets[i + 1] = val_offset;
        node.value_lengths[i + 1] = val_len;
        node.is_deleted[i + 1] = false;
        node.versions[i + 1] = version;
        node.num_keys++;
        
        _write_node(node_offset, node);
    } else {
        while (i >= 0 && key < node.keys[i]) {
            i--;
        }
        i++;
        
        BTreeNode child;
        _read_node(node.child_pointers[i], child);
        
        if (child.num_keys == MAX_KEYS) {
            _split_child(node_offset, node, i, node.child_pointers[i], child);
            if (key > node.keys[i]) {
                i++;
            }
        }
        
        _read_node(node.child_pointers[i], child);
        _insert_non_full(node.child_pointers[i], child, key, val_offset, val_len, version);
    }
}

void BTree::insert(const std::string& key, const std::string& value, uint64_t version) {
    if (key.length() >= MAX_KEY_LEN) {
        std::cerr << "Key too long!" << std::endl;
        return;
    }
    
    dat_file.seekp(0, std::ios::end);
    size_t val_offset = dat_file.tellp();
    size_t val_len = value.length();
    dat_file.write(value.c_str(), val_len);
    dat_file.flush();
    
    BTreeNode root;
    _read_node(root_offset, root);
    
    if (root.num_keys == MAX_KEYS) {
        size_t new_root_offset = _allocate_node();
        BTreeNode new_root;
        new_root.is_leaf = false;
        new_root.num_keys = 0;
        new_root.child_pointers[0] = root_offset;
        
        _split_child(new_root_offset, new_root, 0, root_offset, root);
        
        int i = 0;
        if (new_root.keys[0] < key) {
            i++;
        }
        
        BTreeNode child;
        _read_node(new_root.child_pointers[i], child);
        _insert_non_full(new_root.child_pointers[i], child, key, val_offset, val_len, version);
        
        root_offset = new_root_offset;
        idx_file.seekp(ROOT_POINTER_OFFSET);
        idx_file.write(reinterpret_cast<const char*>(&root_offset), sizeof(size_t));
        idx_file.flush();
    } else {
        _insert_non_full(root_offset, root, key, val_offset, val_len, version);
    }
}

bool BTree::remove(const std::string& key) {
    size_t curr_offset = root_offset;
    while(true) {
        BTreeNode node;
        _read_node(curr_offset, node);
        
        int i = 0;
        while (i < node.num_keys && key > node.keys[i]) {
            i++;
        }
        
        if (i < node.num_keys && key == node.keys[i]) {
            if (!node.is_deleted[i]) {
                node.is_deleted[i] = true;
                _write_node(curr_offset, node);
                return true;
            }
            return false;
        }
        
        if (node.is_leaf) {
            return false;
        }
        
        curr_offset = node.child_pointers[i];
    }
    return false;
}

void BTree::_collect_all(size_t node_offset, std::vector<std::tuple<std::string, std::string, uint64_t>>& results) {
    BTreeNode node;
    _read_node(node_offset, node);
    
    for (int i = 0; i < node.num_keys; i++) {
        // Visit left child first (in-order traversal)
        if (!node.is_leaf) {
            _collect_all(node.child_pointers[i], results);
        }
        
        // Visit this key (skip deleted entries)
        if (!node.is_deleted[i]) {
            dat_file.seekg(node.value_offsets[i]);
            std::string value(node.value_lengths[i], '\0');
            dat_file.read(&value[0], node.value_lengths[i]);
            results.emplace_back(std::string(node.keys[i]), value, node.versions[i]);
        }
    }
    
    // Visit rightmost child
    if (!node.is_leaf) {
        _collect_all(node.child_pointers[node.num_keys], results);
    }
}

std::vector<std::tuple<std::string, std::string, uint64_t>> BTree::get_all_entries() {
    std::vector<std::tuple<std::string, std::string, uint64_t>> results;
    _collect_all(root_offset, results);
    return results;
}
