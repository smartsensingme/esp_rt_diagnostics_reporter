#ifndef ESP_RT_DIAGNOSTICS_REPORTER_H_
#define ESP_RT_DIAGNOSTICS_REPORTER_H_

#include "esp_err.h"
#include "esp_rt_diagnostics.h"
#include "freertos/FreeRTOS.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque, instance-specific reporter handle created by create(). */
typedef struct esp_rt_diag_reporter_context *esp_rt_diag_reporter_handle_t;

/** Classification describing whether earlier logging may have disturbed a
 * window. */
typedef enum {
  ESP_RT_DIAG_WINDOW_QUIET = 0,    /**< No reporter log preceded this window. */
  ESP_RT_DIAG_WINDOW_LOG_AFFECTED, /**< Reporter output may have perturbed it.
                                    */
} esp_rt_diag_window_class_t;

/** Amount of generic text emitted for each diagnostic snapshot. */
typedef enum {
  ESP_RT_DIAG_REPORT_FULL = 0, /**< Multiple descriptive generic log lines. */
  ESP_RT_DIAG_REPORT_COMPACT,  /**< One bounded generic line per snapshot. */
  ESP_RT_DIAG_REPORT_SILENT,   /**< No generic logs; callback still executes. */
} esp_rt_diag_report_format_t;

/**
 * Application presentation hook executed synchronously in the reporter task.
 *
 * Called internally by the reporter task after any generic output. The
 * diagnostics and payload pointers refer to reporter-owned copies and are valid
 * only until the callback returns. The callback may log or format data, but
 * should remain bounded so the one-slot queue does not continually overwrite
 * unpublished snapshots. It must not call esp_rt_diag_reporter_delete() for its
 * own instance.
 *
 * @param diagnostics Immutable completed diagnostic window.
 * @param application_payload Copied payload, or NULL when its configured size
 * is 0.
 * @param window_class Whether reporter logging may have perturbed this window.
 * @param context User pointer supplied in esp_rt_diag_reporter_config_t.
 */
typedef void (*esp_rt_diag_report_callback_t)(
    const esp_rt_diag_snapshot_t *diagnostics, const void *application_payload,
    esp_rt_diag_window_class_t window_class, void *context);

/**
 * Configuration shallow-copied for the reporter lifetime.
 *
 * Scalar fields are copied. All pointers remain owned by the application and
 * must stay valid until esp_rt_diag_reporter_delete() returns.
 */
typedef struct {
  /** Generic logger format; FULL is the zero-initialized default. */
  esp_rt_diag_report_format_t report_format;

  /** Bytes copied after every diagnostic snapshot; zero permits NULL payloads.
   */
  size_t application_payload_size;

  /** Persistent optional stage-label array indexed by the application's IDs. */
  const char *const *stage_names;
  /** Number of valid pointers in stage_names; may be smaller than snapshots. */
  size_t stage_name_count;
  /** Persistent optional event-label array indexed by the application's IDs. */
  const char *const *event_names;
  /** Number of valid pointers in event_names. */
  size_t event_name_count;
  /** Persistent optional interval-label array indexed by application IDs. */
  const char *const *interval_names;
  /** Number of valid pointers in interval_names. */
  size_t interval_name_count;

  /** Optional application-specific presentation performed after generic logs.
   */
  esp_rt_diag_report_callback_t report_callback;
  /** Opaque application pointer passed unchanged to report_callback. */
  void *callback_context;

  /** Static reporter-task stack size in bytes for ESP-IDF; must be nonzero. */
  uint32_t task_stack_size;
  /** FreeRTOS task priority in [0, configMAX_PRIORITIES). */
  UBaseType_t task_priority;
  /** Target core index, or tskNO_AFFINITY. */
  BaseType_t task_core;
} esp_rt_diag_reporter_config_t;

/**
 * Allocate a reporter, its one-slot overwrite queue, and presentation task.
 *
 * External API; not called internally. All owned buffers, queue storage, and
 * task stack are allocated from internal 8-bit RAM. The configuration is
 * shallow-copied. On success, delete() owns the returned handle.
 *
 * @param config Configuration read and shallow-copied before return; objects
 *        referenced by its pointer fields must remain valid for the lifetime.
 * @param reporter Destination handle, set to NULL before allocation begins.
 * @return ESP_OK; ESP_ERR_INVALID_ARG for invalid configuration;
 *         ESP_ERR_INVALID_SIZE on payload-size overflow; or ESP_ERR_NO_MEM if
 *         allocation, static queue creation, or task creation fails.
 * @note Call from ordinary task context, never concurrently with
 * publish/delete.
 */
esp_err_t
esp_rt_diag_reporter_create(const esp_rt_diag_reporter_config_t *config,
                            esp_rt_diag_reporter_handle_t *reporter);

/**
 * Copy the newest snapshot and optional payload without waiting for logging.
 *
 * External producer API; not called internally. Copies into reporter-owned
 * storage, then overwrites the queue's single pending item. Exactly one task
 * may publish to an instance because the staging buffer has no lock. This is a
 * task API, not an ISR API. Older pending data may be intentionally discarded.
 *
 * @param reporter Valid handle returned by create().
 * @param diagnostics Completed snapshot copied by value.
 * @param application_payload Source of exactly application_payload_size bytes;
 *        may be NULL only when that configured size is zero.
 * @return ESP_OK, ESP_ERR_INVALID_ARG for invalid inputs/counts, or ESP_FAIL if
 *         the FreeRTOS overwrite operation unexpectedly fails.
 */
esp_err_t
esp_rt_diag_reporter_publish(esp_rt_diag_reporter_handle_t reporter,
                             const esp_rt_diag_snapshot_t *diagnostics,
                             const void *application_payload);

/**
 * Stop the reporter task and release every resource allocated by create().
 *
 * External API; also called internally by create() to unwind partial setup.
 * NULL is accepted. Stop the publisher first; this function is unsynchronized,
 * discards queued/deferred reports, and must not be called by the reporter's
 * own callback or task. The handle becomes invalid when the call returns.
 */
void esp_rt_diag_reporter_delete(esp_rt_diag_reporter_handle_t reporter);

/**
 * Return stable English text for a window classification.
 *
 * Public utility called internally by both generic formats and externally by
 * application callbacks. Returns "log-affected" for that exact enum value and
 * "quiet" otherwise; the returned static string must not be freed.
 */
const char *
esp_rt_diag_window_class_name(esp_rt_diag_window_class_t window_class);

#ifdef __cplusplus
}
#endif

#endif /* ESP_RT_DIAGNOSTICS_REPORTER_H_ */
