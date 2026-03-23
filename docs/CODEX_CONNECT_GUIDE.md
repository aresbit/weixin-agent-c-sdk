# C SDK 连接 Codex 操作指南

本文档说明如何用当前仓库的 C 可执行程序，把微信消息桥接到 `codex-acp`。

## 1. 环境准备

- Linux/macOS
- `cc`、`make`
- `libcurl`、`openssl`
- `libqrencode`（用于登录时直接打印 UTF 二维码）
- Node.js + npm（用于安装 `codex-acp`）

## 2. 安装 `weixin` 命令

```bash
make install
```

安装成功后确认 `weixin --help` 可执行。

## 3. 首次登录微信

```bash
weixin login
```

终端会直接输出 UTF 二维码（同时保留二维码 URL 日志），扫码确认后会在本地保存账号状态与 token。

## 4. 安装 Codex ACP Agent

```bash
npm install -g @zed-industries/codex-acp
```

验证：

```bash
codex-acp --help
```

## 5. 启动桥接

```bash
weixin start -- codex-acp
```

说明：

- `start --` 后面的命令就是 ACP 子进程命令。
- 当前进程会把微信入站消息转成 ACP 请求，再把 ACP 响应回发到微信。

## 6. 接入 Claude Agent（ACP）

`weixin` 的 `start --` 支持任意 ACP agent 命令，Claude 可通过官方 SDK 的 ACP adapter 接入：

```bash
# 安装 adapter
npm install -g @zed-industries/claude-agent-acp

# 配置鉴权
export ANTHROPIC_API_KEY=sk-...

# 启动
weixin start -- claude-agent-acp
```

## 7. 运行验证

1. 在微信里给机器人连续发送多条消息（例如 `1 2 3`）。
2. 观察终端日志是否出现每条的 `dispatch msg` / `dispatch done`。
3. 确认微信端收到对应回复。

## 8. 常见问题

### 8.1 启动提示 lock 已存在

删除 lock 后重启：

```bash
rm -f ~/.openclaw/openclaw-weixin/accounts/*-im-bot.monitor.lock
```

### 8.2 消息发送慢或漏回

- 先看 `~/.openclaw/last-getupdates-with-msgs.json`，确认消息是否被 getupdates 拉到。
- 再看终端是否有 `sendmessage` 的 `ret/errcode` 异常日志。
- 保持单实例运行，避免两个 monitor 并发抢同一账号状态。

### 8.3 ACP 可执行程序找不到

确认 `npm -g bin` 在 `PATH` 中，或使用绝对路径：

```bash
weixin start -- /absolute/path/to/<acp-agent>
```
