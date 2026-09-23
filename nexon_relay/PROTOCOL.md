# nexon_relay — Protocol v0.1

Contract between the Android browser app and the Termux relay server.

## Roles

**Browser**: an Android app wrapping the system WebView (Chromium engine). Opens
chatbot pages, injects a relay script, detects JSON command blocks in assistant
replies, POSTs them to the local relay server, receives stdout back, types it into
the chat composer, sends.

**Server**: a Python stdlib HTTP server in Termux on `127.0.0.1:<PORT>`. Receives
the JSON block, spawns `nexon_code <project> --plain`, feeds the block to stdin,
captures stdout, returns it verbatim.

The server **never parses** the command. It is a pipe. `nexon_code` owns the
grammar; the relay must not duplicate it.

## Wire format

### `GET /health`

```
200 -> {"ok":true,"version":"0.1","project":"<abs path>","nexon":"<abs path>"}
```

### `POST /run`

Request:

```json
{
  "cmd": "<exact JSON text the model emitted, as a string>",
  "timeout_ms": 120000
}
```

Response 200 on success:

```json
{
  "ok": true,
  "stdout": "<raw stdout>",
  "stderr": "<raw stderr>",
  "exit": 0,
  "ms": 47
}
```

Response 200 on transport failure (server could not spawn, timeout, etc.):

```json
{
  "ok": false,
  "err": "human-readable message",
  "exit": -1
}
```

A `{"err":"..."}` **inside** `stdout` is a *tool-level* error from `nexon_code`,
not a transport failure. Pass it through verbatim. The model needs to see it to
self-correct.

## Bounds

- **Port**: 8787, override with `RELAY_PORT`.
- **Bind**: `127.0.0.1` only. Never `0.0.0.0`.
- **Default timeout**: 120 s.
- **Max body**: 4 MB. Larger gets `{"ok":false,"err":"body too large"}`.
- **Concurrency**: single in-flight. Concurrent requests get HTTP 429. `nexon_code`
is not concurrency-safe on one project root, and serialising avoids races.
- **Cleartext**: the Android app's network security config permits cleartext to
`127.0.0.1` only.

## The JSON block the browser looks for

A block qualifies as a relay command iff **all three** hold:

1. It sits inside a fenced code block whose info string is `json`.
2. The whole fenced text parses as one JSON value.
3. That value is:
   - an object with a non-empty string `t` (or `tool`), `a`/`args` optional; **or**
   - an object with a non-empty array `calls`; **or**
   - a bare top-level array whose every element matches the first shape.

Anything else — prose, examples, tool output the user pasted back — is ignored.
This is the false-positive filter.

## Stop condition

The relay loop ends when a completed assistant message contains **no** qualifying
block. That message is human-facing prose. Browser stops, clears its highlight,
waits for the user to send the next task.

## Iteration cap

20 round-trips per user message by default. On hitting the cap the browser stops
the loop and shows a stop banner — it does not send. Prevents a model loop from
running away.

## Versioning

`PROTOCOL.md` is the source of truth. Any change to request/response shape bumps
the version string in `/health`.
