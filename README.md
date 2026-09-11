# esp_rt_diagnostics_reporter

`esp_rt_diagnostics_reporter` is the asynchronous presentation companion for
`esp_rt_diagnostics`. It copies a completed diagnostic snapshot and an optional
application payload, then formats or presents them from a separate FreeRTOS
task.

Its purpose is to keep logging, floating-point derived calculations, labels,
and application-specific presentation out of the deterministic task. It does
not collect timings itself and does not replace `esp_rt_diagnostics`.

AI coding agents can use `AGENTS.md` in this directory as an evidence-based
procedure for instrumenting a task, interpreting losses and overruns, separating
observer effects, and proposing the next controlled timing experiment.

## Installation

Add only the reporter to the consuming application's
`main/idf_component.yml`:

```yaml
dependencies:
  esp_rt_diagnostics_reporter:
    git: https://github.com/smartsensingme/esp_rt_diagnostics_reporter.git
```

The reporter's own `idf_component.yml` pins and downloads the compatible
`esp_rt_diagnostics` revision automatically. The application does not need to
know or repeat that transitive dependency. `CMakeLists.txt` still declares the
compile/link relationship through `REQUIRES`; the two files serve different
parts of the ESP-IDF build process.

## Division of responsibilities

```text
monitored task (producer)                    reporter task (consumer)
-------------------------                    ------------------------
take immutable snapshot
copy snapshot + payload -- overwrite queue --> classify window
resume real-time work immediately             emit generic logs
                                                call application callback
```

The reporter is instance-based, not a singleton. Applications may create more
than one reporter, provided each has one publisher and enough internal RAM.

## Real-time and memory behavior

`esp_rt_diag_reporter_create()` allocates the instance, item buffers, one-slot
queue storage, and task stack from internal 8-bit RAM. The queue and task use
FreeRTOS static construction APIs over that preallocated memory.

`esp_rt_diag_reporter_publish()`:

- allocates no memory;
- copies a fixed snapshot plus a fixed-size payload;
- uses `xQueueOverwrite()` and does not wait for the consumer;
- intentionally replaces an older pending item when the reporter falls behind.

The operation is bounded, but copying a very large payload still lengthens the
producer path. Keep payloads compact and prefer state summaries over arrays.
The API is not ISR-safe.

Each instance supports exactly one publisher task. The producer staging buffer
is deliberately lock-free; concurrent publishers would race while assembling
an item. The consumer owns separate receive and deferred buffers.

## Why the queue has one slot

These diagnostics describe current behavior rather than a lossless event log.
If presentation becomes slower than snapshot production, freshness is more
useful than blocking a control task or accumulating an unbounded backlog. The
monotonic `window_id` lets a consumer notice skipped snapshots.

Choose a diagnostic window much longer than the time needed to emit one report.
If every snapshot must be retained, this reporter is not the correct transport;
use a separately dimensioned recorder or storage pipeline.

## Quiet and log-affected windows

Serial logging can consume shared CPU, locks, and driver resources. A report
printed between two windows may therefore perturb the following window. The
reporter classifies snapshots as:

- `ESP_RT_DIAG_WINDOW_QUIET`: no reporter burst is expected to have influenced
  the window;
- `ESP_RT_DIAG_WINDOW_LOG_AFFECTED`: reporter activity may have influenced it.

Window 1 is conservatively classified as affected by startup activity. An
affected window is deferred. When a quiet successor arrives, the reporter emits
the affected snapshot first and the quiet snapshot second. The output generated
then marks the immediately following `window_id` as affected.

This design provides paired evidence: one window that may include observation
cost and one that is suitable for estimating normal timing. It assumes the
report burst completes well within one measurement window.

## Configuration reference

### Generic output format

| `report_format` | Behavior |
|---|---|
| `ESP_RT_DIAG_REPORT_FULL` | Multiple descriptive lines for aggregates, every stage, event, and interval. |
| `ESP_RT_DIAG_REPORT_COMPACT` | One bounded line (currently 768 bytes maximum) per snapshot. |
| `ESP_RT_DIAG_REPORT_SILENT` | No generic logs; the optional callback still runs. |

### Payload and callback

| Field | Meaning |
|---|---|
| `application_payload_size` | Exact number of bytes copied after each snapshot. Zero allows a NULL payload. |
| `report_callback` | Optional function called synchronously by the reporter task after generic output. |
| `callback_context` | Opaque pointer passed unchanged to the callback. |

The callback receives reporter-owned copies. Its snapshot and payload pointers
are valid only during that invocation. Do not retain them; copy any data that
must outlive the callback. Do not delete the reporter from its own callback.

### Names

`stage_names`, `event_names`, and `interval_names` translate the numeric IDs
stored by the real-time component. Each has a matching count. A NULL array, a
NULL element, or an ID beyond the supplied count produces a deterministic
fallback such as `stage_2`, `event_1`, or `interval_0`.

The reporter shallow-copies the configuration. Arrays, strings, and
`callback_context` remain application-owned and must stay valid until deletion.
Static constant arrays are the simplest safe choice.

### Reporter task

| Field | Constraint | Meaning |
|---|---|---|
| `task_stack_size` | nonzero bytes | Stack allocated in internal RAM. Size for the callback as well as generic formatting. |
| `task_priority` | below `configMAX_PRIORITIES` | Usually lower than the monitored task. |
| `task_core` | valid core or `tskNO_AFFINITY` | Core on which presentation runs. |

There is no component Kconfig menu: output mode, payload, names, and task
placement are per-instance runtime configuration.

## Complete use example

The following payload contains only state that must be interpreted together
with a diagnostic window:

```c
typedef struct {
    float speed_rpm;
    float control_percent;
    uint32_t sensor_errors;
} application_report_t;

static const char *const stage_names[] = {
    "i2c",
    "estimator",
    "control",
};

static const char *const event_names[] = {
    "estimator_update",
    "sensor_error",
};

static esp_rt_diag_reporter_handle_t reporter;

static void report_application(
    const esp_rt_diag_snapshot_t *diagnostics,
    const void *payload,
    esp_rt_diag_window_class_t window_class,
    void *context)
{
    (void)context;
    const application_report_t *state = payload;
    ESP_LOGI("APP_DIAG", "w=%llu[%s] speed=%.1f rpm output=%.1f%% errors=%lu",
             diagnostics->window_id,
             esp_rt_diag_window_class_name(window_class),
             state->speed_rpm,
             state->control_percent,
             state->sensor_errors);
}

void start_reporter(void)
{
    const esp_rt_diag_reporter_config_t config = {
        .report_format = ESP_RT_DIAG_REPORT_COMPACT,
        .application_payload_size = sizeof(application_report_t),
        .stage_names = stage_names,
        .stage_name_count = sizeof(stage_names) / sizeof(stage_names[0]),
        .event_names = event_names,
        .event_name_count = sizeof(event_names) / sizeof(event_names[0]),
        .report_callback = report_application,
        .callback_context = NULL,
        .task_stack_size = 4096,
        .task_priority = 1,
        .task_core = 0,
    };
    ESP_ERROR_CHECK(esp_rt_diag_reporter_create(&config, &reporter));
}
```

After the monitored task has closed a cycle and extracted a snapshot:

```c
application_report_t state = {
    .speed_rpm = estimated_speed_rpm,
    .control_percent = control_percent,
    .sensor_errors = sensor_error_count,
};

esp_err_t error = esp_rt_diag_reporter_publish(reporter, &snapshot, &state);
if (error != ESP_OK) {
    /* Count or handle locally; do not log synchronously in a hard RT path. */
}
```

During shutdown, first stop or join the producer, then destroy the reporter:

```c
esp_rt_diag_reporter_delete(reporter);
reporter = NULL;
```

Deletion is not synchronized with publication. It stops the task immediately
and discards queued or deferred items.

## API guide

- `esp_rt_diag_reporter_create()` validates configuration, calculates aligned
  item layout, allocates internal memory, creates the queue, and starts the task.
- `esp_rt_diag_reporter_publish()` copies the newest snapshot/payload and
  overwrites the pending queue item.
- `esp_rt_diag_reporter_delete()` stops the task and releases all owned
  resources. NULL is accepted so it can unwind partial construction.
- `esp_rt_diag_window_class_name()` returns persistent English text for logs and
  callbacks. Unknown enum values are treated as `quiet`.

## Errors and troubleshooting

| Symptom / result | Likely cause and response |
|---|---|
| `ESP_ERR_INVALID_ARG` from create | NULL argument, zero stack, invalid priority, or invalid report format. |
| `ESP_ERR_INVALID_SIZE` | Payload size overflowed the combined item calculation. |
| `ESP_ERR_NO_MEM` | Insufficient internal RAM, or queue/task creation failure. Reduce payload/stack or inspect task settings. |
| `ESP_ERR_INVALID_ARG` from publish | NULL payload with nonzero configured size, invalid handle/snapshot, or corrupt active counts. |
| Missing window IDs | Expected overwrite behavior: the reporter was slower than snapshot production. |
| Truncated compact line | The bounded 768-byte report exhausted its buffer; shorten labels or use FULL. |
| Callback crashes later | A configuration pointer or callback payload pointer was retained beyond its documented lifetime. |
| Timing worsens in affected windows | Expected observation effect; compare the paired quiet windows. |

## Component boundary

Keep reusable scheduling, copying, classification, and generic diagnostic
formatting here. Sensor-, motor-, estimator-, or recorder-specific formatting
belongs in the application callback. This preserves reuse without forcing the
real-time accumulator to depend on logging or application data types.

## Integration in this project

`main/realtime_telemetry.c` creates one reporter on Core 0 at priority 1. Static
name arrays translate the IDs defined by `main/realtime_telemetry.h`. The
monitored Core 1 task publishes a copied `realtime_telemetry_payload_t` together
with each completed snapshot. Generic compact timing output remains reusable;
motor, estimator, recorder, and current-sense presentation stays in the
application callback.
