# LeakScope: How to Run — Complete Step-by-Step Guide

This guide covers every way to build, run, and use LeakScope — from first-time setup to checking your own programs and reading symbolized output.

---

## 1. Prerequisites

You need **only one thing** installed on your machine:

| Tool | Why You Need It | Where to Get It |
|---|---|---|
| Docker Desktop | Runs the Ubuntu build environment | https://www.docker.com/products/docker-desktop/ |

You do **NOT** need: Linux, g++, make, addr2line, or any C++ toolchain locally. Everything is inside the Docker image.

Verify Docker is working:
```powershell
docker --version
```
Expected: `Docker version 24.x.x, build ...`

---

## 2. Build (One Command)

Navigate to the project folder (where `Dockerfile` lives) and run:
```powershell
docker build -t leakscope .
```

**What happens inside:**
1. Ubuntu 22.04 base image is pulled.
2. `g++`, `make`, `binutils` are installed.
3. All project files are copied to `/app`.
4. `make` is run, which compiles:
   - `libleakscope.so` — the interceptor shared library
   - `samples/leaky` — intentionally leaky program
   - `samples/double_free` — double-free and invalid-free demo
   - `samples/threads` — multi-threaded leak demo
   - `samples/clean` — clean program (no leaks)

Build success looks like:
```
=> exporting to image
=> => naming to docker.io/library/leakscope
```

---

## 3. Run the Full Test Suite (Default)

```powershell
docker run --rm leakscope
```

This runs two things inside the container:
1. `make test` — runs `scripts/test.sh` over all samples
2. `make demo` — runs `samples/leaky` through `scripts/symbolize.sh`

---

## 4. Open an Interactive Shell

For all advanced usage, open a shell inside the container with your project folder mounted:

```powershell
docker run --rm -it -v ${PWD}:/app leakscope bash
```

You are now inside a Linux shell at `/app`. All commands below run from here.

---

## 5. Run Each Sample Program

### 5-A: The Leaky Program
```bash
LD_PRELOAD=/app/libleakscope.so ./samples/leaky
```
This allocates memory and intentionally never frees it. LeakScope prints a leak report on exit.

**Real output:**
```
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
  Leak #1: 200 bytes leaked across 1 block(s)
    frame [0]: 0x7f48... (libleakscope.so+0x...)
    frame [1]: 0x401210 (/app/samples/leaky+0x1210)
       -> leak_from_main_task() (/app/samples/leaky.cpp:16)
    frame [2]: 0x401265 (/app/samples/leaky+0x1265)
       -> main (/app/samples/leaky.cpp:27)
  Leak #2: 150 bytes leaked across 2 block(s)
    ...
=================================================================
```

---

### 5-B: Double-Free & Invalid-Free Detection
```bash
LD_PRELOAD=/app/libleakscope.so ./samples/double_free
```

**Real output:**
```
[double_free] Allocating memory...
[double_free] Freeing pointer for the first time (valid)...
[double_free] Freeing pointer a second time (should detect DOUBLE FREE)...
[LeakScope] ERROR: DOUBLE FREE detected for pointer 0x55d9a... | stack: 0x... 0x...
[double_free] Freeing wild pointer (should detect INVALID FREE)...
[LeakScope] ERROR: INVALID FREE detected for pointer 0xdeadbeef | stack: 0x... 0x...
[double_free] Completed safely without crashing.
```

> **Key point:** LeakScope does NOT crash. It catches both errors and lets the program finish normally.

---

### 5-C: Multi-Threaded Leak Detection
```bash
LD_PRELOAD=/app/libleakscope.so ./samples/threads
```

4 threads run concurrently. Thread #2 intentionally leaks one `new int[10]` block. LeakScope safely tracks allocations from all threads simultaneously and reports only the leaked block.

**Real output snippet:**
```
[threads] Launching 4 worker threads...
[threads] All threads joined. Process exiting.

=================================================================
                    LeakScope Memory Report
=================================================================
  Total Allocations:   82
  Total Frees:         81
  Peak Memory Usage:   ...
  Leaked Blocks:       1
  Leaked Bytes:        40 bytes
-----------------------------------------------------------------
  Leak #1: 40 bytes leaked across 1 block(s)
    frame [1]: ... -> worker (/app/samples/threads.cpp:13)
    frame [2]: ... -> main (/app/samples/threads.cpp:22)
=================================================================
```

---

### 5-D: Clean Program — No Leaks
```bash
LD_PRELOAD=/app/libleakscope.so ./samples/clean
```

**Real output:**
```
[clean] Performing allocations...
[clean] Freeing all allocated memory...
[clean] All memory cleaned up.

=================================================================
                    LeakScope Memory Report
=================================================================
  Total Allocations:   3
  Total Frees:         3
  Peak Memory Usage:   ...
  Dropped Records:     0
-----------------------------------------------------------------
  LEAK SUMMARY:
  Leaked Blocks:       0 (0 leaked blocks, 0 leaks)
  Leaked Bytes:        0 bytes
-----------------------------------------------------------------
=================================================================
```
No leaks reported. Exit code: `0`.

---

## 6. Run Your Own Program

### Step 1: Compile with debug symbols
```bash
g++ -g -O0 -no-pie -o my_program my_program.cpp
```
- `-g`: Embeds debug symbols so addresses map to function names and line numbers.
- `-O0`: Disables optimization to preserve accurate call stacks.
- `-no-pie`: Required for `addr2line` to resolve addresses in the executable.

### Step 2: Run under LeakScope
```bash
LD_PRELOAD=/app/libleakscope.so ./my_program
```

### Step 3: Get symbolized output
```bash
LD_PRELOAD=/app/libleakscope.so ./my_program 2>&1 | ./scripts/symbolize.sh ./my_program
```

`symbolize.sh` calls `addr2line` on each frame address to translate:
```
Before:  frame [1]: 0x4011bd (/app/samples/leaky+0x11bd)
After:   -> leak_from_helper() (/app/samples/leaky.cpp:6)
```

---

## 7. How to Read the Output

### Reading Each Leak Group

```
  Leak #1: 200 bytes leaked across 1 block(s)
```
- `Leak #1`: This is the largest leak (sorted descending by bytes).
- `200 bytes`: Total bytes that were allocated but never freed with this call stack.
- `1 block(s)`: Number of separate allocation calls with this exact same stack trace.

```
    frame [0]: 0x7f48a1 (libleakscope.so+0x...)
```
- Frame 0 is always LeakScope's own `malloc` wrapper. **You can ignore frame 0.**

```
    frame [1]: 0x401210 (/app/samples/leaky+0x1210)
       -> leak_from_main_task() (/app/samples/leaky.cpp:16)
```
- Frame 1 is the **first user function** that allocated the memory.
- The `->` line (from `symbolize.sh`) tells you: function name, source file, and line number.

```
    frame [2]: 0x401265 (/app/samples/leaky+0x1265)
       -> main (/app/samples/leaky.cpp:27)
```
- Frame 2 is the caller of frame 1. This is the call chain up to `main()`.

---

## 8. Suppressing Leak Reports (NOLEAK Pattern)

If you allocate memory intentionally at startup and keep it for the process lifetime (e.g., a global config object), it will always appear as a "leak." The cleanest workaround is to wrap it in an explicit `free()` at program end, or to structure it as a static local:

```cpp
// Before: shows as a leak
Config* cfg = new Config();

// After: explicitly cleaned
Config* cfg = new Config();
// ... use cfg ...
delete cfg;   // Now free'd before main() returns
```

Alternatively, use RAII (static local with destructor):
```cpp
static Config cfg;  // Allocated in .bss, never shown as a heap leak
```

---

## 9. Quick Reference — All Commands

| What you want to do | Command |
|---|---|
| Build the image | `docker build -t leakscope .` |
| Run test suite + demo | `docker run --rm leakscope` |
| Open interactive shell | `docker run --rm -it -v ${PWD}:/app leakscope bash` |
| Run leaky sample | `LD_PRELOAD=/app/libleakscope.so ./samples/leaky` |
| Run double_free sample | `LD_PRELOAD=/app/libleakscope.so ./samples/double_free` |
| Run threads sample | `LD_PRELOAD=/app/libleakscope.so ./samples/threads` |
| Run clean sample | `LD_PRELOAD=/app/libleakscope.so ./samples/clean` |
| Run your own program | `LD_PRELOAD=/app/libleakscope.so ./my_program` |
| Symbolize output | `LD_PRELOAD=./libleakscope.so ./my_program 2>&1 \| ./scripts/symbolize.sh ./my_program` |
| Rebuild | `make clean && make` |

---

## 10. Troubleshooting

| Problem | Cause | Fix |
|---|---|---|
| `docker: command not found` | Docker Desktop not installed | Install from docker.com |
| `Cannot connect to the Docker daemon` | Docker Desktop not running | Open Docker Desktop app |
| Build fails | Wrong directory | Run `docker build` where `Dockerfile` is |
| No leak report printed | Normal exit not reached | Ensure `main()` returns or `exit()` is called |
| `Dropped Records: N` (N > 0) | Table overflow (>65536 live allocations) | Increase `TABLE_SIZE` in `src/leakscope.cpp` |
| Addresses not symbolized | Missing `-g` or `-no-pie` flag | Compile with `g++ -g -O0 -no-pie` |
| DOUBLE FREE error on your code | Real double-free bug | Fix `free()`/`delete` at the reported stack line |
| INVALID FREE error | Freeing a pointer not from the heap | Check the pointer source at reported line |
