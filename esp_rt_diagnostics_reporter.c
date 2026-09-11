#include "esp_rt_diagnostics_reporter.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REPORTER_QUEUE_LENGTH 1U
#define REPORTER_TASK_NAME "rt_diag_reporter"
#define MICROSECONDS_PER_SECOND 1000000.0f
#define COMPACT_REPORT_BUFFER_SIZE 768U

static const char *TAG = "RT_DIAG";

struct esp_rt_diag_reporter_context {
  esp_rt_diag_reporter_config_t config;
  QueueHandle_t queue;
  TaskHandle_t task;
  StaticQueue_t queue_control;
  StaticTask_t task_control;
  StackType_t *task_stack;
  uint8_t *queue_item_storage;
  size_t payload_offset;
  size_t item_size;
  uint8_t *publish_item;
  uint8_t *receive_item;
  uint8_t *deferred_item;
  char compact_report[COMPACT_REPORT_BUFFER_SIZE];
  bool have_deferred_item;
  uint64_t last_log_after_window_id;
};

/**
 * Round value upward to the next alignment boundary.
 *
 * Internal helper called only by esp_rt_diag_reporter_create() to place the
 * optional payload at an address suitable for any fundamental C type.
 */
static size_t align_up(size_t value, size_t alignment) {
  return (value + alignment - 1U) / alignment * alignment;
}

/**
 * Resolve a configured label or generate a deterministic numeric fallback.
 *
 * Internal helper called by report_generic_full() and
 * report_generic_compact(). The fallback buffer belongs to the caller and only
 * needs to remain valid until the immediate formatting operation completes.
 */
static const char *configured_name(const char *const *names, size_t name_count,
                                   size_t index, char *fallback,
                                   size_t fallback_size,
                                   const char *fallback_prefix) {
  /* Prefer application vocabulary whenever the indexed pointer is valid. */
  if (names != NULL && index < name_count && names[index] != NULL) {
    return names[index];
  }

  /* Missing labels remain distinguishable through prefix plus numeric ID. */
  snprintf(fallback, fallback_size, "%s_%zu", fallback_prefix, index);
  return fallback;
}

/**
 * Append formatted text to a fixed-size, always-terminated compact buffer.
 *
 * Internal helper called only by report_generic_compact(). It truncates excess
 * output instead of allocating memory or writing beyond capacity.
 */
static void append_compact(char *buffer, size_t capacity, size_t *used,
                           const char *format, ...) {
  /* Preserve one byte for the terminator after the buffer becomes full. */
  if (*used >= capacity - 1U) {
    return;
  }

  /* Format directly into the unoccupied suffix. */
  va_list arguments;
  va_start(arguments, format);
  int written = vsnprintf(buffer + *used, capacity - *used, format, arguments);
  va_end(arguments);
  if (written <= 0) {
    return;
  }

  /* Saturate the cursor at the last writable character when truncated. */
  size_t available = capacity - *used;
  *used += (size_t)written < available ? (size_t)written : available - 1U;
}

/** Public classification helper used internally by both generic reporters. */
const char *
esp_rt_diag_window_class_name(esp_rt_diag_window_class_t window_class) {
  return window_class == ESP_RT_DIAG_WINDOW_LOG_AFFECTED ? "log-affected"
                                                         : "quiet";
}

/**
 * Emit a descriptive multi-line generic report for one snapshot.
 *
 * Internal presentation function called only by report_item(). It calls the
 * derived-value helpers from esp_rt_diagnostics and configured_name().
 */
static void report_generic_full(const esp_rt_diag_reporter_config_t *config,
                                const esp_rt_diag_snapshot_t *snapshot,
                                esp_rt_diag_window_class_t window_class) {
  /* Derive reusable names/rates outside the log argument lists. */
  const char *class_name = esp_rt_diag_window_class_name(window_class);
  float duration_s =
      (float)snapshot->window_duration_us / MICROSECONDS_PER_SECOND;
  float expected_rate_hz =
      snapshot->expected_period_us > 0U
          ? MICROSECONDS_PER_SECOND / (float)snapshot->expected_period_us
          : 0.0f;

  /* Emit window, deadline, and aggregate timing summaries first. */
  ESP_LOGI(TAG,
           "window=%" PRIu64 "[%s]: duration=%.3f s cycles=%" PRIu32
           " rate=%.1f/%.1f Hz wake_valid=%" PRIu32,
           snapshot->window_id, class_name, duration_s, snapshot->cycles,
           esp_rt_diag_cycle_rate_hz(snapshot), expected_rate_hz,
           snapshot->cycles_with_valid_wake_time);
  ESP_LOGI(TAG,
           "deadline=%" PRIu64 "[%s]: missed=%" PRIu32 "/%" PRIu32
           " overruns=%" PRIu32 "/%" PRIu32 " overrun_rate=%.3f%%",
           snapshot->window_id, class_name, snapshot->missed_events,
           snapshot->total_missed_events, snapshot->deadline_overruns,
           snapshot->total_deadline_overruns,
           esp_rt_diag_deadline_overrun_percent(snapshot));
  ESP_LOGI(TAG,
           "timing=%" PRIu64 "[%s]: max_wake=%" PRIu32 " processing=%" PRIu32
           " cycle=%" PRIu32 " lifetime_processing=%" PRIu32
           " us deadline=%" PRIu32 " us",
           snapshot->window_id, class_name, snapshot->max_wake_latency_us,
           snapshot->max_processing_time_us, snapshot->max_cycle_time_us,
           snapshot->lifetime_max_processing_time_us, snapshot->deadline_us);

  /* Expand each configured numeric namespace into one readable line. */
  for (size_t index = 0; index < snapshot->stage_count; index++) {
    char fallback[24];
    const char *name =
        configured_name(config->stage_names, config->stage_name_count, index,
                        fallback, sizeof(fallback), "stage");
    ESP_LOGI(TAG,
             "stage=%" PRIu64 "[%s]: %s calls=%" PRIu32 " avg=%.1f max=%" PRIu32
             " us over_budget=%" PRIu32,
             snapshot->window_id, class_name, name,
             snapshot->stages[index].calls,
             esp_rt_diag_stage_average_us(snapshot, (uint8_t)index),
             snapshot->stages[index].max_duration_us,
             snapshot->stages[index].over_budget_calls);
  }

  for (size_t index = 0; index < snapshot->event_count; index++) {
    char fallback[24];
    const char *name =
        configured_name(config->event_names, config->event_name_count, index,
                        fallback, sizeof(fallback), "event");
    ESP_LOGI(TAG,
             "event=%" PRIu64 "[%s]: %s count=%" PRIu32 " total=%" PRIu32
             " rate=%.1f Hz",
             snapshot->window_id, class_name, name,
             snapshot->event_counts[index], snapshot->total_event_counts[index],
             esp_rt_diag_event_rate_hz(snapshot, (uint8_t)index));
  }

  for (size_t index = 0; index < snapshot->interval_count; index++) {
    char fallback[24];
    const char *name =
        configured_name(config->interval_names, config->interval_name_count,
                        index, fallback, sizeof(fallback), "interval");
    ESP_LOGI(TAG,
             "interval=%" PRIu64 "[%s]: %s samples=%" PRIu32 " min=%" PRIu32
             " max=%" PRIu32 " us",
             snapshot->window_id, class_name, name,
             snapshot->intervals[index].samples,
             snapshot->intervals[index].min_us,
             snapshot->intervals[index].max_us);
  }
}

/**
 * Build and emit a bounded one-line generic report for one snapshot.
 *
 * Internal presentation function called only by report_item(). All appends use
 * append_compact(), so long label sets are truncated safely at 768 bytes.
 */
static void report_generic_compact(const esp_rt_diag_reporter_config_t *config,
                                   const esp_rt_diag_snapshot_t *snapshot,
                                   esp_rt_diag_window_class_t window_class,
                                   char *report, size_t report_size) {
  /* Start with aggregate timing and deadline information. */
  size_t used = 0U;
  float expected_rate_hz =
      snapshot->expected_period_us > 0U
          ? MICROSECONDS_PER_SECOND / (float)snapshot->expected_period_us
          : 0.0f;

  append_compact(
      report, report_size, &used,
      "w=%" PRIu64 "[%s] cycles=%" PRIu32 " rate=%.1f/%.1fHz missed=%" PRIu32
      " over=%" PRIu32 "(%.3f%%) max[wake/proc/cycle]=%" PRIu32 "/%" PRIu32
      "/%" PRIu32 "us stages=",
      snapshot->window_id, esp_rt_diag_window_class_name(window_class),
      snapshot->cycles, esp_rt_diag_cycle_rate_hz(snapshot), expected_rate_hz,
      snapshot->missed_events, snapshot->deadline_overruns,
      esp_rt_diag_deadline_overrun_percent(snapshot),
      snapshot->max_wake_latency_us, snapshot->max_processing_time_us,
      snapshot->max_cycle_time_us);

  /* Append variable-length stage, event, and interval sections in ID order. */
  for (size_t index = 0; index < snapshot->stage_count; index++) {
    char fallback[24];
    const char *name =
        configured_name(config->stage_names, config->stage_name_count, index,
                        fallback, sizeof(fallback), "s");
    append_compact(report, report_size, &used, "%s%s:%.1f/%" PRIu32 ">%" PRIu32,
                   index == 0U ? "" : ",", name,
                   esp_rt_diag_stage_average_us(snapshot, (uint8_t)index),
                   snapshot->stages[index].max_duration_us,
                   snapshot->stages[index].over_budget_calls);
  }

  append_compact(report, report_size, &used, " events=");
  for (size_t index = 0; index < snapshot->event_count; index++) {
    char fallback[24];
    const char *name =
        configured_name(config->event_names, config->event_name_count, index,
                        fallback, sizeof(fallback), "e");
    append_compact(report, report_size, &used, "%s%s:%" PRIu32,
                   index == 0U ? "" : ",", name, snapshot->event_counts[index]);
  }

  append_compact(report, report_size, &used, " intervals=");
  for (size_t index = 0; index < snapshot->interval_count; index++) {
    char fallback[24];
    const char *name =
        configured_name(config->interval_names, config->interval_name_count,
                        index, fallback, sizeof(fallback), "i");
    append_compact(report, report_size, &used, "%s%s:%" PRIu32 "..%" PRIu32,
                   index == 0U ? "" : ",", name,
                   snapshot->intervals[index].min_us,
                   snapshot->intervals[index].max_us);
  }

  /* Logging occurs only after the complete bounded line has been assembled. */
  ESP_LOGI(TAG, "%s", report);
}

/**
 * Present one copied queue item according to configured application policy.
 *
 * Internal dispatcher called only by reporter_task(). It calls one generic
 * formatter unless SILENT, then synchronously invokes the optional callback.
 */
static void report_item(esp_rt_diag_reporter_handle_t reporter,
                        const uint8_t *item,
                        esp_rt_diag_window_class_t window_class) {
  /* Recover the aligned snapshot/payload views from the copied byte item. */
  const esp_rt_diag_snapshot_t *snapshot = (const esp_rt_diag_snapshot_t *)item;
  const void *payload = reporter->config.application_payload_size > 0U
                            ? item + reporter->payload_offset
                            : NULL;

  /* Generic text is selected independently of the application callback. */
  if (reporter->config.report_format == ESP_RT_DIAG_REPORT_FULL) {
    report_generic_full(&reporter->config, snapshot, window_class);
  } else if (reporter->config.report_format == ESP_RT_DIAG_REPORT_COMPACT) {
    report_generic_compact(&reporter->config, snapshot, window_class,
                           reporter->compact_report,
                           sizeof(reporter->compact_report));
  }
  /* The callback consumes the same immutable copy before the buffer is reused.
   */
  if (reporter->config.report_callback != NULL) {
    reporter->config.report_callback(snapshot, payload, window_class,
                                     reporter->config.callback_context);
  }
}

/**
 * Decide whether reporter output may have disturbed a measurement window.
 *
 * Internal classifier called only by reporter_task(). Startup window 1 is
 * conservatively affected. Thereafter, the window immediately following a
 * reporting burst is affected because serial logging can delay the monitored
 * task on shared system resources.
 */
static bool window_was_log_affected(esp_rt_diag_reporter_handle_t reporter,
                                    uint64_t window_id) {
  return window_id == 1U ||
         (reporter->last_log_after_window_id != 0U &&
          window_id == reporter->last_log_after_window_id + 1U);
}

/**
 * Receive, classify, defer, and present diagnostic snapshots forever.
 *
 * Internal FreeRTOS task created by esp_rt_diag_reporter_create(). It blocks on
 * the one-slot queue. Affected windows are deferred until a quiet successor is
 * available, then both are reported in causal order.
 */
static void reporter_task(void *argument) {
  esp_rt_diag_reporter_handle_t reporter =
      (esp_rt_diag_reporter_handle_t)argument;

  /* Consume complete item copies; no live control object is ever dereferenced.
   */
  while (true) {
    if (xQueueReceive(reporter->queue, reporter->receive_item, portMAX_DELAY) !=
        pdTRUE) {
      continue;
    }

    /* Hold an affected sample so its own logs cannot taint its classification.
     */
    const esp_rt_diag_snapshot_t *snapshot =
        (const esp_rt_diag_snapshot_t *)reporter->receive_item;
    if (window_was_log_affected(reporter, snapshot->window_id)) {
      memcpy(reporter->deferred_item, reporter->receive_item,
             reporter->item_size);
      reporter->have_deferred_item = true;
      continue;
    }

    /* A quiet arrival provides the safe point to print the deferred pair. */
    if (reporter->have_deferred_item) {
      report_item(reporter, reporter->deferred_item,
                  ESP_RT_DIAG_WINDOW_LOG_AFFECTED);
      reporter->have_deferred_item = false;
    }
    report_item(reporter, reporter->receive_item, ESP_RT_DIAG_WINDOW_QUIET);
    reporter->last_log_after_window_id = snapshot->window_id;
  }
}

/**
 * Create and start one asynchronous reporter instance.
 *
 * Public constructor, not called internally. It calls align_up() and uses
 * esp_rt_diag_reporter_delete() to unwind partial construction.
 */
esp_err_t
esp_rt_diag_reporter_create(const esp_rt_diag_reporter_config_t *config,
                            esp_rt_diag_reporter_handle_t *reporter) {
  /* Reject invalid policy/task values before touching caller output. */
  if (config == NULL || reporter == NULL || config->task_stack_size == 0U ||
      config->task_priority >= configMAX_PRIORITIES ||
      config->report_format > ESP_RT_DIAG_REPORT_SILENT) {
    return ESP_ERR_INVALID_ARG;
  }
  *reporter = NULL;

  /* Lay out [snapshot][padding][payload] with overflow protection. */
  const size_t payload_offset =
      align_up(sizeof(esp_rt_diag_snapshot_t), alignof(max_align_t));
  if (config->application_payload_size > SIZE_MAX - payload_offset) {
    return ESP_ERR_INVALID_SIZE;
  }
  const size_t item_size = payload_offset + config->application_payload_size;

  /* Allocate the instance and every data-path buffer from internal RAM. */
  esp_rt_diag_reporter_handle_t instance = heap_caps_calloc(
      1U, sizeof(*instance), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (instance == NULL) {
    return ESP_ERR_NO_MEM;
  }
  instance->config = *config;
  instance->payload_offset = payload_offset;
  instance->item_size = item_size;
  instance->publish_item =
      heap_caps_malloc(item_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  instance->receive_item =
      heap_caps_malloc(item_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  instance->deferred_item =
      heap_caps_malloc(item_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  instance->queue_item_storage =
      heap_caps_malloc(item_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  instance->task_stack = heap_caps_malloc(
      config->task_stack_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (instance->publish_item == NULL || instance->receive_item == NULL ||
      instance->deferred_item == NULL || instance->queue_item_storage == NULL ||
      instance->task_stack == NULL) {
    esp_rt_diag_reporter_delete(instance);
    return ESP_ERR_NO_MEM;
  }

  /* Bind the preallocated one-item storage to a static FreeRTOS queue. */
  instance->queue = xQueueCreateStatic(
      REPORTER_QUEUE_LENGTH, (UBaseType_t)instance->item_size,
      instance->queue_item_storage, &instance->queue_control);
  if (instance->queue == NULL) {
    esp_rt_diag_reporter_delete(instance);
    return ESP_ERR_NO_MEM;
  }

  /* Start the low-priority consumer using the preallocated internal stack. */
  instance->task = xTaskCreateStaticPinnedToCore(
      reporter_task, REPORTER_TASK_NAME, config->task_stack_size, instance,
      config->task_priority, instance->task_stack, &instance->task_control,
      config->task_core);
  if (instance->task == NULL) {
    esp_rt_diag_reporter_delete(instance);
    return ESP_ERR_NO_MEM;
  }

  *reporter = instance;
  return ESP_OK;
}

/**
 * Publish the newest snapshot and application payload without blocking.
 *
 * Public producer operation, not called internally. Exactly one task may call
 * it for an instance because publish_item is intentionally lock-free.
 */
esp_err_t
esp_rt_diag_reporter_publish(esp_rt_diag_reporter_handle_t reporter,
                             const esp_rt_diag_snapshot_t *diagnostics,
                             const void *application_payload) {
  /* Validate pointers and externally supplied active-array counts. */
  if (reporter == NULL || diagnostics == NULL ||
      (reporter->config.application_payload_size > 0U &&
       application_payload == NULL) ||
      diagnostics->stage_count > ESP_RT_DIAG_MAX_STAGES ||
      diagnostics->event_count > ESP_RT_DIAG_MAX_EVENTS ||
      diagnostics->interval_count > ESP_RT_DIAG_MAX_INTERVALS) {
    return ESP_ERR_INVALID_ARG;
  }

  /* Assemble an owned, aligned item before exposing it to the consumer. */
  memcpy(reporter->publish_item, diagnostics, sizeof(*diagnostics));
  if (reporter->config.application_payload_size > 0U) {
    memcpy(reporter->publish_item + reporter->payload_offset,
           application_payload, reporter->config.application_payload_size);
  }
  /* Queue length one makes freshness explicit: replace any pending old item. */
  return xQueueOverwrite(reporter->queue, reporter->publish_item) == pdPASS
             ? ESP_OK
             : ESP_FAIL;
}

/**
 * Destroy one reporter or unwind a partially created instance.
 *
 * Public destructor also called internally by create() on failures. Resources
 * are released in dependency order after stopping the consumer task.
 */
void esp_rt_diag_reporter_delete(esp_rt_diag_reporter_handle_t reporter) {
  if (reporter == NULL) {
    return;
  }
  /* Stop FreeRTOS users before releasing the queue and backing memory. */
  if (reporter->task != NULL) {
    vTaskDelete(reporter->task);
  }
  if (reporter->queue != NULL) {
    vQueueDelete(reporter->queue);
  }
  /* heap_caps_free(NULL) is valid, which simplifies partial-construction
   * unwind. */
  heap_caps_free(reporter->task_stack);
  heap_caps_free(reporter->queue_item_storage);
  heap_caps_free(reporter->deferred_item);
  heap_caps_free(reporter->receive_item);
  heap_caps_free(reporter->publish_item);
  heap_caps_free(reporter);
}
