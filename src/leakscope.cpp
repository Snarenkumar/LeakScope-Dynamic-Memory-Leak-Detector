// LeakScope - Dynamic Memory Leak Detector using LD_PRELOAD
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>

// Maximum backtrace frames captured per allocation
static constexpr size_t MAX_STACK_FRAMES = 8;
// Fixed-size hash table capacity for tracking allocations
static constexpr size_t TABLE_SIZE = 65536;
// Temporary static buffer size for early glibc dlsym bootstrap allocations
static constexpr size_t BOOTSTRAP_BUF_SIZE = 8192;

// Slot states for open addressing with tombstones
enum SlotState : uint8_t { SLOT_EMPTY = 0, SLOT_ACTIVE = 1, SLOT_FREED = 2 };
enum FreeStatus { FREE_OK, FREE_DOUBLE, FREE_INVALID };

// Allocation record stored entirely in static memory without using malloc
struct Record {
    void* ptr;
    size_t size;
    void* stack[MAX_STACK_FRAMES];
    int stack_depth;
    SlotState state;
};

// Summary structure for grouping leaks by unique stack trace
struct LeakGroup {
    void* stack[MAX_STACK_FRAMES];
    int depth;
    size_t total_bytes;
    size_t count;
};

// Isolated internal storage: pre-allocated static table avoids calling malloc
static Record g_table[TABLE_SIZE];
static size_t g_current_bytes = 0;
static size_t g_peak_bytes = 0;
static size_t g_total_allocs = 0;
static size_t g_total_frees = 0;
static size_t g_dropped_records = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

// Re-entrancy guard: prevents infinite recursion when backtrace() or internal code calls malloc
static thread_local bool g_in_hook = false;

// Function pointers to the real C library allocation routines
typedef void* (*malloc_fn_t)(size_t);
typedef void* (*calloc_fn_t)(size_t, size_t);
typedef void* (*realloc_fn_t)(void*, size_t);
typedef void  (*free_fn_t)(void*);

static malloc_fn_t  real_malloc  = nullptr;
static calloc_fn_t  real_calloc  = nullptr;
static realloc_fn_t real_realloc = nullptr;
static free_fn_t    real_free    = nullptr;

// dlsym bootstrap trap: dlsym calls calloc internally while resolving symbols.
// This static buffer serves memory before real_calloc is available.
static char g_bootstrap_buf[BOOTSTRAP_BUF_SIZE];
static size_t g_bootstrap_used = 0;
static bool g_initializing = false;

// Locates the original C runtime functions using dlsym(RTLD_NEXT)
static void init_real_functions() {
    g_initializing = true;
    real_malloc  = (malloc_fn_t)dlsym(RTLD_NEXT, "malloc");
    real_calloc  = (calloc_fn_t)dlsym(RTLD_NEXT, "calloc");
    real_realloc = (realloc_fn_t)dlsym(RTLD_NEXT, "realloc");
    real_free    = (free_fn_t)dlsym(RTLD_NEXT, "free");
    g_initializing = false;
}

// Bumps a pointer from the static bootstrap buffer aligned to 16 bytes
static void* bootstrap_alloc(size_t size) {
    size_t aligned = (size + 15) & ~((size_t)15);
    if (g_bootstrap_used + aligned > BOOTSTRAP_BUF_SIZE) return nullptr;
    void* ptr = &g_bootstrap_buf[g_bootstrap_used];
    g_bootstrap_used += aligned;
    return ptr;
}

// Checks if an address points to our bootstrap buffer
static inline bool is_bootstrap_ptr(void* ptr) {
    return (ptr >= (void*)g_bootstrap_buf && ptr < (void*)(g_bootstrap_buf + BOOTSTRAP_BUF_SIZE));
}

// Hash function mapping pointer addresses to hash table slots
static inline size_t hash_ptr(void* ptr) {
    uintptr_t v = (uintptr_t)ptr >> 4;
    return (v ^ (v >> 16)) % TABLE_SIZE;
}

// Probes for an existing record matching ptr or finds the first reusable slot
static ssize_t find_or_alloc_slot(void* ptr) {
    size_t idx = hash_ptr(ptr);
    ssize_t slot = -1, avail = -1;
    for (size_t i = 0; i < TABLE_SIZE; i++) {
        if (g_table[idx].ptr == ptr && g_table[idx].state != SLOT_EMPTY) {
            slot = idx;
            break;
        }
        if (avail == -1 && g_table[idx].state != SLOT_ACTIVE) avail = idx;
        if (g_table[idx].state == SLOT_EMPTY) break;
        idx = (idx + 1) % TABLE_SIZE;
    }
    return (slot != -1) ? slot : avail;
}

// Records an active allocation and captures peak watermark
static void record_alloc(void* ptr, size_t size, void** frames, int depth) {
    if (!ptr) return;
    pthread_mutex_lock(&g_mutex);
    g_total_allocs++;
    g_current_bytes += size;
    if (g_current_bytes > g_peak_bytes) g_peak_bytes = g_current_bytes;
    ssize_t slot = find_or_alloc_slot(ptr);
    if (slot != -1) {
        g_table[slot].ptr = ptr;
        g_table[slot].size = size;
        g_table[slot].state = SLOT_ACTIVE;
        g_table[slot].stack_depth = depth;
        for (int i = 0; i < depth; i++) g_table[slot].stack[i] = frames[i];
    } else {
        g_dropped_records++;
    }
    pthread_mutex_unlock(&g_mutex);
}

// Marks an allocation as freed or detects double-free / invalid-free
static FreeStatus record_free(void* ptr) {
    pthread_mutex_lock(&g_mutex);
    size_t idx = hash_ptr(ptr);
    FreeStatus status = FREE_INVALID;
    for (size_t i = 0; i < TABLE_SIZE; i++) {
        if (g_table[idx].ptr == ptr && g_table[idx].state != SLOT_EMPTY) {
            if (g_table[idx].state == SLOT_ACTIVE) {
                g_current_bytes -= g_table[idx].size;
                g_total_frees++;
                g_table[idx].state = SLOT_FREED;
                status = FREE_OK;
            } else {
                status = FREE_DOUBLE;
            }
            break;
        }
        if (g_table[idx].state == SLOT_EMPTY) break;
        idx = (idx + 1) % TABLE_SIZE;
    }
    pthread_mutex_unlock(&g_mutex);
    return status;
}

// Prints one-line diagnostic message for double free or invalid free
static void print_error(const char* type, void* ptr, void** frames, int depth) {
    fprintf(stderr, "[LeakScope] ERROR: %s detected for pointer %p | stack:", type, ptr);
    for (int i = 0; i < depth; i++) fprintf(stderr, " %p", frames[i]);
    fprintf(stderr, "\n");
}

// Intercepts malloc to record size and call stack
extern "C" void* malloc(size_t size) noexcept {
    if (g_in_hook) return real_malloc ? real_malloc(size) : bootstrap_alloc(size);
    g_in_hook = true;
    if (!real_malloc) init_real_functions();
    void* ptr = real_malloc(size);
    void* frames[MAX_STACK_FRAMES];
    int depth = backtrace(frames, MAX_STACK_FRAMES);
    record_alloc(ptr, size, frames, depth);
    g_in_hook = false;
    return ptr;
}

// Intercepts calloc while guarding against the dlsym bootstrap trap
extern "C" void* calloc(size_t nmemb, size_t size) noexcept {
    if (!real_calloc || g_initializing) {
        void* ptr = bootstrap_alloc(nmemb * size);
        if (ptr) memset(ptr, 0, nmemb * size);
        return ptr;
    }
    if (g_in_hook) return real_calloc(nmemb, size);
    g_in_hook = true;
    void* ptr = real_calloc(nmemb, size);
    void* frames[MAX_STACK_FRAMES];
    int depth = backtrace(frames, MAX_STACK_FRAMES);
    record_alloc(ptr, nmemb * size, frames, depth);
    g_in_hook = false;
    return ptr;
}

// Intercepts free, preventing crashes on bad pointers and updating table
extern "C" void free(void* ptr) noexcept {
    if (!ptr || is_bootstrap_ptr(ptr)) return;
    if (g_in_hook) { if (real_free) real_free(ptr); return; }
    g_in_hook = true;
    if (!real_free) init_real_functions();
    void* frames[MAX_STACK_FRAMES];
    int depth = backtrace(frames, MAX_STACK_FRAMES);
    FreeStatus st = record_free(ptr);
    if (st == FREE_OK) {
        real_free(ptr);
    } else if (st == FREE_DOUBLE) {
        print_error("DOUBLE FREE", ptr, frames, depth);
    } else {
        print_error("INVALID FREE", ptr, frames, depth);
    }
    g_in_hook = false;
}

// Intercepts realloc, adjusting tracking records for old and new addresses
extern "C" void* realloc(void* ptr, size_t size) noexcept {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return nullptr; }
    if (g_in_hook) return real_realloc ? real_realloc(ptr, size) : nullptr;
    g_in_hook = true;
    if (!real_realloc) init_real_functions();
    void* frames[MAX_STACK_FRAMES];
    int depth = backtrace(frames, MAX_STACK_FRAMES);
    void* new_ptr = real_realloc(ptr, size);
    if (new_ptr) {
        record_free(ptr);
        record_alloc(new_ptr, size, frames, depth);
    }
    g_in_hook = false;
    return new_ptr;
}

// Operator new and delete overrides route through our intercepted malloc and free
void* operator new(size_t s) { void* p = malloc(s); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t s) { void* p = malloc(s); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
void* operator new(size_t s, const std::nothrow_t&) noexcept { return malloc(s); }
void* operator new[](size_t s, const std::nothrow_t&) noexcept { return malloc(s); }
void operator delete(void* p, const std::nothrow_t&) noexcept { free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { free(p); }

// Prints stack frame address with module name and offset if available
static void print_frame_line(int frame_idx, void* addr) {
    Dl_info info;
    if (dladdr(addr, &info) && info.dli_fname) {
        uintptr_t off = (uintptr_t)addr - (uintptr_t)info.dli_fbase;
        fprintf(stderr, "    frame [%d]: %p (%s+0x%lx)\n", frame_idx, addr, info.dli_fname, (unsigned long)off);
    } else {
        fprintf(stderr, "    frame [%d]: %p\n", frame_idx, addr);
    }
}

// Groups leak records having identical call stacks together
static void add_to_group(LeakGroup* groups, size_t& num_groups, const Record& rec) {
    for (size_t g = 0; g < num_groups; g++) {
        if (groups[g].depth == rec.stack_depth &&
            memcmp(groups[g].stack, rec.stack, rec.stack_depth * sizeof(void*)) == 0) {
            groups[g].total_bytes += rec.size;
            groups[g].count++;
            return;
        }
    }
    if (num_groups < 64) {
        groups[num_groups].depth = rec.stack_depth;
        memcpy(groups[num_groups].stack, rec.stack, rec.stack_depth * sizeof(void*));
        groups[num_groups].total_bytes = rec.size;
        groups[num_groups].count = 1;
        num_groups++;
    }
}

// Sorts leak groups descending by total leaked bytes
static void sort_groups(LeakGroup* groups, size_t num_groups) {
    for (size_t i = 0; i < num_groups; i++) {
        for (size_t j = i + 1; j < num_groups; j++) {
            if (groups[j].total_bytes > groups[i].total_bytes) {
                LeakGroup temp = groups[i];
                groups[i] = groups[j];
                groups[j] = temp;
            }
        }
    }
}

// Formats and prints top 10 leak groups with their call stacks
static void print_leak_groups(LeakGroup* groups, size_t num_groups) {
    size_t limit = num_groups < 10 ? num_groups : 10;
    for (size_t g = 0; g < limit; g++) {
        fprintf(stderr, "  Leak #%zu: %zu bytes leaked across %zu block(s)\n",
                g + 1, groups[g].total_bytes, groups[g].count);
        for (int f = 0; f < groups[g].depth; f++) print_frame_line(f, groups[g].stack[f]);
    }
}

// Prints the header and high-level allocation metrics table
static void print_summary_header(size_t blocks, size_t bytes) {
    fprintf(stderr, "\n=================================================================\n");
    fprintf(stderr, "                    LeakScope Memory Report                      \n");
    fprintf(stderr, "=================================================================\n");
    fprintf(stderr, "  Total Allocations:   %zu\n", g_total_allocs);
    fprintf(stderr, "  Total Frees:         %zu\n", g_total_frees);
    fprintf(stderr, "  Peak Memory Usage:   %zu bytes\n", g_peak_bytes);
    fprintf(stderr, "  Dropped Records:     %zu\n", g_dropped_records);
    fprintf(stderr, "-----------------------------------------------------------------\n");
    fprintf(stderr, "  LEAK SUMMARY:\n");
    fprintf(stderr, "  Leaked Blocks:       %zu (%zu leaked blocks, %zu leaks)\n", blocks, blocks, blocks);
    fprintf(stderr, "  Leaked Bytes:        %zu bytes\n", bytes);
    fprintf(stderr, "-----------------------------------------------------------------\n");
}

// Returns true if an allocation originates from user code rather than runtime internals
static bool is_user_leak(void** stack, int depth) {
    for (int i = 1; i < depth; i++) {
        Dl_info info;
        if (dladdr(stack[i], &info) && info.dli_fname) {
            if (strstr(info.dli_fname, "ld-linux") != nullptr) return false;
            if (strstr(info.dli_fname, ".so") == nullptr) return true;
        }
    }
    return false;
}

// Destructor runs automatically at process exit to print the leak report
__attribute__((destructor))
static void leakscope_exit_report() {
    g_in_hook = true; // Turn off hooks so report generation is not tracked
    size_t leaked_blocks = 0, leaked_bytes = 0, num_groups = 0;
    LeakGroup groups[64];
    for (size_t i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].state == SLOT_ACTIVE && is_user_leak(g_table[i].stack, g_table[i].stack_depth)) {
            leaked_blocks++;
            leaked_bytes += g_table[i].size;
            add_to_group(groups, num_groups, g_table[i]);
        }
    }
    sort_groups(groups, num_groups);
    print_summary_header(leaked_blocks, leaked_bytes);
    print_leak_groups(groups, num_groups);
    fprintf(stderr, "=================================================================\n\n");
}
