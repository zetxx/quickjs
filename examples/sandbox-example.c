/*
 * QuickJS Sandbox Example
 *
 * Demonstrates how to use the sandboxed JavaScript runtime.
 *
 * Build:
 *   gcc -o sandbox-example sandbox-example.c quickjs-sandbox.c quickjs.c \
 *       libregexp.c libunicode.c cutils.c -lm -lpthread
 *
 * Or with CMake (add to CMakeLists.txt):
 *   add_executable(sandbox-example examples/sandbox-example.c quickjs-sandbox.c)
 *   target_link_libraries(sandbox-example qjs)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../quickjs-sandbox.h"

/* Example: Custom function exposed to JavaScript */
static JSValue js_my_custom_function(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    printf("[C] my_custom_function called with %d arguments\n", argc);

    if (argc > 0) {
        const char *str = JS_ToCString(ctx, argv[0]);
        if (str) {
            printf("[C] First argument: %s\n", str);
            JS_FreeCString(ctx, str);
        }
    }

    return JS_NewString(ctx, "Hello from C!");
}

/* Example: Custom print handler */
static void my_print_handler(const char *str, void *opaque)
{
    const char *prefix = (const char *)opaque;
    printf("%s%s", prefix ? prefix : "", str);
}

int main(int argc, char **argv)
{
    JSSandbox *sb;
    int ret;

    printf("=== QuickJS Sandbox Example ===\n\n");

    /*
     * Example 1: Basic usage with default configuration
     */
    printf("--- Example 1: Basic Evaluation ---\n");
    {
        sb = js_sandbox_new();
        if (!sb) {
            fprintf(stderr, "Failed to create sandbox\n");
            return 1;
        }

        /* Simple evaluation */
        const char *code = "console.log('Hello from sandbox!'); 1 + 2";
        char *result = js_sandbox_eval_string(sb, code, strlen(code), "example1.js");
        if (result) {
            printf("Result: %s\n", result);
            free(result);
        } else {
            printf("Error: %s\n", js_sandbox_get_error(sb));
        }

        js_sandbox_free(sb);
    }
    printf("\n");

    /*
     * Example 2: Timer functionality
     */
    printf("--- Example 2: Timers ---\n");
    {
        sb = js_sandbox_new();

        const char *timer_code =
            "let count = 0;\n"
            "const id = setInterval(() => {\n"
            "    count++;\n"
            "    console.log('Tick', count);\n"
            "    if (count >= 3) {\n"
            "        clearInterval(id);\n"
            "        console.log('Done!');\n"
            "    }\n"
            "}, 100);\n"
            "console.log('Timer started');";

        ret = js_sandbox_eval(sb, timer_code, strlen(timer_code), "timers.js", NULL);
        if (ret < 0) {
            printf("Error: %s\n", js_sandbox_get_error(sb));
        } else {
            /* Run the event loop */
            js_sandbox_loop(sb);
        }

        js_sandbox_free(sb);
    }
    printf("\n");

    /*
     * Example 3: Custom configuration with memory limit
     */
    printf("--- Example 3: Memory Limited Sandbox ---\n");
    {
        JSSandboxConfig config = {
            .memory_limit = 1024 * 1024,  /* 1 MB */
            .stack_size = 256 * 1024,     /* 256 KB stack */
            .print_func = my_print_handler,
            .print_opaque = (void *)"[SANDBOX] ",
        };

        sb = js_sandbox_new_with_config(&config);

        const char *code = "print('Custom print handler!'); console.log('Also works');";
        js_sandbox_eval(sb, code, strlen(code), "custom.js", NULL);

        js_sandbox_free(sb);
    }
    printf("\n");

    /*
     * Example 4: Adding custom functions
     */
    printf("--- Example 4: Custom Functions ---\n");
    {
        sb = js_sandbox_new();

        /* Add a custom function */
        js_sandbox_add_function(sb, "myFunction", js_my_custom_function, 1);

        const char *code =
            "const result = myFunction('test argument');\n"
            "console.log('Got from C:', result);";

        js_sandbox_eval(sb, code, strlen(code), "custom-func.js", NULL);

        js_sandbox_free(sb);
    }
    printf("\n");

    /*
     * Example 5: Blocked features (should fail)
     */
    printf("--- Example 5: Blocked Features ---\n");
    {
        sb = js_sandbox_new();

        /* Try dynamic import (should fail) */
        printf("Attempting dynamic import...\n");
        const char *import_code =
            "async function test() {\n"
            "    try {\n"
            "        await import('./module.js');\n"
            "        return 'import succeeded (unexpected)';\n"
            "    } catch (e) {\n"
            "        return 'Blocked: ' + e.message;\n"
            "    }\n"
            "}\n"
            "test().then(console.log);\n";
        ret = js_sandbox_eval(sb, import_code, strlen(import_code), "import.js", NULL);
        if (ret < 0) {
            printf("  Eval error: %s\n", js_sandbox_get_error(sb));
            js_sandbox_clear_error(sb);
        } else {
            /* Run the event loop to get the result */
            js_sandbox_loop(sb);
        }

        /* Try eval() (should fail - disabled by default) */
        printf("Attempting eval()...\n");
        const char *eval_code =
            "try {\n"
            "    eval('1 + 1');\n"
            "    console.log('eval succeeded (unexpected)');\n"
            "} catch (e) {\n"
            "    console.log('Blocked:', e.message);\n"
            "}\n";
        js_sandbox_eval(sb, eval_code, strlen(eval_code), "eval-test.js", NULL);

        /* Try Function constructor (should fail - disabled by default) */
        printf("Attempting Function constructor...\n");
        const char *func_code =
            "try {\n"
            "    const fn = new Function('return 1 + 1');\n"
            "    console.log('Function constructor succeeded (unexpected)');\n"
            "} catch (e) {\n"
            "    console.log('Blocked:', e.message);\n"
            "}\n";
        js_sandbox_eval(sb, func_code, strlen(func_code), "func-test.js", NULL);

        js_sandbox_free(sb);
    }
    printf("\n");

    /*
     * Example 6: Using built-in features
     */
    printf("--- Example 6: Built-in Features ---\n");
    {
        sb = js_sandbox_new();

        const char *code =
            "// JSON\n"
            "const obj = { name: 'test', value: 42 };\n"
            "console.log('JSON:', JSON.stringify(obj));\n"
            "\n"
            "// Math\n"
            "console.log('Math.PI:', Math.PI);\n"
            "console.log('Math.sqrt(2):', Math.sqrt(2));\n"
            "\n"
            "// Date\n"
            "console.log('Date:', new Date().toISOString());\n"
            "\n"
            "// Base64\n"
            "const encoded = btoa('Hello, World!');\n"
            "console.log('Base64 encoded:', encoded);\n"
            "console.log('Base64 decoded:', atob(encoded));\n"
            "\n"
            "// Promise\n"
            "Promise.resolve('async value').then(v => console.log('Promise resolved:', v));\n";

        ret = js_sandbox_eval(sb, code, strlen(code), "builtins.js", NULL);
        if (ret < 0) {
            printf("Error: %s\n", js_sandbox_get_error(sb));
        }

        /* Process promise microtasks */
        js_sandbox_loop_once(sb);

        js_sandbox_free(sb);
    }
    printf("\n");

    /*
     * Example 7: Async/await with timers
     */
    printf("--- Example 7: Async/Await ---\n");
    {
        sb = js_sandbox_new();

        const char *code =
            "function sleep(ms) {\n"
            "    return new Promise(resolve => setTimeout(resolve, ms));\n"
            "}\n"
            "\n"
            "async function main() {\n"
            "    console.log('Starting async function...');\n"
            "    await sleep(50);\n"
            "    console.log('After 50ms');\n"
            "    await sleep(50);\n"
            "    console.log('After another 50ms');\n"
            "    return 'Async complete!';\n"
            "}\n"
            "\n"
            "main().then(result => console.log(result));";

        ret = js_sandbox_eval(sb, code, strlen(code), "async.js", NULL);
        if (ret < 0) {
            printf("Error: %s\n", js_sandbox_get_error(sb));
        } else {
            js_sandbox_loop(sb);
        }

        js_sandbox_free(sb);
    }
    printf("\n");

    printf("=== All examples completed ===\n");
    return 0;
}
