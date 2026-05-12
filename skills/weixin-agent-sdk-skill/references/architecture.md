# Architecture Reference — weixin-agent-c-sdk

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Public API (weixin_agent_sdk.h)](#2-public-api)
3. [ACP Protocol](#3-acp-protocol)
4. [Threading Model](#4-threading-model)
5. [WeChat Message Types](#5-wechat-message-types)
6. [WeChat Client Internals](#6-wechat-client-internals)
7. [ACP Bridge Internals](#7-acp-bridge-internals)
8. [Memory Management](#8-memory-management)

---

## 1. System Overview

```
WeChat Server
    │  HTTPS long-poll (getupdates)
    ▼
wxa_client_t  (weixin_agent_sdk.c)
    │  parses inbound messages, downloads media
    │  calls agent vtable .chat()
    ▼
wxa_acp_agent_t  (weixin_acp_bridge.c)
    │  fork/exec ACP subprocess (e.g. claude-agent-acp)
    │  JSON-RPC 2.0 over stdin/stdout pipes
    ▼
ACP Agent process
    │  returns session/update notifications + final result
    ▼
wxa_client_t  sends text/media reply back to WeChat
```

---

## 2. Public API

### Core Types (`include/weixin_agent_sdk.h`)

```c
typedef enum { WXA_OK = 0, WXA_ERR = 1 } wxa_status_t;

typedef enum {
  WXA_MEDIA_NONE = 0,
  WXA_MEDIA_IMAGE = 1,
  WXA_MEDIA_AUDIO = 2,
  WXA_MEDIA_VIDEO = 3,
  WXA_MEDIA_FILE  = 4,
} wxa_media_type_t;

typedef struct {
  wxa_media_type_t type;
  const char* file_path;   // heap-allocated absolute path; caller owns
  const char* mime_type;   // static or heap; see wxa_chat_response_t notes
  const char* file_name;   // original filename (files only)
} wxa_media_t;

typedef struct {
  const char* conversation_id;  // WeChat conversation/room ID (not owned)
  const char* text;             // UTF-8 message text (not owned)
  wxa_media_t media;
} wxa_chat_request_t;

typedef struct {
  char* text;       // heap-allocated; caller must free
  wxa_media_t media;
} wxa_chat_response_t;

typedef struct {
  int (*chat)(void* user_data, const wxa_chat_request_t*, wxa_chat_response_t*);
} wxa_agent_vtable_t;

typedef struct {
  void (*log_fn)(void* user_data, const char* message);
} wxa_client_options_t;

typedef struct {
  const char* account_id;
  const char* user_id;
} wxa_login_result_t;
```

### Client Lifecycle

```c
wxa_client_t* wxa_client_new(const wxa_client_options_t* options);
wxa_status_t  wxa_client_login(wxa_client_t*, wxa_client_options_t*, wxa_login_result_t*);
wxa_status_t  wxa_client_run(wxa_client_t*, const wxa_agent_vtable_t*, void* user_data, void*);
void          wxa_client_stop(wxa_client_t*);      // signal-safe
void          wxa_client_free(wxa_client_t*);
const char*   wxa_client_last_error(const wxa_client_t*);
```

### ACP Bridge Lifecycle (`include/weixin_acp_bridge.h`)

```c
typedef struct {
  const char*        command;          // executable name or path
  const char* const* args;             // extra argv (after command)
  int                arg_count;
  const char*        cwd;             // NULL = use process cwd
  unsigned int       prompt_timeout_ms; // 0 = 120000 default
} wxa_acp_agent_options_t;

wxa_acp_agent_t* wxa_acp_agent_new(const wxa_acp_agent_options_t*);
int  wxa_acp_agent_chat(wxa_acp_agent_t*, const wxa_chat_request_t*, wxa_chat_response_t*);
void wxa_acp_agent_free(wxa_acp_agent_t*);
const char* wxa_acp_agent_last_error(const wxa_acp_agent_t*);
```

---

## 3. ACP Protocol

ACP is JSON-RPC 2.0 over newline-delimited text streams (one JSON object per line).

### Handshake Sequence

```
Client → Agent:  initialize  {protocolVersion, clientCapabilities, clientInfo}
Agent  → Client: {result: {capabilities: {image, audio, embeddedContext, loadSession}}}

Client → Agent:  session/new  {cwd, mcpServers:[]}
Agent  → Client: {result: {sessionId: "<uuid>"}}

# Per-message:
Client → Agent:  session/prompt  {sessionId, prompt: [ContentBlock...]}
Agent  → Client: (notification) session/update  {sessionId, sessionUpdate, ...}
              …  (0 or more notifications)
Agent  → Client: {id, result: {}}   ← final response (matches request id)
```

### ContentBlock Types (prompt array)

```json
{"type":"text","text":"..."}
{"type":"image","mimeType":"image/jpeg","data":"<base64>"}
{"type":"audio","mimeType":"audio/silk","data":"<base64>"}
{"type":"resource","resource":{"uri":"file:///abs/path","mimeType":"...","blob":"<base64>"}}
```

### Notifications

- `session/update` with `sessionUpdate: "agent_message_chunk"` — streaming text/image output
  - `type: "text"` → `text` field accumulated into `prompt_result.text`
  - `type: "image"` → `data` + `mimeType` stored in `prompt_result.image_base64/image_mime_type`
- `session/request_permission` — auto-approved: picks first `optionId`, sends JSON-RPC response

### Capabilities

The bridge checks `agent->capabilities.image` / `.audio` before including media blocks; currently
media blocks are built unconditionally (`wxa_acp_build_prompt_json`), capabilities are stored but
the guard logic is not enforced — implementation detail to be aware of.

---

## 4. Threading Model

```
Main thread                     Reader thread (wxa_acp_reader_main)
──────────────────────────────  ──────────────────────────────────
wxa_acp_send_request()          getline() loop on stdout_file
  mutex_lock()
  pending.id   = N              ← checks: found_id && pending.id==N && !pending.done
  pending.done = false          ← MUST be reset here (inside lock) not in wait_response
  pending.failed = false
  free(pending.payload)
  mutex_unlock()
  write(stdin_fd, line)
  ↓
wxa_acp_wait_response()         if match:
  mutex_lock()                    pending.done = true
  while !pending.done && ready:   pending.failed = (error key present)
    cond_timedwait()              pending.payload = sp_str_from_cstr(line)
  // read pending.payload          cond_broadcast()
  mutex_unlock()
```

**Critical invariant**: `pending.done/failed/payload` MUST be reset inside the same mutex section
that sets `pending.id`, in `wxa_acp_send_request` — NOT in `wxa_acp_wait_response`. If reset
happens in `wait_response`, the reader thread can set `done=true` in the window between the
`write()` call and `wait_response`'s `mutex_lock()`, and the subsequent reset in `wait_response`
discards the completed response, causing a timeout. See `references/bugs-and-fixes.md` §1.

### Mutex-Protected State in `wxa_acp_agent`

- `pending` (id, done, failed, payload)
- `ready` — subprocess alive flag
- `prompt_active` — true while a `session/prompt` request is in flight
- `active_session_id` — session being tracked for `session/update` notifications
- `prompt_result` (text, image_base64, image_mime_type)

### `stop_reader` flag

Set to `true` in `wxa_acp_agent_free()` before closing `stdin_fd`. The reader thread exits its
`getline()` loop when the pipe EOF is received (triggered by `close(stdin_fd)`). No mutex needed
for `stop_reader` — it is a one-way latch checked in the reader loop.

---

## 5. WeChat Message Types

`item_type` field in getupdates JSON response:

| type | Content | C handling |
|------|---------|------------|
| 1 | Text message | `wxa_body_from_message_item` → `inbound.text` |
| 2 | Image | `wxa_try_extract_media_from_item` → `WXA_MEDIA_IMAGE`, downloads binary |
| 3 | Voice (SILK audio) | Transcript in `voice_item.text` → `inbound.text` if present; else `WXA_MEDIA_AUDIO` download |
| 4 | File | `WXA_MEDIA_FILE`, downloads binary with AES decryption |
| 5 | Video | `WXA_MEDIA_VIDEO`, downloads binary |
| 43 | Quoted/reply message | Extracts inner item; may contain nested image/video |

### Voice Message Flow (type=3)

```
wxa_parse_inbound_message()
  └─ wxa_body_from_message_item()         → returns voice_item.text (transcript or "")
  └─ wxa_extract_media_from_item_list()
       └─ wxa_extract_downloadable_media_from_item()
            └─ wxa_try_extract_media_from_item()  ← Bug 2 was here
                 if voice_text.len == 0:
                   set WXA_MEDIA_AUDIO + download params
                 wxa_free_str(&voice_text)   ← safe: checks len>0 before sp_free
```

If a voice transcript exists: `inbound.text = transcript`, `inbound.media_type = NONE`.
If no transcript: `inbound.text = ""`, `inbound.media_type = WXA_MEDIA_AUDIO` (SILK file).

### Media Priority in `wxa_extract_media_from_item_list`

Order: image(2) > video(5) > file(4) > voice(3). Only one media attachment per message to ACP.

---

## 6. WeChat Client Internals

### Polling Loop (`wxa_client_run`)

- Calls WeChat `getupdates` endpoint (HTTPS long-poll, up to ~30s server timeout)
- Uses `seq` + `message_id` deduplication to avoid replaying messages
- `get_updates_buf` accumulates server sync state across reconnects
- `wxa_dispatch_message_segment` → `wxa_parse_inbound_message` → calls `agent.chat`

### Session Persistence

- State dir: `~/.openclaw/` (or `$OPENCLAW_STATE_DIR` / `$CLAWDBOT_STATE_DIR`)
- Account token: `~/.openclaw/openclaw-weixin/accounts/<account-id>.json`
  ```json
  {"token":"<account>@im.bot:<hex>","baseUrl":"https://ilinkai.weixin.qq.com","userId":"..."}
  ```
- Monitor lock: `<account-id>.monitor.lock` — prevents duplicate monitor processes
- Sync state: `<account-id>.sync.json`

### Media Download

Downloaded files land in `~/.openclaw/weixin-agent-c-sdk-media/` (created on first use).
AES-CBC decryption is applied where `media_aes_key` / `media_hex_aes_key` is present.

---

## 7. ACP Bridge Internals

### `wxa_acp_ensure_ready`

Lazy-starts the subprocess on first `wxa_acp_agent_chat` call:
1. `wxa_acp_start_process` — fork/exec, pipes, starts reader thread
2. `wxa_acp_initialize` — sends `initialize` request, parses capabilities

### Session Mapping

`agent->sessions` is a `sp_da(wxa_acp_session_entry_t)` mapping `conversation_id → session_id`.
On first message per conversation, `wxa_acp_create_session` issues `session/new`.

### `wxa_acp_build_prompt_json`

Builds the ContentBlock JSON array:
- Text block if `request->text != NULL && text[0] != '\0'`
- Image block (base64 from file) if `request->media.type == WXA_MEDIA_IMAGE`
- Audio block if `request->media.type == WXA_MEDIA_AUDIO`
- Resource block for other media types

Image output from ACP (`session/update type=image`) is decoded from base64 and written to
`/tmp/weixin-agent/media/acp-out/acp-<pid>.<ext>`, then returned as `response->media`.

### JSON Parsing

All JSON parsing is hand-rolled (no external library):
- `wxa_acp_json_get_string` — extracts string value with escape decoding
- `wxa_acp_json_get_long` — extracts integer value
- `wxa_acp_json_has_true` — checks boolean true
- `wxa_acp_find_after_key` — finds value position after a quoted key

---

## 8. Memory Management

The project uses `sp.h` (single-header stdlib replacement):

```c
sp_alloc(size)          // malloc equivalent
sp_free(ptr)            // free equivalent — NEVER call on static literals
sp_str_t                // {const char* data; u32 len;}
sp_str_lit("foo")       // {data="foo", len=3} — static, must NOT be sp_free'd
sp_str_from_cstr("foo") // heap-allocated copy
sp_str_copy(s)          // heap-allocated copy of sp_str_t
sp_str_to_cstr(s)       // heap-allocated NUL-terminated copy
```

### Safe String Free Pattern

```c
// ALWAYS use wxa_free_str / wxa_acp_free_str — NOT sp_free directly
static void wxa_free_str(sp_str_t* s) {
  if (s != NULL && s->data != NULL && s->len > 0U) {
    sp_free((void*)s->data);   // only frees heap-allocated (len>0 guard)
  }
  if (s != NULL) *s = sp_str_lit("");
}
```

`sp_str_lit("")` returns `{data="", len=0}`. The `len > 0` guard prevents calling `sp_free("")`
which would be UB (freeing a static string literal). This is the root cause of Bug 2 — see
`references/bugs-and-fixes.md`.
