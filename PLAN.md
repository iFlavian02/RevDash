# Implementation Plan: RevDash Full OBD-II Diagnostics Application

## Goal & Scope
- **Objective:** Build a staged, offline desktop OBD-II diagnostics application for enthusiasts. Windows v1 will support ELM327 USB/Bluetooth SPP and deterministic simulation; a later Linux stage will add SocketCAN and DEB packaging.
- **Success Criteria:** Users can connect or simulate a vehicle, view live telemetry, scan active/pending DTCs and freeze frames, read VIN/ECU metadata, receive heuristic findings, safely clear faults, record/replay sessions, export CSV, and install the Qt/QML application on Windows.
- **Out of Scope:** BLE/Wi-Fi adapters, manufacturer-specific diagnostics, ECU programming/coding, cloud accounts, remote telemetry, mobile/web clients, simultaneous vehicle connections, macOS, automatic session deletion, code signing, and acquisition of the licensed DTC dataset.
- **Assumptions & Defaults:**
  - C++20, CMake 3.28+, MSVC 2022 x64, Qt 6.11.x, dynamic Qt linking, and vcpkg manifest mode.
  - The engine remains independent of Qt and runs in-process using dedicated worker threads; Qt/QML remains on the main thread.
  - One data source is active at a time.
  - Windows v1 includes ELM327, simulation, CLI, all six UI workspaces, and an MSI installer. SocketCAN and DEB packaging follow in a later stage.
  - Core values use SI units; UI and exports support metric and imperial display.
  - Sessions use versioned JSON Lines; CSV is an export format.
  - Generic SAE OBD-II only. A licensed CSV dataset is supplied externally for complete DTC descriptions.
  - English-only v1 UI, with user-facing strings kept localization-ready.
  - Do not modify `.idea/` or generated `cmake-build-debug/` content.

## Affected Files
- [ ] `CMakeLists.txt` (Modify: replace placeholder target with modular core, CLI, UI, tools, tests, and packaging targets)
- [ ] `src/main.cpp` (Remove after dedicated CLI and desktop entry points exist)
- [ ] `CMakePresets.json` (Create: MSVC debug/release and later Linux presets)
- [ ] `vcpkg.json` / `vcpkg-configuration.json` (Create: pinned native dependencies)
- [ ] `.gitignore` (Create: exclude builds, vcpkg output, sessions, and licensed raw datasets)
- [ ] `cmake/CompilerOptions.cmake` / `cmake/Packaging.cmake` (Create: warnings, sanitizers, deployment, and CPack settings)
- [ ] `headers/revdash/core/*.hpp` / `src/core/*.cpp` (Create: common models, results, data-source contracts, threading, and engine service)
- [ ] `headers/revdash/protocol/*.hpp` / `src/protocol/*.cpp` (Create: J1979, PID, DTC, Mode 09, and ISO-TP codecs)
- [ ] `headers/revdash/drivers/*.hpp` / `src/drivers/elm327/*.cpp` (Create: serial enumeration, prompt synchronization, and ELM327 driver)
- [ ] `src/drivers/synthetic/*.cpp` (Create: deterministic engine model and fault injection)
- [ ] `src/drivers/socketcan/*.cpp` (Create later: Linux AF_CAN driver)
- [ ] `headers/revdash/telemetry/*.hpp` / `src/telemetry/*.cpp` (Create: scheduler, SPSC pipeline, aggregation, and metric snapshots)
- [ ] `headers/revdash/diagnostics/*.hpp` / `src/diagnostics/*.cpp` (Create: rule engine, DTC lookup, diagnostic commands, and guarded clearing)
- [ ] `headers/revdash/session/*.hpp` / `src/session/*.cpp` (Create: JSONL recorder, playback source, seek index, and CSV export)
- [ ] `src/cli/main.cpp` (Create: headless integration and diagnostic commands)
- [ ] `src/app/main.cpp` / `src/ui/*.cpp` (Create: Qt application and engine-to-QML adapters)
- [ ] `qml/*.qml` / `qml/components/*.qml` / `qml/workspaces/*.qml` (Create: complete six-workspace interface)
- [ ] `schemas/session-v1.schema.json` (Create: canonical JSONL record contract)
- [ ] `tools/dtc_importer/*` (Create: licensed CSV validation and SQLite generation)
- [ ] `assets/dtc/revdash_dtc.sqlite` (Generate: packaged read-only lookup database)
- [ ] `assets/licenses/*` (Create: Qt, dependency, and DTC dataset notices)
- [ ] `tests/unit/*` / `tests/integration/*` / `tests/fixtures/*` (Create: deterministic protocol, driver, rule, session, and engine tests)
- [ ] `tests/ui/*` / `tests/hardware/*` (Create: QML and hardware-gated validation)
- [ ] `packaging/windows/*` / `packaging/linux/*` (Create: MSI first, DEB later)
- [ ] `.github/workflows/windows.yml` / `.github/workflows/linux.yml` (Create: build and test automation)
- [ ] `docs/architecture.md` / `docs/session-format.md` / `docs/hardware-validation.md` (Create: contracts and operational guidance)

---

## Execution Sequence

### Feature 1: Stage 1 — Build Foundation and Core Contracts
- [ ] **Step 1.1: Establish reproducible targets and dependencies**
  - [ ] *Sub-step 1.1a:* Lower CMake minimum to 3.28, require C++20, and define modular targets `revdash_core`, `revdash_cli`, `revdash_app`, `revdash_dtc_importer`, and test targets.
  - [ ] *Sub-step 1.1b:* Add MSVC x64 configure/build/test presets (`windows-msvc`, `windows-msvc-debug`, `windows-msvc-release`); reserve `linux-gcc-debug` and `linux-gcc-release` for Stage 10.
  - [ ] *Sub-step 1.1c:* Declare Boost.Asio, Boost.Lockfree, tl-expected, nlohmann-json, SQLite3, spdlog, CLI11, and Catch2 through a pinned `vcpkg.json` manifest.
  - [ ] *Sub-step 1.1d:* Locate Qt separately through `Qt6_ROOT`; keep Qt dependencies entirely out of `revdash_core`.
  - [ ] *Sub-step 1.1e:* Enable `/W4`, standard conformance, warnings-as-errors for project targets, and ASan/UBSan configurations.
  - [ ] *Sub-step 1.1f:* Scaffold Catch2 test runner target (`revdash_unit_tests`) and verify CTest discovery.
  - **Verification Criteria:** Run `cmake --preset windows-msvc`, then `cmake --build --preset windows-msvc-debug`.

- [ ] **Step 1.2: Define canonical domain models**
  - [ ] *Sub-step 1.2a:* Define `ErrorDomain`, `Error`, and `Result<T>` using `tl::expected<T, Error>` with stable error codes, messages, and retry flags.
  - [ ] *Sub-step 1.2b:* Define `ConnectionState` enum (`Disconnected`, `Connecting`, `Initializing`, `Ready`, `Reconnecting`, `Disconnecting`, `Faulted`).
  - [ ] *Sub-step 1.2c:* Define fixed-capacity `ObdRequest`, `ObdMessage`, and `RawTransportFrame` structures with monotonic and UTC timestamps.
  - [ ] *Sub-step 1.2d:* Cap ISO-TP payload buffers at 4095 bytes and enforce `Protocol.PayloadTooLarge` rejection.
  - [ ] *Sub-step 1.2e:* Define `MetricId`, `TelemetrySample`, `SampleQuality`, `DtcRecord`, `FreezeFrame`, `EcuMetadata`, `DiagnosticFinding`, `Severity`, and `TelemetrySnapshot`.
  - [ ] *Sub-step 1.2f:* Include full metric definitions (RPM, speed, throttle, MAP, MAF, load, timing, coolant, STFT/LTFT, ambient temp, fuel level, module voltage, O2 channels).
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R core_types --output-on-failure`.

- [ ] **Step 1.3: Define the asynchronous data-source contract**
  - [ ] *Sub-step 1.3a:* Define pure virtual `IDataSource` interface (`connect`, `disconnect`, `reconnect`, `transmit`, `connectionState`, `subscribe`).
  - [ ] *Sub-step 1.3b:* Guarantee non-blocking lifecycle calls; execute completion/stream callbacks strictly on the source worker thread.
  - [ ] *Sub-step 1.3c:* Define `DataSourceConfig` std::variant for serial, synthetic, playback, and SocketCAN configurations.
  - [ ] *Sub-step 1.3d:* Implement idempotent disconnect, `Core.Cancelled` operation aborts, and configuration preservation for reconnection.
  - [ ] *Sub-step 1.3e:* Define move-only RAII subscription token pattern for safe observer unregistration.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R data_source_contract --output-on-failure`.

- [ ] **Step 1.4: Implement concurrency primitives and telemetry store**
  - [ ] *Sub-step 1.4a:* Add fixed-capacity lock-free SPSC ring with acquire/release atomics and cache-line-aligned producer/consumer indices.
  - [ ] *Sub-step 1.4b:* Configure 1024 source-to-pipeline slots and 2048 pipeline-to-recorder slots with non-blocking drop-on-overflow counters.
  - [ ] *Sub-step 1.4c:* Implement latest-value store keyed by `MetricId` using atomic scalar/timestamp pairs for zero-lock telemetry queries.
  - [ ] *Sub-step 1.4d:* Verify steady-state telemetry hot path operates with zero heap allocations post-initialization.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R "spsc|latest_store" --output-on-failure`.

---

### Feature 2: Stage 2 — SAE J1979 Protocol and Telemetry Decoding
- [ ] **Step 2.1: Build the table-driven Mode 01 PID catalog**
  - [ ] *Sub-step 2.1a:* Define PID descriptor table (PID byte, length, unit, priority tier, decoder func, valid bounds).
  - [ ] *Sub-step 2.1b:* Implement decoding formulas for RPM, speed, coolant, calculated load, throttle, trims, MAP, and MAF.
  - [ ] *Sub-step 2.1c:* Implement formulas for timing advance, ambient temp, fuel level, and control module voltage.
  - [ ] *Sub-step 2.1d:* Implement Mode 01 PID `00`/`20`/`40` supported-bitmaps decoder and dynamic query filter.
  - [ ] *Sub-step 2.1e:* Implement narrowband/wideband O2 sensor voltage and equivalence ratio normalization.
  - [ ] *Sub-step 2.1f:* Enforce strict validation: reject short payloads, mode mismatches, negative responses (`7F`), and out-of-bound values.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R mode01 --output-on-failure`.

- [ ] **Step 2.2: Implement diagnostic modes and DTC decoding**
  - [ ] *Sub-step 2.2a:* Decode Mode 03 (stored) and Mode 07 (pending) DTC responses, filtering padding (`0000`) and deduplicating by ECU.
  - [ ] *Sub-step 2.2b:* Implement standard SAE bitfield conversion into `P/C/B/U` DTC strings.
  - [ ] *Sub-step 2.2c:* Implement Mode 02 Freeze Frame frame-zero extraction and PID decoding using the Mode 01 catalog.
  - [ ] *Sub-step 2.2d:* Implement Mode 04 clear diagnostic information request formatting and `0x44` positive-response parsing.
  - [ ] *Sub-step 2.2e:* Implement Mode 09 VIN, calibration ID, and CVN multi-packet message decoding and 17-char VIN validation.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R "dtc_codec|mode02|mode09" --output-on-failure`.

- [ ] **Step 2.3: Implement ISO 15765-4 packing and reassembly**
  - [ ] *Sub-step 2.3a:* Support single (SF), first (FF), consecutive (CF), and flow-control (FC) frames with block size and STmin pacing.
  - [ ] *Sub-step 2.3b:* Support 11-bit standard and 29-bit extended CAN OBD addressing maps.
  - [ ] *Sub-step 2.3c:* Implement reassembly state machine keyed by ECU address with sequence rollover tracking and timeout aborts.
  - [ ] *Sub-step 2.3d:* Ensure ISO-TP codec remains purely platform-neutral for trace replay testing.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R isotp --output-on-failure`.

- [ ] **Step 2.4: Add metric aggregation and quality tracking**
  - [ ] *Sub-step 2.4a:* Implement rolling min, max, mean, and sliding time-window metrics accumulator.
  - [ ] *Sub-step 2.4b:* Ensure monotonic clock source usage for time windows and calculations.
  - [ ] *Sub-step 2.4c:* Implement metric sample quality classifier (`Valid`, `Stale`, `Unsupported`, `Dropped`, `Invalid`).
  - [ ] *Sub-step 2.4d:* Implement window reset logic on source switch, playback seek, or epoch increment.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R metric_aggregator --output-on-failure`.

---

### Feature 3: Stage 3 — Synthetic Powertrain and Procedural Faults
- [ ] **Step 3.1: Implement the deterministic powertrain model**
  - [ ] *Sub-step 3.1a:* Define `SimulationConfig` struct (engine params, vehicle inertia, drag, wheel radius, deterministic seed).
  - [ ] *Sub-step 3.1b:* Implement fixed 10 ms physics timestep integrator independent of frame rates.
  - [ ] *Sub-step 3.1c:* Model torque-balance RPM dynamics with PI idle controller and redline limiter.
  - [ ] *Sub-step 3.1d:* Model drivetrain speed dynamics with rolling friction and aerodynamic resistance.
  - [ ] *Sub-step 3.1e:* Model intake manifold absolute pressure (MAP) and air flow (MAF) based on throttle angle and volumetric efficiency.
  - [ ] *Sub-step 3.1f:* Model thermal curves: cold start, combustion heat, thermostat cycle (88–92°C), and cooling fan control (98°C/93°C).
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R synthetic_physics --output-on-failure`.

- [ ] **Step 3.2: Add fault and noise injection**
  - [ ] *Sub-step 3.2a:* Implement deterministic misfire generator (torque drop, RPM flutter, freeze frame capture, P0300–P0304 emission).
  - [ ] *Sub-step 3.2b:* Implement vacuum leak simulation (idle LTFT > +20%, converging < +5% under load, P0171 emission).
  - [ ] *Sub-step 3.2c:* Implement stuck thermostat simulation (excess cooling, warm-up failure under load, P0128 emission).
  - [ ] *Sub-step 3.2d:* Implement configurable Gaussian sensor noise and packet dropout probabilities using a deterministic PRNG seed.
  - [ ] *Sub-step 3.2e:* Decouple true simulated physical state from noisy sensor output.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R synthetic_faults --output-on-failure`.

- [ ] **Step 3.3: Expose simulation through `IDataSource`**
  - [ ] *Sub-step 3.3a:* Implement `SyntheticDataSource` fulfilling `IDataSource` lifecycle with simulated latency.
  - [ ] *Sub-step 3.3b:* Accept standard raw OBD request buffers and emit corresponding canonical response payloads.
  - [ ] *Sub-step 3.3c:* Support simulated responses for Modes 01, 02, 03, 04, 07, and 09 with virtual ECU state.
  - [ ] *Sub-step 3.3d:* Expose control hooks for start/stop, throttle input, ambient temp, fault injection, and noise.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R synthetic_source --output-on-failure`.

---

### Feature 4: Stage 4 — ELM327, Scheduling, and Streaming Engine
- [ ] **Step 4.1: Build cross-platform serial transport**
  - [ ] *Sub-step 4.1a:* Implement `ISerialTransport` interface wrapping Boost.Asio serial port operations.
  - [ ] *Sub-step 4.1b:* Implement Windows COM port enumeration via SetupAPI (friendly names, VID/PID, Bluetooth SPP tags).
  - [ ] *Sub-step 4.1c:* Support standard baud rates (9600, 38400 default, 115200) with auto-persisted successful settings.
  - [ ] *Sub-step 4.1d:* Ensure transparent handling between physical USB serial ports and virtual Bluetooth SPP COM ports.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R serial_transport --output-on-failure`.

- [ ] **Step 4.2: Implement the ELM327 driver and prompt synchronizer**
  - [ ] *Sub-step 4.2a:* Implement initialization sequence `ATZ → ATE0 → ATL0 → ATH0 → ATSP0` with banner/OK prompt matching.
  - [ ] *Sub-step 4.2b:* Implement 2-retry initialization policy, 5s reset timeout, 2s adaptive command timeout, and `Faulted` state handling.
  - [ ] *Sub-step 4.2c:* Implement non-blocking stream parser handling arbitrary chunks, CR/LF, echoed bytes, and `>` prompt boundaries.
  - [ ] *Sub-step 4.2d:* Classify ELM error signatures (`NO DATA`, `SEARCHING`, `BUS INIT: ERROR`, `UNABLE TO CONNECT`, `STOPPED`, `?`).
  - [ ] *Sub-step 4.2e:* Track round-trip latency, EWMA, timeout counts, and malformed response counts.
  - [ ] *Sub-step 4.2f:* Implement clean cancellation and automatic reconnect state transition on device disconnect / USB unplug.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R elm327 --output-on-failure`.

- [ ] **Step 4.3: Implement the dynamic PID scheduler**
  - [ ] *Sub-step 4.3a:* Configure high-tier polling budget (20–50 Hz aggregate) with fair round-robin for RPM, Speed, and Throttle.
  - [ ] *Sub-step 4.3b:* Configure mid-tier polling (5–10 Hz) for MAP, MAF, load, timing, and active O2 sensors.
  - [ ] *Sub-step 4.3c:* Configure low-tier polling (0.5–1 Hz) for coolant, fuel trims, ambient temp, fuel level, and battery voltage.
  - [ ] *Sub-step 4.3d:* Implement single-flight request serialization for ELM with earliest-deadline-first ordering within priority tiers.
  - [ ] *Sub-step 4.3e:* Monitor EWMA latency and queue depth; adaptively throttle polling to maintain bus utilization under 80%.
  - [ ] *Sub-step 4.3f:* Implement priority congestion backoff: degrade low-tier first, then mid-tier, with 10s recovery ramp.
  - [ ] *Sub-step 4.3g:* Implement atomic polling pause/drain/resume for diagnostic operations (Modes 02/03/04/07/09).
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R pid_scheduler --output-on-failure`.

- [ ] **Step 4.4: Assemble `EngineService` and worker ownership**
  - [ ] *Sub-step 4.4a:* Implement `EngineService` owning source lifecycle, scheduler, decode pipeline, diagnostics, and session recorder.
  - [ ] *Sub-step 4.4b:* Establish threading architecture using `std::jthread`: I/O worker, decode/rule worker, and recording worker.
  - [ ] *Sub-step 4.4c:* Convert raw source frames into `PipelinePacket` variants and enqueue into the lock-free SPSC pipeline.
  - [ ] *Sub-step 4.4d:* Define asynchronous command queue (connect, disconnect, scan, clear, record, playback, simulation control).
  - [ ] *Sub-step 4.4e:* Implement thread-safe event publishing and immutable latest-value store publishing.
  - [ ] *Sub-step 4.4f:* Implement exponential backoff auto-reconnect (0.5s, 1s, 2s, 5s; max 5 attempts).
  - [ ] *Sub-step 4.4g:* Implement engine epoch counter and queue drain on source switch or playback seek.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R engine_pipeline --output-on-failure`.

---

### Feature 5: Stage 5 — Diagnostics Rules, DTC Database, and Safe Commands
- [ ] **Step 5.1: Implement rolling diagnostic evaluation**
  - [ ] *Sub-step 5.1a:* Implement timestamped rolling window evaluator with stale-sample reset logic.
  - [ ] *Sub-step 5.1b:* Implement vacuum leak rule (idle RPM 600–900, load < 30%, median LTFT > +15% for 10s, load convergence check).
  - [ ] *Sub-step 5.1c:* Implement catalyst degradation rule (ECT ≥ 70°C, 20s steady window, O2 oscillation ratio 0.8–1.2, correlation ≥ 0.7).
  - [ ] *Sub-step 5.1d:* Implement stuck thermostat advisory (cold start, load > 20% for 60s, ECT < 80°C, slope < 0.15°C/s).
  - [ ] *Sub-step 5.1e:* Implement alternator critical rule (RPM > 500, voltage < 13.2V or > 14.8V for 5s).
  - [ ] *Sub-step 5.1f:* Implement finding deduplication, evidence capture attachment, and 15s clear-condition resolution.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R diagnostic_rules --output-on-failure`.

- [ ] **Step 5.2: Build the offline DTC database pipeline**
  - [ ] *Sub-step 5.2a:* Implement CSV parser for DTC datasets (`code`, `description`, `severity`, `likely_failure_points`, `source_version`).
  - [ ] *Sub-step 5.2b:* Enforce strict dataset validation (valid DTC regex, permitted severity enums, UTF-8 strings, version check).
  - [ ] *Sub-step 5.2c:* Implement SQLite database generator with optimized indices and schema versioning (`revdash_dtc_importer`).
  - [ ] *Sub-step 5.2d:* Wire build-system dependency for release database generation with fixture fallback for tests.
  - [ ] *Sub-step 5.2e:* Implement offline lookup service (exact match, prefix search, case-insensitive normalization, unknown fallback).
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R dtc_database --output-on-failure`.

- [ ] **Step 5.3: Implement scan, metadata, and guarded Mode 04 workflows**
  - [ ] *Sub-step 5.3a:* Implement combined Mode 03 + 07 diagnostic scan with database enrichment and Mode 02 Freeze Frame extraction.
  - [ ] *Sub-step 5.3b:* Implement Mode 09 ECU metadata query (VIN, CALID, CVN) with ECU source tracking.
  - [ ] *Sub-step 5.3c:* Implement `prepareClearDtc()` interlocks (requires ready physical source, speed ≤ 0.5 km/h, pre-clear evidence snapshot).
  - [ ] *Sub-step 5.3d:* Generate single-use 30-second expiry clear tokens; reject playback, moving vehicle, or expired tokens.
  - [ ] *Sub-step 5.3e:* Implement `confirmClearDtc(token)` sending Mode 04, validating `0x44`, delaying 500 ms, and triggering automated rescan.
  - [ ] *Sub-step 5.3f:* Log structured audit records for all clear attempts, token validations, responses, and rescan outcomes.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R diagnostic_service --output-on-failure`.

---

### Feature 6: Stage 6 — Session Recording, Playback, and Export
- [ ] **Step 6.1: Implement versioned JSONL recording**
  - [ ] *Sub-step 6.1a:* Define JSONL Schema v1 record contracts (header, message, telemetry, DTC, finding, Mode 04 audit, metadata, footer).
  - [ ] *Sub-step 6.1b:* Populate header records with UUID, app/schema version, UTC start, vehicle metadata, and simulation seeds.
  - [ ] *Sub-step 6.1c:* Record timestamps as integer `elapsed_us` and raw payloads as uppercase hex strings.
  - [ ] *Sub-step 6.1d:* Implement high-performance zero-allocation serializer using `std::to_chars` and pre-allocated write buffers.
  - [ ] *Sub-step 6.1e:* Implement atomic session finalization (`.partial` recording ➔ footer write ➔ `.jsonl` atomic rename).
  - [ ] *Sub-step 6.1f:* Handle recorder queue overflow by tracking drop metrics and logging data-loss markers.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R session_recorder --output-on-failure`.

- [ ] **Step 6.2: Implement deterministic playback as a data source**
  - [ ] *Sub-step 6.2a:* Implement `PlaybackDataSource` streaming stored `ObdMessage` records through standard decoder pipelines.
  - [ ] *Sub-step 6.2b:* Implement transport controls: Play, Pause, Step-Frame, Stop, Seek, and Speed multipliers (0.5×, 1×, 2×, 5×).
  - [ ] *Sub-step 6.2c:* Implement `.ridx` index sidecar generator with 1-second interval seek checkpoints.
  - [ ] *Sub-step 6.2d:* Implement rolling-state seek warm-up (jump to target - 120s, fast-forward silently to rebuild state).
  - [ ] *Sub-step 6.2e:* Dynamically re-evaluate telemetry/findings using active rules while exposing historical audit records.
  - [ ] *Sub-step 6.2f:* Enforce strict format checks (schema validation, monotonicity checks, corrupt payload handling).
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R session_playback --output-on-failure`.

- [ ] **Step 6.3: Implement automotive CSV export**
  - [ ] *Sub-step 6.3a:* Implement 10 Hz synchronous telemetry resampler with sample-and-hold interpolation.
  - [ ] *Sub-step 6.3b:* Implement export presets for RevDash, MegaLogViewer, and TunerStudio with standardized column headers.
  - [ ] *Sub-step 6.3c:* Implement export-time unit conversions (Metric and Imperial) with header unit tagging.
  - [ ] *Sub-step 6.3d:* Implement atomic file export via temporary staging files.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R csv_export --output-on-failure`.

---

### Feature 7: Stage 7 — Headless CLI and Backend Acceptance
- [ ] **Step 7.1: Build the diagnostic CLI**
  - [ ] *Sub-step 7.1a:* Implement CLI commands (`sources`, `live`, `scan`, `identify`, `simulate`, `record`, `playback`, `export`, `clear`) using CLI11.
  - [ ] *Sub-step 7.1b:* Add support for formatted terminal tables and machine-readable JSON Line output.
  - [ ] *Sub-step 7.1c:* Standardize exit codes (0: success, 2: usage, 3: connection, 4: protocol, 5: safety rejection, 6: I/O error).
  - [ ] *Sub-step 7.1d:* Implement graceful SIGINT / Ctrl+C signal handling with clean engine and session shutdown.
  - [ ] *Sub-step 7.1e:* Enforce `--acknowledge-data-loss` and two-step token verification for CLI clear commands.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R cli --output-on-failure`.

- [ ] **Step 7.2: Validate complete backend flow**
  - [ ] *Sub-step 7.2a:* Execute end-to-end integration scenarios across synthetic faults, scheduling, decoding, and recording.
  - [ ] *Sub-step 7.2b:* Profile pipeline hot path to confirm zero allocations across 100,000 processed telemetry frames.
  - [ ] *Sub-step 7.2c:* Validate zero-drop 5× playback at 50 packets/sec throughput.
  - [ ] *Sub-step 7.2d:* Verify engine shutdown completes within 2 seconds under active I/O loads.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-release -L backend_e2e --output-on-failure`.

---

### Feature 8: Stage 8 — Qt 6/QML Desktop Interface
- [ ] **Step 8.1: Build the Qt application shell and adapter**
  - [ ] *Sub-step 8.1a:* Implement `AppController`, `TelemetryModel`, `DtcModel`, `FindingModel`, `SessionModel`, and `SourceModel`.
  - [ ] *Sub-step 8.1b:* Implement 20 Hz snapshot polling and 10 Hz batched UI chart updates to keep the main thread unblocked.
  - [ ] *Sub-step 8.1c:* Ensure all QObject mutations remain strictly on the main thread; dispatch engine requests asynchronously.
  - [ ] *Sub-step 8.1d:* Register QML module via `qt_add_qml_module` and configure Qt Graphs 2D components.
  - [ ] *Sub-step 8.1e:* Implement dark/light theming, scalable automotive UI layout, and metric/imperial bindings.
  - **Verification Criteria:** Run `cmake --build --preset windows-msvc-debug --target revdash_app_qmllint`, then `ctest --preset windows-msvc-debug -R ui_shell --output-on-failure`.

- [ ] **Step 8.2: Implement Connect workspace**
  - [ ] *Sub-step 8.2a:* Build connection interface supporting ELM327 USB/BT and Synthetic simulation profiles.
  - [ ] *Sub-step 8.2b:* Add COM port drop-down, baud selector, live connection state indicators, latency readout, and retry counters.
  - [ ] *Sub-step 8.2c:* Add simulation configuration controls (engine presets, seed input) prior to connection.
  - [ ] *Sub-step 8.2d:* Lock source switching while a guarded clear operation is pending confirmation.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R connect_workspace --output-on-failure`.

- [ ] **Step 8.3: Implement Live Dashboard workspace**
  - [ ] *Sub-step 8.3a:* Build primary telemetry gauge cluster (RPM, speed, throttle, coolant, load, MAP, MAF, trims, voltage).
  - [ ] *Sub-step 8.3b:* Build rolling time-series graph components with 10s, 30s, and 120s selectable ranges.
  - [ ] *Sub-step 8.3c:* Add telemetry health status indicators (bus rate Hz, latency ms, sample age, dropped packet counts).
  - [ ] *Sub-step 8.3d:* Bind presentation-layer unit conversion for all displayed metrics.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R dashboard_workspace --output-on-failure`.

- [ ] **Step 8.4: Implement Diagnostics workspace**
  - [ ] *Sub-step 8.4a:* Build DTC scan view (stored/pending groups, severity badges, database descriptions, freeze-frame inspection).
  - [ ] *Sub-step 8.4b:* Build heuristic findings panel displaying active rules, evidence snapshots, and resolution statuses.
  - [ ] *Sub-step 8.4c:* Build bounded raw diagnostic message terminal with pause, copy, and hex filter capabilities.
  - [ ] *Sub-step 8.4d:* Build guarded Mode 04 modal dialog (safety preconditions check ➔ token display ➔ confirmation countdown ➔ rescan result).
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R diagnostics_workspace --output-on-failure`.

- [ ] **Step 8.5: Implement Simulator workspace**
  - [ ] *Sub-step 8.5a:* Build interactive simulation dashboard (ignition toggle, throttle slider, ambient temp, fault injection triggers).
  - [ ] *Sub-step 8.5b:* Add side-by-side display comparing true simulated physics state against noisy OBD-II output.
  - [ ] *Sub-step 8.5c:* Disable simulation controls automatically when connected to a physical vehicle.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R simulator_workspace --output-on-failure`.

- [ ] **Step 8.6: Implement Sessions and Settings/DTC Lookup workspaces**
  - [ ] *Sub-step 8.6a:* Build session manager view (recorded file list, metadata, timestamps, DTC counts, file sizes).
  - [ ] *Sub-step 8.6b:* Build playback control bar (play/pause, scrub slider, speed selectors, recovery actions, CSV export dialog).
  - [ ] *Sub-step 8.6c:* Build application settings view (unit selection, themes, default paths, connection parameters).
  - [ ] *Sub-step 8.6d:* Build offline DTC dictionary search view (code lookup, keyword search, severity descriptions).
  - [ ] *Sub-step 8.6e:* Bind settings persistence to `QStandardPaths::AppConfigLocation`.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-debug -R "sessions_workspace|settings_workspace" --output-on-failure`.

- [ ] **Step 8.7: Validate UI performance and end-to-end navigation**
  - [ ] *Sub-step 8.7a:* Run full-throughput synthetic simulation while stress-testing all 6 workspaces and session recording.
  - [ ] *Sub-step 8.7b:* Verify UI latency maintains p95 < 100 ms with zero main-thread frames exceeding 16 ms.
  - [ ] *Sub-step 8.7c:* Validate edge-case UI handling (connection drops, invalid packets, missing databases, export failures).
  - **Verification Criteria:** Run `ctest --preset windows-msvc-release -L ui_e2e --output-on-failure`.

---

### Feature 9: Stage 9 — Windows v1 Packaging and Hardware Validation
- [ ] **Step 9.1: Produce the Windows installer**
  - [ ] *Sub-step 9.1a:* Build Release target using MSVC 2022 x64 and dynamically linked Qt 6.11.x.
  - [ ] *Sub-step 9.1b:* Run `windeployqt` / Qt deployment automation; stage external DLLs and `revdash_dtc.sqlite`.
  - [ ] *Sub-step 9.1c:* Configure CPack WiX generator for x64 MSI (Start Menu shortcut, version info, licenses).
  - [ ] *Sub-step 9.1d:* Configure installer prerequisites to chain the official Microsoft Visual C++ Redistributable.
  - [ ] *Sub-step 9.1e:* Enforce read-only installation directory structure and isolate user data to app data directories.
  - **Verification Criteria:** Run `cmake --build --preset windows-msvc-release --target package`, then run MSI test install in a clean Windows sandbox.

- [ ] **Step 9.2: Validate real ELM327 hardware**
  - [ ] *Sub-step 9.2a:* Execute hardware test harness with physical USB and Bluetooth Classic ELM327 adapters.
  - [ ] *Sub-step 9.2b:* Verify initialization transcript, PID discovery, bus speed adaptation, and hot-unplug recovery during 15-min runs.
  - [ ] *Sub-step 9.2c:* Validate live Mode 03/07/02/09 retrieval, recording, and CSV export against a live test vehicle.
  - [ ] *Sub-step 9.2d:* Verify guarded Mode 04 clear safety workflow on a stationary vehicle with explicit confirmation.
  - **Verification Criteria:** Verify passing results in `tests/hardware/elm327_acceptance.md` and archive the test run report.

- [ ] **Step 9.3: Establish the Windows release gate**
  - [ ] *Sub-step 9.3a:* Configure GitHub Actions Windows CI (MSVC build, CTest suite, packaging verification).
  - [ ] *Sub-step 9.3b:* Run release gate checks (100% unit/integration pass, DTC DB integrity, MSI smoke test).
  - [ ] *Sub-step 9.3c:* Finalize Windows v1 release documentation and hardware checklist.
  - **Verification Criteria:** Run `ctest --preset windows-msvc-release --output-on-failure` followed by package generation.

---

### Feature 10: Stage 10 — Linux SocketCAN and DEB Release
- [ ] **Step 10.1: Implement native SocketCAN**
  - [ ] *Sub-step 10.1a:* Implement Linux-specific `SocketCanDataSource` using non-blocking `AF_CAN` sockets.
  - [ ] *Sub-step 10.1b:* Configure kernel-level CAN ID filters and route incoming frames through the ISO-TP reassembly engine.
  - [ ] *Sub-step 10.1c:* Handle socket errors, interface-down states, and clean disconnects without requiring root runtime privileges.
  - [ ] *Sub-step 10.1d:* Maintain platform-neutral build guards with Windows stub implementation returning `Core.UnsupportedPlatform`.
  - **Verification Criteria:** Run `ctest --preset linux-gcc-debug -R "socketcan|isotp" --output-on-failure` using `vcan0`.

- [ ] **Step 10.2: Add Linux UI integration**
  - [ ] *Sub-step 10.2a:* Dynamically expose SocketCAN interface selection on Linux builds.
  - [ ] *Sub-step 10.2b:* Verify QML workspace compatibility across all 6 views on Linux desktop environments.
  - [ ] *Sub-step 10.2c:* Provide actionable user notifications for offline or misconfigured CAN interfaces.
  - **Verification Criteria:** Run `ctest --preset linux-gcc-debug -R linux_ui --output-on-failure`.

- [ ] **Step 10.3: Produce and validate the DEB package**
  - [ ] *Sub-step 10.3a:* Configure CPack DEB generator (metadata, desktop entry, application icons, runtime dependencies).
  - [ ] *Sub-step 10.3b:* Document user configuration for physical CAN interfaces and virtual `vcan0` setup.
  - [ ] *Sub-step 10.3c:* Configure Linux CI workflow (GCC/Clang builds, ASan/UBSan, vcan integration tests, DEB package test).
  - **Verification Criteria:** Run `cmake --build --preset linux-gcc-release --target package` and test DEB install on an Ubuntu test machine.

- [ ] **Step 10.4: Complete cross-platform acceptance**
  - [ ] *Sub-step 10.4a:* Replay identical session fixtures on Windows and Linux, verifying exact match in decoded metrics, DTCs, and findings.
  - [ ] *Sub-step 10.4b:* Confirm identical behavior across `IDataSource`, PID scheduler, rules engine, and session pipelines.
  - [ ] *Sub-step 10.4c:* Audit numerical tolerances to confirm zero platform discrepancies in diagnostic outcomes.
  - **Verification Criteria:** Run full release test suites across both platforms and verify parity against golden artifacts.
