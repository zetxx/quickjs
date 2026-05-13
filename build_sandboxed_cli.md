# QuickJS Sandboxed CLI

A restricted JavaScript interpreter with security restrictions for running untrusted code.

## Build

```bash
# Configure
cmake -B build

# Build the sandboxed CLI
cmake --build build --target qjs-sandbox-cli

# The binary will be at: ./build/qjs-sandbox-cli
```

## Usage

```bash
# Run a script file
./build/qjs-sandbox-cli script.js

# Evaluate code directly
./build/qjs-sandbox-cli -e "console.log('Hello!')"

# Read from stdin
echo "print(1 + 2)" | ./build/qjs-sandbox-cli -

# With arguments passed to script
./build/qjs-sandbox-cli script.js arg1 arg2
# Access via: scriptArgs[0], scriptArgs[1], etc.
```

## Options

| Option | Description |
|--------|-------------|
| `-e CODE` | Evaluate CODE directly |
| `-m LIMIT` | Set memory limit (e.g., `1M`, `100K`, `1G`) |
| `-t MS` | Set max execution time in milliseconds |
| `--allow-eval` | Allow `eval()` and `Function` constructor |
| `--allow-timers` | Allow `setInterval` (setTimeout is allowed by default) |
| `--allow-promise` | Allow `Promise`/`async`/`await` |
| `--allow-regexp` | Allow `RegExp` |
| `--allow-proxy` | Allow `Proxy`/`Reflect` |
| `--no-timers` | Disable all timers (`setTimeout` and `setInterval`) |
| `-h, --help` | Show help |

## Security Restrictions

### Always Blocked (cannot be enabled)

- Filesystem access (no file read/write)
- OS module (no `os.exec`, `os.remove`, etc.)
- Dynamic imports (`import()` always fails)
- Static imports (`import x from 'y'`)
- Native module loading (`.so`/`.dll` files)
- Network access

### Blocked by Default (can be enabled with flags)

| Feature | Flag to Enable |
|---------|----------------|
| `eval()` | `--allow-eval` |
| `Function` constructor | `--allow-eval` |
| `setInterval` | `--allow-timers` |
| `Promise` / `async` / `await` | `--allow-promise` |
| `RegExp` | `--allow-regexp` |
| `Proxy` / `Reflect` | `--allow-proxy` |

### Enabled by Default

- `console.log` / `console.warn` / `console.error` / `console.info` / `console.debug`
- `print()`
- `setTimeout` / `clearTimeout`
- `JSON.parse` / `JSON.stringify`
- `Math.*`
- `Date`
- `atob` / `btoa`
- `ArrayBuffer` / `TypedArrays` (`Uint8Array`, etc.)
- `Map` / `Set` / `WeakMap` / `WeakSet`
- `BigInt`
- `Symbol`

## Examples

### Basic computation

```bash
./build/qjs-sandbox-cli -e "console.log(Math.sqrt(2))"
```

### Using setTimeout

```bash
./build/qjs-sandbox-cli -e "
setTimeout(() => console.log('Hello after 1 second'), 1000);
"
```

### Using setInterval (requires --allow-timers)

```bash
./build/qjs-sandbox-cli --allow-timers -e "
let count = 0;
const id = setInterval(() => {
    console.log('tick', ++count);
    if (count >= 3) clearInterval(id);
}, 100);
"
```

### Using Promise (requires --allow-promise)

```bash
./build/qjs-sandbox-cli --allow-promise -e "
Promise.resolve('Hello from Promise!')
    .then(msg => console.log(msg));
"
```

### Using RegExp (requires --allow-regexp)

```bash
./build/qjs-sandbox-cli --allow-regexp -e "
console.log(/hello/.test('hello world'));
"
```

### Using Proxy/Reflect (requires --allow-proxy)

```bash
./build/qjs-sandbox-cli --allow-proxy -e "
const obj = new Proxy({}, {
    get: (target, prop) => 'intercepted'
});
console.log(obj.anything);
"
```

### Memory limit

```bash
./build/qjs-sandbox-cli -m 1M -e "
const arr = [];
for (let i = 0; i < 1000000; i++) {
    arr.push(new Array(100));
}
"
# Will fail when memory limit is exceeded
```

### Blocked features demonstration

```bash
# eval() is blocked
./build/qjs-sandbox-cli -e "eval('1+1')"
# ReferenceError: eval is not defined

# Function constructor is blocked
./build/qjs-sandbox-cli -e "new Function('return 1')()"
# ReferenceError: Function is not defined

# Dynamic import is blocked
./build/qjs-sandbox-cli --allow-promise -e "import('./module.js').catch(e => console.log(e.message))"
# dynamic import is disabled in sandbox: './module.js'

# Promise is blocked by default
./build/qjs-sandbox-cli -e "Promise.resolve(1)"
# ReferenceError: Promise is not defined

# RegExp is blocked by default
./build/qjs-sandbox-cli -e "/test/"
# SyntaxError: RegExp are not supported

# Proxy is blocked by default
./build/qjs-sandbox-cli -e "new Proxy({}, {})"
# ReferenceError: Proxy is not defined

# Reflect is blocked by default
./build/qjs-sandbox-cli -e "Reflect.has({}, 'a')"
# ReferenceError: Reflect is not defined
```

## Using the Sandbox Library in Your Own Code

You can also use the sandbox as a library in your C code:

```c
#include "quickjs-sandbox.h"

int main() {
    // Create sandbox with default restrictions
    JSSandbox *sb = js_sandbox_new();
    
    // Or with custom configuration
    JSSandboxConfig config = {
        .memory_limit = 1024 * 1024,  // 1 MB
        .disable_eval = 1,
        .disable_function_constructor = 1,
        .disable_setTimeout = 0,
        .disable_setInterval = 1,
        .disable_promise = 1,
        .disable_regexp = 1,
        .disable_proxy = 1,
    };
    JSSandbox *sb = js_sandbox_new_with_config(&config);
    
    // Evaluate code
    const char *code = "console.log('Hello from sandbox!')";
    js_sandbox_eval(sb, code, strlen(code), "script.js", NULL);
    
    // Run event loop (for timers)
    js_sandbox_loop(sb);
    
    // Cleanup
    js_sandbox_free(sb);
    return 0;
}
```

Compile with:

```bash
gcc -o myprogram myprogram.c \
    -I/path/to/quickjs \
    -L/path/to/quickjs/build \
    -lqjs-sandbox -lqjs \
    -lm -lpthread
```

## Files

| File | Description |
|------|-------------|
| `quickjs-sandbox.h` | Public API header |
| `quickjs-sandbox.c` | Sandbox implementation |
| `qjs-sandbox-cli.c` | Command-line interface |
| `examples/sandbox-example.c` | Example usage |
