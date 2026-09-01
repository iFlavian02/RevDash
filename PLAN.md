# Implementation Plan: RevDash Full OBD-II Diagnostics Application

## Goal & Scope

### Objective

Build a staged, offline desktop OBD-II diagnostics application for automotive enthusiasts.

Windows v1 will support:

* ELM327-compatible USB serial adapters;
* Bluetooth Classic SPP adapters exposed as Windows COM ports;
* deterministic vehicle simulation;
* generic SAE OBD-II diagnostics;
* live telemetry;
* DTC scanning and freeze-frame inspection;
* vehicle/ECU metadata;
* heuristic diagnostic findings;
* guarded fault clearing;
* session recording/playback;
* CSV export;
* headless CLI operation;
* Qt/QML desktop UI;
* MSI installation.

A later Linux stage will add native SocketCAN support and DEB packaging.

### Success Criteria

Users can:

* connect to a supported physical adapter or deterministic simulator;
* view live OBD-II telemetry with quality/health information;
* scan stored and pending DTCs;
* inspect freeze-frame data;
* read VIN, calibration IDs, CVNs, and available ECU metadata;
* receive clearly identified heuristic diagnostic findings with supporting evidence;
* clear emissions-related diagnostic information through a guarded workflow;
* record complete diagnostic sessions;
* replay sessions deterministically;
* export telemetry to CSV;
* use equivalent backend functionality through a CLI;
* install and run the application on a clean Windows system.

### Out of Scope

* Bluetooth Low Energy adapters;
* Wi-Fi ELM adapters;
* manufacturer-specific diagnostics;
* UDS service coverage beyond what is required internally for generic OBD-II;
* ECU programming, flashing, adaptation, coding, or immobilizer operations;
* cloud accounts;
* remote telemetry;
* mobile/web clients;
* simultaneous active vehicle connections;
* macOS;
* automatic session deletion;
* code signing;
* acquisition of the licensed production DTC dataset;
* proprietary OEM PID databases.

---

## Architecture & Technical Decisions

### Toolchain Baseline

* C++20.
* CMake 3.30+.
* MSVC 2022 x64 for Windows.
* Qt 6.11.2 baseline for Windows v1.
* Dynamic Qt linking.
* vcpkg manifest mode with a pinned baseline.
* GCC/Clang support added during the Linux stage.
* Windows sanitizer configuration uses AddressSanitizer.
* Linux sanitizer configurations use ASan + UBSan; core concurrency tests may additionally use a separate TSan lane when supported.

`MODERN.md` must record the exact adopted toolchain/dependency versions once they are established.

### Qt Licensing Boundary

Default architecture must use Qt modules available under an LGPL-compatible open-source licensing path.

Do not depend on Qt Graphs, Qt Canvas Painter, or another GPL-only Qt module unless the project explicitly adopts a compatible GPL license or commercial Qt licensing.

Telemetry charts will therefore use a custom Qt Quick scene-graph item rather than Qt Graphs.

### Core/UI Boundary

`revdash_core` remains completely independent of Qt.

Qt/QML exists only in the desktop presentation layer.

The core engine, protocol implementation, drivers, diagnostics, sessions, simulation, and CLI must remain usable without creating a Qt application.

### Data-Source Architecture

Only one active `IDataSource` exists at a time.

Supported configurations:

* Serial ELM327;
* Synthetic;
* Playback;
* later SocketCAN.

All sources expose canonical logical OBD requests/responses to the engine.

Transport-specific framing remains inside the source implementation.

### Transport Ownership

#### ELM327

The ELM327 remains responsible for:

* physical OBD protocol handling;
* ISO 15765 transmission formatting;
* ISO-TP flow-control generation;
* transport timing required by the selected vehicle protocol.

RevDash remains responsible for:

* ELM command synchronization;
* parsing responses;
* preserving ECU/header identity;
* normalizing responses into canonical `ObdMessage` objects;
* validating logical OBD payloads.

RevDash must **not** generate ISO-TP flow-control frames through ELM327 during normal operation.

Headers remain enabled so ECU identity is preserved.

#### Linux SocketCAN

Linux production diagnostics use kernel `CAN_ISOTP` sockets for per-ECU ISO-TP diagnostic exchanges.

`CAN_RAW` is used only where appropriate for:

* functional ECU discovery;
* bounded single-frame discovery traffic;
* optional raw monitoring/trace capture.

Do not recreate production ISO-TP segmentation, flow control, and pacing in userspace when the kernel transport is available.

### Portable ISO-TP Utility

A platform-neutral ISO-TP codec may exist for:

* deterministic protocol tests;
* raw CAN trace validation;
* fixture generation;
* reassembling captured frame sequences;
* validating expected SF/FF/CF/FC behavior.

It is not the default production transport implementation for either ELM327 or Linux `CAN_ISOTP`.

### Thread Ownership

Desktop process:

```text
Qt/QML main thread
        |
        | async commands / snapshots
        v
Engine worker
        |
        +---- active IDataSource worker/executor
        |
        +---- recorder worker
```

Responsibilities:

* UI thread owns all `QObject`/QML mutations.
* Each active source owns its transport worker/executor.
* Engine worker owns scheduling, decoding, aggregation, diagnostics, and orchestration.
* Recorder worker owns session file I/O.
* Source → engine and engine → recorder hot paths use bounded SPSC queues.
* UI → engine commands use a safe serialized command queue.
* No blocking vehicle I/O occurs on the UI thread.

### Queue Policy

Use `boost::lockfree::spsc_queue` behind a project wrapper rather than implementing a new lock-free queue algorithm.

The wrapper provides:

* compile-time bounded capacity;
* non-blocking push/pop;
* overflow counters;
* explicit drop policy;
* queue health instrumentation.

Initial capacities:

* source → engine: 1024 packets;
* engine → recorder: 2048 packets.

Capacity values must remain configurable constants and validated under acceptance workloads.

### Latest Telemetry Store

Correctness takes priority over an unnecessary lock-free implementation.

Maintain fixed metric slots and provide coherent `TelemetrySnapshot` reads using a small synchronization primitive such as `std::shared_mutex`.

Do not store logically related value/timestamp/quality fields as unrelated atomics that can be observed from different updates.

Optimize further only if profiling demonstrates meaningful contention.

### Time

Use:

* monotonic time for scheduling, latency, timeouts, playback, rule windows, and aggregation;
* UTC timestamps for durable session/audit metadata.

Provide an injectable/manual test clock where deterministic timeout/window tests require it.

### Units

Core values remain SI.

Conversions occur only at presentation/export boundaries.

Supported display/export modes:

* Metric;
* Imperial.

### Session Format

Canonical session format:

* versioned JSON Lines;
* streaming-friendly;
* integer `elapsed_us` monotonic offsets;
* UTC session metadata;
* raw logical OBD payloads represented as uppercase hex;
* explicit ECU/source identity;
* explicit schema version.

CSV is export-only.

### DTC Dataset

A licensed external CSV dataset supplies production DTC descriptions.

The repository contains only legal fixture data required for tests unless the production dataset license explicitly allows redistribution.

A production package must fail its release gate if the required production DTC database is absent.

Never silently substitute fixture DTC data into a release build.

### Localization

Windows v1 UI is English-only.

All user-facing strings must nevertheless remain localization-ready.

### Repository Safety

Do not modify:

* `.idea/`;
* generated `cmake-build-debug/`;
* other IDE-generated build directories.

---

## Affected Files

Directory/glob entries authorize files created inside the listed path family when required by the corresponding plan Step.

* [x] `PLAN.md` — update execution state during implementation
* [ ] `APP.md` — create once durable system architecture exists; maintain institutional system knowledge
* [x] `MODERN.md` — create once toolchain/technology choices are concrete; maintain technology policy
* [x] `CMakeLists.txt` — replace placeholder target with modular project targets
* [x] `CMakePresets.json` — Windows and later Linux configure/build/test/sanitizer presets
* [x] `vcpkg.json`
* [x] `vcpkg-configuration.json`
* [x] `.gitignore`
* [x] `cmake/CompilerOptions.cmake`
* [x] `cmake/Dependencies.cmake`
* [ ] `cmake/Packaging.cmake`
* [x] `headers/revdash/core/*.hpp`
* [x] `src/core/*.cpp`
* [ ] `headers/revdash/protocol/*.hpp`
* [ ] `src/protocol/*.cpp`
* [ ] `headers/revdash/drivers/*.hpp`
* [ ] `src/drivers/elm327/*.cpp`
* [ ] `src/drivers/synthetic/*.cpp`
* [ ] `src/drivers/playback/*.cpp`
* [ ] `src/drivers/socketcan/*.cpp` — Stage 10
* [ ] `headers/revdash/telemetry/*.hpp`
* [ ] `src/telemetry/*.cpp`
* [ ] `headers/revdash/diagnostics/*.hpp`
* [ ] `src/diagnostics/*.cpp`
* [ ] `headers/revdash/session/*.hpp`
* [ ] `src/session/*.cpp`
* [ ] `src/cli/main.cpp`
* [ ] `src/app/main.cpp`
* [ ] `src/ui/*.cpp`
* [ ] `headers/revdash/ui/*.hpp`
* [ ] `qml/*.qml`
* [ ] `qml/components/*.qml`
* [ ] `qml/workspaces/*.qml`
* [ ] `schemas/session-v1.schema.json`
* [ ] `tools/dtc_importer/*`
* [ ] `assets/dtc/*`
* [ ] `assets/licenses/*`
* [ ] `assets/icons/*`
* [x] `tests/unit/*`
* [x] `tests/integration/*`
* [ ] `tests/fixtures/*`
* [ ] `tests/ui/*`
* [ ] `tests/hardware/*`
* [ ] `packaging/windows/*`
* [ ] `packaging/linux/*`
* [ ] `.github/workflows/windows.yml`
* [ ] `.github/workflows/linux.yml`
* [ ] `docs/session-format.md`
* [ ] `docs/diagnostic-rules.md`
* [ ] `docs/hardware-validation.md`
* [ ] `docs/licensing.md`
* [ ] `src/main.cpp` — remove after dedicated entry points exist

---

# Execution Sequence

# Feature 1: Stage 1 — Build Foundation and Core Contracts

## Step 1.1: Establish reproducible targets, dependencies, and project policy

* [x] Replace placeholder CMake configuration with C++20 project structure.
* [x] Set CMake minimum to 3.30.
* [x] Define:

  * `revdash_core`;
  * `revdash_cli`;
  * `revdash_app`;
  * `revdash_dtc_importer`;
  * `revdash_unit_tests`;
  * `revdash_integration_tests`;
  * later `revdash_ui_tests`.
* [x] Add Windows presets:

  * `windows-msvc`;
  * `windows-msvc-debug`;
  * `windows-msvc-release`;
  * `windows-msvc-asan`.
* [x] Reserve Linux presets for Stage 10.
* [x] Create pinned vcpkg manifest containing:

  * Boost.Asio;
  * Boost.Lockfree;
  * tl-expected;
  * nlohmann-json;
  * SQLite3;
  * spdlog;
  * CLI11;
  * Catch2.
* [x] Locate Qt separately through `Qt6_ROOT`.
* [x] Enforce that `revdash_core` cannot link against Qt.
* [x] Enable MSVC `/W4`, standards conformance, and warnings-as-errors for RevDash targets.
* [x] Configure Windows AddressSanitizer as a separate supported preset.
* [x] Scaffold CTest/Catch2 discovery.
* [x] Create `.gitignore` rules for build output, generated package staging, development sessions, and licensed raw datasets.
* [x] Create `MODERN.md` with the actual adopted toolchain, dependency baseline, Qt licensing boundary, sanitizer policy, and dependency-selection rules.

### Tests

* CMake configure succeeds from a clean build tree.
* CTest discovers the test runner.
* Core target builds without Qt linkage.
* Dependency manifest resolves reproducibly.
* ASan preset configures successfully on supported MSVC environments.

### Verification

```text
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug -N
```

---

## Step 1.2: Define canonical domain models and time contracts

* [x] Define `ErrorDomain`, `Error`, and stable application error codes.
* [x] Define `Result<T>` using `tl::expected<T, Error>`.
* [x] Error values contain:

  * stable code;
  * domain;
  * user-safe message;
  * optional diagnostic context;
  * retryable flag.
* [x] Define `ConnectionState`:

  * `Disconnected`;
  * `Connecting`;
  * `Initializing`;
  * `Ready`;
  * `Reconnecting`;
  * `Disconnecting`;
  * `Faulted`.
* [x] Define fixed-capacity canonical:

  * `ObdRequest`;
  * `ObdMessage`;
  * `RawTransportFrame`;
  * `EcuAddress`.
* [x] Preserve:

  * source/ECU identity;
  * monotonic timestamp;
  * optional UTC timestamp;
  * logical payload length.
* [x] Enforce application-level transport payload ceiling of 4095 bytes with `Protocol.PayloadTooLarge`.
* [x] Define:

  * `MetricId`;
  * `TelemetrySample`;
  * `SampleQuality`;
  * `DtcRecord`;
  * `FreezeFrame`;
  * `EcuMetadata`;
  * `DiagnosticFinding`;
  * `Severity`;
  * `TelemetrySnapshot`.
* [x] Initial metric catalog includes:

  * RPM;
  * speed;
  * throttle;
  * MAP;
  * MAF;
  * calculated load;
  * timing;
  * coolant;
  * STFT/LTFT;
  * ambient temperature;
  * fuel level;
  * control-module voltage;
  * supported O2 channels.
* [x] Add monotonic/system clock abstraction and deterministic manual clock for tests where needed.

### Tests

* Error/result success and failure behavior.
* Stable error-code mapping.
* Payload-cap rejection at 4096+ bytes.
* Boundary payload acceptance.
* `EcuAddress` equality/hash semantics.
* Metric catalog uniqueness.
* Timestamp semantics.
* Manual clock advancement.
* Sample quality construction and transitions.

### Verification

```text
ctest --preset windows-msvc-debug -R core_types --output-on-failure
```

---

## Step 1.3: Define asynchronous `IDataSource` contract

* [x] Define pure virtual `IDataSource`.
* [x] Contract covers:

  * connect;
  * disconnect;
  * reconnect;
  * transmit;
  * connection state;
  * canonical message subscription.
* [x] Lifecycle calls must not block the caller on device I/O.
* [x] Source transport work occurs on a source-owned serialized worker/executor.
* [x] Source completion/message callbacks execute on the source worker unless explicitly marshalled elsewhere.
* [x] Define `DataSourceConfig` variant for:

  * Serial;
  * Synthetic;
  * Playback;
  * SocketCAN.
* [x] Disconnect is idempotent.
* [x] In-flight operations terminate with `Core.Cancelled` during intentional shutdown/source replacement.
* [x] Preserve connection configuration required for reconnect.
* [x] Add move-only RAII subscription token with thread-safe unregistration.
* [x] Reject new transmissions when source state does not permit them.

### Tests

* Connect/disconnect lifecycle state transitions.
* Double disconnect.
* Cancellation of in-flight requests.
* Transmit rejection outside `Ready`.
* Callback thread ownership.
* Subscription destruction/unregistration.
* Reconnect configuration preservation.
* Source destruction with pending callbacks.
* No callback after subscriber lifetime ends.

### Verification

```text
ctest --preset windows-msvc-debug -R data_source_contract --output-on-failure
```

---

## Step 1.4: Implement bounded pipelines and coherent telemetry store

* [ ] Wrap `boost::lockfree::spsc_queue` in project-owned bounded SPSC abstraction.
* [ ] Configure default capacities:

  * 1024 source → engine;
  * 2048 engine → recorder.
* [ ] Implement non-blocking overflow/drop counters.
* [ ] Never silently overwrite queue contents.
* [ ] Define fixed-size/fixed-capacity hot-path packet representations where practical.
* [ ] Implement coherent latest telemetry store keyed by `MetricId`.
* [ ] Use synchronization that guarantees value, timestamp, and quality originate from the same update.
* [ ] Provide efficient complete `TelemetrySnapshot` reads.
* [ ] Avoid steady-state heap allocation inside SPSC enqueue/dequeue operations.
* [ ] Instrument store lock contention rather than prematurely replacing correct synchronization with custom lock-free code.
* [ ] Create initial `APP.md` after these ownership/data-flow contracts are implemented.
* [ ] Record in `APP.md`:

  * core/UI boundary;
  * source ownership;
  * worker responsibilities;
  * queue topology;
  * canonical data flow;
  * source switching rules.

### Tests

* FIFO ordering.
* Capacity boundary.
* Overflow/drop accounting.
* Producer/consumer stress test.
* No data corruption under sustained SPSC use.
* Coherent snapshot reads during concurrent updates.
* Snapshot contains matching value/timestamp/quality tuples.
* Queue teardown under active producer/consumer.
* No steady-state queue allocation after initialization.

### Verification

```text
ctest --preset windows-msvc-debug -R "spsc|latest_store" --output-on-failure
```

---

# Feature 2: Stage 2 — SAE J1979 Protocol and Telemetry Decoding

## Step 2.1: Build table-driven Mode 01 PID catalog

* [ ] Define descriptor table containing:

  * PID;
  * expected response length;
  * canonical SI unit;
  * scheduler priority;
  * decoder;
  * valid bounds;
  * stale policy.
* [ ] Decode:

  * RPM;
  * speed;
  * coolant;
  * calculated load;
  * throttle;
  * fuel trims;
  * MAP;
  * MAF;
  * timing advance;
  * ambient temperature;
  * fuel level;
  * module voltage.
* [ ] Decode Mode 01 supported-PID bitmaps:

  * `00`;
  * `20`;
  * `40`;
  * later ranges when supported by catalog.
* [ ] Build dynamic query filter from supported bitmap results.
* [ ] Normalize supported narrowband/wideband O2 data into typed metrics without pretending unsupported sensor representations are equivalent.
* [ ] Reject:

  * truncated payloads;
  * incorrect service responses;
  * unexpected PID;
  * negative responses;
  * invalid lengths;
  * impossible decoded values.

### Tests

* Published/known vectors for every decoder.
* Minimum/maximum encoded values.
* Supported bitmap interpretation.
* Unsupported PID filtering.
* Truncated responses.
* Wrong response mode.
* Wrong PID echo.
* `0x7F` negative response.
* Invalid/out-of-range result classification.
* Multiple O2 sensor layouts.

### Verification

```text
ctest --preset windows-msvc-debug -R mode01 --output-on-failure
```

---

## Step 2.2: Implement diagnostic services and DTC decoding

* [ ] Decode Mode 03 stored DTC responses.
* [ ] Decode Mode 07 pending DTC responses.
* [ ] Preserve ECU source for every DTC.
* [ ] Filter `0000` padding.
* [ ] Deduplicate only equivalent records from the same ECU/category while preserving multi-ECU provenance.
* [ ] Implement SAE DTC bitfield conversion to `P/C/B/U` codes.
* [ ] Implement Mode 02 frame-zero freeze-frame extraction.
* [ ] Reuse Mode 01 PID decoders for supported freeze-frame PIDs.
* [ ] Format Mode 04 clear request.
* [ ] Parse positive response `0x44`.
* [ ] Parse and classify negative responses.
* [ ] Implement Mode 09:

  * VIN;
  * calibration ID;
  * CVN.
* [ ] Validate VIN length/content.
* [ ] Preserve ECU source for metadata.
* [ ] Keep J1979 multi-record/message sequencing conceptually separate from underlying ISO-TP transport segmentation.

### Tests

* Stored DTC decoding.
* Pending DTC decoding.
* `P/C/B/U` conversion vectors.
* Padding filtering.
* Multi-ECU duplicate behavior.
* Freeze-frame decoding.
* Missing/unsupported freeze-frame PIDs.
* Mode 04 positive/negative responses.
* Valid/invalid VIN.
* Multi-record CALID/CVN.
* Malformed Mode 09 payloads.

### Verification

```text
ctest --preset windows-msvc-debug -R "dtc_codec|mode02|mode04|mode09" --output-on-failure
```

---

## Step 2.3: Implement platform-neutral raw ISO-TP trace codec

This codec is a protocol utility, **not** the normal ELM327 or Linux production transport.

* [ ] Decode/represent:

  * SF;
  * FF;
  * CF;
  * FC.
* [ ] Support 11-bit and 29-bit CAN identifiers in trace metadata.
* [ ] Implement deterministic reassembly keyed by source/destination addressing.
* [ ] Track:

  * expected sequence;
  * rollover;
  * declared length;
  * timeout;
  * malformed sequences.
* [ ] Reject payloads exceeding application limits.
* [ ] Implement fixture helpers for producing known ISO-TP frame sequences.
* [ ] Keep codec independent of Windows/Linux APIs.
* [ ] Explicitly document:

  * ELM327 owns normal transport flow control;
  * Linux `CAN_ISOTP` owns production segmentation/reassembly.

### Tests

* Single-frame payload.
* Multi-frame payload.
* Sequence rollover.
* Missing CF.
* Duplicate CF.
* Incorrect sequence.
* Timeout.
* Invalid FF length.
* FC parsing.
* 11-bit and 29-bit trace metadata.
* Maximum accepted payload.
* Oversized payload rejection.

### Verification

```text
ctest --preset windows-msvc-debug -R isotp_trace --output-on-failure
```

---

## Step 2.4: Add metric aggregation and quality tracking

* [ ] Implement rolling:

  * min;
  * max;
  * mean;
  * median where needed by rules;
  * time-window views.
* [ ] Use monotonic time.
* [ ] Implement:

  * `Valid`;
  * `Stale`;
  * `Unsupported`;
  * `Dropped`;
  * `Invalid`.
* [ ] Define per-metric stale thresholds.
* [ ] Reset rolling state after:

  * source switch;
  * playback seek;
  * engine epoch change.
* [ ] Prevent pre-reset samples from leaking into new diagnostic windows.

### Tests

* Rolling aggregation.
* Window expiry.
* Irregular sample timestamps.
* Stale classification.
* Unsupported metrics.
* Dropped/invalid propagation.
* Epoch reset.
* Seek reset.
* Median behavior with deterministic fixtures.

### Verification

```text
ctest --preset windows-msvc-debug -R metric_aggregator --output-on-failure
```

---

# Feature 3: Stage 3 — Synthetic Powertrain and Procedural Faults

## Step 3.1: Implement deterministic powertrain model

* [ ] Define `SimulationConfig`:

  * engine characteristics;
  * vehicle inertia;
  * drag;
  * rolling resistance;
  * wheel radius;
  * ambient temperature;
  * deterministic seed.
* [ ] Use fixed 10 ms physics timestep independent of UI frame rate.
* [ ] Model torque-balance RPM dynamics.
* [ ] Add PI idle control.
* [ ] Add redline limiter.
* [ ] Model basic drivetrain speed.
* [ ] Model MAP from throttle/load.
* [ ] Model MAF from airflow/volumetric efficiency.
* [ ] Model thermal behavior:

  * cold start;
  * combustion heating;
  * thermostat behavior;
  * fan hysteresis.
* [ ] Maintain deterministic output for identical configuration/input sequence.

### Tests

* Same seed/input produces identical output.
* Fixed-step independence from caller update frequency.
* Stable idle.
* RPM response to throttle.
* Redline enforcement.
* Vehicle acceleration/deceleration.
* MAP/MAF monotonic sanity.
* Cold-start warmup.
* Thermostat/fan hysteresis.

### Verification

```text
ctest --preset windows-msvc-debug -R synthetic_physics --output-on-failure
```

---

## Step 3.2: Add deterministic fault and noise injection

* [ ] Misfire scenario:

  * torque drop;
  * RPM instability;
  * freeze-frame capture;
  * configurable `P0300`–`P0304`.
* [ ] Vacuum leak scenario:

  * positive fuel trims at idle;
  * load-dependent convergence;
  * `P0171`.
* [ ] Stuck-open thermostat scenario:

  * slow/failed warmup;
  * `P0128`.
* [ ] Configurable Gaussian sensor noise.
* [ ] Configurable packet dropout.
* [ ] Use deterministic PRNG.
* [ ] Keep true physical state separate from noisy sensor output.

### Tests

* Deterministic fault reproduction.
* Fault activation/deactivation.
* Expected DTC generation.
* Freeze-frame capture timing.
* Noise distribution within configured tolerance.
* Deterministic dropout sequence.
* Physical state remains unaffected by sensor noise.
* Fault reset behavior.

### Verification

```text
ctest --preset windows-msvc-debug -R synthetic_faults --output-on-failure
```

---

## Step 3.3: Expose simulation through `IDataSource`

* [ ] Implement `SyntheticDataSource`.
* [ ] Fulfil asynchronous lifecycle contract.
* [ ] Simulate configurable latency.
* [ ] Accept canonical OBD requests.
* [ ] Produce canonical logical OBD responses.
* [ ] Support Modes:

  * 01;
  * 02;
  * 03;
  * 04;
  * 07;
  * 9.
* [ ] Maintain virtual ECU identity/state.
* [ ] Expose safe simulation controls:

  * start/stop;
  * throttle;
  * ambient temperature;
  * fault injection;
  * noise;
  * packet loss.

### Tests

* `IDataSource` contract suite passes against synthetic implementation.
* All supported modes.
* Unsupported PID/service response.
* Simulated latency.
* Cancellation.
* Virtual DTC clearing.
* Deterministic source reset.
* Multiple virtual ECU metadata records when configured.

### Verification

```text
ctest --preset windows-msvc-debug -R synthetic_source --output-on-failure
```

---

# Feature 4: Stage 4 — ELM327, Scheduling, and Streaming Engine

## Step 4.1: Build cross-platform serial transport abstraction

* [ ] Implement `ISerialTransport` using Boost.Asio.
* [ ] Support asynchronous:

  * open;
  * close;
  * read;
  * write;
  * cancellation.
* [ ] Implement Windows COM enumeration using SetupAPI.
* [ ] Capture when available:

  * COM identifier;
  * friendly name;
  * VID/PID;
  * device description;
  * Bluetooth SPP hints.
* [ ] Treat USB serial and Bluetooth Classic SPP as serial transports after enumeration.
* [ ] Support common baud rates:

  * 9600;
  * 38400;
  * 115200.
* [ ] Driver reports successful baud/configuration for higher-level persistence.
* [ ] Handle unplug/device removal as asynchronous transport failure.

### Tests

Use fake serial transport plus platform-independent parser tests for:

* open/close.
* partial reads.
* partial writes.
* cancellation.
* disconnect while read pending.
* invalid COM name.
* port disappearance.
* baud configuration.
* enumeration metadata normalization.

### Verification

```text
ctest --preset windows-msvc-debug -R serial_transport --output-on-failure
```

---

## Step 4.2: Implement ELM327 driver and prompt synchronizer

### Initialization

Implement baseline initialization:

```text
ATZ
ATE0
ATL0
ATS0
ATH1
ATCAF1
ATSP0
```

* [ ] Wait for reset banner/prompt after `ATZ`.
* [ ] Preserve headers with `ATH1`.
* [ ] Keep CAN auto-formatting enabled with `ATCAF1`.
* [ ] Probe adapter identity/capabilities where useful using safe commands such as `ATI`.
* [ ] Obtain detected protocol information after successful vehicle communication.
* [ ] Treat optional unsupported commands from imperfect clones separately from failure of required baseline behavior.
* [ ] Do not disable headers merely to simplify parsing.

### Command Synchronization

* [ ] Single active ELM command.
* [ ] Parse arbitrary serial chunks.
* [ ] Handle:

  * CR/LF;
  * optional echoes;
  * prompt `>`;
  * status lines;
  * blank lines.
* [ ] Classify:

  * `NO DATA`;
  * `SEARCHING...`;
  * `BUS INIT: ERROR`;
  * `UNABLE TO CONNECT`;
  * `STOPPED`;
  * `?`;
  * other known adapter status/error responses.
* [ ] Use:

  * bounded reset timeout;
  * adaptive command timeout;
  * bounded initialization retries.
* [ ] Track:

  * current protocol;
  * adapter identity;
  * RTT;
  * EWMA RTT;
  * timeout count;
  * malformed response count;
  * reconnect count.

### OBD Normalization

* [ ] Parse header/address information.
* [ ] Preserve distinct ECU responses.
* [ ] Support CAN 11-bit and 29-bit response identifiers.
* [ ] Handle header forms produced by supported legacy OBD protocols where feasible.
* [ ] Normalize ELM response data into `ObdMessage`.
* [ ] Retain diagnostic raw lines for trace/debug events.
* [ ] When ELM output exposes raw ISO-TP-framed CAN content, receive-side normalization may reuse the raw trace codec.
* [ ] Never send custom FC frames in normal generic OBD operation.
* [ ] Implement clean cancellation and transition to reconnect/fault states after hot unplug.

### Tests

Transcript fixtures must include:

* genuine-style reset sequence.
* common clone banners.
* command echo enabled unexpectedly.
* arbitrary serial chunk boundaries.
* `>` split across chunks.
* multiline CAN response.
* two responding ECUs.
* 11-bit CAN headers.
* 29-bit CAN headers.
* legacy protocol headers where supported.
* Mode 09 multi-frame response.
* `SEARCHING...` followed by valid response.
* `NO DATA`.
* `UNABLE TO CONNECT`.
* malformed hex.
* unsupported optional AT command.
* timeout/retry.
* hot unplug/cancellation.
* confirmation that ECU identity survives normalization.

### Verification

```text
ctest --preset windows-msvc-debug -R elm327 --output-on-failure
```

---

## Step 4.3: Implement adaptive PID scheduler

* [ ] Build desired polling tiers rather than assuming fixed adapter throughput.
* [ ] High priority:

  * RPM;
  * speed;
  * throttle.
* [ ] Medium priority:

  * MAP;
  * MAF;
  * load;
  * timing;
  * active O2 channels.
* [ ] Low priority:

  * coolant;
  * trims;
  * ambient;
  * fuel level;
  * voltage.
* [ ] Use supported-PID filter before scheduling.
* [ ] Enforce ELM single-flight request behavior.
* [ ] Use fair deadline-based ordering inside priority tiers.
* [ ] Estimate sustainable dispatch budget from measured RTT.
* [ ] Target maximum useful throughput while keeping estimated adapter/bus utilization below 80%.
* [ ] Degrade low-priority polling first during congestion.
* [ ] Degrade medium priority only when required.
* [ ] Ramp recovery gradually after congestion.
* [ ] Pause/drain streaming requests for diagnostic operations:

  * 02;
  * 03;
  * 04;
  * 07;
  * 9.
* [ ] Prevent diagnostic starvation.

### Tests

* Fairness.
* Unsupported PIDs are never queried.
* High-priority preference.
* Single-flight enforcement.
* Slow-adapter adaptation.
* Fast-adapter recovery.
* Queue congestion.
* Low-tier degradation before higher tiers.
* Diagnostic pause/drain/resume.
* Timeout handling.
* No starvation during sustained streaming.

### Verification

```text
ctest --preset windows-msvc-debug -R pid_scheduler --output-on-failure
```

---

## Step 4.4: Assemble `EngineService` and explicit worker ownership

* [ ] `EngineService` owns:

  * active source;
  * scheduler;
  * decoder pipeline;
  * telemetry store;
  * diagnostic evaluator;
  * session recorder coordination.
* [ ] Active source owns its own transport worker/executor.
* [ ] Engine owns processing `std::jthread`.
* [ ] Recorder owns recording `std::jthread`.
* [ ] Source callbacks enqueue canonical packets into source → engine SPSC.
* [ ] Engine worker:

  * consumes source messages;
  * decodes;
  * updates telemetry;
  * evaluates rules;
  * dispatches recorder records.
* [ ] Implement safe UI/CLI → engine command queue for:

  * connect;
  * disconnect;
  * scan;
  * identify;
  * clear preparation;
  * clear confirmation;
  * recording;
  * playback;
  * simulation controls.
* [ ] Implement thread-safe event publication.
* [ ] Implement exponential reconnect:

  * 0.5s;
  * 1s;
  * 2s;
  * 5s;
  * maximum five automatic attempts.
* [ ] Increment engine epoch on:

  * source replacement;
  * playback seek;
  * state reset requiring telemetry invalidation.
* [ ] Drain stale pipeline work during epoch transition.
* [ ] Reject late responses belonging to an obsolete epoch.
* [ ] Update `APP.md` with final worker and ownership architecture.

### Tests

* Full source → decode → store path.
* Commands from another thread.
* Reconnect schedule.
* Reconnect cancellation.
* Source switch.
* Epoch invalidation.
* Late packet rejection.
* Queue drain.
* Shutdown with pending source request.
* Recorder worker shutdown.
* No Qt dependencies in engine.
* Concurrent snapshot reads while telemetry flows.

### Verification

```text
ctest --preset windows-msvc-debug -R engine_pipeline --output-on-failure
```

---

# Feature 5: Stage 5 — Diagnostic Rules, DTC Database, and Safe Commands

## Step 5.1: Implement rolling diagnostic evaluation

All findings are explicitly heuristic/advisory rather than definitive mechanical diagnoses.

* [ ] Implement rule applicability gates based on:

  * required PIDs;
  * sample quality;
  * sample freshness;
  * warmup/state conditions.
* [ ] Implement timestamped rolling-window evaluator.
* [ ] Reset incomplete windows on stale/invalid data.
* [ ] Implement vacuum-leak heuristic using:

  * idle-state gate;
  * load gate;
  * sustained positive LTFT;
  * convergence under increased load.
* [ ] Implement catalyst-efficiency heuristic only where supported O2 sensor topology/data makes the calculation meaningful.
* [ ] Require suitable engine temperature and steady operating window.
* [ ] Implement stuck-open thermostat advisory from cold-start/warmup behavior.
* [ ] Implement conservative charging-voltage anomaly rule rather than assuming every vehicle uses fixed alternator voltage behavior.
* [ ] Define rule thresholds in centralized configuration/constants with explanatory documentation.
* [ ] Implement:

  * finding deduplication;
  * first/last seen;
  * active/resolved state;
  * evidence snapshot;
  * rule identifier/version.
* [ ] Require stable clear condition before automatically resolving findings.
* [ ] Document limitations in `docs/diagnostic-rules.md`.

### Tests

* Every rule positive fixture.
* Every rule negative fixture.
* Boundary threshold behavior.
* Missing PID.
* Unsupported PID.
* Stale samples.
* Insufficient window duration.
* Applicability gating.
* Finding deduplication.
* Evidence capture.
* Resolution timing.
* Epoch reset.
* Smart-charging-like voltage fixture does not produce unjustified critical finding.

### Verification

```text
ctest --preset windows-msvc-debug -R diagnostic_rules --output-on-failure
```

---

## Step 5.2: Build offline DTC database pipeline

* [ ] Define expected external CSV fields:

  * `code`;
  * `description`;
  * `severity`;
  * `likely_failure_points`;
  * `source_version`.
* [ ] Validate:

  * DTC format;
  * severity enum;
  * UTF-8;
  * required fields;
  * duplicate/conflicting rows;
  * dataset version.
* [ ] Generate versioned SQLite database.
* [ ] Add indices appropriate for code and search.
* [ ] Implement `revdash_dtc_importer`.
* [ ] Implement read-only runtime lookup:

  * exact code;
  * prefix;
  * normalized keyword search;
  * unknown fallback.
* [ ] Keep small legal fixture CSV/SQLite under tests.
* [ ] Do not require production licensed data for unit/integration tests.
* [ ] Release packaging must require explicitly supplied/generated production database.
* [ ] Never package the fixture database as production data.
* [ ] Do not commit production generated DB unless its license explicitly permits this.

### Tests

* Valid dataset import.
* Invalid DTC code.
* Invalid UTF-8.
* Duplicate conflict.
* Missing field.
* Unsupported schema/source version.
* Exact lookup.
* Prefix search.
* Keyword search.
* Unknown code.
* Read-only runtime behavior.
* Fixture and production paths cannot be confused by release configuration.

### Verification

```text
ctest --preset windows-msvc-debug -R dtc_database --output-on-failure
```

---

## Step 5.3: Implement scans, metadata, and guarded Mode 04 workflow

### Scan

* [ ] Pause streaming scheduler.
* [ ] Query Mode 03 + Mode 07.
* [ ] Enrich DTCs from local DB.
* [ ] Query available Mode 02 freeze frame.
* [ ] Resume scheduler.
* [ ] Preserve ECU source.

### Metadata

* [ ] Query Mode 09 VIN/CALID/CVN.
* [ ] Preserve per-ECU provenance.
* [ ] Tolerate partial support.

### Mode 04 Preparation

`prepareClearDtc()` succeeds only when:

* [ ] active source is physical;
* [ ] source is `Ready`;
* [ ] no incompatible operation is active;
* [ ] engine epoch remains stable;
* [ ] vehicle speed sample exists;
* [ ] speed quality is `Valid`;
* [ ] speed sample is within defined freshness threshold;
* [ ] speed ≤ 0.5 km/h.

Reject:

* playback;
* synthetic user-facing clear;
* stale speed;
* unsupported speed;
* invalid/dropped speed;
* moving vehicle.

Before producing token:

* [ ] capture DTC/freeze-frame/evidence snapshot;
* [ ] record that Mode 04 may erase emissions diagnostic information and reset readiness-related state;
* [ ] generate single-use random confirmation token;
* [ ] bind token to:

  * source identity;
  * engine epoch;
  * preparation snapshot;
  * expiration;
  * vehicle identity when available.
* [ ] expire token after 30 seconds.
* [ ] invalidate token after any source/epoch/precondition change.

### Mode 04 Confirmation

* [ ] Revalidate safety state immediately before transmission.
* [ ] Consume token atomically.
* [ ] Send Mode 04.
* [ ] Validate positive/negative response.
* [ ] Wait a bounded post-clear settling delay.
* [ ] Trigger automatic DTC rescan.
* [ ] Record complete structured audit result.

### Tests

* Stored/pending scan.
* Multi-ECU scan.
* Partial Mode support.
* Metadata enrichment.
* Valid stationary clear preparation.
* Moving rejection.
* Stale-speed rejection.
* Unsupported-speed rejection.
* Playback rejection.
* Synthetic rejection.
* Token expiration.
* Token reuse.
* Token from old epoch.
* Source switch after preparation.
* Vehicle identity change when detectable.
* Positive clear response.
* Negative response.
* Timeout.
* Post-clear rescan.
* Audit record contents.

### Verification

```text
ctest --preset windows-msvc-debug -R diagnostic_service --output-on-failure
```

---

# Feature 6: Stage 6 — Session Recording, Playback, and Export

## Step 6.1: Implement versioned JSONL recording

* [ ] Define Schema v1 record types:

  * header;
  * OBD message;
  * telemetry;
  * DTC;
  * diagnostic finding;
  * Mode 04 audit;
  * ECU metadata;
  * data-loss marker;
  * footer.
* [ ] Header includes:

  * UUID;
  * application version;
  * schema version;
  * UTC start;
  * active source type;
  * adapter/protocol metadata when known;
  * vehicle metadata;
  * simulation configuration/seed when applicable.
* [ ] Use integer `elapsed_us`.
* [ ] Raw logical payloads use uppercase hex.
* [ ] Preserve ECU identity.
* [ ] Use streaming/preallocated serializer path.
* [ ] Benchmark steady telemetry serialization for unnecessary allocations.
* [ ] Record to `.partial`.
* [ ] On successful completion:

  * write footer;
  * flush;
  * close;
  * atomically rename to `.jsonl`.
* [ ] Leave recoverable `.partial` after abnormal termination.
* [ ] Record explicit data-loss marker if recorder queue drops records.
* [ ] Document format in `docs/session-format.md`.

### Tests

* Every record type round trip.
* Schema/version field.
* Unicode metadata.
* Stable integer timestamp formatting.
* Hex encoding.
* ECU identity persistence.
* Successful finalization.
* Interrupted `.partial`.
* Queue overflow/data-loss marker.
* Invalid output path.
* Disk write failure simulation.
* Footer statistics.
* Deterministic serialization of canonical fixtures.

### Verification

```text
ctest --preset windows-msvc-debug -R session_recorder --output-on-failure
```

---

## Step 6.2: Implement deterministic playback source

* [ ] Implement `PlaybackDataSource`.
* [ ] Stream recorded `ObdMessage` records through normal decode/rule pipelines.
* [ ] Controls:

  * Play;
  * Pause;
  * Step;
  * Stop;
  * Seek;
  * 0.5×;
  * 1×;
  * 2×;
  * 5×.
* [ ] Create `.ridx` sidecar.
* [ ] Add approximately 1-second seek checkpoints.
* [ ] Index contains source file fingerprint/schema information so stale index files are rejected/rebuilt.
* [ ] On seek:

  * increment engine epoch;
  * reset rolling state;
  * locate target minus required diagnostic-rule warmup window;
  * silently fast-forward to target;
  * publish rebuilt state.
* [ ] Derive warmup duration from configured maximum active rule window rather than hardcoding it.
* [ ] Re-evaluate current telemetry/findings using current rule implementation.
* [ ] Expose historical recorded findings/audits separately.
* [ ] Validate:

  * schema compatibility;
  * monotonic elapsed timestamps;
  * record structure;
  * hex payloads;
  * required fields.
* [ ] Playback cannot invoke physical Mode 04.

### Tests

* 1× deterministic timing using manual clock.
* Pause/resume.
* Step.
* Each speed multiplier.
* Seek.
* Stale index rebuild.
* Rule-state warmup.
* Corrupt line.
* Unsupported schema.
* Non-monotonic time.
* Truncated session.
* Historical vs re-evaluated finding separation.
* Mode 04 unavailable in playback.

### Verification

```text
ctest --preset windows-msvc-debug -R session_playback --output-on-failure
```

---

## Step 6.3: Implement automotive CSV export

* [ ] Implement configurable 10 Hz telemetry timeline.
* [ ] Use sample-and-hold only while previous sample remains within that metric's valid hold/stale interval.
* [ ] Output empty/missing value rather than holding stale data indefinitely.
* [ ] Use locale-independent decimal formatting.
* [ ] Escape CSV correctly.
* [ ] Implement:

  * RevDash export;
  * MegaLogViewer-compatible preset;
  * TunerStudio-compatible preset.
* [ ] Validate external presets against currently documented import expectations before freezing headers.
* [ ] Apply Metric/Imperial conversion during export only.
* [ ] Tag units in headers/metadata.
* [ ] Export via temporary file followed by atomic replacement/rename.

### Tests

* Exact 10 Hz timeline.
* Sample-and-hold.
* Stale cutoff.
* Missing values.
* Unit conversion.
* Locale independence.
* CSV escaping.
* Golden output fixtures for each preset.
* Export failure.
* Atomic destination behavior.

### Verification

```text
ctest --preset windows-msvc-debug -R csv_export --output-on-failure
```

---

# Feature 7: Stage 7 — Headless CLI and Backend Acceptance

## Step 7.1: Build diagnostic CLI

* [ ] Implement CLI11 commands:

  * `sources`;
  * `live`;
  * `scan`;
  * `identify`;
  * `simulate`;
  * `record`;
  * `playback`;
  * `export`;
  * `clear`.
* [ ] Human-readable table output.
* [ ] Machine-readable JSON Lines output.
* [ ] Standard exit codes:

  * 0 success;
  * 2 usage;
  * 3 connection;
  * 4 protocol;
  * 5 safety rejection;
  * 6 I/O.
* [ ] Graceful console interruption.
* [ ] `clear` remains a same-process guarded interaction:

  1. prepare;
  2. display destructive-effects warning and token;
  3. require explicit acknowledgement;
  4. require token confirmation;
  5. execute confirmation while original engine/source context remains active.
* [ ] Do not weaken Mode 04 safeguards for CLI automation.

### Tests

* Argument parsing for every command.
* Exit-code mapping.
* JSONL output.
* Human tables.
* Simulated live/scan/identify flow.
* Record/playback/export.
* Invalid source.
* Connection failure.
* Ctrl+C shutdown path using testable signal abstraction.
* Clear rejection.
* Clear confirmation token mismatch.
* Successful simulated backend clear path at service-test level without bypassing physical-source production guard.

### Verification

```text
ctest --preset windows-msvc-debug -R cli --output-on-failure
```

---

## Step 7.2: Validate complete backend

* [ ] Execute synthetic end-to-end scenarios:

  * normal engine;
  * vacuum leak;
  * misfire;
  * thermostat;
  * noisy/dropout stream.
* [ ] Validate complete:
  source → scheduler → decoder → telemetry → rules → recorder.
* [ ] Profile steady telemetry pipeline over at least 100,000 frames.
* [ ] Confirm queue path does not allocate after initialization.
* [ ] Investigate any significant unexpected telemetry hot-path allocation rather than enforcing impossible zero-allocation behavior on unrelated diagnostic events.
* [ ] Validate 5× playback at equivalent 250 packets/sec acceptance load with no queue drops on reference development hardware.
* [ ] Validate bounded shutdown under active I/O.
* [ ] Run Windows ASan core/integration tests.

### Tests / Acceptance

* Deterministic expected metrics/DTCs/findings.
* Session replay reproduces expected state.
* No SPSC drops at acceptance workload.
* No ASan findings.
* Engine shutdown target ≤2 seconds under controlled test load.
* Memory remains bounded over sustained run.
* No deadlock during source reconnect/switch/shutdown.

### Verification

```text
ctest --preset windows-msvc-release -L backend_e2e --output-on-failure
cmake --build --preset windows-msvc-asan
ctest --preset windows-msvc-asan --output-on-failure
```

---

# Feature 8: Stage 8 — Qt 6/QML Desktop Interface

## Step 8.1: Build Qt application shell, adapters, and chart primitive

* [ ] Link UI only against required LGPL-compatible Qt modules.
* [ ] Do not introduce Qt Graphs by default.
* [ ] Implement:

  * `AppController`;
  * `TelemetryModel`;
  * `DtcModel`;
  * `FindingModel`;
  * `SessionModel`;
  * `SourceModel`.
* [ ] All `QObject` mutations remain on main thread.
* [ ] Engine commands remain asynchronous.
* [ ] UI polls/consumes immutable telemetry snapshots at approximately 20 Hz.
* [ ] Chart data batches update approximately 10 Hz.
* [ ] Implement custom `TelemetryChartItem` using Qt Quick scene graph.
* [ ] Bound chart history memory.
* [ ] Transfer chart data safely between GUI-side state and scene-graph rendering.
* [ ] Register QML module with `qt_add_qml_module`.
* [ ] Add:

  * dark/light theme;
  * scalable automotive layout;
  * Metric/Imperial presentation binding.
* [ ] Keep strings translation-ready.

### Tests

* Controller thread affinity.
* Model insert/update/reset.
* Engine event marshaling.
* No QObject mutation from engine/source workers.
* Chart history bounds.
* Chart accepts deterministic sample series.
* Unit conversion binding.
* Theme switching.
* QML component load smoke test.
* `qmllint`.

### Verification

```text
cmake --build --preset windows-msvc-debug --target revdash_app_qmllint
ctest --preset windows-msvc-debug -R ui_shell --output-on-failure
```

---

## Step 8.2: Implement Connect workspace

* [ ] Physical ELM327 USB/BT Classic source selection.
* [ ] Synthetic source selection.
* [ ] COM dropdown.
* [ ] Refresh.
* [ ] Baud selector.
* [ ] Connection status.
* [ ] Adapter/protocol identity.
* [ ] RTT/EWMA.
* [ ] retry/error counters.
* [ ] Simulation presets/seed before connection.
* [ ] Disable incompatible source operations during guarded clear confirmation.
* [ ] Present actionable connection errors.

### Tests

* Source enumeration model.
* Port refresh.
* Connect/disconnect state UI.
* Failed connection.
* Reconnect state.
* Simulation configuration.
* Source switch guard during clear.
* Metric/imperial setting does not affect core source configuration.

### Verification

```text
ctest --preset windows-msvc-debug -R connect_workspace --output-on-failure
```

---

## Step 8.3: Implement Live Dashboard workspace

* [ ] Primary telemetry:

  * RPM;
  * speed;
  * throttle;
  * coolant;
  * load;
  * MAP;
  * MAF;
  * trims;
  * voltage.
* [ ] Rolling charts:

  * 10s;
  * 30s;
  * 120s.
* [ ] Telemetry-health display:

  * actual poll rate;
  * RTT;
  * sample age;
  * queue drops;
  * unsupported/stale indicators.
* [ ] Apply unit conversion only in presentation layer.
* [ ] Clearly distinguish stale/unsupported metrics from numeric zero.

### Tests

* Snapshot binding.
* Stale state.
* Unsupported state.
* Chart range switching.
* Unit conversion.
* Drop-counter display.
* No unbounded graph history.

### Verification

```text
ctest --preset windows-msvc-debug -R dashboard_workspace --output-on-failure
```

---

## Step 8.4: Implement Diagnostics workspace

* [ ] Stored/pending DTC groups.
* [ ] ECU source.
* [ ] DTC descriptions.
* [ ] severity/advisory presentation.
* [ ] Freeze-frame inspector.
* [ ] Heuristic finding panel:

  * rule;
  * status;
  * evidence;
  * limitations.
* [ ] Bounded raw diagnostic terminal:

  * pause;
  * copy;
  * hex filter.
* [ ] Guarded Mode 04 dialog:

  * precondition status;
  * data/readiness-loss warning;
  * confirmation token;
  * countdown;
  * result;
  * automatic rescan result.

### Tests

* DTC grouping.
* Multi-ECU source display.
* Freeze-frame display.
* Finding evidence.
* Raw terminal bounds.
* Clear button disabled when unsafe.
* Token expiry UI.
* Clear rejection.
* Successful confirmation flow using mocked engine service.

### Verification

```text
ctest --preset windows-msvc-debug -R diagnostics_workspace --output-on-failure
```

---

## Step 8.5: Implement Simulator workspace

* [ ] Ignition/start controls.
* [ ] Throttle.
* [ ] Ambient temperature.
* [ ] Fault injection.
* [ ] Noise/dropout controls.
* [ ] Display:

  * true physical state;
  * noisy OBD-observed state.
* [ ] Disable simulator controls when a physical source is active.

### Tests

* Simulation command dispatch.
* Fault toggles.
* Seed handling.
* True/noisy state distinction.
* Physical-source lockout.

### Verification

```text
ctest --preset windows-msvc-debug -R simulator_workspace --output-on-failure
```

---

## Step 8.6: Implement Sessions and Settings/DTC Lookup workspaces

### Sessions

* [ ] Session list.
* [ ] Metadata.
* [ ] timestamps.
* [ ] source/vehicle.
* [ ] DTC count.
* [ ] file size.
* [ ] recoverable `.partial` indication.
* [ ] Playback controls.
* [ ] Seek/scrub.
* [ ] speed selection.
* [ ] CSV export.

### Settings & DTC Lookup

* [ ] Metric/Imperial.
* [ ] theme.
* [ ] default session/export paths.
* [ ] preferred connection parameters.
* [ ] DTC exact lookup.
* [ ] keyword search.
* [ ] severity/likely-point display when supplied by licensed data.
* [ ] Use `QSettings` for preferences.
* [ ] Resolve files through appropriate `QStandardPaths` locations.
* [ ] Never write user sessions/configuration into installation directory.

### Tests

* Session discovery.
* `.partial` state.
* Playback controls.
* Export dialog.
* Settings round trip.
* Invalid stored path recovery.
* DTC lookup.
* Missing production DB user experience.
* Unit/theme persistence.

### Verification

```text
ctest --preset windows-msvc-debug -R "sessions_workspace|settings_workspace" --output-on-failure
```

---

## Step 8.7: Validate UI responsiveness and end-to-end navigation

* [ ] Run full-throughput synthetic telemetry with recording active.
* [ ] Navigate repeatedly through all six workspaces.
* [ ] Instrument:

  * engine snapshot age;
  * controller processing;
  * chart update work;
  * visible UI latency.
* [ ] Acceptance targets under defined reference workload:

  * p95 telemetry-to-UI latency <100 ms;
  * p95 main-thread update work <8 ms;
  * p99 main-thread update work <16.7 ms;
  * no sustained UI stall >250 ms.
* [ ] Validate:

  * disconnects;
  * malformed packets;
  * missing DTC DB;
  * recorder errors;
  * export failures;
  * source reconnect;
  * playback seek.
* [ ] Do not fail acceptance because of an isolated OS scheduler outlier if percentile/budget criteria remain satisfied.

### Tests / Acceptance

* Automated navigation smoke.
* Model stress fixtures.
* Long chart history.
* Source disconnect while changing workspace.
* Recording while navigating.
* Measured latency statistics against thresholds.

### Verification

```text
ctest --preset windows-msvc-release -L ui_e2e --output-on-failure
```

---

# Feature 9: Stage 9 — Windows v1 Packaging and Hardware Validation

## Step 9.1: Produce Windows MSI

* [ ] Build Release with MSVC 2022 x64.
* [ ] Use pinned Qt 6.11.2 baseline unless `MODERN.md`/plan is explicitly revised.
* [ ] Run Qt deployment tooling to stage required dynamic Qt runtime components.
* [ ] Deploy required MSVC runtime app-locally with the application package rather than assuming a CPack MSI can act as an external bootstrapper.
* [ ] Configure CPack WiX 4.
* [ ] Define stable MSI UpgradeCode.
* [ ] Package:

  * application;
  * Qt runtime;
  * required vcpkg runtime DLLs;
  * production DTC DB;
  * icons;
  * license/notice files.
* [ ] Install application binaries into read-only program location.
* [ ] Store configuration/sessions under user-writable application data paths.
* [ ] Add Start Menu shortcut.
* [ ] Include version metadata.
* [ ] No code signing in v1.
* [ ] Release package must fail if production DTC DB is absent.
* [ ] Do not fall back to fixture DB.
* [ ] Generate/verify third-party license inventory.
* [ ] Include applicable Qt open-source license/source-availability notices for the exact distributed Qt build.
* [ ] Keep GPL-only Qt modules out unless project licensing policy explicitly changes.
* [ ] Document packaging licensing assumptions in `docs/licensing.md`.

### Tests

* Clean staging directory.
* No debug DLLs.
* No test DTC fixture.
* Production DB present.
* All required Qt plugins.
* Application launches without developer environment.
* User settings path writable.
* Program installation directory not used for mutable data.
* UpgradeCode remains stable across package rebuilds.

### Verification

```text
cmake --build --preset windows-msvc-release --target package
```

Then install, launch, exercise basic simulation, uninstall, and reinstall in a clean Windows Sandbox/VM.

---

## Step 9.2: Validate real ELM327 hardware

Use dedicated test hardware/vehicle and follow the guarded workflow.

* [ ] Test physical USB ELM327-compatible adapter.
* [ ] Test Bluetooth Classic SPP adapter.
* [ ] Validate:

  * port enumeration;
  * initialization transcript;
  * adapter identity;
  * protocol detection;
  * PID discovery;
  * multiple ECU response preservation;
  * scheduler adaptation;
  * 15-minute sustained telemetry;
  * hot-unplug recovery.
* [ ] Validate:

  * Mode 03;
  * Mode 07;
  * Mode 02;
  * Mode 09;
  * recording;
  * playback;
  * CSV export.
* [ ] Validate Mode 04 only on a suitable stationary test vehicle with explicit operator confirmation and understanding of diagnostic/readiness data clearing.
* [ ] Capture reproducible hardware test report.

### Acceptance

* No crashes/deadlocks.
* No ECU identity loss in tested CAN responses.
* Recovery after unplug.
* Scheduler adapts to slow and faster adapters.
* Recorded session replays successfully.
* Guarded Mode 04 rejects unsafe states.

### Verification

Record results in:

```text
tests/hardware/elm327_acceptance.md
```

and archive the generated run report outside source control where appropriate.

---

## Step 9.3: Establish Windows release gate and CI

* [ ] Configure GitHub Actions Windows workflow.
* [ ] Restore pinned vcpkg dependencies.
* [ ] Configure/build MSVC.
* [ ] Run unit/integration tests.
* [ ] Run appropriate ASan lane.
* [ ] Validate DTC fixture tests.
* [ ] Support release job requiring production DTC DB through approved external input.
* [ ] Generate MSI.
* [ ] Smoke-test staged application/package where CI environment permits.
* [ ] Finalize release/hardware checklist.

### Release Gate

Require:

* 100% required unit/integration tests passing;
* no newly introduced compiler warnings;
* no ASan failures;
* production DTC DB integrity;
* packaging dependency scan passing;
* MSI smoke test passing;
* hardware validation checklist completed for release candidates.

### Verification

```text
ctest --preset windows-msvc-release --output-on-failure
cmake --build --preset windows-msvc-release --target package
```

---

# Feature 10: Stage 10 — Linux SocketCAN and DEB Release

## Step 10.1: Implement native SocketCAN source using kernel ISO-TP

* [ ] Add Linux CAN interface enumeration.
* [ ] Implement bounded functional ECU discovery using `CAN_RAW` where required.
* [ ] For standard 11-bit generic OBD discovery:

  * transmit appropriate functional single-frame request;
  * identify valid ECU responses;
  * establish physical request/response address pairs.
* [ ] Add corresponding 29-bit addressing support.
* [ ] Open per-ECU `PF_CAN` / `SOCK_DGRAM` / `CAN_ISOTP` sockets for production diagnostic exchanges.
* [ ] Let kernel handle:

  * segmentation;
  * reassembly;
  * flow control;
  * block size;
  * STmin.
* [ ] Configure appropriate ISO-TP socket options.
* [ ] Route logical ISO-TP payloads into the same canonical J1979 decoder used by ELM/simulation/playback.
* [ ] Use `CAN_RAW` only for:

  * discovery;
  * optional raw trace/monitoring.
* [ ] Do not build a competing userspace production ISO-TP state machine.
* [ ] Handle:

  * interface down;
  * socket error;
  * ECU timeout;
  * source disconnect;
  * cancellation.
* [ ] Runtime should not require root after the CAN interface has been configured by the system/user.
* [ ] Windows build returns `Core.UnsupportedPlatform` for SocketCAN configuration.
* [ ] Update `APP.md` transport architecture.
* [ ] Update `MODERN.md` Linux kernel/toolchain requirements.

### Tests

Using `vcan0` and deterministic virtual ECU responders:

* interface enumeration.
* functional discovery.
* 11-bit ECU mapping.
* 29-bit ECU mapping.
* single-frame request/response.
* kernel multi-frame ISO-TP response.
* multiple discovered ECUs.
* timeout.
* interface-down.
* malformed discovery frame rejection.
* source cancellation.
* Windows unsupported-platform behavior.
* canonical payload parity with ELM fixtures.

### Verification

```text
ctest --preset linux-gcc-debug -R "socketcan|isotp" --output-on-failure
```

with configured `vcan0`.

---

## Step 10.2: Add Linux UI integration

* [ ] Expose SocketCAN source only on supported Linux builds.
* [ ] Interface selector.
* [ ] Connection/state UI.
* [ ] Clear error for:

  * missing interface;
  * down interface;
  * no responding ECU.
* [ ] Maintain all six workspaces.
* [ ] Preserve same engine/UI contract as Windows.

### Tests

* Linux source model.
* Interface selection.
* Missing/down interface.
* SocketCAN connection.
* Workspace compatibility.
* Source switching Synthetic ↔ SocketCAN.

### Verification

```text
ctest --preset linux-gcc-debug -R linux_ui --output-on-failure
```

---

## Step 10.3: Produce and validate DEB package and Linux CI

* [ ] Configure Linux presets:

  * `linux-gcc-debug`;
  * `linux-gcc-release`;
  * `linux-clang-sanitize`;
  * optional core-only `linux-clang-tsan`.
* [ ] Configure CPack DEB.
* [ ] Package:

  * desktop entry;
  * icons;
  * application binaries;
  * required dynamically linked Qt runtime when system Qt version cannot satisfy pinned requirements;
  * production DTC DB;
  * notices/licenses.
* [ ] Use package-private Qt runtime layout when bundling is required.
* [ ] Configure runtime search paths safely.
* [ ] Document physical CAN configuration separately from application runtime.
* [ ] Document `vcan0` setup for development.
* [ ] Configure Linux CI:

  * GCC normal build/tests;
  * Clang ASan+UBSan;
  * optional TSan core suite;
  * `vcan0` integration;
  * DEB generation/smoke install.
* [ ] Do not package the test DTC fixture in release artifact.

### Tests

* Clean DEB installation.
* Launch.
* Synthetic mode.
* `vcan0`.
* uninstall.
* runtime library resolution.
* user-data path.
* no fixture DB.
* sanitizer suites.
* package dependency validation.

### Verification

```text
cmake --build --preset linux-gcc-release --target package
```

Then install/test the DEB on the project's documented supported Ubuntu LTS test environment.

---

## Step 10.4: Complete cross-platform acceptance

* [ ] Replay identical golden sessions on Windows and Linux.
* [ ] Require exact parity for:

  * DTC codes;
  * ECU identity;
  * metadata strings;
  * sample quality;
  * rule state transitions;
  * discrete protocol outcomes.
* [ ] Compare floating-point metrics using explicit per-metric tolerances rather than requiring impossible bit-for-bit platform identity.
* [ ] Verify:

  * `IDataSource` behavior;
  * scheduler behavior;
  * decoder results;
  * session parser;
  * diagnostic rules;
  * CSV export semantics.
* [ ] Ensure deterministic rules use equivalent clock/seed inputs.
* [ ] Investigate any platform-dependent diagnostic outcome.
* [ ] Update `APP.md` and `MODERN.md` with final cross-platform constraints.

### Tests / Acceptance

* Golden protocol vectors.
* Golden session replay.
* Float tolerance checks.
* Exact DTC/finding parity.
* Export golden files with normalized line-ending handling.
* Full release tests Windows + Linux.

### Verification

Run full release suites on both platforms and compare generated parity artifacts.

---

# Final Release-State Requirements

At completion:

## `APP.md`

Must describe durable system knowledge including:

* engine architecture;
* thread ownership;
* source lifecycle;
* canonical transport boundary;
* ELM327 transport ownership;
* SocketCAN/kernel ISO-TP architecture;
* scheduler arbitration;
* telemetry data flow;
* engine epochs;
* diagnostics safety invariants;
* recording/playback behavior;
* major architectural rationale and known constraints.

Do not duplicate class/function listings that are obvious from source code.

## `MODERN.md`

Must describe adopted technical policy including:

* C++ standard;
* compiler/toolchain support;
* exact Qt policy;
* Qt licensing boundary;
* CMake baseline;
* vcpkg policy;
* dependency choices;
* sanitizers;
* preferred C++/Qt patterns;
* deprecated approaches to avoid;
* Windows packaging policy;
* Linux SocketCAN/ISO-TP policy.

## Testing

Every implemented behavior-changing Step must have its planned automated tests implemented and executed.

No Step is complete merely because the application builds.

## Release Definition

Windows v1 is complete only when:

* backend acceptance passes;
* UI acceptance passes;
* required Windows tests pass;
* real ELM327 validation is documented;
* production DTC database is supplied and validated;
* clean MSI installation succeeds;
* release licensing/notices are present;
* no fixture data is substituted for licensed production data;
* `APP.md` and `MODERN.md` accurately describe the released system.
