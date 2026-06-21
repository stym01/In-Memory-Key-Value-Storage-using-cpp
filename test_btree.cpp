#include "include/btree.h"
#include <iostream>

int main() {
    BTree tree;
    std::cout << "Tree created" << std::endl;
    
    std::cout << "Inserting 0" << std::endl;
    tree.insert("key0", "value0");
    std::cout << "Inserted 0" << std::endl;
    
    std::cout << "Inserting 1" << std::endl;
    tree.insert("key1", "value1");
    std::cout << "Inserted 1" << std::endl;
    
    std::cout << "Inserting 2" << std::endl;
    tree.insert("key2", "value2");
    std::cout << "Inserted 2" << std::endl;
    
    std::cout << "Inserting 3" << std::endl;
    tree.insert("key3", "value3");
    std::cout << "Inserted 3" << std::endl;
    
    return 0;
}
