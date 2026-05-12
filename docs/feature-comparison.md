# Feature Comparison: C SDK vs Reference TypeScript SDK

Reference: https://github.com/wong2/weixin-agent-sdk  
Generated: 2026-05-13

## Status Legend

| Symbol | Meaning |
|--------|---------|
| ✅ | Fully implemented and tested |
| ⚠️ | Partial / known limitation |
| ❌ | Not implemented |
| 🚀 | C SDK advantage over TS reference |

---

## Core Messaging

| Feature | C SDK | TS SDK | Notes |
|---------|-------|--------|-------|
| QR code login | ✅ | ✅ | C: renders UTF-8 block QR in terminal via libqrencode |
| Session persistence (token) | ✅ | ✅ | `~/.openclaw/openclaw-weixin/accounts/*.json` |
| getupdates long-poll loop | ✅ | ✅ | Server-recommended timeout, exponential back-off |
| Text message receive | ✅ | ✅ | |
| Text message send | ✅ | ✅ | Includes context_token threading |
| Multi-item text concatenation | ✅ | ✅ | C: joins with newline |
| Typing indicator (send/cancel) | ✅ | ✅ | |
| Outbound retry with jitter | ✅ | ⚠️ | C: full retry queue + exponential jitter backoff |
| seq / message_id deduplication | ✅ | ✅ | |
| Monitor lock (no duplicate processes) | ✅ | ❌ | C-only feature |

## Media — Inbound

| Feature | C SDK | TS SDK | Notes |
|---------|-------|--------|-------|
| Image (type=2) receive + download | ✅ | ✅ | AES-128-ECB + CDN |
| Video (type=5) receive + download | ✅ | ✅ | |
| File (type=4) receive + download | ✅ | ✅ | |
| Voice (type=3) with ASR transcript | ✅ | ✅ | Transcript used as text |
| Voice (type=3) without transcript | ✅ | ✅ | Downloads SILK file |
| Voice SILK → MP3 transcoding | ✅ | ✅ | C: via python3 silkcoder (optional); TS: ffmpeg |
| Quoted/reply message (type=43) | ✅ | ✅ | Nested image/video/file extraction |
| Small program (miniprogram) | ❌ | ⚠️ | Silently ignored |
| Location message | ❌ | ❌ | |
| Emoji / sticker | ❌ | ❌ | |
| Card / red packet | ❌ | ❌ | |

## Media — Outbound

| Feature | C SDK | TS SDK | Notes |
|---------|-------|--------|-------|
| Send image | ✅ | ✅ | CDN upload → sendmessage |
| Send video | ✅ | ✅ | |
| Send file | ✅ | ✅ | |
| Send audio | ✅ | ✅ | |
| Response image MIME type passthrough | ⚠️ | ✅ | C: hardcodes `image/png` for ACP→WeChat path |

## ACP Bridge

| Feature | C SDK | TS SDK | Notes |
|---------|-------|--------|-------|
| `initialize` handshake | ✅ | ✅ | |
| `session/new` per conversation | ✅ | ✅ | |
| `session/prompt` with ContentBlocks | ✅ | ✅ | text/image/audio/resource |
| `session/update` agent_message_chunk | ✅ | ✅ | text + image accumulation |
| `session/update` other types | ❌ | ⚠️ | context_summary, session_complete not handled |
| `session/request_permission` auto-approve | ✅ | ✅ | Picks first optionId |
| ACP capabilities guard (image/audio) | ⚠️ | ✅ | C: capabilities parsed but not enforced in block builder |
| `loadSession` (cross-restart session resume) | ❌ | ⚠️ | Parsed but not implemented |
| Streaming text response aggregation | ✅ | ✅ | |
| ACP → WeChat image output | ✅ | ✅ | base64 decoded to temp file |
| Pending state race condition | 🚀 | N/A | C: fixed; TS: no race (event loop) |

## JSON Parsing

| Feature | C SDK | TS SDK | Notes |
|---------|-------|--------|-------|
| Basic string / number / bool | ✅ | ✅ | |
| `\uXXXX` Unicode escape → UTF-8 | ✅ | ✅ | C: fixed (was `?`); TS: native |
| Surrogate pairs (`𐀀`) | ❌ | ✅ | Rare in WeChat API; emits `?` |
| Deep nesting robustness | ⚠️ | ✅ | C: hand-rolled scanner; no depth limit checking |
| Full RFC 8259 compliance | ⚠️ | ✅ | C: good enough for WeChat API; no external lib |

## Non-Functional / Deployment

| Feature | C SDK | TS SDK | Notes |
|---------|-------|--------|-------|
| Runtime dependencies | 🚀 libcurl + openssl | Node.js ≥18 + npm | |
| Memory footprint (idle) | 🚀 ~2 MB RSS | ~80 MB RSS | |
| Single binary distribution | 🚀 yes | no | |
| Embeddable as .a library | 🚀 yes | no | |
| GC / runtime pauses | 🚀 none | V8 GC | |
| Webhook / HTTP receive mode | ❌ | ✅ | C: poll-only |
| Group chat support | ❌ | ⚠️ | Neither fully tested |

---

## Priority Backlog

Items ordered by user impact:

1. **✅ DONE** Fix `\uXXXX` → UTF-8 decode (was producing `?` for non-ASCII JSON escapes)
2. **✅ DONE** Add SILK → MP3 transcoding via `python3 -m silkcoder` (optional runtime dep)
3. **`image/png` MIME fix** — detect actual image format from ACP response, pass correct MIME to WeChat
4. **ACP capabilities enforcement** — skip image/audio ContentBlocks when agent doesn't advertise support
5. **`session/update` context_summary** — handle to enable long conversation compaction
6. **`loadSession`** — persist ACP session IDs to disk, resume after process restart
7. **Surrogate pair decoding** — `\uD800\uDCxx` → 4-byte UTF-8 (rare but correct to fix)
8. **Webhook receive mode** — add HTTP listener as alternative to getupdates polling
