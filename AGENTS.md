# Repository Guidelines

## Project Structure & Module Organization
- Keep the server sources in `src/checker/` (create as needed), the provided client in `src/client/`, and helpers like `random.c` under `tools/`.
- Put shared headers in `include/`, with protocol structs and socket paths in `include/protocol.h`.
- Store compiled binaries in `build/`; keep the repository tree source-only.

## Build & Run Commands
- `mkdir -p build && gcc -Wall -Wextra -O2 -pthread src/checker/checker.c -o build/checker`: build the multi-process, multi-threaded server.
- `gcc -Wall -Wextra -O2 src/client/prime.c -o build/prime_client` and `gcc -Wall -Wextra -O2 tools/random.c -o build/random_gen`: build the instructor client and deterministic generator.
- Launch a test session with `./build/checker &` followed by `./build/random_gen 100 1 | ./build/prime_client`; stop the server through its wrap-up path or a controlled signal.
- Run `clang-tidy src/checker/checker.c` and `valgrind --tool=memcheck ./build/checker` before milestone submissions to catch UB and leaks.

## Coding Style & Naming Conventions
- Use tabs for indentation (width 8) and brace-on-own-line formatting to match the starter client.
- Prefer `snake_case` for identifiers, `ALL_CAPS` for macros like `SOCK_ADDRESS`, and keep per-module `static` globals minimal.
- Group POSIX headers, then project headers; document non-trivial synchronization or IPC decisions with concise `//` comments.
- Expose the request/reply structs exactly as two `long int` fields to honor the wire format in the brief.

## Concurrency & Resource Management
- Per Requirement 2, fork a child per client and close unused descriptors in parent and child immediately.
- Inside each child, spawn four checking threads plus a sorting thread; share the accepted socket via arguments or globals as described in Requirement 3.
- Marshal replies through a pipe to the sorting thread to enforce in-order delivery (Requirement 6) and buffer out-of-order entries in an indexed structure.
- Protect the `prime_counter` and any shared queues with `pthread_mutex_t`; unlock before blocking operations to avoid priority inversions.
- On wrap-up detect EOF (`recv` returning 0), join threads, close pipe and socket, and log progress per Requirement 8.

## Testing & Submission Practices
- Capture deterministic transcripts under `tests/` (e.g., `tests/seed_1_100.txt`) to verify ordering and progress logs.
- Ensure the server prints thread progress with PID, thread index, request index, and cumulative prime totals per Requirement 7.
- Track compliance level (0–5) in `docs/compliance.txt` and update it as features land.
- Package submissions as instructed (zip source + compliance note) and tag releases locally to mirror delivered snapshots.
