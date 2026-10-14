# LeakScope: Project Flow & How It All Works

This document explains the complete pipeline of LeakScope — from how a program's allocations are intercepted, tracked, checked for errors, and reported at exit. Read this first before looking at any source code.

---

## 1. The Big Picture: 5-Stage Pipeline

Every time a target program allocates or frees memory, LeakScope executes this pipeline:

```
+-------------------------------------------------------------+
|  STAGE 1: INTERCEPTION                                      |
|  LD_PRELOAD loads libleakscope.so before libc              |
|  Our malloc/free/calloc/realloc are called instead         |
+-----------------------------+-------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|  STAGE 2: BOOTSTRAP & INITIALIZATION                        |
|  dlsym(RTLD_NEXT) resolves the real glibc functions        |
|  Bootstrap buffer handles early calloc from dlsym itself   |
+-----------------------------+-------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|  STAGE 3: TRACKING                                          |
|  backtrace() captures call stack frames                    |
|  g_table[65536] stores ptr, size, stack — no heap used     |
|  pthread_mutex_t protects table from concurrent threads    |
+-----------------------------+-------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|  STAGE 4: ERROR DETECTION (on every free)                   |
|  FREE_OK     -> pointer matched, freed cleanly             |
|  FREE_DOUBLE -> pointer already freed -> ERROR printed     |
|  FREE_INVALID -> pointer never seen  -> ERROR printed      |
+-----------------------------+-------------------------------+
                              |
                              v
+-------------------------------------------------------------+
|  STAGE 5: EXIT REPORT                                       |
|  __attribute__((destructor)) fires at process exit         |
|  Active slots = leaks. Grouped by stack trace.             |
|  Sorted by leaked bytes. Printed to stderr.                |
+-------------------------------------------------------------+
```

---

## 2. How LD_PRELOAD Interception Works

When Linux launches any dynamically linked program, the dynamic linker (`ld-linux.so`) searches shared libraries **in load order** to resolve symbol names like `malloc`.

```
Without LeakScope:
  Program calls malloc() -> linker resolves -> libc.so malloc()

With LeakScope (LD_PRELOAD=./libleakscope.so):
  Program calls malloc() -> linker resolves -> libleakscope.so malloc()
                                                (our wrapper runs first)
                                                -> then calls real_malloc
```

No recompilation. No source changes. No OS patches. Just one environment variable.

---

## 3. The 2 Classic Traps and How LeakScope Solves Them

### Trap 1: Infinite Recursion Hook Trap

Our `malloc()` calls `backtrace()` to capture the call stack. But `backtrace()` internally calls `malloc()` too. Without a guard, this loops forever:

```
User -> malloc() -> backtrace() -> malloc() -> backtrace() -> CRASH
```

**Solution:** `thread_local bool g_in_hook`

```
User -> malloc() [g_in_hook=false, set to true]
     -> backtrace() -> malloc() [g_in_hook=true -> bypass to real_malloc]
     -> record, reset g_in_hook=false
```

Each thread has its own `g_in_hook`. One thread's re-entrancy never blocks another.

---

### Trap 2: The dlsym Bootstrap Trap

To find `real_malloc`, we call `dlsym(RTLD_NEXT, "malloc")`. But `dlsym` internally calls `calloc` to allocate its own lookup structures!

**This creates a chicken-and-egg deadlock:**
```
First calloc() call
  -> init_real_functions() -> dlsym() -> calloc()
  -> real_calloc is still nullptr -> CRASH
```

**Solution:** Static bootstrap buffer

```cpp
static char g_bootstrap_buf[8192];  // Lives in .bss, no malloc needed
static size_t g_bootstrap_used = 0;
```

If `calloc` is called while `real_calloc == nullptr` or `g_initializing == true`, we carve bytes directly from `g_bootstrap_buf`. When `free` is called on a bootstrap pointer, `is_bootstrap_ptr()` detects it and skips it safely.

---

## 4. The Static Tracking Table: Why No std::map

Standard containers like `std::unordered_map` use `malloc` internally. If LeakScope used one:
- Each insertion would call `malloc` -> re-enter our hook -> lock contention -> recursive tracking

**Solution:** A fixed-size open-addressing hash table in static memory:

```cpp
static Record g_table[65536];  // Sits in .bss, allocated at process startup
```

| Property | Value |
|---|---|
| Capacity | 65,536 entries |
| Hash key | The allocation pointer address itself |
| Collision resolution | Linear probing |
| Heap allocations by LeakScope itself | 0 (zero) |
| Overflow behavior | Increment `g_dropped_records`, never crash |

---

## 5. The 15 Tracked Events

LeakScope intercepts these 15 function calls:

| Intercepted Function | What LeakScope Does |
|---|---|
| `malloc(size)` | Calls real_malloc, records ptr+size+stack |
| `calloc(n, size)` | Calls real_calloc, records ptr+(n*size)+stack |
| `realloc(ptr, size)` | Records old ptr as freed, new ptr as allocated |
| `free(ptr)` | Marks slot as FREED or reports DOUBLE/INVALID FREE |
| `operator new(size)` | Routes through our `malloc` |
| `operator new[](size)` | Routes through our `malloc` |
| `operator delete(ptr)` | Routes through our `free` |
| `operator delete[](ptr)` | Routes through our `free` |
| `operator new(size, nothrow)` | Routes through our `malloc` |
| `operator new[](size, nothrow)` | Routes through our `malloc` |
| `operator delete(ptr, nothrow)` | Routes through our `free` |
| `operator delete[](ptr, nothrow)` | Routes through our `free` |
| `operator delete(ptr, size)` | Routes through our `free` |
| `operator delete[](ptr, size)` | Routes through our `free` |
| Exit destructor | Fires automatically, prints leak report |

---

## 6. Thread Safety Model

```
Thread A                     Thread B
   |                            |
malloc(64)                   malloc(128)
   |                            |
g_in_hook[A] = true          g_in_hook[B] = true     <- per-thread, independent
   |                            |
backtrace()                  backtrace()              <- captured before mutex
   |                            |
pthread_mutex_lock(g_mutex)  pthread_mutex_lock(g_mutex)  <- one at a time
   |                            |
record_alloc(...)            [waits...]
   |                            |
pthread_mutex_unlock()       record_alloc(...)
   |                            |
g_in_hook[A] = false         pthread_mutex_unlock()
```

Backtrace is captured **before** locking to minimize time spent inside the critical section.

---

## 7. The Exit Report Pipeline

When the process exits, `leakscope_exit_report()` runs automatically:

```
1. g_in_hook = true        <- Stop intercepting (prevents fprintf from re-entering)
2. Scan g_table[65536]     <- Collect all SLOT_ACTIVE entries
3. is_user_leak()          <- Filter out ld-linux.so internal runtime allocations
4. add_to_group()          <- Group leaks with identical stack traces together
5. sort_groups()           <- Sort groups descending by total_bytes
6. print_summary_header()  <- Print totals: allocs, frees, peak, dropped
7. print_leak_groups()     <- Print top 10 groups with stack frames
8. dladdr() per frame      <- Resolve addresses to library names + offsets
```

---

## 8. Summary of Design Decisions

| Design Decision | Why |
|---|---|
| `LD_PRELOAD` | Zero-intrusion: no source changes needed |
| `dlsym(RTLD_NEXT)` | Gets the real glibc function pointer, avoids infinite recursion |
| `thread_local g_in_hook` | Stops re-entrancy per-thread without cross-thread interference |
| Bootstrap buffer | Breaks the dlsym/calloc circular dependency at startup |
| Static `g_table[65536]` | Zero heap usage by the detector itself |
| `__attribute__((destructor))` | Automatic exit reporting without modifying program code |
| `backtrace()` before mutex | Keeps critical section time minimal |
| `dladdr()` per frame | Resolves raw addresses to human-readable library names |
