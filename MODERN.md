# RevDash Technology Policy

## Toolchain

- RevDash uses C++20 with CMake 3.30 or later.
- Windows v1 is built with MSVC 2022 x64 and dynamically linked runtime dependencies.
- vcpkg manifest mode is required. The builtin registry baseline in `vcpkg-configuration.json` is pinned; dependency changes must update that baseline deliberately.
- Windows presets are `windows-msvc`, `windows-msvc-debug`, `windows-msvc-release`, and `windows-msvc-asan`. Linux presets are deferred to Stage 10.

## Qt

- Windows v1 targets Qt 6.11.2. Qt is installed separately and discovered through `Qt6_ROOT`.
- UI code may use Qt Core, Gui, Qml, and Quick under the LGPL-compatible licensing path.
- Do not add Qt Graphs, Qt Canvas Painter, or another GPL-only Qt module unless the project license changes compatibly or commercial Qt licensing is adopted.
- `revdash_core` must not include or link Qt. Qt/QML belongs only to the desktop presentation layer.

## C++ and quality practices

- Use `tl::expected<T, Error>` for expected operational failures; do not use exceptions for normal telemetry-pipeline control flow.
- Build RevDash-owned targets with strict warnings: `/W4`, `/WX`, `/permissive-`, `/Zc:__cplusplus`, and UTF-8 source handling on MSVC.
- Windows AddressSanitizer is enabled only through the dedicated `windows-msvc-asan` preset. Linux ASan/UBSan and optional TSan policy will be added with the Linux stage.
- Use Boost.Asio for asynchronous transports and Boost.Lockfree only behind a project-owned bounded queue wrapper. Correct coherent snapshots take priority over speculative lock-free telemetry storage.

## Dependency policy

- The manifest contains Boost.Asio/Lockfree, tl-expected, nlohmann-json, SQLite3, spdlog, CLI11, and Catch2.
- Add or upgrade dependencies only for a concrete requirement after checking current primary documentation and compatibility with this policy.
- Keep licensed production DTC data out of source control unless its license explicitly permits redistribution; fixtures remain clearly non-production.
