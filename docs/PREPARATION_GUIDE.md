# LeakScope: How to Prepare, Build & Run — Step-by-Step

This guide tells you **exactly what to do and in what order** to get LeakScope working from a fresh clone. It covers environment setup, building, testing, using the detector on your own code, and interpreting results.

---

## Overview: The 3 Phases

```
+------------------------------------------------------------------+
|  PHASE 1: ENVIRONMENT SETUP                                      |
|  Install Docker (Windows) -> No Linux or g++ needed locally     |
|  [X] Skip -> nothing will build or run                          |
+-------------------------+----------------------------------------+
                          | (Only if Phase 1 is done)
                          v
+------------------------------------------------------------------+
|  PHASE 2: BUILD                                                  |
|  docker build -> compiles libleakscope.so + all sample binaries |
|  [X] Skip -> you have nothing to run                            |
+-------------------------+----------------------------------------+
                          | (Only if Phase 2 is done)
                          v
+------------------------------------------------------------------+
|  PHASE 3: RUN & DETECT                                           |
|  docker run -> runs test suite, demo, and your own programs      |
|  [OK] Shows leak report, double-free/invalid-free errors        |
+------------------------------------------------------------------+
```

---

## Phase 1 — Environment Setup

You do **not** need Linux, g++, or any compiler installed on your machine.
Everything runs inside Docker.

### Step 1-A: Install Docker Desktop (Windows)

Download and install Docker Desktop from:
```
https://www.docker.com/products/docker-desktop/
```

After installing, open **PowerShell** and verify it works:
```powershell
docker --version
```

Expected output (version numbers will vary):
```
Docker version 24.x.x, build ...
```

> **If this fails:** Docker Desktop is not running. Open it from the Start Menu and wait for the whale icon to appear in the system tray before continuing.

### Step 1-B: Clone the Repository

```powershell
git clone https://github.com/Snarenkumar/LeakScope-Dynamic-Memory-Leak-Detector.git
cd "LeakScope-Dynamic-Memory-Leak-Detector"
```

---

## Phase 2 — Build

All build steps happen **inside Docker**. One command compiles the shared library and all sample programs.

### Step 2-A: Build the Docker Image

Run this from the project root (the folder containing `Dockerfile`):

```powershell
docker build -t leakscope .
```

**What this does internally:**
1. Pulls Ubuntu 22.04 as the base image.
2. Installs `g++`, `make`, and `binutils` (provides `addr2line` for symbolization).
3. Copies all project files into `/app` inside the container.
4. Runs `make` which compiles:
   - `libleakscope.so` — the interceptor shared library (from `src/leakscope.cpp`)
   - `samples/leaky` — a program that leaks memory intentionally
   - `samples/threads` — a multi-threaded leak test
   - `samples/double_free` — demonstrates double-free and invalid-free detection
   - `samples/clean` — a clean program with no leaks

Expected final output line:
```
 => exporting to image
 => => naming to docker.io/library/leakscope
```

> **If the build fails** with a compiler error, check that you are in the correct directory that contains the `Dockerfile`. Run `dir` (Windows) to confirm.

---

## Phase 3 — Run & Detect

### Step 3-A: Run the Automated Test Suite

```powershell
docker run --rm leakscope
```

**What this does:**
- Runs `make test` — executes `scripts/test.sh` which runs each sample program through LeakScope.
- Then runs `make demo` — pipes `samples/leaky` output through `scripts/symbolize.sh` to convert raw stack addresses to human-readable function names and line numbers.

Expected output (abbreviated):
```
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
    frame [0]: 0x7f... (libleakscope.so+0x...)
    frame [1]: 0x4012... (/app/samples/leaky+0x...)
       -> leak_from_main_task() (/app/samples/leaky.cpp:16)
    frame [2]: 0x4012... (/app/samples/leaky+0x...)
       -> main (/app/samples/leaky.cpp:27)
=================================================================
```

---

### Step 3-B: Test Double-Free & Invalid-Free Detection

Open an interactive shell inside the container:

```powershell
docker run --rm -it -v ${PWD}:/app leakscope bash
```

Now inside the container shell, run the `double_free` sample:

```bash
LD_PRELOAD=/app/libleakscope.so ./samples/double_free
```

LeakScope will detect and print errors before the program crashes:
```
[LeakScope] ERROR: DOUBLE FREE detected for pointer 0x55d9a... | stack: 0x... 0x...
[LeakScope] ERROR: INVALID FREE detected for pointer 0xdeadbeef | stack: 0x... 0x...
```

> **Key point:** LeakScope prevents the actual crash by **not** calling the real `free()` on a detected bad pointer. This lets the rest of the program continue and still print the full leak report.

---

### Step 3-C: Run Your Own Program Through LeakScope

This is how you use LeakScope on any arbitrary C/C++ program.

**Inside the interactive container shell** (`docker run --rm -it -v ${PWD}:/app leakscope bash`):

#### 1. Compile your program with debug symbols (`-g -no-pie`)
```bash
g++ -g -O0 -no-pie -o my_program my_program.cpp
```

> **Why `-g`?** Enables debug symbols so `symbolize.sh` can map addresses back to source file names and line numbers.
> **Why `-no-pie`?** Required for `addr2line` to resolve addresses correctly in non-position-independent executables.

#### 2. Run it under LeakScope
```bash
LD_PRELOAD=/app/libleakscope.so ./my_program
```

#### 3. See symbolized output (readable function names)
Pipe `stderr` through `symbolize.sh`:
```bash
LD_PRELOAD=/app/libleakscope.so ./my_program 2>&1 | ./scripts/symbolize.sh ./my_program
```

---

### Step 3-D: Run the Multi-Threaded Sample

```bash
LD_PRELOAD=/app/libleakscope.so ./samples/threads
```

LeakScope uses a `pthread_mutex_t` to protect the tracking table and a `thread_local` re-entrancy flag per thread, so concurrent leaks from multiple threads are all captured safely without crashes or missed records.

---

## Quick Reference — All Commands

| What you want to do | Command (PowerShell unless noted) |
| :--- | :--- |
| Build Docker image | `docker build -t leakscope .` |
| Run full test suite + demo | `docker run --rm leakscope` |
| Open interactive shell | `docker run --rm -it -v ${PWD}:/app leakscope bash` |
| Run leaky sample (inside shell) | `LD_PRELOAD=/app/libleakscope.so ./samples/leaky` |
| Run double-free sample (inside shell) | `LD_PRELOAD=/app/libleakscope.so ./samples/double_free` |
| Run threaded sample (inside shell) | `LD_PRELOAD=/app/libleakscope.so ./samples/threads` |
| Run clean sample — no leaks (inside shell) | `LD_PRELOAD=/app/libleakscope.so ./samples/clean` |
| Run your own program (inside shell) | `LD_PRELOAD=/app/libleakscope.so ./my_program` |
| Symbolize output (inside shell) | `LD_PRELOAD=./libleakscope.so ./my_program 2>&1 \| ./scripts/symbolize.sh ./my_program` |
| Clean build artifacts (inside shell) | `make clean` |
| Rebuild manually (inside shell) | `make` |

---

## How to Read the Leak Report

```
=================================================================
                    LeakScope Memory Report
=================================================================
  Total Allocations:   4        <- Total malloc/calloc/new calls intercepted
  Total Frees:         1        <- Total free/delete calls that matched an active record
  Peak Memory Usage:   350 bytes <- Highest simultaneous live bytes seen
  Dropped Records:     0        <- Allocations not tracked (table full at 65536 limit)
-----------------------------------------------------------------
  LEAK SUMMARY:
  Leaked Blocks:       3        <- Allocations still ACTIVE at process exit
  Leaked Bytes:        350 bytes <- Total bytes that were never freed
-----------------------------------------------------------------
  Leak #1: 200 bytes leaked across 1 block(s)
    frame [0]: 0x7f...          <- LeakScope's own malloc interceptor (ignore this frame)
    frame [1]: 0x401210 (...)   <- Your function that allocated the memory
       -> leak_from_main_task() <- Symbolized: function name + source file + line
    frame [2]: 0x401265 (...)   <- Caller of that function
       -> main                  <- Call chain going up to main()
=================================================================
```

**Leak groups are sorted by total leaked bytes (largest first)**, so the worst offender is always `Leak #1`.

---

## Troubleshooting

| Problem | Likely Cause | Fix |
| :--- | :--- | :--- |
| `docker: command not found` | Docker Desktop not installed | Install from docker.com |
| `Cannot connect to the Docker daemon` | Docker Desktop not running | Open Docker Desktop from Start Menu |
| Build fails: `No such file or directory` | Wrong working directory | `cd` into the folder containing `Dockerfile` |
| No leak output at all | Program exits before destructor runs | Ensure program terminates normally (`main` returns or `exit()` is called) |
| `Dropped Records: N` (N > 0) | More than 65,536 simultaneous live allocations | Reduce allocation count or increase `TABLE_SIZE` in `src/leakscope.cpp` |
| Addresses not symbolized | Missing `-g` flag or not using `symbolize.sh` | Compile with `-g -O0 -no-pie`, pipe through `scripts/symbolize.sh` |
| `DOUBLE FREE` / `INVALID FREE` in your code | Real bug in your program | Fix the memory management at the reported call stack location |

---

## Summary of Files You Need to Know

| File | What It Is |
| :--- | :--- |
| `src/leakscope.cpp` | The entire detector — single source file compiled to `libleakscope.so` |
| `Dockerfile` | Builds the Ubuntu container with all dependencies |
| `Makefile` | Build rules for the `.so` and all sample binaries |
| `scripts/symbolize.sh` | Converts raw hex addresses to function names + line numbers |
| `scripts/test.sh` | Automated test runner called by `make test` |
| `samples/leaky.cpp` | Sample program with intentional memory leaks |
| `samples/double_free.cpp` | Sample demonstrating double-free and invalid-free detection |
| `samples/threads.cpp` | Multi-threaded leak sample |
| `docs/EXPLAIN.md` | Deep-dive explanation of every design decision in the source code |
