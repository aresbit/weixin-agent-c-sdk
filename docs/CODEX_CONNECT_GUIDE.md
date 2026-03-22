# C SDK 连接 Codex 操作指南

本文档说明如何用当前仓库的 C 可执行程序，把微信消息桥接到 `codex-acp`。

## 1. 环境准备

- Linux/macOS
- `cc`、`make`
- `libcurl`、`openssl`
- Node.js + npm（用于安装 `codex-acp`）

## 2. 构建

```bash
make
```

构建成功后确认有 `bin/weixin_acp_c`。

## 3. 首次登录微信

```bash
./bin/weixin_acp_c login
```

终端会输出二维码 URL 或二维码内容，扫码确认后会在本地保存账号状态与 token。

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
./bin/weixin_acp_c start -- codex-acp
```

说明：

- `start --` 后面的命令就是 ACP 子进程命令。
- 当前进程会把微信入站消息转成 ACP 请求，再把 ACP 响应回发到微信。

## 6. 运行验证

1. 在微信里给机器人连续发送多条消息（例如 `1 2 3`）。
2. 观察终端日志是否出现每条的 `dispatch msg` / `dispatch done`。
3. 确认微信端收到对应回复。

## 7. 常见问题

### 7.1 启动提示 lock 已存在

删除 lock 后重启：

```bash
rm -f ~/.openclaw/openclaw-weixin/accounts/*-im-bot.monitor.lock
```

### 7.2 消息发送慢或漏回

- 先看 `~/.openclaw/last-getupdates-with-msgs.json`，确认消息是否被 getupdates 拉到。
- 再看终端是否有 `sendmessage` 的 `ret/errcode` 异常日志。
- 保持单实例运行，避免两个 monitor 并发抢同一账号状态。

### 7.3 `codex-acp` 找不到

确认 `npm -g bin` 在 `PATH` 中，或使用绝对路径：

```bash
./bin/weixin_acp_c start -- /absolute/path/to/codex-acp
```
