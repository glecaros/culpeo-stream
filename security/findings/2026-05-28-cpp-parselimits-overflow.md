## Finding: C++ ParseLimits + 4U addition can overflow on extreme limit values

**Severity:** Low
**Target:** C++
**Phase:** Phase 1

### Description

In `parse_headers` (`frame.cpp`, line 183), the parser computes the search window for `\r\n\r\n` as:

```cpp
const auto search_length =
    frame.size() < (limits.max_header_block_bytes + 4U)
        ? frame.size()
        : (limits.max_header_block_bytes + 4U);
```

`limits.max_header_block_bytes` is a `std::size_t`. If a caller sets `max_header_block_bytes` to a value near `std::numeric_limits<std::size_t>::max()` (e.g., `SIZE_MAX - 2`), the expression `limits.max_header_block_bytes + 4U` wraps around to a value of 1–2, making the ternary evaluate `frame.size()` for almost all non-trivial inputs (because most `frame.size()` values will be greater than 1). The effective `search_length` then becomes `frame.size()`, bypassing the configured block-size limit entirely.

In practice:
- The default value is 8192, which is safe.
- The library is only vulnerable when a caller explicitly sets an extreme `ParseLimits` value.
- The consequence is that the size limit is **silently bypassed** rather than enforced, without any diagnostic.

### Attack Scenario

1. An operator mistakenly configures `ParseLimits{.max_header_block_bytes = SIZE_MAX}` intending to "disable" the limit.
2. The overflow causes `search_length` to wrap to 3, so the parser searches only the first 3 bytes of `frame` for `\r\n\r\n`.
3. Valid frames are now rejected with `missing_header_terminator`.
4. Alternatively, if the caller sets `max_header_block_bytes = SIZE_MAX - 3`, the addition wraps to 0, `search_length` becomes `min(frame.size(), 0) = 0`, and `find("\r\n\r\n", 0, 0)` always returns `npos`, so every frame is rejected.

This is a misconfiguration DoS rather than a remote attack, but the silent failure mode (no compile-time diagnostic, no runtime assertion) makes it a footgun.

### Impact

Silent misconfiguration failure. A developer who sets `max_header_block_bytes` to a very large value to accommodate unusually metadata-heavy frames will instead cause the parser to reject all frames. The error they receive is `missing_header_terminator`, which does not point to the actual root cause.

### Proposed Mitigation

Add an overflow-safe computation using a saturating add, or clamp the limit before use:

```cpp
// Option A: checked addition
const std::size_t kTerminatorLen = 4U;
const std::size_t safe_limit = (limits.max_header_block_bytes <= std::numeric_limits<std::size_t>::max() - kTerminatorLen)
    ? limits.max_header_block_bytes + kTerminatorLen
    : std::numeric_limits<std::size_t>::max();
const auto search_length = std::min(frame.size(), safe_limit);
```

Also add a `ParseLimits` validation helper or a `static_assert` / runtime check that rejects values near `SIZE_MAX`.

### Spec Reference

§4.1.1 — Parser Limits (specifies 8 KiB default; the overflow only manifests with non-default extreme values)
