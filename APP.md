# RevDash Application Blueprint

## 1. Product Vision
RevDash is a high-performance, offline desktop OBD-II diagnostics and vehicle telemetry application built for automotive enthusiasts, DIY mechanics, and technicians.

## 2. Core User Flows
1. **Connect Flow:**
   - Detect and list local COM ports (USB/Bluetooth SPP).
   - Alternatively, select a Synthetic Engine Simulation profile or load a recorded session (`.jsonl`).
   - Initialize the ELM327 protocol and confirm readiness.
2. **Telemetry Flow:**
   - Stream live vehicle metrics (RPM, Speed, Throttle, Coolant, MAP, MAF, Trims, Voltage, O2) with dynamic scheduling (high/mid/low tiers).
   - Display rolling 2D charts and instrument gauges at 20 Hz snapshot polling with zero heap allocations on the hot loop.
3. **Diagnostics & Fault Management Flow:**
   - Scan Modes 03 (Confirmed) and 07 (Pending) DTCs.
   - Extract Mode 02 Freeze Frames.
   - Run rolling heuristic rules (Vacuum Leak, Catalyst Degradation, Thermostat Stuck, Alternator Fault).
   - Look up codes offline via embedded SQLite DTC database.
   - Execute Mode 04 DTC Clear safely (Stationary check $\le 0.5\text{ km/h}$, pre-clear evidence snapshot, 30s confirmation token, post-clear rescan).
4. **Recording, Playback & Export Flow:**
   - Record raw OBD and telemetry to versioned microsecond JSON Lines (`.jsonl`).
   - Replay sessions deterministically with `.ridx` index sidecar (0.5x to 5x speeds).
   - Export 10 Hz resampled automotive CSV (presets for RevDash, MegaLogViewer, TunerStudio).

## 3. Architecture & Boundaries
- **Core Engine (`revdash_core`):** Qt-independent, native C++20. Runs in-process with dedicated worker threads:
  - `I/O Worker` (`std::jthread`): Async Boost.Asio / serial communications.
  - `Pipeline Worker` (`std::jthread`): J1979 decoding, ISO-TP reassembly, metric aggregation, diagnostic rules.
  - `Recorder Worker` (`std::jthread`): Non-blocking buffered JSONL disk writer.
- **Inter-Thread Communication:** Fixed-capacity lock-free SPSC ring buffers and atomic latest-value telemetry store.
- **Presentation Layer (`revdash_app` & `revdash_cli`):**
  - Desktop UI: Qt 6 / QML with 6 dedicated workspaces (`Connect`, `Dashboard`, `Diagnostics`, `Simulator`, `Sessions`, `Settings`).
  - Headless CLI: CLI11-powered diagnostic tool.
- **Data Persistence:**
  - DTC Database: Read-only SQLite (`assets/dtc/revdash_dtc.sqlite`).
  - Sessions: User-selected folder (`.jsonl`, `.ridx`, `.csv`).
  - Configuration: `QStandardPaths::AppConfigLocation`.

## 4. Current Status & Roadmap
- **Stage 1 (Current):** Build Foundation, Core Contracts & Concurrency Primitives.
- **Stage 2:** SAE J1979 Protocol & ISO-TP Codecs.
- **Stage 3:** Synthetic Powertrain & Fault Simulator.
- **Stage 4:** ELM327 Serial Driver & Engine Service.
- **Stage 5:** Diagnostics Rules, DTC Database & Guarded Clear.
- **Stage 6:** Session Recording, Playback & CSV Export.
- **Stage 7:** Headless CLI & Backend Acceptance.
- **Stage 8:** Qt 6 / QML Desktop Interface.
- **Stage 9:** Windows v1 Packaging & Hardware Validation.
- **Stage 10:** Linux SocketCAN & DEB Packaging.
