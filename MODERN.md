# Tech Stack, Standards & Dependencies (MODERN.md)

## 1. Core Toolchain & Standards
- **C++ Standard:** C++20 (`/std:c++20` on MSVC, `-std=c++20` on GCC/Clang).
- **Compilers:** MSVC 2022 x64 (v143 / v144 / MSVC 18+), Windows 11 SDK.
- **Build System:** CMake 3.28+ with `CMakePresets.json`.
- **Package Manager:** `vcpkg` manifest mode (`vcpkg.json`).
- **GUI Framework:** Qt 6.x (Qt Quick, QML, Qt Graphs 2D) located via `Qt6_ROOT` or `CMAKE_PREFIX_PATH`.

## 2. Dependencies & Usage Guidelines
| Library | Purpose | Dependency Rule |
| :--- | :--- | :--- |
| `boost-asio` | Asynchronous I/O and cross-platform serial transport | Pinned in `vcpkg.json`, used in I/O worker |
| `boost-lockfree` | Lock-free queue concepts & utilities | Pinned in `vcpkg.json` |
| `tl-expected` | `tl::expected<T, Error>` for modern zero-overhead error handling | Core result types |
| `nlohmann-json` | JSON schema parsing and session header serialization | Session schema & CLI I/O |
| `sqlite3` | Local embedded read-only DTC query database | Used in `revdash_core` & `revdash_dtc_importer` |
| `spdlog` | Fast structured logging | Logging in core and CLI |
| `cli11` | Command-line option parsing | Headless `revdash_cli` |
| `catch2` (v3) | Modern C++ test framework | All unit and integration test targets |

## 3. Best Practices & Code Invariants
1. **Memory & Concurrency Safety:**
   - The telemetry hot path MUST perform zero heap allocations after startup. Use fixed-capacity ring buffers (`SpscRing`) and pre-allocated buffers.
   - Core domain models use standard value types (`std::string_view`, fixed byte arrays, `std::span`).
2. **Error Handling:**
   - Functions that can fail return `Result<T>` (`tl::expected<T, Error>`).
   - Exceptions are not used for normal control flow in the core telemetry pipeline.
3. **Compiler Warnings & Quality:**
   - MSVC: `/W4`, `/WX` (warnings as errors), `/permissive-`, `/Zc:__cplusplus`, `/utf-8`.
   - Sanitizers: Support `/fsanitize=address` on debug builds.
4. **Architectural Isolation:**
   - `revdash_core` MUST NOT link or include Qt headers. All Qt dependencies are restricted to `revdash_app` (`src/app/adapters/` and `qml/`).
