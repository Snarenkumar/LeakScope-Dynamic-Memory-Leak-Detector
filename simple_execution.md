# LeakScope: Super Simple Guide 🔍
> **A complete, easy-to-understand explanation for absolute beginners.**  
> *No confusing jargon. Just plain, simple English with real-world examples.*

---

## 📑 Table of Contents
1. [The 10-Second Summary](#1-the-10-second-summary)
2. [What is a Memory Leak? (The Library Analogy)](#2-what-is-a-memory-leak-the-library-analogy)
3. [What is LeakScope? (The Security Guard)](#3-what-is-leakscope-the-security-guard)
4. [Input and Output (What Goes In, What Comes Out)](#4-input-and-output)
5. [The Step-by-Step Flow of Execution](#5-the-step-by-step-flow-of-execution)
6. [Classes, Objects & Data Structures (Is this OOP?)](#6-classes-objects--data-structures)
7. [The 2 Genius Tricks Inside the Code](#7-the-2-genius-tricks-inside-the-code)
8. [How to Run It (Copy & Paste Commands)](#8-how-to-run-it)
9. [Beginner's Glossary (Plain English Terms)](#9-beginners-glossary)

---

## 1. The 10-Second Summary

- When programs run, they borrow chunks of computer memory (RAM) using `malloc()` or `new`.
- When they are done with that memory, they are supposed to give it back using `free()` or `delete`.
- If a programmer forgets to give it back, that memory stays locked forever. That is called a **Memory Leak**.
- **LeakScope** is an invisible detective tool. It watches your program while it runs, takes notes of every piece of memory borrowed and returned, and prints a neat report at the end showing **exactly which line of code forgot to return memory!**

---

## 2. What is a Memory Leak? (The Library Analogy)

Imagine you go to a public library:
1. You ask the librarian for **5 books** (`malloc`).
2. You sit at a table and read them.
3. When you leave, you return **2 books** to the desk (`free`).
4. But you put **3 books in your backpack** and walk home!

Those 3 missing books are a **memory leak**.
- The library still thinks the books are in use.
- Nobody else can borrow them.
- If every visitor does this, after a few days the library has **zero books left**.
- On a computer, if a program leaks memory for hours, your computer runs out of RAM and crashes!

---

## 3. What is LeakScope? (The Security Guard)

**LeakScope** is a security guard standing right at the library counter:
- Whenever you borrow a book (`malloc`), the guard writes down:
  - Who you are (Which function asked for it)
  - The book ID (Memory address pointer, like `0x55a10`)
  - How big the book is (Number of bytes, e.g. 200 bytes)
  - Exactly what line in your code asked for it
- Whenever you return a book (`free`), the guard finds your entry and draws a green checkmark next to it.
- When the program closes (`exit`), the guard opens his notebook:
  - If there are unchecked entries, the guard sounds the alarm:  
    `"Hey! Line 26 in leaky.cpp took 200 bytes and NEVER returned it!"`

### The Best Part?
**You do not have to touch the original program's code!**
You don't need to rewrite anything or recompile your program. LeakScope slips in quietly from the outside using a Linux feature called `LD_PRELOAD`.

---

## 4. Input and Output

### 📥 Input: What does LeakScope take?
1. **Any compiled C or C++ program** (for example: `./samples/leaky`).
2. **The LeakScope spy library** (`libleakscope.so`).

### 📤 Output: What does LeakScope produce?
When your program finishes running, LeakScope prints this clean summary on your terminal screen:

```text
=================================================================
                    LeakScope Memory Report                      
=================================================================
  Total Allocations:   4             <-- Borrowed memory 4 times
  Total Frees:         1             <-- Returned memory only 1 time
  Peak Memory Usage:   350 bytes     <-- Most memory used at once
  Dropped Records:     0
-----------------------------------------------------------------
  LEAK SUMMARY:
  Leaked Blocks:       3 blocks      <-- 3 pieces were never returned!
  Leaked Bytes:        350 bytes     <-- 350 bytes wasted!
-----------------------------------------------------------------
  Leak #1: 200 bytes across 1 block(s)
     ↳ leak_from_main_task() (/app/samples/leaky.cpp:16)
     ↳ main (/app/samples/leaky.cpp:27)

  Leak #2: 150 bytes across 2 block(s)
     ↳ leak_from_helper() (/app/samples/leaky.cpp:6)
     ↳ main (/app/samples/leaky.cpp:26)
=================================================================
```

### 🚨 Bonus Detection: Bad Frees
If your code tries to return the same memory twice, LeakScope catches it immediately:
```text
[LeakScope] ERROR: DOUBLE FREE detected for pointer 0x55d9a...
```
If your code tries to return random memory that was never borrowed:
```text
[LeakScope] ERROR: INVALID FREE detected for pointer 0xdeadbeef
```

---

## 5. The Step-by-Step Flow of Execution

Here is the exact journey from the second you press Enter:

```
+-------------------------------------------------------------------------+
| STEP 1: YOU RUN THE COMMAND                                             |
| Terminal: LD_PRELOAD=./libleakscope.so ./samples/leaky                  |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| STEP 2: LINUX LOADS LEAKSCOPE FIRST                                     |
| Linux looks at LD_PRELOAD and says:                                     |
| "Okay, whenever this program calls malloc() or free(),                  |
| send it to LeakScope instead of standard Linux C library!"               |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| STEP 3: PROGRAM CALLS malloc(200)                                       |
| - LeakScope intercepts the call!                                        |
| - LeakScope calls the real OS malloc to actually get the 200 bytes.     |
| - LeakScope takes a snapshot of the code lines (stack trace).           |
| - LeakScope writes {ptr: 0x55a10, size: 200, state: ACTIVE} in table.   |
| - LeakScope hands the pointer to your program.                          |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| STEP 4: PROGRAM CALLS free(0x55a10)                                     |
| - LeakScope intercepts free!                                            |
| - Looks up 0x55a10 in its notebook table.                               |
| - If found and ACTIVE -> changes state to FREED, then calls real free.  |
| - If already FREED -> prints "[ERROR: DOUBLE FREE]".                    |
| - If never seen -> prints "[ERROR: INVALID FREE]".                      |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| STEP 5: PROGRAM FINISHES & EXITS                                        |
| - The program hits `return 0;` and closes.                              |
| - LeakScope's automatic exit destructor runs (leakscope_exit_report).   |
| - It scans its notebook for any slot still marked as ACTIVE.            |
| - Groups them by function name and prints the final report!             |
+-------------------------------------------------------------------------+
```

---

## 6. Classes, Objects & Data Structures

You might ask: *"Where are the C++ OOP Classes like `class LeakDetector`?"*

In low-level systems programming (like operating system tools and memory profilers), we **deliberately avoid complex C++ classes with heavy constructors and dynamic memory**.  
Why? Because `class` often calls `new` or `malloc` under the hood! And if our leak detector used `malloc`, it would spy on itself and freeze in a loop!

Instead, we use simple **`struct`** (data containers) and static memory:

### 1. `struct Record` (One Notebook Entry)
Think of this as a single card in a filing cabinet:
```cpp
struct Record {
    void* ptr;                     // The memory address (e.g. 0x55d9a)
    size_t size;                   // How many bytes were borrowed (e.g. 200)
    void* stack[MAX_STACK_FRAMES]; // Which functions called it (up to 8 lines)
    int stack_depth;               // How many stack levels deep
    SlotState state;               // Is it EMPTY, ACTIVE, or FREED?
};
```

### 2. `g_table` (The Master Notebook)
```cpp
static Record g_table[65536];
```
This is a fixed list of 65,536 `Record` slots already allocated when the program starts.  
It does **not** call `malloc`. It sits quietly in computer RAM ready to store data instantly.

### 3. `struct LeakGroup` (The Summary Folder)
```cpp
struct LeakGroup {
    void* stack[8];
    int depth;
    size_t total_bytes;
    size_t count;
};
```
If line 16 leaks memory 500 times in a loop, we don't want to print 500 lines of the same thing!  
`LeakGroup` bundles all 500 identical leaks together and says:  
`"Line 16 leaked 500 times for a total of 10,000 bytes."`

### 4. `g_mutex` (The Bathroom Door Lock)
What if two threads in a multi-threaded program call `malloc` at the exact same microsecond?  
`pthread_mutex_t g_mutex;` acts like a lock on a single-stall bathroom. One thread locks it, writes in the notebook, and unlocks it. The other thread waits. Nobody corrupts the notebook!

---

## 7. The 2 Genius Tricks Inside the Code

If you build a memory detector naively, your program will crash instantly due to two classic traps. Here is how LeakScope solves both:

### 🐔 Trap 1: The "Chicken-and-Egg" Trap (dlsym Bootstrap)
- To call the real `malloc`, LeakScope needs to ask Linux: *"Hey Linux, where is the real malloc?"* using a tool called `dlsym()`.
- But inside Linux, `dlsym()` secretly calls `calloc()` to do its job!
- But wait! LeakScope intercepts `calloc()`!
- So `calloc` calls `dlsym`, which calls `calloc`, which calls `dlsym`... **CRASH!**
- **The Solution:**  
  LeakScope keeps a tiny static emergency lunchbox of 8,192 bytes:
  ```cpp
  static char g_bootstrap_buf[8192];
  ```
  During the first few milliseconds of startup, if `calloc` is called, LeakScope hands out bytes from this private lunchbox without calling any real functions. Once Linux is fully loaded, LeakScope switches to the real functions.

### 🪞 Trap 2: The "Mirror / Infinite Loop" Trap (Re-entrancy)
- When someone calls `malloc()`, LeakScope runs `backtrace()` to find the line number.
- What if `backtrace()` internally needs 16 bytes of memory and calls `malloc()`?
- LeakScope would catch its own call, call `backtrace()` again, call `malloc()` again... forever until the stack overflows!
- **The Solution:**  
  A simple flag called `thread_local bool g_in_hook = false;`:
  - When entering LeakScope: *"Am I already in the hook?"*
  - If YES: Just give them real memory directly, don't record it!
  - If NO: Set `g_in_hook = true`, record the details, and set it back to `false` when leaving.

---

## 8. How to Run It

You don't even need Linux installed on your Windows machine if you have Docker!

### Method A: Using Docker (Works right in Windows PowerShell)

1. Open PowerShell in this folder:
   ```powershell
   docker build -t leakscope .
   ```
2. Run the test suite:
   ```powershell
   docker run --rm leakscope
   ```
3. That's it! You will see LeakScope build the project, run test samples, detect leaks, catch double frees, and print reports.

---

### Method B: On Linux / Ubuntu / WSL

1. Compile the tool:
   ```bash
   make
   ```
2. Run any program with LeakScope:
   ```bash
   LD_PRELOAD=./libleakscope.so ./samples/leaky
   ```
3. Run the clean sample (zero leaks):
   ```bash
   LD_PRELOAD=./libleakscope.so ./samples/clean
   ```

---

## 9. Beginner's Glossary

| Word | What it means in plain English |
| :--- | :--- |
| **RAM** | The temporary working memory of your computer where running programs store data. |
| **Pointer (`void* ptr`)** | An address number (like a house address: `0x7ffee4`) pointing to where data lives in RAM. |
| **`malloc()`** | "Memory Allocate" — C function that asks the OS: *"Please give me X bytes of RAM."* |
| **`free()`** | C function that tells the OS: *"I am done with this memory, take it back."* |
| **`new` / `delete`** | The C++ equivalent of `malloc` and `free`. |
| **Memory Leak** | Forgetting to call `free()` on memory you asked for, wasting RAM until the computer slows down. |
| **Double Free** | Calling `free()` on the same memory address twice, which can crash the program or create security bugs. |
| **`LD_PRELOAD`** | A Linux setting that tells the system: *"Put my custom code first in line before standard libraries."* |
| **`dlsym(RTLD_NEXT)`** | A function that says: *"Find me the REAL version of malloc provided by Linux."* |
| **Stack Trace (`backtrace`)** | The breadcrumb trail of which functions called which functions (e.g. `main() -> helper() -> task()`). |
| **`__attribute__((destructor))`** | A special GCC/Clang tag that tells Linux: *"Run this function automatically right before the program exits."* |

---

### 💡 Still Have Questions?
- Want to read the actual code? Look at [`src/leakscope.cpp`](file:///f:/main%20servers%20under%20productions/LeakScope/src/leakscope.cpp). Every single line is documented!
- Want to test a leaky program? Check out [`samples/leaky.cpp`](file:///f:/main%20servers%20under%20productions/LeakScope/samples/leaky.cpp).
- Want to see the technical deep dive? Read [`docs/EXPLAIN.md`](file:///f:/main%20servers%20under%20productions/LeakScope/docs/EXPLAIN.md).
