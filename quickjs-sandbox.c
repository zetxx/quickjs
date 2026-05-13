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

#include "quickjs-sandbox.h"
#include "quickjs.h"
#include "cutils.h"
#include "list.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/time.h>
#include <unistd.h>
#endif

/*
 * Internal data structures
 */

#ifndef MAX_SAFE_INTEGER
#define MAX_SAFE_INTEGER (((int64_t)1 << 53) - 1)
#endif

/* Timer entry */
typedef struct JSSandboxTimer {
    struct list_head link;
    int64_t timer_id;
    uint8_t repeats : 1;
    int64_t timeout;  /* absolute time in ms */
    int64_t delay;    /* interval for repeating timers */
    JSValue func;
} JSSandboxTimer;

/* Sandbox state */
struct JSSandbox {
    JSRuntime *rt;
    JSContext *ctx;

    /* Timer management */
    struct list_head timers;
    int64_t next_timer_id;

    /* Configuration */
    JSSandboxConfig config;

    /* Error handling */
    char *last_error;

    /* Interrupt flag */
    volatile int interrupted;
};

/*
 * Time utilities
 */

static uint64_t sandbox_hrtime_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER counter, frequency;
    double scaled_freq, result;

    if (!QueryPerformanceFrequency(&frequency))
        return 0;
    if (!QueryPerformanceCounter(&counter))
        return 0;

    scaled_freq = (double)frequency.QuadPart / 1000000000.0;
    result = (double)counter.QuadPart / scaled_freq;
    return (uint64_t)result;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return 0;
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
#endif
}

static uint64_t sandbox_hrtime_ms(void)
{
    return sandbox_hrtime_ns() / 1000000ULL;
}

/*
 * Timer implementation
 */

static void sandbox_free_timer(JSRuntime *rt, JSSandboxTimer *th)
{
    list_del(&th->link);
    JS_FreeValueRT(rt, th->func);
    js_free_rt(rt, th);
}

static JSSandboxTimer *sandbox_find_timer_by_id(JSSandbox *sb, int64_t timer_id)
{
    struct list_head *el;
    if (timer_id <= 0)
        return NULL;
    list_for_each(el, &sb->timers) {
        JSSandboxTimer *th = list_entry(el, JSSandboxTimer, link);
        if (th->timer_id == timer_id)
            return th;
    }
    return NULL;
}

/* setTimeout/setInterval implementation */
static JSValue js_sandbox_setTimeout(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv, int magic)
{
    JSSandbox *sb = JS_GetContextOpaque(ctx);
    int64_t delay;
    JSValueConst func;
    JSSandboxTimer *th;

    if (!sb)
        return JS_ThrowInternalError(ctx, "sandbox not initialized");

    func = argv[0];
    if (!JS_IsFunction(ctx, func))
        return JS_ThrowTypeError(ctx, "first argument must be a function");

    if (argc < 2) {
        delay = 0;
    } else if (JS_ToInt64(ctx, &delay, argv[1])) {
        return JS_EXCEPTION;
    }

    if (delay < 1)
        delay = 1;

    th = js_mallocz(ctx, sizeof(*th));
    if (!th)
        return JS_EXCEPTION;

    th->timer_id = sb->next_timer_id++;
    if (sb->next_timer_id > MAX_SAFE_INTEGER)
        sb->next_timer_id = 1;

    th->repeats = (magic > 0);  /* magic=1 for setInterval */
    th->timeout = sandbox_hrtime_ms() + delay;
    th->delay = delay;
    th->func = JS_DupValue(ctx, func);

    list_add_tail(&th->link, &sb->timers);

    return JS_NewInt64(ctx, th->timer_id);
}

/* clearTimeout/clearInterval implementation */
static JSValue js_sandbox_clearTimeout(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    JSSandbox *sb = JS_GetContextOpaque(ctx);
    JSSandboxTimer *th;
    int64_t timer_id;

    if (!sb)
        return JS_ThrowInternalError(ctx, "sandbox not initialized");

    if (JS_ToInt64(ctx, &timer_id, argv[0]))
        return JS_EXCEPTION;

    th = sandbox_find_timer_by_id(sb, timer_id);
    if (th)
        sandbox_free_timer(sb->rt, th);

    return JS_UNDEFINED;
}

/*
 * Console implementation
 */

static JSValue js_sandbox_print_internal(JSContext *ctx, int argc, JSValueConst *argv,
                                         FILE *fp, const char *prefix)
{
    JSSandbox *sb = JS_GetContextOpaque(ctx);
    const char *str;
    int i;

    if (prefix && fp) {
        fprintf(fp, "%s", prefix);
    }

    for (i = 0; i < argc; i++) {
        if (i > 0) {
            if (sb && sb->config.print_func) {
                sb->config.print_func(" ", sb->config.print_opaque);
            } else if (fp) {
                fputc(' ', fp);
            }
        }

        str = JS_ToCString(ctx, argv[i]);
        if (str) {
            if (sb && sb->config.print_func) {
                sb->config.print_func(str, sb->config.print_opaque);
            } else if (fp) {
                fprintf(fp, "%s", str);
            }
            JS_FreeCString(ctx, str);
        }
    }

    if (sb && sb->config.print_func) {
        sb->config.print_func("\n", sb->config.print_opaque);
    } else if (fp) {
        fputc('\n', fp);
        fflush(fp);
    }

    return JS_UNDEFINED;
}

static JSValue js_sandbox_print(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    return js_sandbox_print_internal(ctx, argc, argv, stdout, NULL);
}

static JSValue js_sandbox_console_log(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    return js_sandbox_print_internal(ctx, argc, argv, stdout, NULL);
}

static JSValue js_sandbox_console_warn(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    return js_sandbox_print_internal(ctx, argc, argv, stderr, "Warning: ");
}

static JSValue js_sandbox_console_error(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv)
{
    return js_sandbox_print_internal(ctx, argc, argv, stderr, "Error: ");
}

/*
 * Module loader that blocks all dynamic imports
 */

static JSModuleDef *js_sandbox_module_loader(JSContext *ctx,
                                             const char *module_name,
                                             void *opaque)
{
    JS_ThrowReferenceError(ctx, "dynamic import is disabled in sandbox: '%s'",
                           module_name);
    return NULL;
}

/*
 * Interrupt handler
 */

static int js_sandbox_interrupt_handler(JSRuntime *rt, void *opaque)
{
    JSSandbox *sb = opaque;
    return sb->interrupted;
}

/*
 * Error handling
 */

static void sandbox_set_error(JSSandbox *sb, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    if (sb->last_error) {
        free(sb->last_error);
        sb->last_error = NULL;
    }

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    sb->last_error = strdup(buf);
}

static void sandbox_capture_exception(JSSandbox *sb)
{
    JSValue exception_val;
    const char *str;

    exception_val = JS_GetException(sb->ctx);
    str = JS_ToCString(sb->ctx, exception_val);
    if (str) {
        sandbox_set_error(sb, "%s", str);
        JS_FreeCString(sb->ctx, str);
    } else {
        sandbox_set_error(sb, "Unknown error");
    }

    /* Also capture stack trace if available */
    if (JS_IsError(exception_val)) {
        JSValue stack = JS_GetPropertyStr(sb->ctx, exception_val, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *stack_str = JS_ToCString(sb->ctx, stack);
            if (stack_str) {
                /* Append stack trace to error */
                char *new_error = malloc(strlen(sb->last_error) + strlen(stack_str) + 3);
                if (new_error) {
                    sprintf(new_error, "%s\n%s", sb->last_error, stack_str);
                    free(sb->last_error);
                    sb->last_error = new_error;
                }
                JS_FreeCString(sb->ctx, stack_str);
            }
        }
        JS_FreeValue(sb->ctx, stack);
    }

    JS_FreeValue(sb->ctx, exception_val);
}

/*
 * Public API implementation
 */

JSSandbox *js_sandbox_new(void)
{
    /* Default config: maximum restrictions for security */
    JSSandboxConfig config = {
        .memory_limit = 0,
        .stack_size = 0,
        .max_execution_time_ms = 0,
        .print_func = NULL,
        .print_opaque = NULL,
        .disable_eval = 1,                  /* Disabled by default */
        .disable_function_constructor = 1,  /* Disabled by default */
        .disable_setTimeout = 0,            /* Enabled by default */
        .disable_setInterval = 1,           /* Disabled by default */
        .disable_promise = 1,               /* Disabled by default */
        .disable_date = 0,                  /* Enabled by default */
        .disable_regexp = 1,                /* Disabled by default */
        .disable_proxy = 1,                 /* Disabled by default */
        .disable_map_set = 0,               /* Enabled by default */
        .disable_typed_arrays = 0,          /* Enabled by default */
        .disable_bigint = 0,                /* Enabled by default */
        .disable_atob = 0,                  /* Enabled by default */
    };
    return js_sandbox_new_with_config(&config);
}

JSSandbox *js_sandbox_new_with_config(const JSSandboxConfig *config)
{
    JSSandbox *sb;
    JSRuntime *rt;
    JSContext *ctx;
    JSValue global_obj, console;

    sb = calloc(1, sizeof(*sb));
    if (!sb)
        return NULL;

    /* Copy configuration */
    if (config) {
        sb->config = *config;
    }

    /* Create runtime */
    rt = JS_NewRuntime();
    if (!rt) {
        free(sb);
        return NULL;
    }
    sb->rt = rt;

    /* Apply memory limit */
    if (sb->config.memory_limit > 0) {
        JS_SetMemoryLimit(rt, sb->config.memory_limit);
    }

    /* Apply stack size */
    if (sb->config.stack_size > 0) {
        JS_SetMaxStackSize(rt, sb->config.stack_size);
    }

    /* Set interrupt handler */
    JS_SetInterruptHandler(rt, js_sandbox_interrupt_handler, sb);

    /* Block dynamic imports - set a module loader that always fails */
    JS_SetModuleLoaderFunc(rt, NULL, js_sandbox_module_loader, sb);

    /* Create raw context and add only selected intrinsics */
    ctx = JS_NewContextRaw(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        free(sb);
        return NULL;
    }

    /* Always add base objects (Object, Array, String, Number, Boolean, etc.) */
    if (JS_AddIntrinsicBaseObjects(ctx)) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        free(sb);
        return NULL;
    }

    /* Always add eval intrinsic - needed for JS_Eval to work
     * We'll delete the global eval() function later if disabled */
    JS_AddIntrinsicEval(ctx);

    /* Conditionally add intrinsics */
    if (!sb->config.disable_date) {
        JS_AddIntrinsicDate(ctx);
    }

    if (!sb->config.disable_regexp) {
        JS_AddIntrinsicRegExp(ctx);
    }

    /* JSON is always useful and safe */
    JS_AddIntrinsicJSON(ctx);

    if (!sb->config.disable_proxy) {
        JS_AddIntrinsicProxy(ctx);
    }

    if (!sb->config.disable_map_set) {
        JS_AddIntrinsicMapSet(ctx);
    }

    if (!sb->config.disable_typed_arrays) {
        JS_AddIntrinsicTypedArrays(ctx);
    }

    if (!sb->config.disable_promise) {
        JS_AddIntrinsicPromise(ctx);
    }

    if (!sb->config.disable_bigint) {
        JS_AddIntrinsicBigInt(ctx);
    }

    if (!sb->config.disable_atob) {
        JS_AddIntrinsicAToB(ctx);
    }
    if (!ctx) {
        JS_FreeRuntime(rt);
        free(sb);
        return NULL;
    }
    sb->ctx = ctx;

    /* Store sandbox pointer in context for callbacks */
    JS_SetContextOpaque(ctx, sb);

    /* Initialize timer list */
    init_list_head(&sb->timers);
    sb->next_timer_id = 1;

    /* Set up whitelisted globals */
    global_obj = JS_GetGlobalObject(ctx);

    /* console object */
    console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_sandbox_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "warn",
                      JS_NewCFunction(ctx, js_sandbox_console_warn, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunction(ctx, js_sandbox_console_error, "error", 1));
    JS_SetPropertyStr(ctx, console, "info",
                      JS_NewCFunction(ctx, js_sandbox_console_log, "info", 1));
    JS_SetPropertyStr(ctx, console, "debug",
                      JS_NewCFunction(ctx, js_sandbox_console_log, "debug", 1));
    JS_SetPropertyStr(ctx, global_obj, "console", console);

    /* print function */
    JS_SetPropertyStr(ctx, global_obj, "print",
                      JS_NewCFunction(ctx, js_sandbox_print, "print", 1));

    /* Timer functions - conditionally enabled */
    if (!sb->config.disable_setTimeout) {
        JS_SetPropertyStr(ctx, global_obj, "setTimeout",
                          JS_NewCFunctionMagic(ctx, js_sandbox_setTimeout, "setTimeout", 2,
                                               JS_CFUNC_generic_magic, 0));
        JS_SetPropertyStr(ctx, global_obj, "clearTimeout",
                          JS_NewCFunction(ctx, js_sandbox_clearTimeout, "clearTimeout", 1));
    }

    if (!sb->config.disable_setInterval) {
        JS_SetPropertyStr(ctx, global_obj, "setInterval",
                          JS_NewCFunctionMagic(ctx, js_sandbox_setTimeout, "setInterval", 2,
                                               JS_CFUNC_generic_magic, 1));
        JS_SetPropertyStr(ctx, global_obj, "clearInterval",
                          JS_NewCFunction(ctx, js_sandbox_clearTimeout, "clearInterval", 1));
    }

    /*
     * Security: Disable dangerous functions by deleting from global object
     */

    /* Disable eval() */
    if (sb->config.disable_eval) {
        JSAtom atom = JS_NewAtom(ctx, "eval");
        JS_DeleteProperty(ctx, global_obj, atom, 0);
        JS_FreeAtom(ctx, atom);
    }

    /* Disable Function constructor */
    if (sb->config.disable_function_constructor) {
        JSAtom atom = JS_NewAtom(ctx, "Function");
        JS_DeleteProperty(ctx, global_obj, atom, 0);
        JS_FreeAtom(ctx, atom);
    }

    /* Disable Reflect (it's part of base objects, not Proxy intrinsic) */
    if (sb->config.disable_proxy) {
        JSAtom atom = JS_NewAtom(ctx, "Reflect");
        JS_DeleteProperty(ctx, global_obj, atom, 0);
        JS_FreeAtom(ctx, atom);
    }

    JS_FreeValue(ctx, global_obj);

    /*
     * Note: The following are already available as built-ins in QuickJS:
     * - JSON (JSON.parse, JSON.stringify)
     * - Math (all math functions)
     * - Date (Date object)
     * - atob/btoa (base64)
     * - ArrayBuffer, TypedArrays
     * - Map, Set, WeakMap, WeakSet
     * - Symbol
     */

    return sb;
}

void js_sandbox_free(JSSandbox *sb)
{
    struct list_head *el, *el1;

    if (!sb)
        return;

    /* Free all timers */
    list_for_each_safe(el, el1, &sb->timers) {
        JSSandboxTimer *th = list_entry(el, JSSandboxTimer, link);
        sandbox_free_timer(sb->rt, th);
    }

    /* Free error string */
    if (sb->last_error) {
        free(sb->last_error);
    }

    /* Free context and runtime */
    if (sb->ctx) {
        JS_FreeContext(sb->ctx);
    }
    if (sb->rt) {
        JS_FreeRuntime(sb->rt);
    }

    free(sb);
}

JSContext *js_sandbox_get_context(JSSandbox *sb)
{
    return sb ? sb->ctx : NULL;
}

JSRuntime *js_sandbox_get_runtime(JSSandbox *sb)
{
    return sb ? sb->rt : NULL;
}

int js_sandbox_eval(JSSandbox *sb, const char *code, size_t code_len,
                    const char *filename, JSValue *result)
{
    JSValue val;

    if (!sb || !code)
        return -1;

    if (!filename)
        filename = "<sandbox>";

    sb->interrupted = 0;

    /* Evaluate as global script (not module to prevent static imports) */
    val = JS_Eval(sb->ctx, code, code_len, filename, JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(val)) {
        sandbox_capture_exception(sb);
        JS_FreeValue(sb->ctx, val);
        if (result)
            *result = JS_UNDEFINED;
        return -1;
    }

    if (result) {
        *result = val;
    } else {
        JS_FreeValue(sb->ctx, val);
    }

    return 0;
}

char *js_sandbox_eval_string(JSSandbox *sb, const char *code, size_t code_len,
                             const char *filename)
{
    JSValue result;
    const char *str;
    char *ret = NULL;

    if (js_sandbox_eval(sb, code, code_len, filename, &result) < 0)
        return NULL;

    if (!JS_IsUndefined(result)) {
        str = JS_ToCString(sb->ctx, result);
        if (str) {
            ret = strdup(str);
            JS_FreeCString(sb->ctx, str);
        }
    }

    JS_FreeValue(sb->ctx, result);
    return ret;
}

/* Run timers and return the minimum delay until next timer */
static int sandbox_run_timers(JSSandbox *sb, int *min_delay)
{
    JSSandboxTimer *th;
    int64_t cur_time, delay;
    struct list_head *el;
    JSValue func, ret;

    if (list_empty(&sb->timers)) {
        *min_delay = -1;
        return 0;
    }

    cur_time = sandbox_hrtime_ms();
    *min_delay = INT32_MAX;

    list_for_each(el, &sb->timers) {
        th = list_entry(el, JSSandboxTimer, link);
        delay = th->timeout - cur_time;

        if (delay > 0) {
            if (delay < *min_delay)
                *min_delay = (int)delay;
        } else {
            /* Timer expired, execute callback */
            *min_delay = 0;
            func = JS_DupValue(sb->ctx, th->func);

            if (th->repeats) {
                th->timeout = cur_time + th->delay;
            } else {
                sandbox_free_timer(sb->rt, th);
            }

            ret = JS_Call(sb->ctx, func, JS_UNDEFINED, 0, NULL);
            JS_FreeValue(sb->ctx, func);

            if (JS_IsException(ret)) {
                sandbox_capture_exception(sb);
                JS_FreeValue(sb->ctx, ret);
                return -1;
            }
            JS_FreeValue(sb->ctx, ret);

            /* Return after executing one timer to allow job processing */
            return 0;
        }
    }

    return 0;
}

int js_sandbox_loop_once(JSSandbox *sb)
{
    JSContext *ctx1;
    int err, min_delay;

    if (!sb)
        return -2;

    /* Execute all pending jobs (microtasks) */
    for (;;) {
        err = JS_ExecutePendingJob(sb->rt, &ctx1);
        if (err < 0) {
            sandbox_capture_exception(sb);
            return -2;
        }
        if (err == 0)
            break;
    }

    /* Run at most one expired timer */
    if (sandbox_run_timers(sb, &min_delay) < 0)
        return -2;

    /* Check if more work is pending */
    if (JS_IsJobPending(sb->rt))
        return 0;  /* More microtasks pending */

    if (min_delay == 0)
        return 0;  /* Timer ready to fire immediately */

    if (min_delay > 0)
        return min_delay;  /* Next timer delay in ms */

    return -1;  /* Idle, no pending work */
}

int js_sandbox_loop(JSSandbox *sb)
{
    int delay;

    if (!sb)
        return -1;

    while (!sb->interrupted) {
        delay = js_sandbox_loop_once(sb);

        if (delay == -2) {
            /* Error occurred */
            return -1;
        }

        if (delay == -1) {
            /* No more work */
            break;
        }

        if (delay > 0) {
            /* Sleep until next timer */
#if defined(_WIN32)
            Sleep(delay);
#else
            usleep(delay * 1000);
#endif
        }
    }

    return sb->interrupted ? -1 : 0;
}

int js_sandbox_has_pending_work(JSSandbox *sb)
{
    if (!sb)
        return 0;

    if (JS_IsJobPending(sb->rt))
        return 1;

    if (!list_empty(&sb->timers))
        return 1;

    return 0;
}

const char *js_sandbox_get_error(JSSandbox *sb)
{
    return sb ? sb->last_error : NULL;
}

void js_sandbox_clear_error(JSSandbox *sb)
{
    if (sb && sb->last_error) {
        free(sb->last_error);
        sb->last_error = NULL;
    }
}

int js_sandbox_add_function(JSSandbox *sb, const char *name,
                            JSCFunction *func, int argc)
{
    JSValue global_obj;

    if (!sb || !name || !func)
        return -1;

    global_obj = JS_GetGlobalObject(sb->ctx);
    JS_SetPropertyStr(sb->ctx, global_obj, name,
                      JS_NewCFunction(sb->ctx, func, name, argc));
    JS_FreeValue(sb->ctx, global_obj);

    return 0;
}

int js_sandbox_add_value(JSSandbox *sb, const char *name, JSValue val)
{
    JSValue global_obj;

    if (!sb || !name)
        return -1;

    global_obj = JS_GetGlobalObject(sb->ctx);
    JS_SetPropertyStr(sb->ctx, global_obj, name, val);
    JS_FreeValue(sb->ctx, global_obj);

    return 0;
}

void js_sandbox_interrupt(JSSandbox *sb)
{
    if (sb)
        sb->interrupted = 1;
}
