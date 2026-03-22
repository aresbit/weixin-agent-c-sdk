# Changelog

## Unreleased

### Added

- 初始纯 C SDK ABI：`wxa_client_new()`、`wxa_client_login()`、`wxa_client_run()`、`wxa_client_stop()`。
- 初始 `Agent.chat` 对应的 C 回调接口：`wxa_agent_vtable_t.chat`。
- 基于 `sp.h` 的字符串、动态内存和构建接入。
- 基于 `libcurl` 的最小 HTTP 客户端封装。
- QR 登录流程：获取二维码、轮询扫码状态、提取 `bot_token` / `account_id`。
- `getupdates` 长轮询主循环。
- 文本消息回调与文本消息回复。
- 账号与 `get_updates_buf` 的本地持久化。
- OpenSSL 支撑的 AES-128-ECB、Base64、MD5 能力。
- 基础 CDN 下载/上传链路。
- 基础媒体消息支持：图片、视频、文件、语音的入站提取与回调前下载。
- 基础出站媒体发送：本地文件上传到 CDN 后封装为微信消息。
- `item_list` 逐项扫描解析，开始覆盖引用文本与语音转文字场景。
- `ref_msg.message_item` 的引用媒体开始接入入站解析。
- `getupdates` 的 `msgs` 遍历已改为基于数组与对象边界扫描。
- 消息项解析已改为递归路径，开始覆盖嵌套 `ref_msg` 的文本与媒体提取。
- 多文本 item 现在按行拼接，不再只取第一段文本。
- 递归解析主路径已开始使用带边界的字段提取 helper，减少跨对象误命中。
- 已补基础 typing 行为：chat 前发送 typing，结束后发送 cancel。
- 已补基础 `getupdates` 运行时行为：读取 server 建议的 long-poll timeout，并处理 session expired / 退避场景。
- 已抽出纯解析入口，消息解包与网络发送开始分层。
- 已补内建自测入口 `wxa_selftest_run()` 与 `make test`。
- 当前自测已覆盖递归 `item_list`、引用消息媒体提取、`msgs` 边界遍历、边界字段提取与 session expired 识别。
- 已将 `sp.h` 复制进仓库 `include/sp.h`，构建不再依赖外部 skill 目录。
- 已补 C 版 ACP bridge：`wxa_acp_agent_*` 与 `bin/weixin_acp_c`。
- 已补 mock ACP smoke test，覆盖 `initialize` / `session/new` / `session/prompt` / `session/update` 主链。
- 已修正运行时关键 JSON 构造，避免 `sp_format` 与字面 `{}` 冲突导致请求体构造崩溃。
- 已修正一组 `sp_str_t` 所有权与空串释放问题，统一收口到 helper，避免将 `sp_str_lit("")` 当堆内存释放。
- 已修正 `sp_format()` 返回的 `sp_str_t` 被误当作 NUL 结尾 C 字符串的问题，去掉登录路径和 ACP bridge 里的 `strlen/strstr` 越界读。
- `Makefile` 构建链与 `examples/echo_bot.c` 示例程序。

### Notes

- 当前主线验证结果：`make` 可成功构建静态库、示例程序、C 版 ACP bridge 与自测程序。
- 当前版本已从“最小文本链路”推进到“文本 + 基础媒体 + 基础状态持久化”阶段。
- 当前最大缺口仍是 JSON / 消息结构解析不够严格，虽然关键路径已有边界扫描与自测，但整体仍属于轻量解析器。
- 当前多 item / 引用链路已比前一阶段稳定，但仍未完全对齐 TS 版的全部细节。
- 当前已经拿到“C SDK -> ACP -> mock agent”端到端证据，并已验证 `./bin/weixin_acp_c start -- codex-acp` 可真实拉起到 token 校验阶段。
- 当前 `login` 已从内存崩溃推进到可复现的网络层 `SSL connect error`，说明主障碍已从内存安全转移到 TLS / 请求兼容性。

### Next

- 更稳健的 JSON / 消息项解析，减少对字符串扫描的依赖。
- 完善嵌套 `ref_msg`、多 item 组合消息的行为一致性。
- 补齐语音转码与更多 MIME / 缩略图相关细节。
- 继续增加可重复验证样例与回归测试，覆盖真实 API 返回体。
- 更完整的示例与测试。
- 跟进 `login` 的 `SSL connect error`，把真实微信链路从二维码获取推进到扫码确认。
- 在 `sp_str_t` 使用上继续做静态审计，避免再次把非 NUL 结尾字符串送进 libc 字符串 API。
