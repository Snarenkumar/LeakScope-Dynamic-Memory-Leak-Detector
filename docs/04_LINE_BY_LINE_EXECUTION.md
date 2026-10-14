# LeakScope: Line-by-Line Source Code Walkthrough

This document walks through `src/leakscope.cpp` section by section, explaining exactly what each block does, why it was written that way, and what would break if it were removed.

---

## Part 1: Headers, Constants, and Data Structures (Lines 1–41)

### Headers

```cpp
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
```
- **Why:** Enables GNU extensions in glibc. Required for `RTLD_NEXT` in `<dlfcn.h>` and `backtrace()` in `<execinfo.h>`. Without this, `dlsym(RTLD_NEXT, ...)` would fail to compile.

```cpp
#include <dlfcn.h>     // dlsym, dladdr, RTLD_NEXT
#include <execinfo.h>  // backtrace()
#include <pthread.h>   // pthread_mutex_t, PTHREAD_MUTEX_INITIALIZER
```
- `<dlfcn.h>`: Provides `dlsym(RTLD_NEXT, "func")` to find the real glibc function and `dladdr()` to resolve addresses back to library names.
- `<execinfo.h>`: Provides `backtrace(frames, max)` which walks the call stack filling an array of `void*` instruction pointers.
- `<pthread.h>`: Provides `pthread_mutex_t` and lock/unlock — required for thread safety.

---

### Constants

```cpp
static constexpr size_t MAX_STACK_FRAMES = 8;
static constexpr size_t TABLE_SIZE = 65536;
static constexpr size_t BOOTSTRAP_BUF_SIZE = 8192;
```

| Constant | Value | Why That Value |
|---|---|---|
| `MAX_STACK_FRAMES` | 8 | Captures enough call chain to identify the leak site without large memory cost per record |
| `TABLE_SIZE` | 65536 | Powers-of-two size makes modulo `% TABLE_SIZE` efficient. Covers typical programs without dynamic growth |
| `BOOTSTRAP_BUF_SIZE` | 8192 | 8 KB is enough for all internal `calloc` calls that `dlsym` makes during initialization |

---

### Slot States

```cpp
enum SlotState : uint8_t { SLOT_EMPTY = 0, SLOT_ACTIVE = 1, SLOT_FREED = 2 };
enum FreeStatus { FREE_OK, FREE_DOUBLE, FREE_INVALID };
```
- `SLOT_EMPTY`: The table slot has never been used (zero-initialized from `.bss`).
- `SLOT_ACTIVE`: Holds a live allocation that has not been freed yet.
- `SLOT_FREED`: The allocation was freed. The slot is a "tombstone" — kept so double-free detection still works (linear probe can find it again).
- `FREE_OK`: `free()` matched a live slot — correct usage.
- `FREE_DOUBLE`: `free()` found an already-`SLOT_FREED` slot — the pointer was freed twice.
- `FREE_INVALID`: `free()` hit a `SLOT_EMPTY` during probe — the pointer was never allocated here.

---

### Record and LeakGroup

```cpp
struct Record {
    void* ptr;
    size_t size;
    void* stack[MAX_STACK_FRAMES];
    int stack_depth;
    SlotState state;
};
```
- `ptr`: The raw memory address returned by `malloc`/`calloc`/`realloc`.
- `size`: Number of bytes allocated.
- `stack[8]`: Raw instruction pointer addresses from `backtrace()`.
- `stack_depth`: Actual number of valid frames captured (up to 8).
- `state`: `SLOT_EMPTY`, `SLOT_ACTIVE`, or `SLOT_FREED`.

```cpp
struct LeakGroup {
    void* stack[MAX_STACK_FRAMES];
    int depth;
    size_t total_bytes;
    size_t count;
};
```
Used only at exit: multiple `Record` entries with identical stacks are merged into one `LeakGroup` for compact reporting.

---

## Part 2: Global State (Lines 44–70)

```cpp
static Record g_table[TABLE_SIZE];           // The 65536-slot hash table
static size_t g_current_bytes = 0;           // Currently live bytes
static size_t g_peak_bytes = 0;              // Highest g_current_bytes ever seen
static size_t g_total_allocs = 0;            // Total allocation call count
static size_t g_total_frees = 0;             // Total successful free count
static size_t g_dropped_records = 0;         // Allocations not tracked (table full)
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
```
- All of these live in `.bss` (zero-initialized static storage). No `malloc` is ever called to initialize them.
- `PTHREAD_MUTEX_INITIALIZER` is a compile-time macro — no `pthread_mutex_init()` call is needed.

```cpp
static thread_local bool g_in_hook = false;
```
- **`thread_local`**: Each thread gets its own copy. Thread A's re-entrancy state never interferes with Thread B.
- Set to `true` before entering any hook logic; reset to `false` on exit. Prevents `backtrace()` or `fprintf()` from re-entering our `malloc` hook recursively.

```cpp
static malloc_fn_t  real_malloc  = nullptr;
static calloc_fn_t  real_calloc  = nullptr;
static realloc_fn_t real_realloc = nullptr;
static free_fn_t    real_free    = nullptr;
```
- Function pointers to the real glibc allocators. Initialized once by `init_real_functions()`.
- Until they are set, the bootstrap buffer serves any `calloc` calls.

```cpp
static char g_bootstrap_buf[BOOTSTRAP_BUF_SIZE];  // 8192-byte static scratch buffer
static size_t g_bootstrap_used = 0;               // Bump pointer into the buffer
static bool g_initializing = false;               // True while dlsym is running
```
- Breaks the circular dependency: `dlsym` calls `calloc` before `real_calloc` is resolved.

---

## Part 3: Initialization and Bootstrap (Lines 73–100)

```cpp
static void init_real_functions() {
    g_initializing = true;
    real_malloc  = (malloc_fn_t)dlsym(RTLD_NEXT, "malloc");
    real_calloc  = (calloc_fn_t)dlsym(RTLD_NEXT, "calloc");
    real_realloc = (realloc_fn_t)dlsym(RTLD_NEXT, "realloc");
    real_free    = (free_fn_t)dlsym(RTLD_NEXT, "free");
    g_initializing = false;
}
```
- `RTLD_NEXT`: "Find the next occurrence of this symbol after the current shared object in the load order." This skips our own `libleakscope.so` and returns glibc's implementation.
- `g_initializing = true` during this call: if `dlsym` calls `calloc`, our `calloc` intercept sees `g_initializing == true` and immediately serves from `g_bootstrap_buf`.

```cpp
static void* bootstrap_alloc(size_t size) {
    size_t aligned = (size + 15) & ~((size_t)15);  // Round up to 16-byte alignment
    if (g_bootstrap_used + aligned > BOOTSTRAP_BUF_SIZE) return nullptr;
    void* ptr = &g_bootstrap_buf[g_bootstrap_used];
    g_bootstrap_used += aligned;
    return ptr;
}
```
- Pure bump allocator. Never frees. Safe because bootstrap allocations are short-lived and small.
- 16-byte alignment satisfies all POSIX alignment requirements (SIMD, struct padding, etc.).

```cpp
static inline bool is_bootstrap_ptr(void* ptr) {
    return (ptr >= (void*)g_bootstrap_buf && ptr < (void*)(g_bootstrap_buf + BOOTSTRAP_BUF_SIZE));
}
```
- Used in `free()`: if the pointer lands inside our static buffer, skip real `free` silently.

---

## Part 4: Hash Table Operations (Lines 97–136)

```cpp
static inline size_t hash_ptr(void* ptr) {
    uintptr_t v = (uintptr_t)ptr >> 4;
    return (v ^ (v >> 16)) % TABLE_SIZE;
}
```
- Right-shift by 4: pointers from `malloc` are always 16-byte aligned, so the bottom 4 bits are always 0. Removing them spreads distribution better.
- XOR with high bits: mixes high-entropy bits into the lower range to reduce clustering.

```cpp
static ssize_t find_or_alloc_slot(void* ptr) {
    size_t idx = hash_ptr(ptr);
    ssize_t slot = -1, avail = -1;
    for (size_t i = 0; i < TABLE_SIZE; i++) {
        if (g_table[idx].ptr == ptr && g_table[idx].state != SLOT_EMPTY) {
            slot = idx; break;
        }
        if (avail == -1 && g_table[idx].state != SLOT_ACTIVE) avail = idx;
        if (g_table[idx].state == SLOT_EMPTY) break;
        idx = (idx + 1) % TABLE_SIZE;
    }
    return (slot != -1) ? slot : avail;
}
```
- **Linear probing**: on collision, advance by 1. Stops at `SLOT_EMPTY` (never used).
- If the exact pointer is found (active or freed), returns it (for double-free detection).
- If the exact pointer is not found, returns the first non-ACTIVE slot (either EMPTY or FREED tombstone) as the insertion point.

---

## Part 5: Record Management (Lines 119–167)

```cpp
static void record_alloc(void* ptr, size_t size, void** frames, int depth) {
    if (!ptr) return;
    pthread_mutex_lock(&g_mutex);
    g_total_allocs++;
    g_current_bytes += size;
    if (g_current_bytes > g_peak_bytes) g_peak_bytes = g_current_bytes;
    ssize_t slot = find_or_alloc_slot(ptr);
    if (slot != -1) {
        g_table[slot].ptr   = ptr;
        g_table[slot].size  = size;
        g_table[slot].state = SLOT_ACTIVE;
        g_table[slot].stack_depth = depth;
        for (int i = 0; i < depth; i++) g_table[slot].stack[i] = frames[i];
    } else {
        g_dropped_records++;
    }
    pthread_mutex_unlock(&g_mutex);
}
```
- `if (!ptr) return`: `malloc(0)` is legal and can return null or a unique pointer; skip null.
- Peak watermark is updated inside the mutex so it is always consistent.
- Backtrace frames are copied into the slot — they were captured before locking (by the caller).

```cpp
static FreeStatus record_free(void* ptr) {
    pthread_mutex_lock(&g_mutex);
    ...
    if (g_table[idx].state == SLOT_ACTIVE) {
        g_current_bytes -= g_table[idx].size;
        g_total_frees++;
        g_table[idx].state = SLOT_FREED;    // Tombstone — keeps slot for double-free detection
        status = FREE_OK;
    } else {                                 // SLOT_FREED
        status = FREE_DOUBLE;
    }
    ...
    if (g_table[idx].state == SLOT_EMPTY) break;  // Probe gap — pointer was never tracked
    ...
    pthread_mutex_unlock(&g_mutex);
    return status;
}
```
- The tombstone (`SLOT_FREED`) is critical: it ensures the next `free(ptr)` on the same pointer still finds the slot (not a gap) and returns `FREE_DOUBLE`.

---

## Part 6: The Four Intercepted Functions (Lines 170–246)

### `malloc`
```cpp
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
```
- `extern "C"`: Prevents C++ name mangling. The dynamic linker resolves the symbol `malloc` (not `_Z6mallocm`).
- `noexcept`: Required because the C standard says `malloc` must not throw.
- If `g_in_hook`: bypass all tracking and call real allocator directly. This is the re-entrancy guard.

### `calloc`
```cpp
extern "C" void* calloc(size_t nmemb, size_t size) noexcept {
    if (!real_calloc || g_initializing) {
        void* ptr = bootstrap_alloc(nmemb * size);
        if (ptr) memset(ptr, 0, nmemb * size);   // calloc must zero-initialize
        return ptr;
    }
    ...
}
```
- **Only `calloc`** has the bootstrap guard because `dlsym` specifically calls `calloc` internally during its initialization.

### `realloc`
```cpp
extern "C" void* realloc(void* ptr, size_t size) noexcept {
    if (!ptr) return malloc(size);    // Standard: realloc(null, n) == malloc(n)
    if (size == 0) { free(ptr); return nullptr; }  // Standard: realloc(p, 0) == free(p)
    ...
    void* new_ptr = real_realloc(ptr, size);
    if (new_ptr) {
        record_free(ptr);             // Old address is gone
        record_alloc(new_ptr, size, frames, depth);  // New address tracked
    }
    ...
}
```
- Handles the two edge cases that the C standard mandates before doing any real work.
- If `new_ptr != ptr` (common when size grows), the old slot is freed and a new slot is created.

### `free`
```cpp
extern "C" void free(void* ptr) noexcept {
    if (!ptr || is_bootstrap_ptr(ptr)) return;
    ...
    FreeStatus st = record_free(ptr);
    if (st == FREE_OK)     real_free(ptr);
    else if (st == FREE_DOUBLE) print_error("DOUBLE FREE", ptr, frames, depth);
    else                        print_error("INVALID FREE", ptr, frames, depth);
    ...
}
```
- `if (!ptr) return`: `free(null)` is a no-op by the C standard.
- `is_bootstrap_ptr(ptr)`: Bootstrap memory is not heap memory — `real_free` must not be called on it.
- On `FREE_DOUBLE` or `FREE_INVALID`: print error and **do not call `real_free`** — calling it would cause the exact crash we are trying to detect.

---

## Part 7: Exit Report (Lines 260–346)

```cpp
__attribute__((destructor))
static void leakscope_exit_report() {
    g_in_hook = true;  // Disable hooks: fprintf, dladdr internally use malloc
```
- `__attribute__((destructor))`: The C runtime executes this function automatically after `main()` returns or `exit()` is called, but before the process fully terminates.
- `g_in_hook = true` globally stops all tracking so the report's own `fprintf` and `dladdr` calls are not intercepted.

```cpp
    for (size_t i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].state == SLOT_ACTIVE && is_user_leak(g_table[i].stack, g_table[i].stack_depth)) {
```
- Only counts `SLOT_ACTIVE` entries (never-freed allocations).
- `is_user_leak()` filters out allocations whose entire call chain is inside `ld-linux.so` — those are internal dynamic linker bookkeeping, not user leaks.

```cpp
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
```
- Walks each stack frame using `dladdr()` to get the shared library name.
- If any frame points to a non-`.so` file (i.e., the user's executable), it is a user leak.
- If the chain is entirely in `ld-linux`, it is a linker-internal allocation — skip it.

```cpp
    sort_groups(groups, num_groups);       // Bubble sort descending by total_bytes
    print_summary_header(leaked_blocks, leaked_bytes);
    print_leak_groups(groups, num_groups); // Top 10 groups with their call stacks
```
- Groups are sorted so the most severe leak (most bytes wasted) is always `Leak #1`.
- Only the top 10 groups are printed to keep output readable.

---

## Part 8: C++ Language Features Used

| Feature | Where | Why |
|---|---|---|
| `extern "C"` | `malloc`, `calloc`, `realloc`, `free` | Disables C++ name mangling so the linker resolves standard C symbol names |
| `thread_local` | `g_in_hook` | Each thread gets its own boolean — no cross-thread interference |
| `__attribute__((destructor))` | `leakscope_exit_report` | Guaranteed automatic call at process exit without any code change |
| `static constexpr` | `MAX_STACK_FRAMES`, `TABLE_SIZE`, `BOOTSTRAP_BUF_SIZE` | Compile-time constants with type safety |
| `noexcept` | All 4 allocator overrides | C standard allocators must not throw — satisfies ABI contract |
| `dlsym(RTLD_NEXT)` | `init_real_functions` | Finds the glibc function pointer, skipping our own interception layer |
| `backtrace()` | `malloc`, `calloc`, `realloc`, `free` | Captures raw instruction pointer stack frames without any heap allocation |
| `dladdr()` | `print_frame_line`, `is_user_leak` | Resolves raw `void*` addresses to shared library file names and base offsets |
| `operator new/delete` (10 overloads) | Global scope | Intercepts all C++ `new`, `new[]`, `delete`, `delete[]` expressions |
| `uintptr_t` cast | `hash_ptr` | Safely converts a pointer to an integer for bitwise hash operations |
| `ssize_t` | `find_or_alloc_slot` return type | Allows `-1` sentinel return (no valid slot found) unlike `size_t` |
