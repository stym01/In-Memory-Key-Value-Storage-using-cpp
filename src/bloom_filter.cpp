#include "../include/bloom_filter.h"
#include <functional>

BloomFilter::BloomFilter(size_t size, size_t hashes) : bits(size, false), num_hashes(hashes) {}

size_t BloomFilter::hash1(const std::string& key) const {
    return std::hash<std::string>{}(key);
}

// FNV-1a hash
size_t BloomFilter::hash2(const std::string& key) const {
    size_t hash = 2166136261u;
    for (char c : key) {
        hash ^= static_cast<size_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

// DJB2 hash
size_t BloomFilter::hash3(const std::string& key) const {
    size_t hash = 5381;
    for (char c : key) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash;
}

void BloomFilter::add(const std::string& key) {
    if (bits.empty()) return;
    bits[hash1(key) % bits.size()] = true;
    if (num_hashes > 1) bits[hash2(key) % bits.size()] = true;
    if (num_hashes > 2) bits[hash3(key) % bits.size()] = true;
}

bool BloomFilter::possibly_contains(const std::string& key) const {
    if (bits.empty()) return false;
    if (!bits[hash1(key) % bits.size()]) return false;
    if (num_hashes > 1 && !bits[hash2(key) % bits.size()]) return false;
    if (num_hashes > 2 && !bits[hash3(key) % bits.size()]) return false;
    return true;
}
