# Diagnostic Rule Policy

RevDash findings are heuristic, advisory observations. They are not definitive mechanical diagnoses, do not replace manufacturer procedures, and must not be presented as proof that a component has failed. Rule output includes a stable identifier and version, first/last-seen times, active/resolved state, and a snapshot of the measurements that caused activation.

## Applicability and lifecycle

Every rule requires all of its input metrics to be supported, valid, fresh, and continuously sampled across its observation window. A gap longer than twice the metric freshness interval makes that window incomplete. Invalid, dropped, unsupported, or stale input clears incomplete candidate windows; it never counts as healthy evidence and cannot resolve an active finding.

An engine epoch change clears windows and findings so evidence cannot cross a source replacement, simulation reset, or playback seek. Findings are deduplicated by rule identifier. A resolved finding is reactivated in place if the condition returns. Resolution requires applicable healthy evidence to remain stable for 30 seconds after the rule-specific clear gate is satisfied.

Thresholds live in `DiagnosticRuleConfig`; the defaults below are deliberately conservative and can be shortened only in deterministic tests.

## `HEURISTIC_VACUUM_LEAK` version 1.0

Applicability requires RPM, vehicle speed, calculated load, coolant temperature, and bank-one LTFT. The engine must be warm (at least 70 degC). The rule first requires 20 seconds at 600–1,100 rpm, at most 2 km/h, at most 30% calculated load, and LTFT at or above +12%. It then requires 10 seconds at 1,500 rpm or above and at least 35% calculated load, with LTFT no higher than +8% and at least five percentage points below the captured idle mean.

This pattern is consistent with unmetered air having a greater proportional effect at idle, but exhaust leaks, fuel delivery, sensor bias, learned adaptations, and engine-specific strategies can look similar. Resolution requires warm idle/low-load LTFT within -8% to +8% for a complete stable window and the global stable-clear interval.

## `HEURISTIC_CATALYST_EFFICIENCY` version 1.0

This rule is disabled until explicit narrowband upstream/downstream topology is supplied from supported topology discovery. RevDash does not infer sensor roles from PID number alone. It requires 30 seconds with coolant at least 75 degC, RPM range no greater than 300 rpm, calculated-load range no greater than 12 percentage points, and upstream voltage swing of at least 0.50 V.

The finding activates when downstream voltage swing is at least 75% of upstream swing. A ratio at or below 45% supplies clear evidence. This simplified oxygen-storage indicator is not meaningful for unsupported layouts, wideband-current data, deceleration fuel cut, open-loop operation, exhaust leaks, or manufacturer strategies that invalidate the assumptions; those conditions must be excluded by future topology/state inputs rather than guessed.

## `HEURISTIC_THERMOSTAT_STUCK_OPEN` version 1.0

A cold-start candidate requires a running engine, initial coolant no warmer than 50 degC, and coolant within 10 degC of ambient. The finding activates if coolant never reaches 75 degC during a continuously valid 10-minute observation. Reaching 75 degC before the deadline cancels the candidate. Reaching at least 82 degC provides clear evidence for an active finding.

Short trips, cabin-heater load, extreme ambient conditions, coolant-sensor bias, and engine-specific thermal management can affect warmup, so the result remains advisory.

## `HEURISTIC_CHARGING_VOLTAGE` version 1.0

The engine must be warm (at least 70 degC) and remain at or above 1,000 rpm for 15 seconds. Voltage must remain below 11.5 V or above 16.0 V for the complete window to activate the advisory. Values from 11.5 V through 16.0 V count as clear evidence.

The broad limits intentionally avoid treating normal smart-charging variation, including sustained values around 12.2–15.2 V, as a critical alternator failure. Battery chemistry, load, temperature, regenerative charging, module measurement point, and manufacturer control strategy must be considered during follow-up diagnosis.
