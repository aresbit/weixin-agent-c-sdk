# weixin-agent-c-sdk

> 本项目非微信官方项目，仅供学习交流使用。

纯 C 的微信 Agent SDK 与 ACP Bridge。你可以直接把 Codex/Claude 等 ACP Agent 接进微信。

## 当前目录

- `include/` 公开头文件
- `src/` SDK 与 ACP bridge 实现
- `examples/` 示例程序（`weixin_acp_c`、`echo_bot`）
- `tests/` 自测与 ACP smoke 测试

## 构建与测试

```bash
make
make test
```

说明：`weixin login` 会在终端直接打印 UTF 二维码（由 `libqrencode` 渲染，不依赖 `qrencode` 命令行）。

安装（默认到 `/usr/local`）：

```bash
make install
```

安装后可直接使用 `weixin` 命令。若无 root 权限，可安装到用户目录：

```bash
make install PREFIX=$HOME/.local
export PATH="$HOME/.local/bin:$PATH"
```

默认产物：

- `build/libweixin_agent_sdk.a`
- `bin/echo_bot`
- `bin/weixin_acp_c`
- `bin/selftest`
- `bin/acp_bridge_smoke`

## 快速接入 Codex

```bash
# 1) 安装 weixin 命令
make install

# 2) 首次扫码登录（生成 bot token/account）
weixin login

# 3) 安装 codex-acp（需本机已安装 node/npm）
npm install -g @zed-industries/codex-acp

# 4) 启动 C bridge + codex-acp
weixin start -- codex-acp
```

## 接入 Claude Agent（ACP）

```bash
# 1) 安装 Claude 的 ACP adapter
npm install -g @zed-industries/claude-agent-acp

# 2) 配置 Anthropic Key
export ANTHROPIC_API_KEY=sk-...

# 3) 启动桥接
weixin start -- claude-agent-acp
```

详细步骤见文档：

- `docs/CODEX_CONNECT_GUIDE.md`

## C API 对应关系

- `wxa_client_login()` 对应登录
- `wxa_client_run()` 对应消息循环
- `wxa_agent_vtable_t.chat` 对应上层 Agent 回调

## 状态

已覆盖：登录、长轮询、文本收发、基础媒体收发、`get_updates_buf` 持久化、ACP bridge。
| `OPENAI_MODEL` | 否 | 模型名称，默认 `gpt-5.4` |
| `SYSTEM_PROMPT` | 否 | 系统提示词 |

## 支持的消息类型

### 接收（微信 → Agent）

| 类型 | `media.type` | 说明 |
|------|-------------|------|
| 文本 | — | `request.text` 直接拿到文字 |
| 图片 | `image` | 自动从 CDN 下载解密，`filePath` 指向本地文件 |
| 语音 | `audio` | SILK 格式自动转 WAV（需安装 `silk-wasm`） |
| 视频 | `video` | 自动下载解密 |
| 文件 | `file` | 自动下载解密，保留原始文件名 |
| 引用消息 | — | 被引用的文本拼入 `request.text`，被引用的媒体作为 `media` 传入 |
| 语音转文字 | — | 微信侧转写的文字直接作为 `request.text` |

### 发送（Agent → 微信）

| 类型 | 用法 |
|------|------|
| 文本 | 返回 `{ text: "..." }` |
| 图片 | 返回 `{ media: { type: "image", url: "/path/to/img.png" } }` |
| 视频 | 返回 `{ media: { type: "video", url: "/path/to/video.mp4" } }` |
| 文件 | 返回 `{ media: { type: "file", url: "/path/to/doc.pdf" } }` |
| 文本 + 媒体 | `text` 和 `media` 同时返回，文本作为附带说明发送 |
| 远程图片 | `url` 填 HTTPS 链接，SDK 自动下载后上传到微信 CDN |

## 清理
# 1) 停掉所有旧进程
pkill -f weixin_acp_c || true
pkill -f 'weixin start' || true
pkill -f claude-agent-acp || true
pkill -f claude-code-acp || true

# 2) 删除运行锁
find ~/.openclaw/openclaw-weixin/accounts -name '*.monitor.lock' -delete 2>/dev/null || true

# 3) 删除会话游标（避免脏消息循环）
find ~/.openclaw/openclaw-weixin/accounts -name '*.sync.json' -delete 2>/dev/null || true
rm -f ~/.openclaw/last-getupdates-with-msgs.json 2>/dev/null || true

## 内置斜杠命令

在微信中发送以下命令：

- `/echo <消息>` —— 直接回复（不经过 Agent），附带通道耗时统计
- `/toggle-debug` —— 开关 debug 模式，启用后每条回复追加全链路耗时

## 技术细节

- 使用 **长轮询** (`getUpdates`) 接收消息，无需公网服务器
- 媒体文件通过微信 CDN 中转，**AES-128-ECB** 加密传输
- 单账号模式：每次 `login` 覆盖之前的账号
- 断点续传：`get_updates_buf` 持久化到 `~/.openclaw/`，重启后从上次位置继续
- 会话过期自动重连（errcode -14 触发 1 小时冷却后恢复）
- Node.js >= 22
