# Guidance for AI agents diagnosing real-time timing

Use this file when integrating or interpreting `esp_rt_diagnostics_reporter`.
Its scope includes this component and its files. The companion accumulator lives
in `../esp_rt_diagnostics`.

Read `../esp_rt_diagnostics/AGENTS.md` when choosing the measured cycle,
placing ISR/task timestamps, defining stages/events/intervals, assigning
budgets, or reviewing deterministic-path safety. This file assumes the snapshot
was instrumented correctly and focuses on asynchronous presentation and
evidence-based diagnosis.

## Objective

Use the reporter to produce evidence about deadline violations, missed periodic
events, scheduler wake latency, execution-time spikes, stage budgets, and period
jitter without making the observation mechanism the dominant disturbance.

Do not treat one maximum, one log line, or one `log-affected` window as proof of
a deterministic failure. Prefer repeated `quiet` windows, counts with explicit
denominators, and controlled comparisons.

## Preserve the reporter boundary

- Keep timing accumulation and hot-path measurement rules in
  `esp_rt_diagnostics`; keep asynchronous presentation in this reporter.
- Keep sensor-, bus-, estimator-, motor-, and application-specific fields in a
  copied application payload and callback. Do not add them to the generic
  reporter.
- Supply persistent names for application-defined numeric stages, events, and
  intervals, but do not define their measurement semantics in this component.
- Use a separate `esp_rt_diag_t` for each independently scheduled real-time
  task. A reporter instance has exactly one publisher task; create another
  reporter if a second task must publish independently.
- Never move reporter logs, string formatting, callbacks, or derived-value
  presentation into the monitored task.

## Snapshot prerequisite

Before interpreting a report, perform a brief integration audit using the
companion guidance. Confirm that the reported period corresponds to the cycle
begin event, every begun cycle is ended, notification counts are preserved, and
the relevant variable-duration operations are instrumented. If any item is
uncertain, correct or document the measurement boundary before inferring a
cause from the reporter output.

## Reporter configuration for diagnosis

- Run the reporter below the monitored task's priority. Prefer another core
  when the target permits, while remembering that logging drivers and system
  locks can still be shared across cores.
- Start with `ESP_RT_DIAG_REPORT_COMPACT`. Use FULL only when item-by-item output
  is necessary, and SILENT plus a bounded callback when machine-readable output
  is preferable.
- Choose a measurement window much longer than one reporting burst. Five
  seconds is a useful starting point; increase it if output or USB traffic lasts
  a significant fraction of the window.
- Do not reduce the one-slot overwrite queue merely to avoid missing IDs. A
  missing `window_id` is evidence that presentation could not keep up. Blocking
  or queueing an unbounded backlog would be worse for a control task.
- Remember that configuration pointer fields are shallow-copied. Keep names and
  callback context valid for the complete reporter lifetime.

## Meaning of the principal measurements

| Measurement | Interpretation | Important limitation |
|---|---|---|
| `cycles` | Cycles completed during the actual window. | Compare with `window_duration_us / expected_period_us`, allowing boundary rounding. |
| `cycle_rate_hz` | Completed cycles divided by actual elapsed window time. | A correct average rate can coexist with isolated jitter or overruns. |
| `missed_events` | Sum of `pending_events - 1` when notifications accumulated. | It reports failure to service periodic events individually, not merely a slow sample. |
| `cycles_with_valid_wake_time` | Cycles for which exactly one ISR event was pending. | Wake statistics do not represent cycles with accumulated events. |
| `max_wake_latency_us` | Worst valid ISR-to-task start delay in the window. | Sensitive to scheduling, interrupt masking, flash/cache stalls, logging, and other tasks. |
| `max_processing_time_us` | Worst task execution from cycle begin to end. | Includes uninstrumented code but excludes valid wake latency. |
| `max_cycle_time_us` | Worst valid wake latency plus processing time. | Calculated only where wake time is unambiguous. |
| `deadline_overruns` | Completed cycles whose processing plus valid wake latency exceeded the deadline; processing alone is used when wake is ambiguous. | Always report it as count/`cycles` and percentage. |
| `stage.max_duration_us` | Worst observed duration of one named operation. | A stage may not explain all processing time; check instrumentation coverage. |
| `stage.over_budget_calls` | Stage executions strictly above its configured nonzero budget. | It is not the same as a whole-cycle deadline overrun. |
| `interval.min_us/max_us` | Observed spacing or duration supplied by the application. | Its meaning depends entirely on where the application timestamps it. |
| lifetime/`total_` fields | Accumulation since initialization. | Use window values to correlate a local disturbance; use totals for trend confirmation. |

## Quiet versus log-affected windows

Treat `ESP_RT_DIAG_WINDOW_QUIET` as the primary evidence for normal operation.
Treat `ESP_RT_DIAG_WINDOW_LOG_AFFECTED` as a measurement of possible observer
impact, not as data to discard automatically.

The reporter deliberately pairs an affected window with a quiet successor.
Logging that pair can perturb the next window. If affected windows are bad but
quiet windows are consistently healthy, the real-time implementation may be
sound while presentation traffic is intrusive. If both classes are bad, search
the real-time path and other system activity.

The classification only models this reporter's own bursts. Console logs from
drivers, status polling over USB, Wi-Fi/Bluetooth tasks, filesystem activity,
or other components can disturb a window still labelled `quiet`. Inventory all
external activity before claiming that a quiet window was interference-free.

## Diagnostic workflow

### 1. Establish a baseline

Collect several complete window pairs after startup transients. Record:

- firmware/configuration revision;
- exact acquisition and control rates;
- deadline and stage budgets;
- reporter format, core, priority, and window duration;
- active transports, application logs, and host polling interval;
- load state, such as idle motor, closed loop, capture, or calibration.

Use at least three quiet windows for a preliminary conclusion. For rare faults,
collect enough windows to reproduce the event more than once.

### 2. Check event delivery before execution time

If `missed_events > 0`, the task woke with accumulated periodic notifications.
This is direct evidence that individual releases were not serviced on time.
Check whether the losses appear only during startup, capture/status commands,
USB dumps, or reporting bursts.

Do not infer the number of missed events from `cycles` alone. Use the explicit
counter and inspect whether `cycles_with_valid_wake_time` also decreased.

### 3. Check whole-cycle deadlines

Report overruns as, for example, `7/15000 cycles (0.047%)`, not simply "there
were overruns." Compare the window maximum against the deadline, but distinguish
a single extreme from a persistent distributional problem.

- High `max_wake_latency_us` with normal processing suggests scheduling,
  interrupt masking, cache/flash effects, or shared-system interference.
- High `max_processing_time_us` suggests work inside the monitored task.
- Both high may indicate a system-wide disturbance or more than one cause.

### 4. Attribute processing time to stages

Compare stage maxima and violation counts with `max_processing_time_us`.

- A bus-stage spike points toward transaction duration, driver locking, bus
  clock, sensor clock stretching, retries, or contention.
- An estimator/controller spike points toward computation, conditional paths,
  data-dependent work, or code/cache placement.
- A large processing maximum not explained by any stage indicates missing
  instrumentation between stages, snapshot/publication cost, or bookkeeping.

When an SPI stage and its fault events are present, correlate transfer maxima
with timeouts, CRC/parity faults, and retries. Duration alone cannot distinguish
those causes; if the required counters are absent, refer the instrumentation
change to `../esp_rt_diagnostics/AGENTS.md`.

### 5. Inspect interval jitter

Use sample and control interval minima/maxima to detect release jitter and
rate-divider mistakes. A correct average cycle rate does not prove uniform
spacing. Relate each interval to the point timestamped by the application:
timer release, transaction completion, estimator update, or controller update.

### 6. Run one controlled comparison at a time

When observer interference is plausible, use this sequence:

1. silence unrelated periodic logs while preserving diagnostic accumulation;
2. compare compact reporter output with FULL output;
3. increase host status-polling intervals (in this project, changing frequent
   0.5 s polling to 2 s was a useful isolation test);
4. increase the diagnostic window to reduce report frequency;
5. compare capture inactive versus armed/capturing/dumping;
6. compare detailed timing enabled versus base diagnostics only;
7. finally compare diagnostics completely disabled to quantify residual
   instrumentation cost.

Change one variable per build/test. Preserve the same physical load, controller
gains, sample rate, and test sequence while comparing timing. Do not tune PID or
Kalman parameters as a remedy for a scheduler or bus-timing failure.

## Pattern-based hypotheses

Use these as hypotheses to test, not automatic diagnoses:

| Observed pattern | First hypotheses |
|---|---|
| Missed events and high wake latency; stages remain below budget | Higher-priority task/ISR, long critical section, interrupt masking, logging/USB activity, or cache/flash stall. |
| No missed events; processing and one stage exceed budget | Slow or contended operation inside that stage. |
| Overruns only in `log-affected` windows | Reporter or application logging is perturbing execution. Confirm with compact/SILENT output. |
| Overruns in quiet and affected windows at similar rates | Root cause likely exists in the real-time path or unclassified external activity. |
| Expected average rate but wide sample intervals | Jitter is being compensated by short following intervals; average rate hides it. |
| Missing window IDs but healthy monitored timing | Reporter cannot keep up; snapshots are being overwritten without blocking the producer. |
| Stage maxima are low but processing maximum is high | Important code lies outside measured stages or snapshot/publication is included. |
| Sensor errors rise with bus-stage maxima | Investigate electrical integrity, bus speed, retries, or sensor/driver behavior before estimator tuning. |

## Reporting conclusions to the user

Every AI-generated timing conclusion should include:

1. whether evidence came from quiet, affected, or both window classes;
2. number and duration of windows examined;
3. expected versus measured cycle rate;
4. missed events as window and lifetime counts;
5. overruns as count, denominator, and percentage;
6. maximum wake, processing, and valid cycle times versus deadline;
7. stage average/maximum/budget violations for the likely bottleneck;
8. interval range for each relevant periodic activity;
9. correlation with capture, dump, host command, log, or operating state;
10. confidence level and the smallest next experiment that can falsify the
    leading hypothesis.

A suitable summary format is:

```text
Quiet baseline: 4 windows x 5.0 s.
Rate: 3000.0/3000.0 Hz; missed: 0 window, 2 lifetime.
Deadline: 334 us; overruns: 3/15000 (0.020%).
Max wake/processing/cycle: 18/291/305 us.
Largest stage: SPI 244 us, 2 calls above its 220 us budget.
Hypothesis: rare SPI transaction delay, not scheduler wake latency.
Next test: keep load fixed and count SPI retries/timeouts in a separate event.
```

Avoid statements such as "the system is deterministic" based solely on an
average rate, or "the reporter caused the loss" based solely on temporal
coincidence. State what the counters prove and label causal explanations as
hypotheses until a controlled comparison supports them.

## Safety and modification discipline

- Diagnose from existing logs and configuration before changing firmware.
- Do not increase task priorities, disable watchdogs, change bus timing, or move
  core affinity merely to make counters disappear; explain the causal model and
  verify the change under the same workload.
- Preserve application safety behavior during timing experiments.
- Do not flash hardware or start motors unless the user has requested that
  external action and the test conditions are understood.
- When changing instrumentation, update the relevant README/API comments and
  verify an enabled build, a disabled or reduced-instrumentation build when
  applicable, and `git diff --check`.
