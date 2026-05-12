---
name: weixin-agent-sdk-skill
description: >
  Domain expertise for the weixin-agent-c-sdk project at /home/ares/yyscode/weixin-agent-c-sdk.
  A pure-C SDK that bridges WeChat messaging to ACP (Agent Client Protocol) agents such as
  claude-agent-acp or codex-acp. Use this skill when: working on bugs in weixin_acp_bridge.c
  or weixin_agent_sdk.c; adding new message type handling (voice, image, video, file); debugging
  ACP protocol flow or threading issues; building/running/testing the project; wiring new ACP
  agent backends; or understanding the WeChat polling loop and session lifecycle.
---

# weixin-agent-sdk-skill

Pure-C SDK that bridges WeChat IM to ACP (Agent Client Protocol) agents. Two main source files
under `src/`, one CLI binary `bin/weixin_acp_c`, tests in `tests/`.

## Quick Build & Run

```bash
make                   # builds all: libweixin_agent_sdk.a + all binaries
make test              # selftest (69 checks) + acp_bridge_smoke
make debug=1           # debug build (-O0 -g3)
make clean && make     # full rebuild
```

### Login (first time or session expired)

```bash
./bin/weixin_acp_c login
# Renders UTF-8 block QR code in terminal — scan with WeChat mobile app
# Session token saved to: ~/.openclaw/openclaw-weixin/accounts/<account-id>.json
```

### Start the bridge

```bash
./bin/weixin_acp_c start -- claude-agent-acp
./bin/weixin_acp_c start -- codex-acp
./bin/weixin_acp_c start -- npx @zed-industries/codex-acp
ACP_CWD=/path/to/project ./bin/weixin_acp_c start -- claude-agent-acp
```

`claude-code-acp` auto-falls-back to `claude-agent-acp` if not found on PATH (see `wxa_do_start`
in `examples/weixin_acp_c.c`).

## Project Layout

```
src/weixin_agent_sdk.c       WeChat client: polling, auth, media download, message dispatch
src/weixin_acp_bridge.c      ACP bridge: subprocess lifecycle, JSON-RPC, response tracking
src/sp_shim.c                sp.h allocator shim
include/weixin_agent_sdk.h   Public API (wxa_client_t, wxa_acp_agent_t, all types)
include/weixin_acp_bridge.h  ACP bridge public API
examples/weixin_acp_c.c      CLI entry point (login / start subcommands)
tests/selftest.c             Unit tests for SDK parsing (fixtures in tests/fixtures/)
tests/acp_bridge_smoke.c     End-to-end ACP bridge test via mock_acp_agent.py
tests/mock_acp_agent.py      Synchronous Python mock ACP agent for smoke test
tests/fixtures/              getupdates JSON response fixtures
```

## Key Reference Files

| Topic | File |
|-------|------|
| Architecture, threading model, ACP protocol, WeChat message types, public API | `references/architecture.md` |
| Known bug root causes, race condition pattern, sp_free crash, voice handling | `references/bugs-and-fixes.md` |
| Session persistence paths, build variants, ACP command discovery, env vars | `references/startup.md` |

**Always read `references/architecture.md` first** when touching `weixin_acp_bridge.c` or the
threading/response-tracking code. Read `references/bugs-and-fixes.md` before any bug
investigation in either source file.
