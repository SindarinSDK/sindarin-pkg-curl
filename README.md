# sindarin-pkg-curl

An HTTP client for the [Sindarin](https://github.com/SindarinSDK/sindarin-compiler) programming language, backed by [libcurl](https://curl.se/libcurl/). Supports GET, POST, PUT, PATCH, and DELETE with persistent client state, custom headers, configurable timeouts, and **streaming uploads and downloads** for SSE / NDJSON / large-body workloads.

## Installation

Add the package as a dependency in your `sn.yaml`:

```yaml
dependencies:
- name: sindarin-pkg-curl
  git: git@github.com:SindarinSDK/sindarin-pkg-curl.git
  branch: main
```

Then run `sn --install` to fetch the package.

## Quick Start

```sindarin
import "sindarin-pkg-curl/src/curl"

fn main(): void =>
    var client: CurlClient = CurlClient.new()

    var resp: CurlResponse = client.get("https://api.example.com/users")
    print($"status: {resp.status()}\n")
    print(resp.body())
    resp.dispose()

    client.dispose()
```

---

## CurlClient

```sindarin
import "sindarin-pkg-curl/src/curl"
```

A reusable HTTP client. Custom headers and timeouts set on the client apply to every request it makes.

| Method | Signature | Description |
|--------|-----------|-------------|
| `new` | `static fn new(): CurlClient` | Create a new HTTP client |
| `get` | `fn get(url: str): CurlResponse` | Perform a GET request |
| `post` | `fn post(url: str, contentType: str, body: str): CurlResponse` | Perform a POST request |
| `put` | `fn put(url: str, contentType: str, body: str): CurlResponse` | Perform a PUT request |
| `patch` | `fn patch(url: str, contentType: str, body: str): CurlResponse` | Perform a PATCH request |
| `delete` | `fn delete(url: str): CurlResponse` | Perform a DELETE request |
| `setTimeout` | `fn setTimeout(ms: int): void` | Set request timeout in milliseconds (0 = no timeout) |
| `setHeader` | `fn setHeader(name: str, value: str): void` | Add a header sent with every request |
| `getStream` | `fn getStream(url: str, onChunk: fn(str): bool): CurlResponse` | GET, response body delivered chunk-by-chunk |
| `postStream` | `fn postStream(url: str, ct: str, body: str, onChunk: fn(str): bool): CurlResponse` | POST, streaming response body |
| `putStream` | `fn putStream(url: str, ct: str, body: str, onChunk: fn(str): bool): CurlResponse` | PUT, streaming response body |
| `patchStream` | `fn patchStream(url: str, ct: str, body: str, onChunk: fn(str): bool): CurlResponse` | PATCH, streaming response body |
| `deleteStream` | `fn deleteStream(url: str, onChunk: fn(str): bool): CurlResponse` | DELETE, streaming response body |
| `postChunked` | `fn postChunked(url: str, ct: str, produceChunk: fn(): str): CurlResponse` | POST, streaming request body |
| `putChunked` | `fn putChunked(url: str, ct: str, produceChunk: fn(): str): CurlResponse` | PUT, streaming request body |
| `patchChunked` | `fn patchChunked(url: str, ct: str, produceChunk: fn(): str): CurlResponse` | PATCH, streaming request body |
| `dispose` | `fn dispose(): void` | Free client resources |

```sindarin
var client: CurlClient = CurlClient.new()
client.setTimeout(5000)
client.setHeader("Authorization", "Bearer my-token")

// GET
var resp: CurlResponse = client.get("https://api.example.com/items")
print(resp.body())
resp.dispose()

// POST with JSON body
var created: CurlResponse = client.post(
    "https://api.example.com/items",
    "application/json",
    "{\"name\": \"widget\"}"
)
print($"created: {created.status()}\n")
created.dispose()

client.dispose()
```

---

## CurlResponse

The result of any HTTP request. Always call `dispose()` when done to free the response buffers.

| Method | Signature | Description |
|--------|-----------|-------------|
| `status` | `fn status(): int` | HTTP status code (e.g. `200`, `404`) |
| `body` | `fn body(): str` | Response body as a string |
| `headers` | `fn headers(): str` | All response headers as a raw string |
| `header` | `fn header(name: str): str` | Value of a named header (case-insensitive); empty string if absent |
| `dispose` | `fn dispose(): void` | Free response memory |

```sindarin
var resp: CurlResponse = client.get("https://api.example.com/data")

if resp.status() == 200 =>
    print(resp.body())
    print(resp.header("Content-Type"))

resp.dispose()
```

---

## Examples

### JSON API request

```sindarin
import "sindarin-pkg-curl/src/curl"

fn main(): void =>
    var client: CurlClient = CurlClient.new()
    client.setHeader("Accept", "application/json")

    var resp: CurlResponse = client.get("https://httpbin.org/json")
    print($"{resp.status()}: {resp.body()}\n")
    resp.dispose()

    client.dispose()
```

### PUT and DELETE

```sindarin
import "sindarin-pkg-curl/src/curl"

fn main(): void =>
    var client: CurlClient = CurlClient.new()
    client.setHeader("Authorization", "Bearer token")

    var updated: CurlResponse = client.put(
        "https://api.example.com/items/42",
        "application/json",
        "{\"name\": \"updated\"}"
    )
    print($"PUT: {updated.status()}\n")
    updated.dispose()

    var deleted: CurlResponse = client.delete("https://api.example.com/items/42")
    print($"DELETE: {deleted.status()}\n")
    deleted.dispose()

    client.dispose()
```

### Streaming a download (SSE / NDJSON / large bodies)

Each chunk arrives as a `str` for the duration of one libcurl write tick — typically one TCP read. Return `false` from the callback to abort the transfer early; the returned `CurlResponse` will still have `status()` and `headers()` populated, but `body()` is always empty (the bytes already went to your callback).

```sindarin
import "sindarin-pkg-curl/src/curl"

fn main(): void =>
    var client: CurlClient = CurlClient.new()

    var lineCount: int = 0
    var resp: CurlResponse = client.getStream("https://httpbin.org/stream/10", fn(chunk: str): bool =>
        lineCount += 1
        print(chunk)
        return true   # return false to abort
    )

    print($"status: {resp.status()}, chunks: {lineCount}\n")
    resp.dispose()
    client.dispose()
```

### Streaming an upload

The producer is called repeatedly to fill libcurl's send buffer; return `""` to signal end-of-body. libcurl uses HTTP chunked transfer-encoding when no Content-Length is known up front, so this works for arbitrary-length streams.

```sindarin
import "sindarin-pkg-curl/src/curl"

fn main(): void =>
    var client: CurlClient = CurlClient.new()

    var pieces: str[] = {"hello ", "from ", "sindarin\n"}
    var i: int = 0
    var resp: CurlResponse = client.postChunked("https://httpbin.org/post", "text/plain", fn(): str =>
        if i >= pieces.length =>
            return ""
        var p: str = pieces[i]
        i += 1
        return p
    )

    print($"status: {resp.status()}\n{resp.body()}")
    resp.dispose()
    client.dispose()
```

### Reading response headers

```sindarin
import "sindarin-pkg-curl/src/curl"

fn main(): void =>
    var client: CurlClient = CurlClient.new()
    var resp: CurlResponse = client.get("https://httpbin.org/response-headers?X-Custom=hello")

    print(resp.header("Content-Type"))
    print(resp.header("X-Custom"))
    resp.dispose()

    client.dispose()
```

---

## Development

```bash
# Install dependencies (required before make test)
sn --install

make test    # Build and run all tests
make clean   # Remove build artifacts
```

Tests require outbound HTTPS access (they run against `httpbin.org`).

## Dependencies

- [sindarin-pkg-sdk](https://github.com/SindarinSDK/sindarin-pkg-sdk) — provides [sindarin-pkg-libs](https://github.com/SindarinSDK/sindarin-pkg-libs) with pre-built `libcurl`, `libssl`, `libcrypto`, and `libz` static libraries for Linux, macOS, and Windows.

## License

MIT License
