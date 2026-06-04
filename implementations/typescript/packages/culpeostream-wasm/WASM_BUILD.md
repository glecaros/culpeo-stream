# WASM_BUILD.md — Rebuilding `dist/culpeo_parser.{js,wasm}` from Source

## What is committed vs generated

| File | Status | Why |
|---|---|---|
| `dist/culpeo_parser.wasm` | **Stub (8 bytes)** | Minimal valid WASM (magic + version, no functions). Allows CI to run without Emscripten. |
| `dist/culpeo_parser.js` | **Stub** | Emscripten glue stub. Returns sentinel so TypeScript fallback activates. |
| `c/culpeo_parser.h` | Reference header | Mirrors `libculpeo-message/include/culpeo/c_api.h`; documents the C ABI. |
| `libculpeo-message/src/c_api.cpp` | **Implementation** | `extern "C"` shim over the C++20 `culpeo::message` parser. |
| `libculpeo-message/src/message.cpp` | **Implementation** | Core C++20 parser/serializer. |

The stub artefacts ensure that `initWasm()` resolves `false` and the
pure-TypeScript fallback takes over transparently in all CI/CD environments.

> **Key behaviour change**: The C++ implementation does **not** lowercase header
> keys in the WASM module.  Keys are returned in their original case; the
> TypeScript wrapper in `src/wasm-loader.ts` calls `.toLowerCase()` on each key
> after decoding, so the observable API is identical to the previous behaviour.

---

## Prerequisites

Emscripten SDK **3.1.x** (tested with 3.1.74).  Any 3.x release should work.
**C++20 is required** (`-std=c++20`) because `libculpeo-message` uses C++20
features (concepts, `std::string_view`, structured bindings).

---

## Option A — Docker (recommended, no local install)

```bash
cd implementations/typescript/packages/culpeostream-wasm

docker run --rm \
  -v "$(pwd)/../../../..":/repo \
  -w /repo/implementations/typescript/packages/culpeostream-wasm \
  emscripten/emsdk:3.1.74 \
  em++ \
    ../../../../cpp/libculpeo-message/src/message.cpp \
    ../../../../cpp/libculpeo-message/src/c_api.cpp \
    -std=c++20 \
    -I../../../../cpp/libculpeo-message/include \
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
current directory (mounted as `/repo` inside the container).

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

The `Makefile` at the package root wraps the `em++` invocation.  It will exit
with an error if `em++` is not on `$PATH`.

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

Target: **Emscripten 3.1.x**.  The C++ code requires C++20 and uses only
standard library headers (`<string_view>`, `<span>`, `<algorithm>`, `<cstring>`).
Future Emscripten upgrades are expected to be drop-in compatible as long as
C++20 support is retained.

---

## Memory model

The TypeScript wrapper allocates all WASM heap buffers via `_malloc` and frees
them via `_free` before each call returns.  No WASM heap memory is retained
between calls.  See `src/wasm-loader.ts` for the detailed allocation strategy.

### Key case normalisation

The C++ parser (`libculpeo-message`) preserves original header key case —
it does not lowercase keys.  The TypeScript wrapper in `src/wasm-loader.ts`
calls `.toLowerCase()` on each decoded key string, so the result is identical
to the pure-TypeScript fallback (all lowercase keys).
