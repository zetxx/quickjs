/*
 * QuickJS Sandboxed CLI
 *
 * A restricted JavaScript interpreter with:
 * - No filesystem access
 * - No OS module
 * - No dynamic imports
 * - No eval() or Function constructor
 * - Only safe APIs (console, timers, JSON, Math, Date, etc.)
 *
 * Usage:
 *   qjs-sandbox [options] [script.js] [args...]
 *   qjs-sandbox -e "code"
 *
 * Options:
 *   -e CODE      Evaluate CODE
 *   -m LIMIT     Set memory limit (e.g., 10M, 100K)
 *   -t MS        Set max execution time in milliseconds
 *   --allow-eval Allow eval() and Function constructor (not recommended)
 *   -h, --help   Show help
 *
 * Copyright (c) 2024 QuickJS-NG Authors
 * MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "quickjs-sandbox.h"

static JSSandbox *g_sandbox = NULL;

static void signal_handler(int sig)
{
    if (g_sandbox) {
        js_sandbox_interrupt(g_sandbox);
    }
}

static void print_help(const char *progname)
{
    printf("QuickJS Sandboxed CLI\n\n");
    printf("Usage: %s [options] [script.js] [args...]\n\n", progname);
    printf("Options:\n");
    printf("  -e CODE          Evaluate CODE directly\n");
    printf("  -m LIMIT         Set memory limit (e.g., 10M, 100K, 1G)\n");
    printf("  -t MS            Set max execution time in milliseconds\n");
    printf("  --allow-eval     Allow eval() and Function constructor (unsafe)\n");
    printf("  --allow-timers   Allow setInterval (setTimeout allowed by default)\n");
    printf("  --allow-promise  Allow Promise/async/await\n");
    printf("  --allow-regexp   Allow RegExp\n");
    printf("  --allow-proxy    Allow Proxy/Reflect\n");
    printf("  --no-timers      Disable all timers (setTimeout and setInterval)\n");
    printf("  -h, --help       Show this help\n");
    printf("\n");
    printf("Security restrictions (always enabled):\n");
    printf("  - No filesystem access\n");
    printf("  - No OS module\n");
    printf("  - No dynamic imports\n");
    printf("  - No native module loading\n");
    printf("\n");
    printf("Disabled by default:\n");
    printf("  - eval() / Function      (use --allow-eval to enable)\n");
    printf("  - setInterval            (use --allow-timers to enable)\n");
    printf("  - Promise / async / await(use --allow-promise to enable)\n");
    printf("  - RegExp                 (use --allow-regexp to enable)\n");
    printf("  - Proxy / Reflect        (use --allow-proxy to enable)\n");
    printf("\n");
    printf("Enabled by default:\n");
    printf("  - console.log/warn/error/info/debug\n");
    printf("  - print()\n");
    printf("  - setTimeout/clearTimeout\n");
    printf("  - JSON, Math, Date, atob/btoa\n");
    printf("  - ArrayBuffer, TypedArrays, Map, Set, BigInt\n");
}

static size_t parse_size(const char *str)
{
    char *end;
    double val = strtod(str, &end);
    
    if (end == str) {
        fprintf(stderr, "Invalid size: %s\n", str);
        return 0;
    }
    
    switch (*end) {
    case 'k': case 'K':
        val *= 1024;
        break;
    case 'm': case 'M':
        val *= 1024 * 1024;
        break;
    case 'g': case 'G':
        val *= 1024 * 1024 * 1024;
        break;
    case '\0':
    case 'b': case 'B':
        break;
    default:
        fprintf(stderr, "Invalid size suffix: %s\n", str);
        return 0;
    }
    
    return (size_t)val;
}

static char *read_file(const char *filename, size_t *out_len)
{
    FILE *f;
    char *buf;
    size_t len;
    
    if (strcmp(filename, "-") == 0) {
        f = stdin;
    } else {
        f = fopen(filename, "rb");
        if (!f) {
            perror(filename);
            return NULL;
        }
    }
    
    /* Read file into buffer */
    buf = NULL;
    len = 0;
    
    if (f == stdin) {
        /* Read stdin incrementally */
        size_t capacity = 4096;
        buf = malloc(capacity);
        if (!buf) {
            fprintf(stderr, "Out of memory\n");
            return NULL;
        }
        
        while (!feof(f)) {
            if (len + 4096 > capacity) {
                capacity *= 2;
                char *newbuf = realloc(buf, capacity);
                if (!newbuf) {
                    free(buf);
                    fprintf(stderr, "Out of memory\n");
                    return NULL;
                }
                buf = newbuf;
            }
            len += fread(buf + len, 1, 4096, f);
        }
    } else {
        /* Get file size */
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        buf = malloc(len + 1);
        if (!buf) {
            fclose(f);
            fprintf(stderr, "Out of memory\n");
            return NULL;
        }
        
        if (fread(buf, 1, len, f) != len) {
            fclose(f);
            free(buf);
            perror(filename);
            return NULL;
        }
        fclose(f);
    }
    
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

int main(int argc, char **argv)
{
    JSSandboxConfig config = {
        .memory_limit = 0,
        .stack_size = 0,
        .max_execution_time_ms = 0,
        .print_func = NULL,
        .print_opaque = NULL,
        .disable_eval = 1,
        .disable_function_constructor = 1,
        .disable_setTimeout = 0,
        .disable_setInterval = 1,
        .disable_promise = 1,
        .disable_date = 0,
        .disable_regexp = 1,
        .disable_proxy = 1,
        .disable_map_set = 0,
        .disable_typed_arrays = 0,
        .disable_bigint = 0,
        .disable_atob = 0,
    };
    
    const char *eval_code = NULL;
    const char *script_file = NULL;
    int script_arg_start = 0;
    int i;
    
    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            /* Script file */
            script_file = argv[i];
            script_arg_start = i + 1;
            break;
        }
        
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-e") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for -e\n");
                return 1;
            }
            eval_code = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for -m\n");
                return 1;
            }
            config.memory_limit = parse_size(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for -t\n");
                return 1;
            }
            config.max_execution_time_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--allow-eval") == 0) {
            config.disable_eval = 0;
            config.disable_function_constructor = 0;
        } else if (strcmp(argv[i], "--allow-timers") == 0) {
            config.disable_setTimeout = 0;
            config.disable_setInterval = 0;
        } else if (strcmp(argv[i], "--no-timers") == 0) {
            config.disable_setTimeout = 1;
            config.disable_setInterval = 1;
        } else if (strcmp(argv[i], "--allow-promise") == 0) {
            config.disable_promise = 0;
        } else if (strcmp(argv[i], "--allow-regexp") == 0) {
            config.disable_regexp = 0;
        } else if (strcmp(argv[i], "--allow-proxy") == 0) {
            config.disable_proxy = 0;
        } else if (strcmp(argv[i], "-") == 0) {
            /* Read from stdin */
            script_file = "-";
            script_arg_start = i + 1;
            break;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Use -h for help\n");
            return 1;
        }
    }
    
    /* Need either -e or script file */
    if (!eval_code && !script_file) {
        fprintf(stderr, "No input specified. Use -e CODE or provide a script file.\n");
        fprintf(stderr, "Use -h for help\n");
        return 1;
    }
    
    /* Create sandbox */
    g_sandbox = js_sandbox_new_with_config(&config);
    if (!g_sandbox) {
        fprintf(stderr, "Failed to create sandbox\n");
        return 1;
    }
    
    /* Set up signal handler for Ctrl+C */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Add scriptArgs global */
    if (script_arg_start > 0 && script_arg_start < argc) {
        JSContext *ctx = js_sandbox_get_context(g_sandbox);
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue args = JS_NewArray(ctx);
        
        for (int j = script_arg_start; j < argc; j++) {
            JS_SetPropertyUint32(ctx, args, j - script_arg_start,
                                 JS_NewString(ctx, argv[j]));
        }
        JS_SetPropertyStr(ctx, global, "scriptArgs", args);
        JS_FreeValue(ctx, global);
    }
    
    int ret = 0;
    
    if (eval_code) {
        /* Evaluate -e code */
        if (js_sandbox_eval(g_sandbox, eval_code, strlen(eval_code), "<cmdline>", NULL) < 0) {
            fprintf(stderr, "%s\n", js_sandbox_get_error(g_sandbox));
            ret = 1;
        }
    } else {
        /* Read and evaluate script file */
        size_t code_len;
        char *code = read_file(script_file, &code_len);
        if (!code) {
            js_sandbox_free(g_sandbox);
            return 1;
        }
        
        /* Skip shebang if present */
        const char *start = code;
        if (code_len >= 2 && code[0] == '#' && code[1] == '!') {
            while (*start && *start != '\n')
                start++;
            if (*start == '\n')
                start++;
            code_len -= (start - code);
        }
        
        const char *filename = (strcmp(script_file, "-") == 0) ? "<stdin>" : script_file;
        
        if (js_sandbox_eval(g_sandbox, start, code_len, filename, NULL) < 0) {
            fprintf(stderr, "%s\n", js_sandbox_get_error(g_sandbox));
            ret = 1;
        }
        
        free(code);
    }
    
    /* Run event loop for timers */
    if (ret == 0 && js_sandbox_has_pending_work(g_sandbox)) {
        if (js_sandbox_loop(g_sandbox) < 0) {
            const char *err = js_sandbox_get_error(g_sandbox);
            if (err) {
                fprintf(stderr, "%s\n", err);
            }
            ret = 1;
        }
    }
    
    js_sandbox_free(g_sandbox);
    g_sandbox = NULL;
    
    return ret;
}
