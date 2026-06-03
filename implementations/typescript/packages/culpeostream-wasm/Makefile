# culpeostream-wasm — WASM build convenience wrapper
#
# Prerequisites: Emscripten SDK activated in the current shell
#   (run `source /path/to/emsdk/emsdk_env.sh` first)
#
# Alternatively use the Docker one-liner documented in WASM_BUILD.md.
#
# Targets:
#   wasm   — compile culpeo_parser.c → dist/culpeo_parser.{js,wasm}
#   clean  — remove build artefacts

EMCC        := emcc
SRC         := c/culpeo_parser.c
OUT_JS      := dist/culpeo_parser.js
OUT_WASM    := dist/culpeo_parser.wasm

EXPORTED_FUNCTIONS := \
  _culpeo_parse_headers,\
  _culpeo_serialize_frame,\
  _malloc,\
  _free

EMFLAGS := \
  -O2 \
  -s WASM=1 \
  -s "EXPORTED_FUNCTIONS=[\"_culpeo_parse_headers\",\"_culpeo_serialize_frame\",\"_malloc\",\"_free\"]" \
  -s "EXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\",\"HEAPU8\",\"getValue\",\"setValue\"]" \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=createCulpeoParserModule \
  --no-entry

.PHONY: wasm clean check-emcc

wasm: check-emcc $(OUT_JS)

$(OUT_JS): $(SRC) c/culpeo_parser.h
	@mkdir -p dist
	$(EMCC) $(SRC) $(EMFLAGS) -o $(OUT_JS)
	@echo "Built $(OUT_JS) and $(OUT_WASM)"

check-emcc:
	@command -v $(EMCC) >/dev/null 2>&1 || { \
	  echo "ERROR: emcc not found."; \
	  echo "See WASM_BUILD.md for Docker-based rebuild instructions."; \
	  exit 1; \
	}

clean:
	rm -f $(OUT_JS) $(OUT_WASM)
