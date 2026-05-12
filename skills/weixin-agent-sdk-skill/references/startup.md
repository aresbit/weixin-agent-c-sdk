# Startup & Operations Reference — weixin-agent-c-sdk

## Table of Contents

1. [Build System](#1-build-system)
2. [Session Persistence](#2-session-persistence)
3. [Login Flow](#3-login-flow)
4. [Start Command Reference](#4-start-command-reference)
5. [ACP Command Discovery](#5-acp-command-discovery)
6. [Environment Variables](#6-environment-variables)
7. [Signal Handling & Shutdown](#7-signal-handling--shutdown)
8. [Install / Uninstall](#8-install--uninstall)

---

## 1. Build System

The project uses a flat Makefile at the repo root with no subdirectory recursion.

### Targets

| Target | Output |
|--------|--------|
| `make` / `make all` | `build/libweixin_agent_sdk.a`, `bin/echo_bot`, `bin/weixin_acp_c`, `bin/selftest`, `bin/acp_bridge_smoke` |
| `make test` | Runs `bin/selftest` + `bin/acp_bridge_smoke` |
| `make smoke` | Runs `bin/echo_bot` (interactive smoke test) |
| `make clean` | Removes `build/` and `bin/` |
| `make install` | Installs to `$PREFIX/bin`, `$PREFIX/lib`, `$PREFIX/include` (default `/usr/local`) |
| `make uninstall` | Removes installed files |

### Build Variables

```bash
make debug=1               # -O0 -g3 (default: -O2)
make PREFIX=/opt/myprefix  # install prefix
make DESTDIR=/staging      # staging root for package builds
make CC=clang              # override compiler (default: cc)
```

### Dependencies (pkg-config)

- `libcurl` — HTTP client for WeChat API
- `openssl` — AES decryption for media, base64 for ACP image transport
- `pthread` — reader thread
- `libqrencode` (optional) — renders UTF-8 QR codes in terminal; detected via pkg-config;
  if absent, login still works but QR URL is printed as text only

### Object / Dep Files

Built objects land in `build/src/`, `build/examples/`, `build/tests/`.
Dependency (.d) files are auto-generated via `-MMD -MP` for incremental rebuilds.

---

## 2. Session Persistence

### State Directory Resolution (priority order)

1. `$OPENCLAW_STATE_DIR`
2. `$CLAWDBOT_STATE_DIR`
3. `$HOME/.openclaw`  ← default

### File Layout

```
~/.openclaw/
└── openclaw-weixin/
    └── accounts/
        ├── <account-id>.json           # auth token + server URLs
        ├── <account-id>.monitor.lock   # PID lock (prevents duplicate monitors)
        └── <account-id>.sync.json      # getupdates seq/sync state
```

### Account JSON Schema

```json
{
  "token": "<account-id>@im.bot:<hex-token>",
  "baseUrl": "https://ilinkai.weixin.qq.com",
  "userId": "<openid>"
}
```

Regenerated on each successful `login`. If this file is absent or token expired, run
`./bin/weixin_acp_c login`.

### Monitor Lock

`<account-id>.monitor.lock` contains the PID of the running `weixin_acp_c` process.
On startup, the SDK checks if the PID in the lock file is alive and belongs to a `weixin`/
`weixin_acp_c` process (`wxa_pid_is_weixin_monitor` reads `/proc/<pid>/cmdline`). Stale locks
(process dead or different process) are removed automatically.

### Media Cache

Downloaded WeChat media (images, voice, video, files) lands in:
```
~/.openclaw/weixin-agent-c-sdk-media/
```
Created automatically on first download. Files are not cleaned up automatically.

---

## 3. Login Flow

```bash
./bin/weixin_acp_c login
```

1. `wxa_client_new` creates a client with the `wxa_cli_log` callback
2. `wxa_client_login` calls the WeChat auth API to get a QR URL
3. Log callback receives `"scan this QR URL with WeChat: <url>"` and renders the QR if
   `libqrencode` is linked (macro `WXA_HAVE_QRENCODE`)
4. User scans QR with WeChat mobile app
5. SDK polls for confirmation; on success prints:
   `login ok: account=<account-id> user=<user-id>`
6. Token written to `~/.openclaw/openclaw-weixin/accounts/<account-id>.json`

**Note**: The QR code is printed to **stderr**, not stdout. Login result goes to **stdout**.

---

## 4. Start Command Reference

```
./bin/weixin_acp_c start -- <acp-command> [acp-args...]
```

Everything after `--` is the ACP subprocess argv. Examples:

```bash
# Simplest — uses PATH lookup
./bin/weixin_acp_c start -- claude-agent-acp

# With working directory override
ACP_CWD=/home/user/myproject ./bin/weixin_acp_c start -- claude-agent-acp

# npx-based agent (no global install needed)
./bin/weixin_acp_c start -- npx @zed-industries/codex-acp

# Extra args passed through to the ACP agent
./bin/weixin_acp_c start -- claude-agent-acp --model claude-opus-4-7
```

### Startup Sequence

1. Parse `--` separator; collect ACP command + args
2. Check for `claude-code-acp` → `claude-agent-acp` fallback (if `claude-code-acp` not on PATH)
3. `wxa_acp_agent_new` — allocates bridge struct, does NOT start subprocess yet
4. `wxa_client_new` — loads stored session token
5. Register SIGINT/SIGTERM handler → calls `wxa_client_stop`
6. `wxa_client_run` — enters polling loop
   - On first inbound message: `wxa_acp_ensure_ready` → fork/exec ACP subprocess → `initialize`
   - Each message: `wxa_acp_agent_chat` → `session/new` (first time) → `session/prompt`
7. On stop signal: polling loop exits, returns `WXA_OK`
8. Cleanup: `wxa_acp_agent_free` (kills subprocess) → `wxa_client_free`

---

## 5. ACP Command Discovery

`wxa_command_exists(command)` in `examples/weixin_acp_c.c`:

- If `command` contains `/`: uses `access(command, X_OK)` directly (absolute/relative path)
- Otherwise: splits `$PATH` on `:`, constructs `<dir>/<command>`, checks `access(..., X_OK)`
- Empty path components are skipped

Automatic fallback logic (in `wxa_do_start`):

```c
if (strcmp(acp_command, "claude-code-acp") == 0 && !wxa_command_exists("claude-code-acp")) {
  if (wxa_command_exists("claude-agent-acp")) {
    acp_command = "claude-agent-acp";
  }
}
```

Known ACP binary locations as of 2026:
- `claude-agent-acp` — installed by `npm i -g @anthropic-ai/claude-code`
  (typically `~/.nvm/versions/node/<version>/bin/claude-agent-acp`)
- `codex-acp` — OpenAI Codex agent
- `npx @zed-industries/codex-acp` — Zed agent via npx (no global install)

---

## 6. Environment Variables

| Variable | Effect |
|----------|--------|
| `ACP_CWD` | Working directory passed to `session/new` (overrides process cwd) |
| `OPENCLAW_STATE_DIR` | Override default `~/.openclaw` state directory |
| `CLAWDBOT_STATE_DIR` | Secondary override for state directory |
| `HOME` | Used to construct default state dir `$HOME/.openclaw` |
| `PATH` | Used by `wxa_command_exists` for ACP command lookup |

---

## 7. Signal Handling & Shutdown

`SIGINT` and `SIGTERM` both route to `wxa_handle_signal` → `wxa_client_stop(g_client)`.

`wxa_client_stop` is signal-safe (it sets an atomic stop flag, does not lock mutexes).

Shutdown order:
1. `wxa_client_stop` sets stop flag
2. Polling loop in `wxa_client_run` detects flag, exits loop, returns `WXA_OK`
3. `wxa_acp_agent_free`:
   - Sets `stop_reader = true`
   - Closes `stdin_fd` → reader thread gets EOF → exits loop
   - Sends `SIGTERM` to child PID, waits with `waitpid`
   - Joins reader thread
4. `wxa_client_free` releases all resources

---

## 8. Install / Uninstall

```bash
# Install to /usr/local (needs sudo if not owned by user)
sudo make install

# Custom prefix
make install PREFIX=$HOME/.local

# Staged install (for package creation)
make install DESTDIR=/tmp/staging PREFIX=/usr/local

# Files installed:
#   $BINDIR/weixin_acp_c
#   $BINDIR/weixin          (symlink-equivalent copy of weixin_acp_c)
#   $BINDIR/echo_bot
#   $LIBDIR/libweixin_agent_sdk.a
#   $INCLUDEDIR/weixin_agent_sdk.h
#   $INCLUDEDIR/weixin_acp_bridge.h

sudo make uninstall         # removes the above files
```
