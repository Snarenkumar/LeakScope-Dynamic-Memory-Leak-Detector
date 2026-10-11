#include <iostream>
#include <thread>
#include <vector>
#include <cstdlib>

void worker(int id) {
    for (int i = 0; i < 20; i++) {
        void* p = malloc(64);
        free(p);
    }
    // Thread 2 intentionally leaks one block
    if (id == 2) {
        int* leaked = new int[10];
        leaked[0] = 123;
    }
}

int main() {
    std::cout << "[threads] Launching 4 worker threads..." << std::endl;
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }
    std::cout << "[threads] All threads joined. Process exiting." << std::endl;
    return 0;
}
