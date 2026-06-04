# culpeostream-py

Python bindings for the CulpeoStream C++ core library (Phase 5).

## Installation

```bash
# From the monorepo root — build via CMake then install the wheel
cd implementations/cpp
cmake -B build-py -DCMAKE_BUILD_TYPE=Release
cmake --build build-py --target culpeostream
pip install build-py/culpeostream-py/culpeostream*.so

# Or install directly from source (requires scikit-build-core)
pip install -e .  # run from implementations/cpp/culpeostream-py/
```

## Running tests

```bash
cd implementations/cpp/culpeostream-py
pytest tests/ -v
```

## Architecture notes

See [`DECISIONS.md`](../DECISIONS.md) for detailed rationale on every design
choice, including the GIL policy and zero-copy deviation.
