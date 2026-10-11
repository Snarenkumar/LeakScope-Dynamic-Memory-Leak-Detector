#include <iostream>
#include <cstdlib>

// Helper function 1: allocates two separate leaky blocks
void leak_from_helper() {
    int* p1 = new int[25];        // Leaks 100 bytes (25 * 4)
    char* p2 = (char*)malloc(50); // Leaks 50 bytes
    p1[0] = 42;
    p2[0] = 'A';
}

// Helper function 2: allocates one leaky block
void leak_from_main_task() {
    int* p3 = new int[50];        // Leaks 200 bytes (50 * 4)
    p3[0] = 99;
}

int main() {
    std::cout << "[leaky] Starting execution..." << std::endl;
    // A clean allocation that gets properly freed
    void* clean_ptr = malloc(128);
    free(clean_ptr);

    // Call leaking functions
    leak_from_helper();
    leak_from_main_task();

    std::cout << "[leaky] Finished. Process exiting." << std::endl;
    return 0;
}
