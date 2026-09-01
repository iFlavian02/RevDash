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

Stage 1 establishes contracts and concurrency primitives only. No physical adapter, synthetic ECU behavior, protocol decoder, scheduler, recorder, or UI workflow is implemented yet. The temporary no-Qt application fallback is a development build path; a complete desktop application begins in Stage 8.
