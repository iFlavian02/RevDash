# Backend acceptance notes

## Steady telemetry allocation profile

The `backend_e2e` suite profiles 100,000 Mode 01 frames through the source and
recorder SPSC queues, decoder, latest-value store, metric aggregator, and
diagnostic rule evaluator.

The fixed-capacity SPSC queue path performs no heap allocation after
construction. The wider telemetry path is intentionally not held to a false
zero-allocation requirement. Its expected steady allocations are:

- the decoder's result vector, because one OBD PID can produce multiple
  canonical telemetry samples;
- bounded deque blocks retained by the 30-second metric aggregation window and
  the longest active diagnostic-rule window;
- transient diagnostic evidence only when a rule changes state.

The acceptance workload records allocation and deallocation counts, limits
unexpected allocation growth, fills the longest rule window, and then runs an
additional 10,000 frames to verify that retained memory has reached a plateau.
It also requires throughput above the 250 packets/second playback acceptance
rate.

## Playback load

The playback acceptance fixture contains 500 packets spaced at 20 ms (50
packets/second recorded rate). Replaying it at 5x produces an equivalent 250
packets/second load. The test verifies the final decoded state, exact queue
counts, zero source/recorder queue drops, and bounded shutdown.
