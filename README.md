# LeakScope 🔍
> **A lightweight, dynamic memory leak detector for Linux built using `LD_PRELOAD` and `dlsym(RTLD_NEXT)`.**

LeakScope intercepts dynamic memory allocations (`malloc`, `calloc`, `realloc`, `free`, and C++ `new`/`delete`) at runtime without requiring any source code modifications or recompilation. Everything is containerized in Docker, so you can build and run it without needing a native Linux environment installed.

---

## How It Works (in 5 Lines)

1. **Interception**: Linux's dynamic linker preloads `libleakscope.so` before standard C runtime (`libc.so`) using `LD_PRELOAD`.
2. **Resolution**: `dlsym(RTLD_NEXT, ...)` dynamically resolves pointers to original glibc allocators so memory operations still succeed.
3. **Tracking**: Each active allocation is stored in an isolated, pre-allocated static hash table alongside raw call-stack frames captured via `backtrace()`.
4. **Safety**: A `thread_local` re-entrancy flag and bootstrap memory buffer prevent recursive hook traps during internal glibc calls.
5. **Exit Reporting**: An automatic destructor (`__attribute__((destructor))`) prints an exit-time leak summary grouped by call stack, sorted by leaked volume, with peak watermarks and error diagnostics.

---

## Quick Start with Docker

You do not need Linux or g++ installed locally. Run these commands directly from **Windows PowerShell**:

### 1. Build the Docker Image
```powershell
docker build -t leakscope .
```

### 2. Run Test Suite & Demo
```powershell
docker run --rm leakscope
```

### 3. Open an Interactive Shell Inside the Container
```powershell
docker run --rm -it -v ${PWD}:/app leakscope bash
```

### 4. Run Any Program with LeakScope
```bash
LD_PRELOAD=/app/libleakscope.so ./samples/leaky
```

---

## Sample Output

When running `./samples/leaky` through LeakScope:

```text
[leaky] Starting execution...
[leaky] Finished. Exiting with 3 leaked blocks.

=================================================================
                    LeakScope Memory Report                      
=================================================================
  Total Allocations:   4
  Total Frees:         1
  Peak Memory Usage:   350 bytes
  Dropped Records:     0
-----------------------------------------------------------------
  LEAK SUMMARY:
  Leaked Blocks:       3 (3 leaked blocks, 3 leaks)
  Leaked Bytes:        350 bytes
-----------------------------------------------------------------
  Leak #1: 200 bytes across 1 block(s)
    frame [0]: 0x7f48a1 (libleakscope.so+0x...)
    frame [1]: 0x401210 (/app/samples/leaky+0x1210)
       ↳ leak_from_main_task() (/app/samples/leaky.cpp:16)
    frame [2]: 0x401265 (/app/samples/leaky+0x1265)
       ↳ main (/app/samples/leaky.cpp:27)
  Leak #2: 150 bytes across 2 block(s)
    frame [0]: 0x7f48a1 (libleakscope.so+0x...)
    frame [1]: 0x4011bd (/app/samples/leaky+0x11bd)
       ↳ leak_from_helper() (/app/samples/leaky.cpp:6)
    frame [2]: 0x40125c (/app/samples/leaky+0x125c)
       ↳ main (/app/samples/leaky.cpp:26)
=================================================================
```

When bad memory frees occur (e.g. in `samples/double_free`):
```text
[LeakScope] ERROR: DOUBLE FREE detected for pointer 0x55d9a... | stack: 0x...
[LeakScope] ERROR: INVALID FREE detected for pointer 0xdeadbeef | stack: 0x...
```

---

## Honest Limitations

- **Linux & glibc only**: Relies on Linux ELF dynamic loader semantics (`LD_PRELOAD`), `dlsym(RTLD_NEXT)`, and glibc `<execinfo.h>`.
- **Fixed-size tracking table**: Uses a static table of 65,536 entries. If exhausted, allocations are counted as dropped records rather than growing dynamically.
- **Fixed stack depth**: Captures up to 8 stack frames per allocation to maintain low overhead and static bounds.
- **Educational / Profiling purpose**: Intended for learning systems programming, dynamic instrumentation, and debugging leaks; not a drop-in replacement for production tools like Valgrind or AddressSanitizer (ASan).

---

## Documentation

For a comprehensive line-by-line guide explaining the recursion trap, bootstrap allocator, multithreading locks, and C++ design decisions, see [docs/EXPLAIN.md](file:///f:/main%20servers%20under%20productions/LeakScope/docs/EXPLAIN.md).
