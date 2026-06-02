# [SEC-028] C++ H2 server ignores all HTTP/2 request headers — no `Authorization`, `Content-Type`, or `Culpeostream-Version` validation

**Severity:** Medium  
**Component:** C++  
**Phase:** 4  
**Status:** Open  

## Description

`H2Session::on_header_callback` discards every HTTP/2 request header without
inspection:

```cpp
int H2Session::on_header_callback(nghttp2_session* /*session*/,
                                   const nghttp2_frame* frame,
                                   const uint8_t* name, std::size_t /*namelen*/,
                                   const uint8_t* /*value*/, std::size_t /*valuelen*/,
                                   uint8_t /*flags*/,
                                   void* /*user_data*/)
{
    // We only care about :method / content-type for basic routing.
    // For now, accept any request.
    (void)frame; (void)name;
    return 0;
}
```

As a result:

1. **`Authorization` is never extracted.**  The application handler receives an
   `IAsyncTransport` with no method to retrieve the HTTP-level bearer token.
   Spec Addendum A says clients MUST send credentials; the H2 server silently
   ignores them.

2. **`Content-Type: application/culpeostream` is never validated.**  Any HTTP/2
   POST to the server endpoint is accepted, regardless of declared content type.

3. **`Culpeostream-Version` is never checked.**  A future protocol version bump
   would be silently accepted without negotiation.

The C# server (`CulpeoHttp2Server.cs`) extracts `Authorization` and passes it
to the handler, and validates both `Content-Type` and `Culpeostream-Version`
before dispatching.  The TypeScript server at minimum extracts `Authorization`.
The C++ server provides none of these.

## Impact

- **No HTTP-layer authentication surface:** Deployments that expect to enforce
  auth at the HTTP transport layer (e.g., via API gateway rules on the
  `Authorization` header) will not see any header to check.  Only frame-level
  auth inside `culpeo.init` remains, which relies entirely on correct
  application-layer implementation of `ISessionHandler`.

- **Reachable from any HTTP/2 client:** Any HTTP/2 client can send a POST and
  start exchanging frames — no `Content-Type` or version header is required,
  making the endpoint indistinguishable from a generic HTTP/2 handler when
  probed.

- **No version negotiation:** A client sending `Culpeostream-Version: 2.0` will
  be silently accepted, then encounter frame-level protocol errors later.

## Location

`implementations/cpp/libculpeo-transport-h2/src/h2_client.cpp`,
`H2Session::on_header_callback` (line ~181).

## Recommendation

Implement `on_header_callback` to:

1. Store the `:method`, `content-type`, `culpeostream-version`, and
   `authorization` header values in `StreamState`.
2. After `on_frame_recv_callback` receives the complete HEADERS frame (cat =
   `NGHTTP2_HCAT_REQUEST`), validate `content-type` and `culpeostream-version`
   and reject non-conforming requests with RST_STREAM error code
   `NGHTTP2_INTERNAL_ERROR` (or a 400 response before DATA begins).
3. Surface `authorization` through `IAsyncTransport` or pass it to the
   `ISessionHandler` via an extended callback signature so applications can
   authenticate at the transport layer.

Minimal example (store headers per stream):

```cpp
int H2Session::on_header_callback(nghttp2_session*, const nghttp2_frame* frame,
    const uint8_t* name, std::size_t namelen,
    const uint8_t* value, std::size_t valuelen,
    uint8_t, void* user_data)
{
    auto* self = static_cast<H2Session*>(user_data);
    std::string_view k(reinterpret_cast<const char*>(name), namelen);
    std::string_view v(reinterpret_cast<const char*>(value), valuelen);

    auto& st = self->get_or_create_stream(frame->hd.stream_id);
    if (k == "content-type")         st.content_type = std::string(v);
    else if (k == "culpeostream-version") st.proto_version = std::string(v);
    else if (k == "authorization")   st.authorization = std::string(v);
    return 0;
}
```

Then in `submit_server_response` validate `content_type` and `proto_version`
before sending the 200.
