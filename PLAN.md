# Implementation Plan: RevDash Full OBD-II Diagnostics Application

## Goal & Scope
- **Objective:** Build a staged, offline desktop OBD-II diagnostics application for enthusiasts. Windows v1 will support ELM327 USB/Bluetooth SPP and deterministic simulation; a later Linux stage will add SocketCAN and DEB packaging.
- **Success Criteria:** Users can connect or simulate a vehicle, view live telemetry, scan active/pending DTCs and freeze frames, read VIN/ECU metadata, receive heuristic findings, safely clear faults, record/replay sessions, export CSV, and install the Qt/QML application on Windows.
- **Out of Scope:** BLE/Wi-Fi adapters, manufacturer-specific diagnostics, ECU programming/coding, cloud accounts, remote telemetry, mobile/web clients, simultaneous vehicle connections, macOS, automatic session deletion, code signing, and acquisition of the licensed DTC dataset.
- **Assumptions & Defaults:**
  - C++20, CMake 3.28+, MSVC 2022 x64, Qt 6.11.x, dynamic Qt linking, and vcpkg manifest mode.
  - Qt 6.11 supports MSVC 2022 on Windows 10/11 x64. [Qt Windows support](https://doc.qt.io/qt-6/windows.html)
  - The engine remains independent of Qt and runs in-process using dedicated worker threads; Qt/QML remains on the main thread.
  - One data source is active at a time.
  - Windows v1 includes ELM327, simulation, CLI, all six UI workspaces, and an MSI installer. SocketCAN and DEB packaging follow in a later stage.
  - Core values use SI units; UI and exports support metric and imperial display.
  - Sessions use versioned JSON Lines; CSV is an export format.
  - Generic SAE OBD-II only. A licensed CSV dataset is supplied externally for complete DTC descriptions.
  - English-only v1 UI, with user-facing strings kept localization-ready.
  - Do not modify `.idea/` or generated `cmake-build-debug/` content.

## Affected Files
- `CMakeLists.txt` (Modify: replace the placeholder target with modular core, CLI, UI, tools, tests, and packaging targets)
- `src/main.cpp` (Remove after dedicated CLI and desktop entry points exist)
- `CMakePresets.json` (Create: MSVC debug/release and later Linux presets)
- `vcpkg.json` / `vcpkg-configuration.json` (Create: pinned native dependencies)
- `.gitignore` (Create: exclude builds, vcpkg output, sessions, and licensed raw datasets)
- `cmake/CompilerOptions.cmake` / `cmake/Packaging.cmake` (Create: warnings, sanitizers, deployment, and CPack settings)
- `headers/revdash/core/*.hpp` / `src/core/*.cpp` (Create: common models, results, data-source contracts, threading, and engine service)
- `headers/revdash/protocol/*.hpp` / `src/protocol/*.cpp` (Create: J1979, PID, DTC, Mode 09, and ISO-TP codecs)
- `headers/revdash/drivers/*.hpp` / `src/drivers/elm327/*.cpp` (Create: serial enumeration, prompt synchronization, and ELM327 driver)
- `src/drivers/synthetic/*.cpp` (Create: deterministic engine model and fault injection)
- `src/drivers/socketcan/*.cpp` (Create later: Linux AF_CAN driver)
- `headers/revdash/telemetry/*.hpp` / `src/telemetry/*.cpp` (Create: scheduler, SPSC pipeline, aggregation, and metric snapshots)
- `headers/revdash/diagnostics/*.hpp` / `src/diagnostics/*.cpp` (Create: rule engine, DTC lookup, diagnostic commands, and guarded clearing)
- `headers/revdash/session/*.hpp` / `src/session/*.cpp` (Create: JSONL recorder, playback source, seek index, and CSV export)
- `src/cli/main.cpp` (Create: headless integration and diagnostic commands)
- `src/app/main.cpp` / `src/ui/*.cpp` (Create: Qt application and engine-to-QML adapters)
- `qml/*.qml` / `qml/components/*.qml` / `qml/workspaces/*.qml` (Create: complete six-workspace interface)
- `schemas/session-v1.schema.json` (Create: canonical JSONL record contract)
- `tools/dtc_importer/*` (Create: licensed CSV validation and SQLite generation)
- `assets/dtc/revdash_dtc.sqlite` (Generate: packaged read-only lookup database)
- `assets/licenses/*` (Create: Qt, dependency, and DTC dataset notices)
- `tests/unit/*` / `tests/integration/*` / `tests/fixtures/*` (Create: deterministic protocol, driver, rule, session, and engine tests)
- `tests/ui/*` / `tests/hardware/*` (Create: QML and hardware-gated validation)
- `packaging/windows/*` / `packaging/linux/*` (Create: MSI first, DEB later)
- `.github/workflows/windows.yml` / `.github/workflows/linux.yml` (Create: build and test automation)
- `docs/architecture.md` / `docs/session-format.md` / `docs/hardware-validation.md` (Create: contracts and operational guidance)

## Execution Sequence

### Feature 1: Stage 1 — Build Foundation and Core Contracts
- **Step 1.1: Establish reproducible targets and dependencies**
  - *Sub-step 1.1a:* Lower the excessive CMake minimum to 3.28, require C++20, and create targets `revdash_core`, `revdash_cli`, `revdash_app`, `revdash_dtc_importer`, and corresponding test targets.
  - *Sub-step 1.1b:* Add MSVC x64 configure/build/test presets. Use `windows-msvc`, `windows-msvc-debug`, and `windows-msvc-release`; reserve `linux-gcc-debug` and `linux-gcc-release` for Stage 10.
  - *Sub-step 1.1c:* Declare Boost.Asio, Boost.Lockfree, tl-expected, nlohmann-json, SQLite3, spdlog, CLI11, and Catch2 through a pinned vcpkg manifest. Manifest mode is the recommended project-local dependency workflow. [vcpkg manifest documentation](https://learn.microsoft.com/en-us/vcpkg/concepts/manifest-mode)
  - *Sub-step 1.1d:* Locate Qt separately through `Qt6_ROOT`; keep Qt entirely out of `revdash_core`.
  - *Sub-step 1.1e:* Enable `/W4`, standard conformance, warnings-as-errors for project targets, and optional ASan/UBSan on supported presets.
  - *Verification Criteria:* Run `cmake --preset windows-msvc`, then `cmake --build --preset windows-msvc-debug`.

- **Step 1.2: Define canonical domain models**
  - *Sub-step 1.2a:* Define `ErrorDomain`, `Error`, and `Result<T>` using `tl::expected<T, Error>`. Every error must include a stable code, human-readable message, retryable flag, and optional context.
  - *Sub-step 1.2b:* Define `ConnectionState` as `Disconnected`, `Connecting`, `Initializing`, `Ready`, `Reconnecting`, `Disconnecting`, and `Faulted`.
  - *Sub-step 1.2c:* Define fixed-capacity `ObdRequest`, `ObdMessage`, and `RawTransportFrame` models containing source, optional ECU address, monotonic timestamp, UTC timestamp, sequence number, payload length, and bytes.
  - *Sub-step 1.2d:* Cap ISO-TP payloads at 4095 bytes and reject oversized messages with `Protocol.PayloadTooLarge`.
  - *Sub-step 1.2e:* Define `MetricId`, `TelemetrySample`, `SampleQuality`, `DtcRecord`, `FreezeFrame`, `EcuMetadata`, `DiagnosticFinding`, `Severity`, and immutable `TelemetrySnapshot`.
  - *Sub-step 1.2f:* Include RPM, speed, throttle, MAP, MAF, load, timing, coolant, STFT/LTFT, ambient temperature, fuel level, module voltage, and upstream/downstream O2 channels.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R core_types --output-on-failure`.

- **Step 1.3: Define the asynchronous data-source contract**
  - *Sub-step 1.3a:* Define `IDataSource::connect(config, completion)`, `disconnect(completion)`, `reconnect(completion)`, `transmit(request, completion)`, `connectionState()`, and `subscribe(messageHandler, stateHandler)`.
  - *Sub-step 1.3b:* Make lifecycle and transmit calls non-blocking; completion and stream callbacks execute on the source worker, never the UI thread.
  - *Sub-step 1.3c:* Define `DataSourceConfig` variants for serial, synthetic, playback, and later SocketCAN configuration.
  - *Sub-step 1.3d:* Make disconnect idempotent, cancel outstanding operations with `Core.Cancelled`, stop callbacks before destruction, and preserve the previous configuration for reconnect.
  - *Sub-step 1.3e:* Define a move-only RAII subscription token so observers cannot receive callbacks after unsubscription.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R data_source_contract --output-on-failure`.

- **Step 1.4: Implement the concurrency primitives**
  - *Sub-step 1.4a:* Add a fixed-capacity SPSC ring with acquire/release atomics, cache-line-separated producer/consumer indices, and compile-time capacity validation.
  - *Sub-step 1.4b:* Use 1024 source-to-pipeline slots and 2048 pipeline-to-recorder slots; reject newest entries on overflow, increment counters, and never block I/O.
  - *Sub-step 1.4c:* Add a fixed latest-value store keyed by `MetricId`; use atomic scalar/timestamp fields for telemetry and a mutex only for low-frequency DTC/finding collections.
  - *Sub-step 1.4d:* Prove the steady-state telemetry hot path performs no heap allocation after connection initialization.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R "spsc|latest_store" --output-on-failure`, including a producer/consumer stress test and allocation counter.

### Feature 2: Stage 2 — SAE J1979 Protocol and Telemetry Decoding
- **Step 2.1: Build the table-driven Mode 01 PID catalog**
  - *Sub-step 2.1a:* Define each PID’s identifier, required byte count, canonical unit, polling tier, decode function, valid range, and display metadata.
  - *Sub-step 2.1b:* Implement the specified formulas for RPM, speed, coolant, load, throttle, trims, MAP, and MAF.
  - *Sub-step 2.1c:* Add timing advance `A / 2 - 64`, ambient temperature `A - 40`, fuel level `100A / 255`, and module voltage `(256A+B)/1000`.
  - *Sub-step 2.1d:* Decode supported-PID bitmaps from PIDs `00`, `20`, `40`, and subsequent ranges; never poll an unsupported PID.
  - *Sub-step 2.1e:* Select available upstream/downstream O2 channels from the supported narrowband or wideband PID sets and normalize their voltage/equivalence data for the catalyst rule.
  - *Sub-step 2.1f:* Reject short payloads, invalid response modes, malformed hex, `7F` negative responses, and physically impossible decoded values without publishing a valid sample.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R mode01 --output-on-failure`, covering boundary bytes and known values such as RPM `1A F8 = 1726`.

- **Step 2.2: Implement diagnostic modes and DTC decoding**
  - *Sub-step 2.2a:* Decode Modes 03 and 07 into stored and pending DTC records, ignoring `0000` padding and deduplicating by code/status/ECU.
  - *Sub-step 2.2b:* Decode the top two bits into `P/C/B/U`, the next two bits into the first numeric digit, and the remaining nibbles into the final three characters.
  - *Sub-step 2.2c:* Implement Mode 02 frame-zero extraction, supported-freeze-PID discovery, and conversion through the same PID catalog used by Mode 01.
  - *Sub-step 2.2d:* Define Mode 04 request/positive-response parsing, accepting success only after a valid `0x44` response.
  - *Sub-step 2.2e:* Implement Mode 09 VIN, calibration ID, and CVN parsing; strip padding and validate VIN as exactly 17 printable characters.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R "dtc_codec|mode02|mode09" --output-on-failure`.

- **Step 2.3: Implement ISO 15765-4 packing and reassembly**
  - *Sub-step 2.3a:* Support single, first, consecutive, and flow-control frame types, 12-bit payload lengths, sequence rollover, block size, and STmin pacing.
  - *Sub-step 2.3b:* Support generic 11-bit and 29-bit OBD addressing through configurable request/response ID maps.
  - *Sub-step 2.3c:* Key reassembly state by source and ECU address; reject wrong sequence numbers, timeouts, overflow frames, and unrelated CAN IDs.
  - *Sub-step 2.3d:* Make the codec platform-neutral so SocketCAN tests can run from trace fixtures before the Linux driver exists.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R isotp --output-on-failure`, covering single/multi-frame messages, sequence rollover, flow control, and timeouts.

- **Step 2.4: Add metric aggregation and quality tracking**
  - *Sub-step 2.4a:* Maintain latest, minimum, maximum, arithmetic mean, and time-window history for each supported metric.
  - *Sub-step 2.4b:* Use monotonic timestamps for calculations and UTC only for session metadata.
  - *Sub-step 2.4c:* Mark samples `Valid`, `Stale`, `Unsupported`, `Dropped`, or `Invalid`; declare stale after the greater of three expected intervals or a PID-specific minimum timeout.
  - *Sub-step 2.4d:* Reset windows on source changes, playback seeks, timestamp regression, or explicit engine epoch changes.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R metric_aggregator --output-on-failure`.

### Feature 3: Stage 3 — Synthetic Powertrain and Procedural Faults
- **Step 3.1: Implement the deterministic powertrain model**
  - *Sub-step 3.1a:* Define `SimulationConfig` with a four-cylinder default engine, displacement, idle/redline RPM, inertia, friction, mass, drag, gearing, wheel radius, ambient temperature, and deterministic seed.
  - *Sub-step 3.1b:* Advance physics with a fixed 10 ms timestep independent of UI frame rate.
  - *Sub-step 3.1c:* Derive RPM from combustion torque minus friction and drivetrain load; clamp to zero/redline and maintain idle through an explicit idle controller.
  - *Sub-step 3.1d:* Derive vehicle speed from wheel torque, rolling resistance, and aerodynamic drag; prevent negative speed.
  - *Sub-step 3.1e:* Derive MAP from ambient pressure and throttle/vacuum state; derive MAF from displacement, RPM, volumetric efficiency, pressure, and intake temperature.
  - *Sub-step 3.1f:* Model cold start, load-dependent heat generation, thermostat hysteresis at 88–92°C, fan-on at 98°C, fan-off at 93°C, and ambient-dependent cooling.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R synthetic_physics --output-on-failure`, proving identical traces for identical seeds and physically monotonic throttle/warm-up responses.

- **Step 3.2: Add fault and noise injection**
  - *Sub-step 3.2a:* Implement random or cylinder-specific misfires that remove combustion torque, create RPM flutter, capture freeze data, and emit P0300–P0304.
  - *Sub-step 3.2b:* Implement a vacuum leak that drives idle LTFT above +20%, converges below +5% at high load, and emits P0171 after persistence.
  - *Sub-step 3.2c:* Implement a stuck-open thermostat that increases cooling, prevents normal warm-up under load, and emits P0128.
  - *Sub-step 3.2d:* Add per-metric Gaussian noise, configurable standard deviation, packet dropout probability, and deterministic random sequencing.
  - *Sub-step 3.2e:* Preserve the raw physics state separately from noisy sensor output so fault behavior remains reproducible.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R synthetic_faults --output-on-failure`.

- **Step 3.3: Expose simulation through `IDataSource`**
  - *Sub-step 3.3a:* Implement normal lifecycle transitions and configurable simulated response latency.
  - *Sub-step 3.3b:* Accept the same OBD requests as a physical source and generate canonical response bytes before they enter the J1979 decoder.
  - *Sub-step 3.3c:* Support Modes 01, 02, 03, 04, 07, and 09, including a stable test VIN, calibration metadata, pending/stored DTC state, freeze-frame capture, and simulated clearing.
  - *Sub-step 3.3d:* Expose commands for engine start/stop, throttle, ambient temperature, seed, fault activation, noise, and dropout.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R synthetic_source --output-on-failure`, confirming that simulated responses traverse the production decoder.

### Feature 4: Stage 4 — ELM327, Scheduling, and Streaming Engine
- **Step 4.1: Build cross-platform serial transport**
  - *Sub-step 4.1a:* Wrap Boost.Asio serial operations behind `ISerialTransport` so fake transports can provide chunked input, delayed prompts, timeouts, and disconnects.
  - *Sub-step 4.1b:* On Windows enumerate COM devices through SetupAPI, returning port name, friendly label, VID/PID, serial number, and Bluetooth indication where available.
  - *Sub-step 4.1c:* Support configured baud rates 9600, 38400, and 115200; default to 38400 and retain the successful setting.
  - *Sub-step 4.1d:* Treat USB and Bluetooth Classic SPP identically after Windows exposes them as COM ports.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R serial_transport --output-on-failure`.

- **Step 4.2: Implement the ELM327 driver and prompt synchronizer**
  - *Sub-step 4.2a:* Execute `ATZ → ATE0 → ATL0 → ATH0 → ATSP0` in order; accept an ELM banner plus prompt for `ATZ` and require `OK` plus prompt for later commands.
  - *Sub-step 4.2b:* Retry each initialization command twice, use a five-second reset timeout and adaptive two-second command ceiling, then enter `Faulted` with the failed command recorded.
  - *Sub-step 4.2c:* Parse arbitrary serial chunks, CR/LF variations, optional spaces, echoed commands, multiple response lines, and `>` prompt boundaries without blocking.
  - *Sub-step 4.2d:* Classify `NO DATA`, `SEARCHING`, `BUS INIT: ERROR`, `UNABLE TO CONNECT`, `STOPPED`, and `?` into stable protocol/connection errors.
  - *Sub-step 4.2e:* Measure round-trip time from final command-byte write to prompt receipt and expose EWMA, last latency, timeout count, and malformed-response count.
  - *Sub-step 4.2f:* On hot-unplug, cancel the active transaction and transition through reconnect rather than leaving an outstanding completion.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R elm327 --output-on-failure` against transcript fixtures split at every possible byte boundary.

- **Step 4.3: Implement the dynamic PID scheduler**
  - *Sub-step 4.3a:* Give RPM, speed, and throttle an aggregate 20–50 response/second high-tier budget with fair round-robin distribution.
  - *Sub-step 4.3b:* Target each supported MAP, MAF, load, timing, and required O2 metric at 5–10 Hz.
  - *Sub-step 4.3c:* Target each coolant, trim, ambient, fuel-level, and module-voltage metric at 0.5–1 Hz.
  - *Sub-step 4.3d:* Serialize ELM transactions to one outstanding request and use earliest-deadline-first ordering inside priority tiers.
  - *Sub-step 4.3e:* Track EWMA response latency, timeouts, queue occupancy, and achieved per-PID rates; keep estimated bus utilization below 80%.
  - *Sub-step 4.3f:* On congestion, stretch low-tier intervals first, then mid-tier, and high-tier last; recover rates gradually after ten seconds of stable operation.
  - *Sub-step 4.3g:* Pause normal polling for Mode 02/03/04/07/09 transactions, drain the current response, execute the diagnostic operation, and resume without a burst.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R pid_scheduler --output-on-failure` using a fake clock and latency profiles from 10–500 ms.

- **Step 4.4: Assemble `EngineService` and worker ownership**
  - *Sub-step 4.4a:* Let `EngineService` exclusively own the active source, scheduler, pipeline, diagnostics, recorder, and worker lifetimes.
  - *Sub-step 4.4b:* Use one `std::jthread` for Asio/source I/O, one for decode/aggregation/rules, and one writer thread only while recording.
  - *Sub-step 4.4c:* Decode source messages into fixed `PipelinePacket` variants before placing normalized packets into the SPSC consumer pipeline.
  - *Sub-step 4.4d:* Define non-blocking engine commands for connect, disconnect, scan, identify, prepare/confirm clear, recording, playback, export, and simulation control.
  - *Sub-step 4.4e:* Publish connection, telemetry, DTC, finding, session, and error events while exposing telemetry through the latest-value snapshot store.
  - *Sub-step 4.4f:* Auto-reconnect recoverable serial failures after 0.5, 1, 2, and 5 seconds, stop after five failures, and reset attempts after a stable connection.
  - *Sub-step 4.4g:* Increment an engine epoch and clear pending queues/state whenever the source changes or playback seeks.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R engine_pipeline --output-on-failure`, including connect/disconnect races and cooperative shutdown.

### Feature 5: Stage 5 — Diagnostics Rules, DTC Database, and Safe Commands
- **Step 5.1: Implement rolling diagnostic evaluation**
  - *Sub-step 5.1a:* Use timestamped windows rather than sample counts; gaps or stale inputs reset persistence timers and produce `Unavailable`, not a fault.
  - *Sub-step 5.1b:* Trigger vacuum-leak warning when idle RPM is 600–900, load is below 30%, median LTFT exceeds +15% for ten seconds, and a five-second high-load window within the last 120 seconds has RPM above 1500, load at least 60%, and median LTFT below +5%.
  - *Sub-step 5.1c:* Trigger catalyst warning only when coolant is at least 70°C and a 20-second steady window has downstream/upstream O2 oscillation ratio 0.8–1.2 plus normalized correlation of at least 0.7.
  - *Sub-step 5.1d:* Trigger thermostat advisory when a cold-start session sustains load above 20% for 60 seconds, coolant remains below 80°C, and robust temperature slope is below 0.15°C/s.
  - *Sub-step 5.1e:* Trigger alternator critical when RPM exceeds 500 and module voltage remains below 13.2 V or above 14.8 V for five seconds.
  - *Sub-step 5.1f:* Deduplicate findings, attach the exact evidence window, and resolve them after 15 consecutive healthy seconds.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R diagnostic_rules --output-on-failure`, including matching and near-threshold non-matching scenarios.

- **Step 5.2: Build the offline DTC database pipeline**
  - *Sub-step 5.2a:* Accept licensed CSV columns `code`, `description`, `severity`, `likely_failure_points`, and `source_version`.
  - *Sub-step 5.2b:* Validate uppercase DTC format, allowed severity, non-empty descriptions, duplicates, UTF-8, and consistent dataset version before writing output.
  - *Sub-step 5.2c:* Generate deterministic SQLite tables for codes, likely failure points, source metadata, schema version, record count, and source checksum.
  - *Sub-step 5.2d:* Require `REVDASH_DTC_DATASET` for release packaging; use a small redistributable fixture database for development tests.
  - *Sub-step 5.2e:* Expose exact-code lookup, prefix search, normalized case-insensitive search, and `Unknown code` fallback without network access.
  - *Verification Criteria:* Run `cmake --build --preset windows-msvc-debug --target revdash_dtc_importer`, then `ctest --preset windows-msvc-debug -R dtc_database --output-on-failure`.

- **Step 5.3: Implement scan, metadata, and guarded Mode 04 workflows**
  - *Sub-step 5.3a:* Scan Modes 03 and 07 as one operation, enrich results from SQLite, and query Mode 02 frame zero for stored faults.
  - *Sub-step 5.3b:* Read Mode 09 VIN, calibration IDs, and CVNs through one metadata command and preserve optional ECU address provenance.
  - *Sub-step 5.3c:* Define `prepareClearDtc()` to require a ready physical source, a fresh speed sample no greater than 0.5 km/h, and successful pre-clear capture of DTCs/freeze evidence.
  - *Sub-step 5.3d:* Return a one-use confirmation token, captured evidence, readiness warning, and 30-second expiry; reject playback, stale speed, movement, source changes, or expired tokens.
  - *Sub-step 5.3e:* Define `confirmClearDtc(token)` to send Mode 04 once, require `0x44`, wait 500 ms, rescan Modes 03/07, and report cleared/remaining codes.
  - *Sub-step 5.3f:* Write an audit event for preparation, confirmation, response, timeout, and final rescan without claiming success on an ambiguous response.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R diagnostic_service --output-on-failure`, including all safety rejection branches.

### Feature 6: Stage 6 — Session Recording, Playback, and Export
- **Step 6.1: Implement versioned JSONL recording**
  - *Sub-step 6.1a:* Define schema v1 records for header, canonical OBD message, telemetry, connection, DTC scan, diagnostic finding, Mode 04 audit, metadata, dropout, and footer.
  - *Sub-step 6.1b:* Put session UUID, application/schema versions, UTC start, source configuration, VIN/ECU metadata, units, and simulation seed in the header.
  - *Sub-step 6.1c:* Store every record with integer `elapsed_us`; encode canonical request/response payloads as uppercase hex.
  - *Sub-step 6.1d:* Serialize into reusable fixed buffers using `to_chars`, flush at most once per second, and avoid per-record heap allocation after startup.
  - *Sub-step 6.1e:* Record into a `.partial` file, append a footer with counts/drop counters, flush, and atomically rename to `.jsonl`; preserve incomplete files for recovery.
  - *Sub-step 6.1f:* When the recorder queue overflows, count rejected records and write a data-loss marker as soon as capacity returns.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R session_recorder --output-on-failure`, including schema validation, truncated-file recovery, and steady-state allocation checks.

- **Step 6.2: Implement deterministic playback as a data source**
  - *Sub-step 6.2a:* Implement `PlaybackDataSource` using the same `ObdMessage` stream consumed from live and simulated sources.
  - *Sub-step 6.2b:* Support play, pause, one-frame step, stop, seek, and 0.5×/1×/2×/5× speed with monotonic timing.
  - *Sub-step 6.2c:* Build a rebuildable `.ridx` sidecar containing session checksum and byte offsets at one-second intervals.
  - *Sub-step 6.2d:* On seek, reset the engine epoch, jump to at most 120 seconds before the target, fast-forward silently to rebuild rolling state, then publish at the requested position.
  - *Sub-step 6.2e:* Recompute telemetry and findings using current decoders/rules; expose originally recorded findings separately as audit history.
  - *Sub-step 6.2f:* Accept a final incomplete JSON line only as recoverable truncation; reject invalid headers, incompatible major schema versions, non-monotonic timestamps, and corrupt hex.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R session_playback --output-on-failure`, comparing live-recorded and replayed metric sequences.

- **Step 6.3: Implement automotive CSV export**
  - *Sub-step 6.3a:* Resample asynchronous telemetry at 10 Hz, carrying forward only non-stale values and leaving unavailable columns empty.
  - *Sub-step 6.3b:* Provide wide-column presets for RevDash, MegaLogViewer, and TunerStudio using stable names for Time, RPM, Speed, TPS, Load, MAP, MAF, ECT, trims, O2, voltage, and diagnostics.
  - *Sub-step 6.3c:* Convert values from canonical SI at export time and include selected units in headers/metadata.
  - *Sub-step 6.3d:* Export through a temporary file and rename only after successful completion; cancellation must leave the original session unchanged.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R csv_export --output-on-failure` against golden metric/imperial fixtures.

### Feature 7: Stage 7 — Headless CLI and Backend Acceptance
- **Step 7.1: Build the diagnostic CLI**
  - *Sub-step 7.1a:* Add commands for `sources`, `live`, `scan`, `identify`, `simulate`, `record`, `playback`, `export`, and guarded `clear`.
  - *Sub-step 7.1b:* Support human-readable output and line-delimited JSON output without changing engine behavior.
  - *Sub-step 7.1c:* Use exit codes 0 success, 2 usage, 3 connection, 4 protocol, 5 safety rejection, and 6 storage/database failure.
  - *Sub-step 7.1d:* Handle Ctrl+C through cooperative engine shutdown, session finalization, source disconnect, and thread joins.
  - *Sub-step 7.1e:* Require the Mode 04 confirmation token and explicit `--acknowledge-data-loss` flag; do not provide a force override for movement or stale speed.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R cli --output-on-failure`.

- **Step 7.2: Validate the complete backend flow**
  - *Sub-step 7.2a:* Run synthetic healthy, misfire, vacuum leak, thermostat, noise, and dropout scenarios through scheduling, decoding, rules, recording, playback, and export.
  - *Sub-step 7.2b:* Verify the source/pipeline hot path processes 100,000 prepared telemetry packets with zero allocations after warm-up.
  - *Sub-step 7.2c:* Verify 5× playback of a 50-response/second session produces no pipeline drops on the reference Windows machine.
  - *Sub-step 7.2d:* Confirm shutdown completes within two seconds with an active source, recorder, pending diagnostic request, or paused playback.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-release -L backend_e2e --output-on-failure`.

### Feature 8: Stage 8 — Qt 6/QML Desktop Interface
- **Step 8.1: Build the Qt application shell and adapter**
  - *Sub-step 8.1a:* Create `AppController`, `TelemetryModel`, `DtcModel`, `FindingModel`, `SessionModel`, and `SourceModel` as the only QML-facing C++ layer.
  - *Sub-step 8.1b:* Poll immutable engine snapshots at 20 Hz and batch chart updates at 10 Hz; never emit a Qt signal for every raw packet.
  - *Sub-step 8.1c:* Keep QML and all QObject mutation on the main thread; post commands into `EngineService`.
  - *Sub-step 8.1d:* Register the UI through `qt_add_qml_module` and use Qt Graphs 2D for telemetry lines. [Qt QML CMake integration](https://doc.qt.io/qt-6/qtqml-cmake-integration.html) [Qt Graphs](https://doc.qt.io/qt-6/qtgraphs-index.html)
  - *Sub-step 8.1e:* Add dark/light themes, scalable typography, keyboard navigation, accessible names, stale-data styling, and metric/imperial preferences.
  - *Verification Criteria:* Run `cmake --build --preset windows-msvc-debug --target revdash_app_qmllint`, then `ctest --preset windows-msvc-debug -R ui_shell --output-on-failure`.

- **Step 8.2: Implement Connect workspace**
  - *Sub-step 8.2a:* Present ELM327 and Synthetic sources on Windows; add Playback entry points from the Sessions workspace.
  - *Sub-step 8.2b:* For ELM327 show refreshed COM devices, friendly labels, baud selection, connection state, initialization progress, measured latency, reconnect attempts, and actionable errors.
  - *Sub-step 8.2c:* For simulation expose healthy/scenario presets and deterministic seed before connection.
  - *Sub-step 8.2d:* Disable source changes while a guarded clear is awaiting confirmation; otherwise disconnect cleanly before switching.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R connect_workspace --output-on-failure`.

- **Step 8.3: Implement Live Dashboard workspace**
  - *Sub-step 8.3a:* Show primary RPM, speed, throttle, coolant, load, MAP, MAF, trims, and voltage cards/gauges.
  - *Sub-step 8.3b:* Provide selectable 10-second, 30-second, and 120-second graphs without copying unbounded session history into QML.
  - *Sub-step 8.3c:* Display achieved update rate, sample age, supported/unsupported state, drop counter, and source latency.
  - *Sub-step 8.3d:* Apply unit conversion only in presentation; rules and recorded canonical values remain SI.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R dashboard_workspace --output-on-failure`, including stale and unsupported metrics.

- **Step 8.4: Implement Diagnostics workspace**
  - *Sub-step 8.4a:* Add active/pending scans, severity grouping, descriptions, likely failure points, ECU metadata, and freeze-frame evidence.
  - *Sub-step 8.4b:* Display heuristic findings separately from ECU-reported DTCs, including evidence values and active/resolved state.
  - *Sub-step 8.4c:* Add an advanced raw-message trace with bounded retention, pause, copy, and filter controls.
  - *Sub-step 8.4d:* Implement Mode 04 as prepare → evidence review → explicit second confirmation → progress → rescan result; surface every safety rejection.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R diagnostics_workspace --output-on-failure`.

- **Step 8.5: Implement Simulator workspace**
  - *Sub-step 8.5a:* Add engine start/stop, throttle, ambient temperature, seed, misfire selection, vacuum leak, thermostat fault, noise, and dropout controls.
  - *Sub-step 8.5b:* Show raw physics state separately from noisy sensor output.
  - *Sub-step 8.5c:* Disable controls for non-synthetic sources and preserve selected values only as local settings.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R simulator_workspace --output-on-failure`.

- **Step 8.6: Implement Sessions and Settings/DTC Lookup workspaces**
  - *Sub-step 8.6a:* List sessions with timestamp, duration, source, VIN, DTC/finding count, completion state, and file size.
  - *Sub-step 8.6b:* Add play, pause, frame-step, scrub, speed selection, progress, data-loss warnings, recovery, and CSV export.
  - *Sub-step 8.6c:* Add settings for units, theme, default recording path, chart window, serial defaults, and diagnostics explanation level.
  - *Sub-step 8.6d:* Add offline DTC exact/prefix/text search with severity and likely failure points.
  - *Sub-step 8.6e:* Store settings locally under `QStandardPaths::AppConfigLocation` and sessions under a user-selectable directory; uninstall must not remove sessions.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-debug -R "sessions_workspace|settings_workspace" --output-on-failure`.

- **Step 8.7: Validate UI performance and end-to-end navigation**
  - *Sub-step 8.7a:* Run the synthetic source at full configured rates while navigating all workspaces and recording.
  - *Sub-step 8.7b:* Require p95 engine-to-visible-snapshot latency below 100 ms and no main-thread operation longer than 16 ms on the reference Windows machine.
  - *Sub-step 8.7c:* Test connection loss, malformed data, unsupported PIDs, recorder overrun, database absence, failed export, corrupt playback, and expired clear confirmation.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-release -L ui_e2e --output-on-failure`.

### Feature 9: Stage 9 — Windows v1 Packaging and Hardware Validation
- **Step 9.1: Produce the Windows installer**
  - *Sub-step 9.1a:* Build Release with MSVC 2022 x64 and dynamically linked Qt 6.11.x.
  - *Sub-step 9.1b:* Use Qt’s QML deployment script/`windeployqt` to collect Qt DLLs, plugins, and QML modules; explicitly stage non-Qt DLLs and the DTC database. [Qt Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html)
  - *Sub-step 9.1c:* Use CPack’s WiX generator to create an unsigned x64 MSI with Start Menu entry, uninstall support, version metadata, and license notices. [CPack generators](https://cmake.org/cmake/help/latest/manual/cpack-generators.7.html)
  - *Sub-step 9.1d:* Chain the official Visual C++ Redistributable rather than distributing compiler runtime DLLs copied from the development machine.
  - *Sub-step 9.1e:* Install application files read-only under Program Files and keep settings/sessions entirely in user locations.
  - *Verification Criteria:* Run `cmake --build --preset windows-msvc-release --target package`, then install/uninstall the MSI in a clean Windows sandbox.

- **Step 9.2: Validate real ELM327 hardware**
  - *Sub-step 9.2a:* Test one USB serial and one Bluetooth Classic SPP adapter against the same driver.
  - *Sub-step 9.2b:* Capture the initialization transcript, capability bitmap, measured rates, adaptive degradation, reconnect after unplug, and a 15-minute live session.
  - *Sub-step 9.2c:* Validate Modes 03/07, Mode 02 where supported, Mode 09 metadata, recording, playback, and CSV export.
  - *Sub-step 9.2d:* Validate real Mode 04 only on a stationary vehicle and only after explicit owner approval; otherwise validate the full guarded exchange against the fake serial ECU and synthetic source.
  - *Verification Criteria:* Run the documented commands in `tests/hardware/elm327_acceptance.md` and attach the resulting JSON report.

- **Step 9.3: Establish the Windows release gate**
  - *Sub-step 9.3a:* Configure, build, test, package, and archive test reports in Windows CI.
  - *Sub-step 9.3b:* Require all unit/integration/UI tests, DTC release database validation, installer smoke test, license inventory, and hardware checklist before tagging Windows v1.
  - *Sub-step 9.3c:* Document that code signing and automatic update delivery remain out of scope.
  - *Verification Criteria:* Run `ctest --preset windows-msvc-release --output-on-failure` followed by the packaging job.

### Feature 10: Stage 10 — Linux SocketCAN and DEB Release
- **Step 10.1: Implement native SocketCAN**
  - *Sub-step 10.1a:* Add a Linux-only `SocketCanDataSource` using non-blocking `AF_CAN`/`CAN_RAW` sockets bound to a configured `can0` or `vcan0` interface. SocketCAN is the Linux kernel’s native CAN socket interface. [Linux SocketCAN documentation](https://www.kernel.org/doc/html/latest/networking/)
  - *Sub-step 10.1b:* Apply OBD request/response CAN filters, preserve CAN IDs as ECU addresses, and pass frames through the platform-neutral ISO-TP codec.
  - *Sub-step 10.1c:* Handle interface absence, link-down, socket errors, malformed frames, reconnect, and application shutdown without attempting privileged interface configuration.
  - *Sub-step 10.1d:* Keep a Windows factory stub that reports `Core.UnsupportedPlatform` without linking Linux headers.
  - *Verification Criteria:* Run `ctest --preset linux-gcc-debug -R "socketcan|isotp" --output-on-failure` with `vcan0`.

- **Step 10.2: Add Linux UI integration**
  - *Sub-step 10.2a:* Show SocketCAN only on Linux and enumerate available CAN interfaces.
  - *Sub-step 10.2b:* Reuse all dashboard, diagnostic, recorder, playback, and export models without Linux-specific QML branches beyond source configuration.
  - *Sub-step 10.2c:* Add user guidance when an interface exists but is down or lacks system configuration.
  - *Verification Criteria:* Run `ctest --preset linux-gcc-debug -R linux_ui --output-on-failure`.

- **Step 10.3: Produce and validate the DEB package**
  - *Sub-step 10.3a:* Add CPack DEB metadata, desktop entry, icons, runtime dependencies, DTC database, license notices, and uninstall-safe user-data behavior.
  - *Sub-step 10.3b:* Do not grant elevated permissions or bring CAN interfaces up from the application; document `vcan0` and physical CAN setup separately.
  - *Sub-step 10.3c:* Add Ubuntu CI for GCC/Clang builds, sanitizers, vcan integration, QML tests, and package installation.
  - *Verification Criteria:* Run `cmake --build --preset linux-gcc-release --target package`, install the DEB on a clean supported Ubuntu VM, and execute the vcan end-to-end scenario.

- **Step 10.4: Complete cross-platform acceptance**
  - *Sub-step 10.4a:* Replay identical session fixtures on Windows and Linux and require equivalent decoded metrics, DTCs, findings, and exports.
  - *Sub-step 10.4b:* Confirm platform-specific sources share the same `IDataSource`, scheduler, protocol, diagnostics, and session contracts.
  - *Sub-step 10.4c:* Record any floating-point tolerance differences explicitly; no platform may change DTC or rule outcomes.
  - *Verification Criteria:* Run all Windows and Linux release presets and compare golden cross-platform artifacts.
