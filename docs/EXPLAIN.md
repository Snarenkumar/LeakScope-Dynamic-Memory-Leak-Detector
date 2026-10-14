# LeakScope: Architecture & Deep-Dive Explanation

> 🐣 **New to systems programming?**  
> If this deep dive feels overwhelming, read **[simple_execution.md](../simple_execution.md)** first for a plain-English explanation!

This document breaks down **LeakScope** block-by-block in plain English. It covers why and how each component works, the classic traps encountered when building an `LD_PRELOAD` memory profiler, and the C/C++ language features used.

---

## 1. What is `LD_PRELOAD`?

When a standard Linux program executes, the dynamic linker (`ld-linux.so`) loads shared libraries (such as `libc.so.6`) to provide standard functions like `malloc` and `free`.

`LD_PRELOAD` is an environment variable telling the dynamic linker:
> *"Load this custom shared library BEFORE any other library."*

Because the dynamic linker resolves symbol names in load order, any function defined in `libleakscope.so` (such as `extern "C" void* malloc(size_t)`) preempts glibc's implementation. Every call to `malloc` across the application, libraries, and runtime flows through our wrapper. No source code changes or recompilation are required.

---

## 2. What is `dlsym(RTLD_NEXT)`?

When LeakScope intercepts a call to `malloc(size)`, it needs to actually allocate that memory in the operating system.

If we simply wrote:
```cpp
void* malloc(size_t size) {
    return malloc(size); // 💥 Infinite recursion! Calls our own malloc forever.
}
```
We need the pointer to glibc's *original* `malloc`. We obtain this using `dlsym`:
```cpp
real_malloc = (malloc_fn_t)dlsym(RTLD_NEXT, "malloc");
```
- `dlsym` queries the dynamic linker symbol table.
- `RTLD_NEXT` instructs the linker: *"Find the NEXT occurrence of this symbol name in the search order after the current shared object."*
- This gives us a direct function pointer to glibc's `malloc`.

---

## 3. The Two Classic Traps

Building an allocator interceptor exposes two tricky traps in Linux systems programming.

### Trap 1: The Infinite Recursion Hook Trap
When a program calls our intercepted `malloc`, LeakScope runs additional profiling logic:
- `backtrace()`: captures the call stack.
- `fprintf()`: outputs diagnostics.
- `pthread_mutex_lock()`: acquires internal locks.

Under the hood, glibc's `backtrace()` or `fprintf()` may call `malloc`! If this happens:
1. User calls `malloc()` $\to$ enters LeakScope `malloc()`
2. LeakScope calls `backtrace()` $\to$ `backtrace()` internally calls `malloc()`
3. This re-enters LeakScope `malloc()` $\to$ calls `backtrace()` again...
4. **Result:** Infinite recursion and stack overflow crash (`SIGSEGV`).

**The Solution:**
We use a `thread_local bool g_in_hook = false;`.
- When entering our hook, we check: `if (g_in_hook) return real_malloc(size);`
- Otherwise, set `g_in_hook = true`, perform tracking, and reset `g_in_hook = false` on exit.
- Any secondary allocation triggered while inside the hook directly calls the real allocator without recursion.

### Trap 2: The `dlsym` Bootstrap Trap
To find `real_malloc`, we must call `dlsym(RTLD_NEXT, "malloc")`.
However, inside glibc, `dlsym` internally calls `calloc` during its initialization to allocate internal lookup structures!

This creates a chicken-and-egg deadlock:
1. First allocation triggers `calloc()`
2. `calloc()` calls `init_real_functions()` to find `real_calloc` via `dlsym()`
3. `dlsym()` calls `calloc()` before `real_calloc` has been resolved!
4. `real_calloc` is still `nullptr` $\to$ crash with null pointer dereference.

**The Solution:**
We declare an isolated static bootstrap buffer:
```cpp
static char g_bootstrap_buf[8192];
static size_t g_bootstrap_used = 0;
```
If `calloc` is called while `real_calloc == nullptr` or during initialization (`g_initializing == true`), we carve out bytes directly from `g_bootstrap_buf` (a bump allocator). When `free` is called, we check `is_bootstrap_ptr()` and safely ignore it.

---

## 4. Why the Tracking Table is Static (Isolated Internal Allocator)

Standard containers like `std::vector` or `std::unordered_map` use the heap by calling `malloc` and `free`. If a leak detector used `std::unordered_map` to track allocations:
- Each insertion would trigger `malloc`.
- Our hook would intercept it, causing overhead, lock contention, or recursive tracking loops.

**LeakScope's Approach:**
We allocate a fixed-size array in static memory (`.bss` segment):
```cpp
static Record g_table[65536];
```
- Open addressing with linear probing uses the pointer address itself as the hash key.
- If an allocation occurs, it goes directly into an available slot in `g_table`.
- Zero dynamic allocations are performed by LeakScope itself.
- If the table capacity (65,536 entries) is ever reached, we increment `g_dropped_records` and report it cleanly, ensuring zero crashes.

---

## 5. Thread Safety with `pthread_mutex_t`

Multiple threads in a program can allocate and free memory simultaneously.
- A single global mutex `pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;` protects access to `g_table`, `g_total_allocs`, and watermark counters.
- We capture the backtrace into a thread-local array *before* acquiring the mutex, keeping critical sections short and contention minimal.
- Each thread has its own `thread_local bool g_in_hook`, ensuring one thread's re-entrancy status never blocks or alters another thread.

---

## 6. Error Detection: Double Free & Invalid Free

Whenever `free(ptr)` is intercepted:
1. If `ptr == nullptr` or points to the bootstrap buffer, it returns immediately (safe standard behavior).
2. We probe `g_table` for `ptr`:
   - **Matching active record found**: Marked as `SLOT_FREED`, totals updated, calls `real_free(ptr)`.
   - **Matching record found in `SLOT_FREED` state**: Indicates the pointer was already freed $\to$ outputs `[LeakScope] ERROR: DOUBLE FREE` with call stack. We do **not** call `real_free`, preventing a crash.
   - **Empty slot reached without finding pointer**: Indicates memory was never allocated by this process $\to$ outputs `[LeakScope] ERROR: INVALID FREE` with call stack. We do **not** call `real_free`.

---

## 7. Exit Report via Destructor

LeakScope installs an automatic destructor:
```cpp
__attribute__((destructor))
static void leakscope_exit_report()
```
When `main()` finishes or `exit()` is called, the C runtime executes functions marked with `__attribute__((destructor))`:
1. Sets `g_in_hook = true` so formatting and printing functions (`fprintf`, `dladdr`) are never intercepted.
2. Iterates over `g_table` to tally active allocations.
3. Groups leaks with matching stack traces into unique leak clusters.
4. Sorts leak groups descending by total volume (leaked bytes).
5. Prints a formatted summary table to `stderr` with peak watermark metrics and stack frame addresses.

---

## 8. Summary of C / C++ Features Used

| Feature | Where It Is Used | Why We Used It |
| :--- | :--- | :--- |
| `extern "C"` | `malloc`, `calloc`, `realloc`, `free` | Prevents C++ name mangling so the dynamic linker matches standard C symbol names. |
| `thread_local` | `g_in_hook` | Ensures each thread maintains independent re-entrancy state without cross-thread interference. |
| `__attribute__((destructor))` | `leakscope_exit_report` | Guarantees the leak report executes automatically right before process termination. |
| `dlsym(RTLD_NEXT)` | `init_real_functions` | Bypasses LeakScope to fetch pointers to the underlying glibc allocation implementations. |
| `backtrace()` | `record_alloc`, `print_error` | Glibc function that walks the stack frames to capture raw instruction pointer addresses. |
| `dladdr()` | `print_frame_line` | Resolves raw instruction pointers to shared library names and base offsets. |
| `operator new / delete` | Global overrides | Intercepts C++ `new`, `new[]`, `delete`, and `delete[]` expressions, routing them to tracked `malloc`/`free`. |
