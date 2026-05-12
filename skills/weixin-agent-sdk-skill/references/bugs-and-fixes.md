# Bugs & Fixes Reference — weixin-agent-c-sdk

## Table of Contents

1. [Race Condition: First Message Gets No Response](#1-race-condition-first-message-gets-no-response)
2. [sp_free Crash on Voice Without Transcript](#2-sp_free-crash-on-voice-without-transcript)
3. [General Debugging Patterns](#3-general-debugging-patterns)
4. [Safe Patterns Checklist](#4-safe-patterns-checklist)

---

## 1. Race Condition: First Message Gets No Response

**Symptom**: First WeChat message receives no reply; second message works fine.

**Root Cause** (`src/weixin_acp_bridge.c` — `wxa_acp_wait_response`):

The race window exists between `write(stdin_fd, line)` and `wxa_acp_wait_response`'s
`pthread_mutex_lock`. If the ACP subprocess responds quickly (< ~1ms), the reader thread can:

1. Lock mutex
2. See `pending.id == request_id` and `!pending.done == true`
3. Set `pending.done = true`, `pending.payload = response`
4. Broadcast on cond
5. Unlock mutex

Then `wxa_acp_wait_response` locks the mutex and — if the resets were placed there — does:

```c
agent->pending.done    = false;   // ← DESTROYS the already-completed response!
agent->pending.failed  = false;
wxa_acp_free_str(&agent->pending.payload);
```

The wait loop sees `!pending.done = true`, blocks, and eventually times out.

**Fix**: Resets MUST happen inside `wxa_acp_send_request`'s locked section, BEFORE
`pthread_mutex_unlock()`, NEVER in `wxa_acp_wait_response`:

```c
// In wxa_acp_send_request — CORRECT:
pthread_mutex_lock(&agent->mutex);
request_id = agent->pending.id + 1;
agent->pending.id      = request_id;
agent->pending.done    = false;    // ← reset here, inside the lock
agent->pending.failed  = false;
wxa_acp_free_str(&agent->pending.payload);
pthread_mutex_unlock(&agent->mutex);
// write() call goes here
return wxa_acp_wait_response(agent, request_id, timeout_ms, out_payload);

// In wxa_acp_wait_response — NO resets:
pthread_mutex_lock(&agent->mutex);
// do NOT touch pending.done/failed/payload here
while (!agent->pending.done && agent->ready) {
  pthread_cond_timedwait(&agent->cond, &agent->mutex, &ts);
  ...
}
```

**Why this wasn't caught by tests**: `mock_acp_agent.py` takes ~5ms to respond. The race window
is ~10µs (time between `write()` returning and `wait_response` locking). Probability ≈ 0.2% per
call. Under real-world agents (claude-agent-acp) the response time is seconds, so the race never
fires in testing.

---

## 2. sp_free Crash on Voice Without Transcript

**Symptom**: Crash/SIGABRT when WeChat user sends a voice message with no ASR transcript.

**Root Cause** (`src/weixin_agent_sdk.c` — `wxa_try_extract_media_from_item`):

```c
// BUGGY CODE:
sp_str_t voice_text = wxa_json_get_string_after_range(..., "text", ...);
if (voice_text.len == 0U) {
  // ... set up audio download ...
}
if (voice_text.data != NULL) {
  sp_free((void*)voice_text.data);  // ← UB when voice_text = sp_str_lit("")!
}
```

When no transcript exists, `wxa_json_get_string_after_range` returns `sp_str_lit("")` which is
`{data = "", len = 0}` — a **static string literal**. `""` lives in the `.rodata` segment.
Calling `sp_free("")` = `free(literal_address)` = undefined behavior, typically SIGABRT under
glibc.

The `data != NULL` check does NOT protect against this because `""` is not NULL.

**Fix**:

```c
// CORRECT:
wxa_free_str(&voice_text);   // checks len > 0 before calling sp_free
```

`wxa_free_str` is defined as:

```c
static void wxa_free_str(sp_str_t* s) {
  if (s != NULL && s->data != NULL && s->len > 0U) {
    sp_free((void*)s->data);   // only runs for heap-allocated strings
  }
  if (s != NULL) *s = sp_str_lit("");
}
```

**Why this wasn't caught by tests**: `tests/selftest.c` has a voice fixture
(`getupdates-voice-transcript.json`) that includes a transcript. There is no fixture for a voice
message WITHOUT a transcript, so the crash path was never exercised.

**Prevention**: Always use `wxa_free_str` / `wxa_acp_free_str` to free `sp_str_t` values. Never
call `sp_free(s.data)` directly — it will crash on `sp_str_lit("")`.

---

## 3. General Debugging Patterns

### ACP subprocess not starting

```bash
# Check command exists:
which claude-agent-acp codex-acp

# Check the bridge smoke test — it exercises the full ACP handshake:
./bin/acp_bridge_smoke

# Force-rebuild with debug symbols:
make clean && make debug=1
```

### Response never arrives (timeout)

1. Check `wxa_acp_agent_last_error(agent)` — may say "acp subprocess stopped: exit_code=127"
   (command not found) or "acp request timed out".
2. Add temporary `fprintf(stderr, ...)` in `wxa_acp_reader_main` after `getline()` to see raw
   ACP output.
3. Verify `pending.done` is reset in `send_request` (not `wait_response`) — see §1.

### Voice message crashes

Check glibc backtrace points to `wxa_try_extract_media_from_item`. Verify the fix in §2 is
applied: search for `sp_free((void*)voice_text.data)` — it must not exist; only
`wxa_free_str(&voice_text)` should be present.

### Session/update notifications not collected

- `prompt_active` must be `true` before `send_request` is called (set in `wxa_acp_agent_chat`)
- `active_session_id` must match the `sessionId` in the notification
- Check that `wxa_acp_handle_prompt_update` is called from the reader thread (the mutex is held)

### WeChat polling stops

- Check monitor lock file: `~/.openclaw/openclaw-weixin/accounts/<id>.monitor.lock`
- Stale locks from crashed processes are detected via `/proc/<pid>/cmdline` check
  (`wxa_pid_is_weixin_monitor`)
- Session token may have expired — run `./bin/weixin_acp_c login` again

---

## 4. Safe Patterns Checklist

| Operation | Safe Pattern | Unsafe Pattern |
|-----------|-------------|----------------|
| Free sp_str_t | `wxa_free_str(&s)` | `sp_free(s.data)` or `if (s.data) sp_free(s.data)` |
| Free ACP str | `wxa_acp_free_str(&s)` | same as above |
| Reset pending state | Inside `send_request`'s mutex section | Inside `wait_response` after lock |
| Check empty string | `s.len == 0U` | `s.data == NULL` (data is never NULL for lit) |
| Allocate string | `sp_str_from_cstr(cstr)` | `strdup(cstr)` |
| Build string | `sp_format("{}...", ...)` | `sprintf` into fixed buffer |
| Temporary builder | `sp_str_builder_t b = SP_ZERO_INITIALIZE()` | malloc+realloc manually |
