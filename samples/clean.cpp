#include <iostream>
#include <cstdlib>

int main() {
    std::cout << "[clean] Performing allocations..." << std::endl;
    void* p1 = malloc(64);
    int* p2 = new int[10];
    void* p3 = calloc(4, 16);

    std::cout << "[clean] Freeing all allocated memory..." << std::endl;
    free(p1);
    delete[] p2;
    free(p3);

    std::cout << "[clean] All memory cleaned up." << std::endl;
    return 0;
}
