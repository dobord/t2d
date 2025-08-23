# Copilot instructions for android-libcoro

This repository uses C++20 coroutines with libcoro and yaml-cpp. Please follow these project-specific rules when proposing changes or generating code.

## Coroutines and scheduling
- Do NOT implement coroutines as lambdas with captures. Prefer free functions (or static member functions) with explicit parameters.
- Every coroutine MUST begin with one scheduling/yield operation to bind execution to the io_scheduler context BEFORE any I/O or blocking operations. Allowed first await expressions (choose one):
  - `co_await scheduler->schedule();`
  - `co_await scheduler->schedule_after(duration);`
  - `co_await scheduler->schedule_at(time_point);`
  - `co_await scheduler->yield();`
  - `co_await scheduler->yield_for(duration);`
  - `co_await scheduler->yield_until(time_point);`
  Rationale: any of these establishes cooperative resumption on the correct scheduler thread before proceeding.
- When spawning background work, use the existing scheduler and pass pointers/references to long‑lived objects owned by the caller; do not construct a new scheduler inside a coroutine.
- Example pattern:
  ```cpp
  coro::task<void> do_something(std::shared_ptr<coro::io_scheduler> scheduler,
                                coro::net::udp::peer* p,
                                SomeState* state) {
    co_await scheduler->schedule();
    // ... coroutine body ...
  }

  // Spawning:
  scheduler->spawn(do_something(scheduler, &peer, &state));
  ```

## Networking (libcoro)
- TCP send loop: always poll for write, send, then flush any `rest` until empty; handle `would_block`.
  ```cpp
  std::span<const char> sp{...};
  co_await client.poll(coro::poll_op::write);
  auto [s, rest] = client.send(sp);
  if (s != coro::net::send_status::ok) co_return;
  while (!rest.empty()) {
    co_await client.poll(coro::poll_op::write);
    std::tie(s, rest) = client.send(rest);
    if (s != coro::net::send_status::ok && s != coro::net::send_status::would_block) co_return;
  }
  ```
- UDP receive: `poll(read)` → `recvfrom(buf)`; ignore `would_block` and continue. For replies, `poll(write)` then `sendto`.
- IPv4 addresses stored in frames are in network byte order. To build `coro::net::ip_address` from `uint32_t` in frames, convert via dotted string helpers to avoid endianness mistakes:
  ```cpp
  dst.address = coro::net::ip_address::from_string(udp2tcp::ipv4_to_string(frame.dst_ip));
  dst.port = ntohs(frame.dst_port);
  ```
## Error handling
- Don’t use `co_await` inside `catch` blocks. Handle errors via status checks and return early on fatal errors.
- Treat `would_block` as a non‑fatal retry signal after a `poll()`.

## Build and deps
- CMake standard is C++20; do not lower.

## Style specifics
- Avoid blocking syscalls without prior `poll()`; prefer non‑blocking and cooperative scheduling.
- When adding TLS client logic, guard with `#ifdef LIBCORO_FEATURE_TLS` and use the awaitable TLS API.
- Project language policy: all source code comments, log messages, commit messages, and documentation MUST be written in English only (no mixed languages) to keep the codebase consistent and accessible. This also applies to build scripts (`build.gradle`, `settings.gradle`, CMake files) and shell/python scripts under `scripts/`.
 - Git commit messages: must be written in English only. Keep the subject line short (<=50 chars) and in present tense; use the commit body to explain the rationale and any important implementation details when necessary.
 - Do NOT use raw `std::cout` / `std::cerr` for runtime messages in first‑party code; always use the project logger (`t2d::log::info|warn|error|debug`). Direct stdout/stderr usage is allowed only inside the logging implementation itself or third_party code.


Following these rules keeps coroutine lifetimes explicit, avoids scheduler mismatches, and prevents subtle endianness and I/O pitfalls.

## Code formatting
- All modifications to C/C++ files (*.c, *.cc, *.cpp, *.cxx, *.h, *.hpp) must be auto-formatted with `clang-format` using the root `.clang-format` file.
- Before finalizing a git commit: run `cmake --build <build_dir> --target format` (or manually `clang-format -i` for the changed files) and ensure `scripts/check_format.sh` passes without errors.
- Non-formatted changes must not be committed.
- Pre-build rule: Before invoking any CMake build (`cmake --build`) in local or automated workflows, first run the formatting target (`cmake --build <build_dir> --target format`) or apply `clang-format -i` to all changed C/C++ files. Builds should assume code is already formatted; do not rely on post-build formatting.
 - CMake files should be formatted with `cmake-format` (target `format_cmake` if available, or `format_all` to include both C++ and CMake). If `cmake-format` is not installed, do not commit manual reflow that diverges from existing style.
 - Every first-party source file must start with an SPDX header: `// SPDX-License-Identifier: Apache-2.0` (or `# SPDX-License-Identifier: Apache-2.0` for CMake/shell). Do NOT modify files under `third_party/`.
 - Pre-commit hook auto-inserts the SPDX header for new first-party C/C++ files if you forget; still add it manually for other file types (proto, CMake, scripts).

## Android build & deployment (APK install) guidance
To avoid intermittent install failures caused by relative working directories, always use the absolute path when installing an APK via `adb install -r`.

Examples:
```bash
# Build (if needed)
# cd to the project root
./gradlew :app:assembleDebug

# Resolve absolute path and install
ABS_APK="$(realpath app/build/outputs/apk/debug/app-debug.apk)"
adb install -r "$ABS_APK"
```

PowerShell:
```powershell
#cd to the project root
./gradlew :app:assembleDebug
$apk = Resolve-Path .\app\build\outputs\apk\debug\app-debug.apk
adb install -r $apk
```

Rationale:
- Prevents `adb: failed to stat ...` when commands are run from unexpected directories or via CI wrappers.
- Makes logs and reproduction steps unambiguous.

When writing automation (scripts, CI steps), always compute and echo the resolved APK path before calling `adb install`.
