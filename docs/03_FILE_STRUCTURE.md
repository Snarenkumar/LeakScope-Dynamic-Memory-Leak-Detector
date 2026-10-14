# LeakScope: File Structure — Every File Explained

This document describes every file in the project, what it does, what it contains, and which other files it depends on. Read this to understand how the project is organized before diving into the source code.

---

## 1. Top-Level Overview

```
LeakScope/
+-- src/
|   +-- leakscope.cpp          <- The entire detector (single source file)
+-- samples/
|   +-- leaky.cpp              <- Program that leaks memory intentionally
|   +-- double_free.cpp        <- Demonstrates double-free & invalid-free
|   +-- threads.cpp            <- Multi-threaded leak from thread #2
|   +-- clean.cpp              <- Fully correct program, 0 leaks
+-- scripts/
|   +-- symbolize.sh           <- Converts hex addresses to function names
|   +-- test.sh                <- Automated test runner
+-- docs/
|   +-- 01_PROJECT_FLOW.md     <- Pipeline overview and design rationale
|   +-- 02_HOW_TO_RUN.md       <- Step-by-step build and run guide
|   +-- 03_FILE_STRUCTURE.md   <- This file
|   +-- 04_LINE_BY_LINE.md     <- Deep source code walkthrough
|   +-- 05_REAL_WORLD_AND_WHY.md <- Industry context and why this matters
|   +-- 06_FACTS_AND_UNIQUE.md <- Unique technical facts and trivia
|   +-- 07_MAKEFILE_GUIDE.md   <- Makefile explained line-by-line
|   +-- EXPLAIN.md             <- Architecture deep-dive (plain English)
|   +-- PREPARATION_GUIDE.md   <- Ordered preparation checklist
+-- Dockerfile                 <- Ubuntu 22.04 container build recipe
+-- Makefile                   <- Build rules for .so and sample binaries
+-- README.md                  <- Quick-start reference
+-- .gitignore                 <- Excludes compiled binaries from git
```

---

## 2. Every File in Detail

### Core Source

#### `src/leakscope.cpp` (350 lines)
- **What it does:** The entire memory leak detector in one file. Compiled to `libleakscope.so`.
- **What it contains:**
  - `struct Record`: Holds one allocation: pointer, size, stack frames, stack depth, and slot state.
  - `struct LeakGroup`: Groups leaks that share the same call stack for summary reporting.
  - `g_table[65536]`: Static hash table in `.bss` — LeakScope's isolated internal storage.
  - `g_bootstrap_buf[8192]`: Static bump allocator to serve `dlsym`'s internal `calloc` calls.
  - `init_real_functions()`: Uses `dlsym(RTLD_NEXT)` to find the real `malloc`, `calloc`, `realloc`, `free`.
  - `hash_ptr()` + `find_or_alloc_slot()`: Open-addressing hash table operations.
  - `record_alloc()`: Stores a new allocation in `g_table` with its stack trace.
  - `record_free()`: Marks a slot `SLOT_FREED` or returns `FREE_DOUBLE`/`FREE_INVALID`.
  - `extern "C" malloc/calloc/realloc/free`: The four intercepted C allocators.
  - `operator new/delete` (10 overloads): Routes all C++ allocation through intercepted `malloc`/`free`.
  - `leakscope_exit_report()`: `__attribute__((destructor))` function that groups, sorts, and prints the report.
- **Dependencies:** `<dlfcn.h>` (dlsym, dladdr), `<execinfo.h>` (backtrace), `<pthread.h>`, `<cstdio>`, `<cstring>`, `<new>`.

---

### Sample Programs

#### `samples/leaky.cpp`
- **What it does:** Intentionally leaks memory to demonstrate LeakScope's detection.
- **What it contains:**
  - `leak_from_helper()`: Calls `malloc(50)` and `malloc(100)` and never frees them (2 leaked blocks).
  - `leak_from_main_task()`: Calls `malloc(200)` and never frees it (1 leaked block).
  - `main()`: Calls both leak functions, then calls `free()` on one intermediate allocation.
- **Expected report:** 3 leaked blocks, 350 bytes, 2 unique leak groups.

#### `samples/double_free.cpp` (24 lines)
- **What it does:** Demonstrates LeakScope's double-free and invalid-free error detection.
- **What it contains:**
  - `malloc(128)` then `free(ptr)` (valid — clears the slot).
  - `free(ptr)` again (DOUBLE FREE — slot is already `SLOT_FREED`).
  - `free((void*)0xDEADBEEF)` (INVALID FREE — address never in table).
- **Expected output:** 2 error lines from LeakScope. Program completes without crashing.

#### `samples/threads.cpp` (30 lines)
- **What it does:** Demonstrates thread-safe leak detection across 4 concurrent threads.
- **What it contains:**
  - `worker(int id)`: Each of 4 threads does 20 `malloc`+`free` pairs. Thread #2 additionally does `new int[10]` without a matching `delete[]`.
  - `main()`: Launches threads, joins them, exits.
- **Expected report:** 1 leaked block, 40 bytes (the `int[10]` from thread 2).

#### `samples/clean.cpp` (18 lines)
- **What it does:** A fully correct reference program — every allocation is freed.
- **What it contains:**
  - `malloc(64)` -> `free(p1)`.
  - `new int[10]` -> `delete[] p2`.
  - `calloc(4, 16)` -> `free(p3)`.
- **Expected report:** 0 leaked blocks. Leaked Bytes: 0.

---

### Scripts

#### `scripts/symbolize.sh`
- **What it does:** Post-processes LeakScope's raw stderr output, converting hex addresses into function names and source line numbers.
- **How it works:**
  - Reads stdin line by line.
  - Detects lines matching `frame [N]: 0x... (binary+0xOFFSET)` using `grep`/`sed`.
  - Calls `addr2line -e <binary> -f -p 0xOFFSET` to resolve each address.
  - Prints the symbolized result indented with `->`.
- **Tool it uses:** `addr2line` from GNU binutils (installed in Docker via `apt-get install binutils`).
- **Usage:** `command 2>&1 | ./scripts/symbolize.sh ./binary_name`

#### `scripts/test.sh`
- **What it does:** Runs each sample binary through LeakScope and checks that the detector behaves correctly.
- **What it checks:**
  - `leaky`: Expects a non-zero leaked bytes count in the report.
  - `double_free`: Expects DOUBLE FREE and INVALID FREE error lines.
  - `threads`: Expects at least 1 leaked block.
  - `clean`: Expects 0 leaked blocks.
- **Called by:** `make test`

---

### Build & Container

#### `Dockerfile` (22 lines)
- **What it does:** Defines a reproducible Ubuntu 22.04 Linux build environment.
- **Line by line:**
  - `FROM ubuntu:22.04`: Base image.
  - `ENV DEBIAN_FRONTEND=noninteractive`: Prevents interactive package prompts.
  - `RUN apt-get install -y g++ make binutils`: Installs the C++ compiler, build system, and addr2line.
  - `WORKDIR /app`: All subsequent paths are relative to `/app`.
  - `COPY . /app`: Copies all project files into the container.
  - `RUN make`: Compiles everything.
  - `CMD ["sh", "-c", "make test && echo '' && make demo"]`: Default command when `docker run` is called.

#### `Makefile` (30 lines)
- **What it does:** Defines build targets for the shared library and all sample binaries.
- **Key targets:**

| Target | What It Builds |
|---|---|
| `all` | `libleakscope.so` + all samples |
| `libleakscope.so` | Compiles `src/leakscope.cpp` with `-shared -fPIC -ldl -pthread` |
| `samples/%` | Compiles any sample from `samples/%.cpp` |
| `test` | Runs `scripts/test.sh` |
| `demo` | Runs `leaky` piped through `symbolize.sh` |
| `clean` | Removes `.so` and all sample binaries |

#### `.gitignore`
- Excludes: `libleakscope.so`, `samples/leaky`, `samples/double_free`, `samples/threads`, `samples/clean` — all compiled binaries.

---

## 3. "Read the Files in This Order" Learning Plan

| Step | File | What You Learn | Suggested Time |
|---|---|---|---|
| **1** | `docs/01_PROJECT_FLOW.md` | The full 5-stage pipeline and 2 bootstrap traps | 15 mins |
| **2** | `src/leakscope.cpp` lines 1–100 | Data structures, bootstrap buffer, hash table, dlsym | 30 mins |
| **3** | `src/leakscope.cpp` lines 100–250 | record_alloc, record_free, the 4 intercepted functions | 30 mins |
| **4** | `src/leakscope.cpp` lines 250–350 | Exit report: grouping, sorting, symbolizing frames | 20 mins |
| **5** | `samples/leaky.cpp` | How a real leak looks from the caller's perspective | 10 mins |
| **6** | `samples/double_free.cpp` | How double-free and invalid-free are triggered | 10 mins |
| **7** | `samples/threads.cpp` | How concurrent allocation is safely tracked | 10 mins |
| **8** | `scripts/symbolize.sh` | How hex addresses become readable function names | 10 mins |
| **9** | `Makefile` + `Dockerfile` | How everything is compiled and containerized | 15 mins |
| **Total** | **Complete Mastery** | From zero to explaining every component in an interview | ~2.5 hours |
