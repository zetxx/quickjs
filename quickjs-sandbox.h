/*
 * QuickJS Sandbox - Restricted JavaScript Runtime
 *
 * A minimal, secure JavaScript sandbox with:
 * - setTimeout/setInterval/clearTimeout/clearInterval
 * - console.log/warn/error
 * - print
 * - atob/btoa (built-in)
 * - JSON (built-in)
 * - Math (built-in)
 * - Date (built-in)
 *
 * Disabled by default (configurable):
 * - eval() function
 * - Function constructor
 *
 * Permanently disabled:
 * - Filesystem access
 * - OS module
 * - Dynamic imports
 * - Static imports (code runs as script, not module)
 * - Native module loading (.so/.dll)
 * - Network access
 *
 * Copyright (c) 2024 QuickJS-NG Authors
 * MIT License
 */
#ifndef QUICKJS_SANDBOX_H
#define QUICKJS_SANDBOX_H

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque sandbox state */
typedef struct JSSandbox JSSandbox;

/* Sandbox configuration */
typedef struct JSSandboxConfig {
    /* Memory limit in bytes (0 = no limit) */
    size_t memory_limit;
    /* Maximum stack size in bytes (0 = default) */
    size_t stack_size;
    /* Maximum execution time in milliseconds (0 = no limit) */
    uint32_t max_execution_time_ms;
    /* Custom print function (NULL = use default stdout) */
    void (*print_func)(const char *str, void *opaque);
    void *print_opaque;
    /* Disable eval() function (default: disabled/true for security) */
    int disable_eval;
    /* Disable Function constructor (default: disabled/true for security) */
    int disable_function_constructor;
    /* Disable setTimeout (default: enabled/false) */
    int disable_setTimeout;
    /* Disable setInterval (default: disabled/true - can cause infinite loops) */
    int disable_setInterval;
    /* Disable Promise (default: disabled/true) */
    int disable_promise;
    /* Disable Date (default: enabled/false) */
    int disable_date;
    /* Disable RegExp (default: enabled/false) */
    int disable_regexp;
    /* Disable Proxy/Reflect (default: enabled/false) */
    int disable_proxy;
    /* Disable Map/Set/WeakMap/WeakSet (default: enabled/false) */
    int disable_map_set;
    /* Disable TypedArrays/ArrayBuffer (default: enabled/false) */
    int disable_typed_arrays;
    /* Disable BigInt (default: enabled/false) */
    int disable_bigint;
    /* Disable atob/btoa (default: enabled/false) */
    int disable_atob;
} JSSandboxConfig;

/* Create a new sandbox with default configuration */
JSSandbox *js_sandbox_new(void);

/* Create a new sandbox with custom configuration */
JSSandbox *js_sandbox_new_with_config(const JSSandboxConfig *config);

/* Free the sandbox and all associated resources */
void js_sandbox_free(JSSandbox *sb);

/* Get the JSContext for advanced use (be careful!) */
JSContext *js_sandbox_get_context(JSSandbox *sb);

/* Get the JSRuntime for advanced use (be careful!) */
JSRuntime *js_sandbox_get_runtime(JSSandbox *sb);

/* Evaluate JavaScript code
 * Returns 0 on success, -1 on error
 * If result is not NULL, it will contain the result value (caller must free with JS_FreeValue)
 */
int js_sandbox_eval(JSSandbox *sb, const char *code, size_t code_len,
                    const char *filename, JSValue *result);

/* Evaluate JavaScript code and return string result
 * Returns newly allocated string on success (caller must free), NULL on error
 */
char *js_sandbox_eval_string(JSSandbox *sb, const char *code, size_t code_len,
                             const char *filename);

/* Run the event loop (process timers and pending jobs)
 * Returns:
 *   > 0: next timer delay in milliseconds
 *   0: more work pending immediately
 *   -1: idle, no pending work
 *   -2: error occurred
 */
int js_sandbox_loop_once(JSSandbox *sb);

/* Run the event loop until all work is complete
 * Returns 0 on success, -1 on error
 */
int js_sandbox_loop(JSSandbox *sb);

/* Check if the sandbox has pending work (timers, jobs) */
int js_sandbox_has_pending_work(JSSandbox *sb);

/* Get the last error message (returns NULL if no error) */
const char *js_sandbox_get_error(JSSandbox *sb);

/* Clear the last error */
void js_sandbox_clear_error(JSSandbox *sb);

/* Add a custom global function
 * This allows embedding applications to expose safe APIs to JavaScript
 */
int js_sandbox_add_function(JSSandbox *sb, const char *name,
                            JSCFunction *func, int argc);

/* Add a custom global value */
int js_sandbox_add_value(JSSandbox *sb, const char *name, JSValue val);

/* Interrupt execution (call from another thread or signal handler) */
void js_sandbox_interrupt(JSSandbox *sb);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QUICKJS_SANDBOX_H */
