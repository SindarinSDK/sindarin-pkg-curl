# sindarin-pkg-curl

An HTTP client for the [Sindarin](https://github.com/SindarinSDK/sindarin-compiler) programming language, backed by [libcurl](https://curl.se/libcurl/). Supports GET, POST, PUT, PATCH, and DELETE with persistent client state, custom headers, and configurable timeouts.

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

    var resp: HttpResponse = client.get("https://api.example.com/users")
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
| `get` | `fn get(url: str): HttpResponse` | Perform a GET request |
| `post` | `fn post(url: str, contentType: str, body: str): HttpResponse` | Perform a POST request |
| `put` | `fn put(url: str, contentType: str, body: str): HttpResponse` | Perform a PUT request |
| `patch` | `fn patch(url: str, contentType: str, body: str): HttpResponse` | Perform a PATCH request |
| `delete` | `fn delete(url: str): HttpResponse` | Perform a DELETE request |
| `setTimeout` | `fn setTimeout(ms: int): void` | Set request timeout in milliseconds (0 = no timeout) |
| `setHeader` | `fn setHeader(name: str, value: str): void` | Add a header sent with every request |
| `dispose` | `fn dispose(): void` | Free client resources |

```sindarin
var client: CurlClient = CurlClient.new()
client.setTimeout(5000)
client.setHeader("Authorization", "Bearer my-token")

// GET
var resp: HttpResponse = client.get("https://api.example.com/items")
print(resp.body())
resp.dispose()

// POST with JSON body
var created: HttpResponse = client.post(
    "https://api.example.com/items",
    "application/json",
    "{\"name\": \"widget\"}"
)
print($"created: {created.status()}\n")
created.dispose()

client.dispose()
```

---

## HttpResponse

The result of any HTTP request. Always call `dispose()` when done to free the response buffers.

| Method | Signature | Description |
|--------|-----------|-------------|
| `status` | `fn status(): int` | HTTP status code (e.g. `200`, `404`) |
| `body` | `fn body(): str` | Response body as a string |
| `headers` | `fn headers(): str` | All response headers as a raw string |
| `header` | `fn header(name: str): str` | Value of a named header (case-insensitive); empty string if absent |
| `dispose` | `fn dispose(): void` | Free response memory |

```sindarin
var resp: HttpResponse = client.get("https://api.example.com/data")

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

    var resp: HttpResponse = client.get("https://httpbin.org/json")
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

    var updated: HttpResponse = client.put(
        "https://api.example.com/items/42",
        "application/json",
        "{\"name\": \"updated\"}"
    )
    print($"PUT: {updated.status()}\n")
    updated.dispose()

    var deleted: HttpResponse = client.delete("https://api.example.com/items/42")
    print($"DELETE: {deleted.status()}\n")
    deleted.dispose()

    client.dispose()
```

### Reading response headers

```sindarin
import "sindarin-pkg-curl/src/curl"

fn main(): void =>
    var client: CurlClient = CurlClient.new()
    var resp: HttpResponse = client.get("https://httpbin.org/response-headers?X-Custom=hello")

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
