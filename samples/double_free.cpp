#include <iostream>
#include <cstdlib>

int main() {
    std::cout << "[double_free] Allocating memory..." << std::endl;
    void* ptr = malloc(128);

    std::cout << "[double_free] Freeing pointer for the first time (valid)..." << std::endl;
    free(ptr);

    std::cout << "[double_free] Freeing pointer a second time (should detect DOUBLE FREE)..." << std::endl;
    free(ptr);

    std::cout << "[double_free] Freeing wild pointer (should detect INVALID FREE)..." << std::endl;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
    void* bogus_ptr = (void*)0xDEADBEEF;
    free(bogus_ptr);
#pragma GCC diagnostic pop

    std::cout << "[double_free] Completed safely without crashing." << std::endl;
    return 0;
}
