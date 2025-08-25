// SPDX-License-Identifier: Apache-2.0
# Tooling & Workflow Guide

This document centralizes developer tooling, formatting, logging, and local loop guidance for the t2d project.

## 1. Formatting Pipeline

| Layer | Scope | Tool | Trigger |
|-------|-------|------|---------|
| `t2d_format` | C/C++ | `clang-format` | Manual (CMake target) or dev loop pre-build |
| `format_qml` | QML | `qmlformat` | Present only if tool discovered |
| `format_cmake` | CMake | `cmake-format` | Optional (installed locally / CI) |
| `format_all` | Aggregate | Invokes available targets above | Preferred entrypoint |
| Pre-commit hook | Staged & modified tracked C/C++/QML | Direct tool invocation | Automatically on `git commit` |

Pre-commit hook responsibilities:
1. Detect staged C/C++ & QML (excluding `third_party/`)
2. Include modified-but-unstaged tracked files for formatting (so devs can commit without manual add after edits)
3. Inject SPDX header if missing (C/C++/QML first-party files)
4. Discover `qmlformat` (PATH → CMakeCache → `qt_local.cmake` prefixes) and reconfigure once if absent but needed
5. Restage changed files, optionally block commit when `T2D_FORMAT_BLOCK=1` or `git config t2d.formatBlock true`
6. Fail commit if `T2D_QML_FORMAT_REQUIRED=1` and QML changed but tool not found

Useful env flags:
| Variable | Effect |
|----------|--------|
| `T2D_PRECOMMIT_DEBUG=1` | Bash xtrace for the hook |
| `T2D_QML_FORMAT_REQUIRED=1` | Hard failure if QML changed without `qmlformat` |
| `T2D_FORMAT_BLOCK=1` | Abort after formatting so user reviews diff |

Install / update hook:
```bash
scripts/install_precommit_format_hook.sh
```

## 2. Qt Tool Discovery (`qt_local.cmake`)
If your Qt install is not on standard prefix paths, create a root-level `qt_local.cmake`:
```cmake
set(CMAKE_PREFIX_PATH "/opt/Qt/6.8.3/gcc_64" ${CMAKE_PREFIX_PATH})
```
This is loaded early in the top-level `CMakeLists.txt` so that `find_package(Qt6 ...)` and custom tool lookup (`qmlformat`) succeed. The pre-commit hook also scans these prefixes (`*/gcc_64/bin/qmlformat`). Do not commit workstation-specific absolute paths unless intentionally shared; use `qt_local.example.cmake` as a template.

## 3. Dev Loop Script
`scripts/run_dev_loop.sh` orchestrates continuous rebuild, restart and focus on rapid iteration.

Features:
* Mandatory formatting before each incremental build
* Auto-reconfigure when `qt_local.cmake` changes or when `qmlformat` becomes available after first build
* Clean shutdown & restart of server and Qt client when either exits
* Optional single-run mode (`LOOP=0`)

Key environment variables / flags:
| Var / Flag | Default | Values | Description |
|------------|---------|--------|-------------|
| `PORT` / `-p` | 40000 | int | Server/client TCP port |
| `BUILD_DIR` / `-d` | build | path | Shared build directory |
| `BUILD_TYPE` / `-t` / `-r` | Debug | CMake types | CMake build type (`-r` = Release shortcut) |
| `CMAKE_ARGS` / `--cmake-args` | (empty) | string | Extra CMake configure args (repeat to append) |
| `LOOP` / `--once` / `--loop` | 1 | 0/1 | Auto-restart loop (0 single run) |
| `NO_BUILD` / `--no-build` | 0 | 0/1 | Skip build phase |
| `VERBOSE` / `-v` | 0 | 0/1 | Enable bash trace + default LOG_LEVEL=DEBUG |
| `LOG_LEVEL` / `--log-level` | INFO | TRACE/DEBUG/INFO/WARN/ERROR | Script log filter + seeds `T2D_LOG_LEVEL` if unset |
| `QML_LOG_LEVEL` / `--qml-log-level` | (inherits LOG_LEVEL) | trace..error | QML logging level passed to client |
| `NO_BOT_FIRE` / `--no-bot-fire` | 0 | 0/1 | Disable bot firing (server) |
| `NO_BOT_AI` / `--no-bot-ai` | 0 | 0/1 | Disable all bot AI (server) |

Script log levels only affect the script’s own output; runtime of server/client uses exported `T2D_LOG_LEVEL` (lowercased) and QML-specific flags. Millisecond timestamps used for alignment with C++ logs.

## 4. Logging (C++ & QML)

### C++ Logger
Environment variables:
| Env | Effect |
|-----|--------|
| `T2D_LOG_LEVEL` | Minimum level (`trace|debug|info|warn|error`) |
| `T2D_LOG_JSON` | Structured JSON output (millisecond timestamp) |
| `T2D_LOG_APP_ID` | Short tag in prefix (e.g. `srv`, `qt`) |

### Qt/QML Logging
QML implements its own lightweight level filter separate from the C++ logger. Resolution order:
1. `--qml-log-level=<level>` argument
2. Fallback: `--log-level=<level>` argument (passed through dev loop)
3. Default: `INFO`

Helper functions (logT/logD/logI/logW/logE) prefix with millisecond timestamp + `[qml]` format for visual alignment with C++ logs.

### Command-line Flags (Client)
| Flag | Purpose |
|------|---------|
| `--log-level=<level>` | Sets C++ logger level unless env already overrides |
| `--qml-log-level=<level>` | Sets QML logging filter |

Example:
```bash
T2D_LOG_LEVEL=debug ./build/t2d_qt_client --qml-log-level=warn
# Full trace (very verbose):
T2D_LOG_LEVEL=trace ./build/t2d_qt_client --qml-log-level=trace
```

## 5. Failure Handling
`run_dev_loop.sh` surfaces build errors explicitly:
```
[run_dev] [E] Build failed for targets: t2d_server t2d_qt_client (see errors above). Aborting.
```
The script also validates the expected binaries exist after a successful build.

## 6. SPDX & Licensing
New first-party C/C++/QML files missing an SPDX header get one injected automatically by the pre-commit hook. Always retain:
```cpp
// SPDX-License-Identifier: Apache-2.0
```

## 7. Troubleshooting Quick Reference
| Symptom | Likely Cause | Fix |
|---------|--------------|-----|
| QML not auto-formatted | `qmlformat` not found | Install Qt or add `qt_local.cmake`; re-run hook install |
| Commit blocked after formatting | Strict mode active | Unset `T2D_FORMAT_BLOCK` or `git config --unset t2d.formatBlock` |
| Dev loop ignores new Qt path | Stale cache | Touch / edit `qt_local.cmake` or delete build dir |
| Excess input debug logs | QML debug level active | Pass `--qml-log-level=info` or higher |

## 8. Future Enhancements (Planned Tooling)
* CI job to enforce presence of `qmlformat` when QML changes (mirroring local strict flag)
* Central logging configuration message on startup summarizing active levels (server + client + QML)
* Optional colorized TTY logs (config gated)

---
For any questions or suggestions, open an issue tagged `docs` or `build`.
