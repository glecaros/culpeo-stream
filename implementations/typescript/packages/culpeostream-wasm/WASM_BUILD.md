# WASM_BUILD.md — Rebuilding `dist/culpeo_parser.{js,wasm}` from Source

## What is committed vs generated

| File | Status | Why |
|---|---|---|
| `dist/culpeo_parser.wasm` | **Stub (8 bytes)** | Minimal valid WASM (magic + version, no functions). Allows CI to run without Emscripten. |
| `dist/culpeo_parser.js` | **Stub** | Emscripten glue stub. Returns sentinel so TypeScript fallback activates. |
| `c/culpeo_parser.c` | Source | The real C implementation. |
| `c/culpeo_parser.h` | Source | C API declaration. |

The stub artefacts ensure that `initWasm()` resolves `false` and the
pure-TypeScript fallback takes over transparently in all CI/CD environments.

---

## Prerequisites

Emscripten SDK **3.1.x** (tested with 3.1.74).  Any 3.x release should work.

---

## Option A — Docker (recommended, no local install)

```bash
cd implementations/typescript/packages/culpeostream-wasm

docker run --rm \
  -v "$(pwd)":/src \
  -w /src \
  emscripten/emsdk:3.1.74 \
  emcc c/culpeo_parser.c \
    -O2 \
    -s WASM=1 \
    -s "EXPORTED_FUNCTIONS=[\"_culpeo_parse_headers\",\"_culpeo_serialize_frame\",\"_malloc\",\"_free\"]" \
    -s "EXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\",\"HEAPU8\",\"getValue\",\"setValue\"]" \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s MODULARIZE=1 \
    -s EXPORT_NAME=createCulpeoParserModule \
    --no-entry \
    -o dist/culpeo_parser.js
```

This writes `dist/culpeo_parser.js` and `dist/culpeo_parser.wasm` into the
current directory (mounted as `/src` inside the container).

---

## Option B — Local Emscripten install

```bash
# 1. Install emsdk
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install 3.1.74
./emsdk activate 3.1.74
source ./emsdk_env.sh

# 2. Build
cd implementations/typescript/packages/culpeostream-wasm
make wasm
```

The `Makefile` at the package root wraps the `emcc` invocation.  It will exit
with an error if `emcc` is not on `$PATH`.

---

## Option C — CMake + emcmake

```bash
cd implementations/typescript/packages/culpeostream-wasm

emcmake cmake -S c -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# Output lands in dist/ per CMakeLists.txt RUNTIME_OUTPUT_DIRECTORY
```

---

## Verifying the build

```bash
# Check magic bytes (should print: 00 61 73 6d)
xxd dist/culpeo_parser.wasm | head -1

# Run the full test suite (stub replaced by real module)
npm test
```

After a successful real build `initWasm()` will return `true` during tests
and the WASM code path will be exercised.

---

## Emscripten version policy

Target: **Emscripten 3.1.x**.  The C code is C99 and uses only `<string.h>`,
`<stdint.h>`, `<stddef.h>` — no platform-specific headers.  Future Emscripten
upgrades are expected to be drop-in compatible.

---

## Memory model

The TypeScript wrapper allocates all WASM heap buffers via `_malloc` and frees
them via `_free` before each call returns.  No WASM heap memory is retained
between calls.  See `src/wasm-loader.ts` for the detailed allocation strategy.
