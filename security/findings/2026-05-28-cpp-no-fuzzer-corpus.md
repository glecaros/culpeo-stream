## Finding: C++ fuzzer has no corpus or seed files

**Severity:** Medium
**Target:** C++
**Phase:** Phase 1

### Description

The fuzz directory (`implementations/cpp/libculpeo-frame/fuzz/`) contains only the harness file `frame_parser_fuzz.cpp`. There is no `corpus/` subdirectory and no seed files of any kind:

```
fuzz/
  frame_parser_fuzz.cpp   ← harness only
  (no corpus/, no seeds)
```

The security agent instructions and `security/required-security-tests.md` explicitly require a fuzzer corpus that includes:
- Truncated frames (no `\r\n\r\n` terminator at all)
- Overlength headers (header block exceeding `max_header_block_bytes`)
- Null bytes in header names and values
- Binary frames with no `\r\n\r\n` terminator
- Frames with 10,000 headers (or as many as fit within the block limit)

Without any seeds, libFuzzer begins from random mutation of empty/random inputs. Structured inputs like CulpeoStream frames, which require specific ASCII header syntax followed by a CRLF-CRLF terminator, are statistically unlikely to emerge from random byte flipping in a reasonable time budget. A seed corpus guides the fuzzer to explore structurally valid and near-valid inputs, which is where the interesting parser edge cases live.

### Attack Scenario

1. A parser bug exists in a path reachable only when a frame has, say, a header block exactly at the size limit with the terminator straddling the boundary.
2. The CI fuzzer run runs for its allotted time (say, 60 seconds) but never generates a frame that reaches this path because it has no seeds close enough to the target input space.
3. The bug ships, and a targeted crafted input triggers it in production.

### Impact

Without seeds, the fuzzer provides much weaker coverage guarantees than intended. The security testing gate that the corpus is meant to fulfill cannot be confirmed as passing. Any latent parser vulnerability in paths requiring structured input is unlikely to be discovered before release.

### Proposed Mitigation

1. Create `implementations/cpp/libculpeo-frame/fuzz/corpus/` and add the following seed files (binary content described inline):

   - `seed_truncated` — valid header line with no CRLF-CRLF: `Event: culpeo.init\r\n`
   - `seed_overlength_header_block` — header value of exactly 8193 bytes (one over the default limit), terminated with `\r\n\r\n`
   - `seed_null_in_name` — `Eve\x00nt: test\r\n\r\n`
   - `seed_null_in_value` — `Event: culpeo.\x00init\r\n\r\n`
   - `seed_no_terminator_binary` — 100 random bytes with no CRLF-CRLF sequence
   - `seed_many_headers` — frame with 70 unique minimal headers (`A0: x\r\n` through `A9: x\r\nB0: x\r\n`…) terminating with `\r\n\r\n`
   - `seed_valid_control` — a complete valid `culpeo.init` frame (gives the fuzzer a working baseline to mutate from)
   - `seed_valid_pcm_content_type` — a valid `audio/pcm; rate=16000; channels=1; bits=16` content-type header
   - `seed_empty_body` — valid headers with empty body

2. Update the `CMakeLists.txt` fuzz target to document the corpus directory location.

3. Consider adding a CI step that runs the fuzzer against the corpus for a minimum of 30 seconds (smoke test) and fails if any crash is found.

### Spec Reference

C++ implementation checklist: "Fuzzer corpus includes: truncated frames, overlength headers, null bytes, binary frames with no `\r\n\r\n`, frames with 10,000 headers"
`security/required-security-tests.md` — ST-PARSER-02 through ST-PARSER-05
