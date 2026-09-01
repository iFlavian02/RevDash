# RevDash System Knowledge

## Stage 1 architecture

`revdash_core` is a native C++20 library with no Qt dependency. The CLI and future Qt/QML desktop layer are consumers of the core; Qt must not cross into protocol, transport, diagnostics, simulation, session, or scheduling code.

The canonical boundary between a source and the engine is `ObdRequest` and `ObdMessage`. Sources retain transport framing internally and publish logical messages with source type, optional ECU identity, monotonic time, optional UTC time, sequence number, and bounded payload length. `EcuAddress` preserves address format as well as its numeric value so future 11-bit and 29-bit CAN traffic cannot be conflated.

## Source lifecycle and worker ownership

`IDataSource` expresses the common lifecycle for Serial ELM327, Synthetic, Playback, and later SocketCAN sources. `AsyncDataSource` is the reusable implementation base: it owns one Boost.Asio executor thread, serializes all lifecycle and transmit work there, and invokes source completions and subscriptions on that worker.

Lifecycle methods return immediately after queuing work. A source rejects transmissions unless it is `Ready`; disconnect is idempotent. Intentional disconnect and destruction cancel tracked work with `Core.Cancelled`. The last successful `DataSourceConfig` is retained for reconnect. A subscription token is move-only and unregisters safely even while messages are being published.

Only one source will be active in the eventual engine. Source replacement is a shutdown boundary: cancel old work, stop accepting old callbacks, then connect the new source. Future source implementations must derive from `AsyncDataSource` or preserve these same ownership and callback guarantees.

## Pipeline and telemetry consistency

Hot paths use project-owned `BoundedSpscQueue` wrappers over `boost::lockfree::spsc_queue`. The wrapper accepts only trivially copyable packets, has compile-time capacity, never overwrites unread data, and rejects the newest packet on overflow while incrementing a drop counter.

The initial topology is:

```text
active source worker -- ObdMessage / 1024 --> engine worker
engine worker -- RecorderPacket / 2048 --> recorder worker
```

`LatestTelemetryStore` is deliberately lock-based rather than custom lock-free. A `std::shared_mutex` protects complete `TelemetrySample` replacement and whole-snapshot reads, so value, timestamp, quality, sequence, and ECU identity always originate from one update. The store records read/write lock contention for future profiling. It is not valid to publish related telemetry fields through independent atomics.

## Time, values, and errors

Scheduling and processing use monotonic time; durable metadata may carry optional UTC time. `ManualClock` makes time-dependent unit tests deterministic. Core values remain SI; conversion is reserved for UI and export stages.

Expected failures use `Result<T>` and stable domain-qualified error codes. Operational errors must be user-safe and may include diagnostic context without exposing sensitive implementation details.

## Current limitations

Stage 2.1 adds a Qt-independent Mode 01 decoder and descriptor catalog. The catalog is the source of truth for each implemented PID's response length, SI unit, scheduling class, value bounds, and stale interval. `decodeMode01Response` consumes canonical logical OBD messages only; it validates the positive service byte and PID echo before decoding, classifies ECU negative responses separately, and preserves message timestamps, sequence, and ECU identity in every telemetry sample.

Supported-PID discovery is intentionally separate from the telemetry query filter. Bitmaps use the SAE high-bit-first ordering for the next 32 PIDs. Discovery begins with 00 and follows 20 then 40 only if advertised, while the query filter schedules only catalogued PIDs advertised by the ECU. Narrowband O2 PIDs publish voltage metrics; wideband PIDs publish equivalence-ratio and sensor-current metrics. These are distinct metric types and must not be converted into each other.

Stage 2.2 diagnostic services also consume complete logical J1979 messages, never raw ISO-TP frames. Mode 03 and Mode 07 records retain the originating ECU address and status; padding pairs are removed, and deduplication is deliberately limited to the same code, status category, and ECU. Mode 02 frame-zero extraction adapts its `0x42` response into the existing Mode 01 decoding boundary, so supported freeze-frame telemetry has the same units and validation as live values.

Mode 04 is limited here to request formatting and strict response parsing; a safety workflow for actually clearing faults is deferred to the engine stage. Mode 09 parsing supports VIN, calibration-ID, and CVN records. VINs require one record marker followed by exactly 17 permitted characters. Multi-record CALID/CVN data may occur in one logical message and cross-message accumulation must use `mergeMode09Metadata`, which rejects mixing ECU sources or conflicting VIN values.

The ISO-TP trace codec is a platform-neutral validation and fixture utility. Its explicit source/destination address key preserves 11-bit and 29-bit CAN identity while deterministic reassembly tracks declared length, consecutive-frame sequence rollover, duplicate/missing frames, and timeouts. It enforces the 4095-byte application ceiling. It must not be used to send production ISO-TP flow control: ELM327 owns normal flow control, and future Linux transport uses kernel `CAN_ISOTP` segmentation and reassembly.

`MetricAggregator` uses monotonic timestamps for per-metric rolling windows and provides min/max/mean/median only from valid samples. Its most recent status retains unsupported, dropped, and invalid outcomes; valid samples become stale according to per-metric thresholds. Source-switch, playback-seek, and epoch changes clear both the quality state and all rolling windows so no pre-reset samples can influence new diagnostic decisions.

## Synthetic simulation

Stage 3 supplies an offline `SyntheticPowertrain` with a fixed 10 ms integration step. Its torque balance, PI idle controller, redline limiter, drivetrain, airflow, and coolant model are deterministic for a matching configuration and input sequence; caller/UI update cadence must not change the resulting state. The physical state is authoritative and remains distinct from the optional Gaussian-noise sensor view.

Simulation faults are procedural rather than recorded fixtures. Misfire produces torque instability, an ECU-scoped P0300–P0304 DTC, and a first-occurrence freeze frame; vacuum leak raises low-load trims and emits P0171; a stuck-open thermostat limits warmup and emits P0128. Packet loss and noise share the deterministic PRNG seed. Clearing diagnostic information resets injected faults and its captured frame.

`SyntheticDataSource` derives from `AsyncDataSource`, so it preserves the common non-blocking lifecycle and source-worker callback rules. It accepts complete canonical requests and emits complete logical Mode 01/02/03/04/07/09 responses with virtual CAN ECU addresses. Unsupported requests receive an ECU negative response. Configured reply latency is scheduled on the source worker and cancelled during source destruction; it never creates an independent I/O thread. Public controls post throttle, ambient, fault, and reset changes to that same worker.

No physical ELM327 source, scheduler, recorder, or UI workflow is implemented yet. The temporary no-Qt application fallback is a development build path; a complete desktop application begins in Stage 8.

## Serial transport boundary

Stage 4.1 introduces a driver-level `ISerialTransport` boundary for ELM327 work. It owns asynchronous open, close, read, write, and cancellation operations; buffer ownership remains with the caller until each read/write completion. `AsioSerialTransport` owns its Boost.Asio serial-port executor and maps cancellation and I/O failures into core transport results. The portable interface is mockable and does not require hardware in tests.

On Windows, physical port discovery uses SetupAPI's present Ports device class. It normalizes COM identifiers, reads the friendly name, device description, and hardware identifiers when present, extracts VID/PID where available, and marks Bluetooth Classic SPP hints. USB serial and Bluetooth SPP are otherwise the same serial transport. The currently supported physical baud rates are 9600, 38400, and 115200; the successful normalized configuration remains available for higher-layer persistence.

## ELM327 command boundary

`Elm327DataSource` serializes all adapter commands: it writes one command, accepts arbitrary read chunks until the `>` prompt, and only then admits the next command. Its baseline initialization is `ATZ`, `ATE0`, `ATL0`, `ATS0`, `ATH1`, `ATCAF1`, and `ATSP0`; headers and ELM CAN auto-formatting stay enabled. `ATI` and `ATDP` are capability probes, so a clone rejecting either does not prevent baseline connection.

The prompt parser tolerates echoes, CR/LF variation, blank lines, and prompts split from prior chunks. It classifies known ELM status output; vehicle communication failures become retryable transport failures, while `NO DATA` remains a completed query with no payload. Commands use a bounded timeout based on the configured response limit and observed RTT EWMA, and initialization retries twice before failing. ELM-generated flow control is never replaced by custom RevDash frames. Response normalization retains raw diagnostic lines and preserves each 11-bit or 29-bit ECU response as its own canonical `ObdMessage`.

## Live polling scheduler

`AdaptivePidScheduler` only admits PIDs discovered as supported by the active ECU. It serializes ELM work to one in-flight request, favors RPM/speed/throttle, then normal telemetry/O2 channels, then slow metrics. Dispatch uses observed RTT to estimate a safe cadence. Congestion stretches medium and low schedules first; queued diagnostic modes preempt streaming as soon as the active request completes, preventing diagnostic starvation.
