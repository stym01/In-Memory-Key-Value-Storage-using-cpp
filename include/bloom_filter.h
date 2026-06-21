#ifndef BLOOM_FILTER_H
#define BLOOM_FILTER_H

#include <vector>
#include <string>

class BloomFilter {
private:
    std::vector<bool> bits;
    size_t num_hashes;

    // Hash functions
    size_t hash1(const std::string& key) const;
    size_t hash2(const std::string& key) const;
    size_t hash3(const std::string& key) const;

public:
    // Default config: 1,000,000 bits (approx 122 KB) and 3 hash functions
    BloomFilter(size_t size = 1000000, size_t hashes = 3);
    
    void add(const std::string& key);
    bool possibly_contains(const std::string& key) const;
};

#endif
