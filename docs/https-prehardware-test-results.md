# HTTPS 无设备测试记录

日期：2026-09-29。源码基线：`bd39e7a`，分支 `experiment/https-upload`。
测试平台：Mac，Homebrew Python 3.12.13，Apple Clang。

## 结论

原有 11 项接收服务测试通过。新增边界测试复现了服务端 TLS 握手阻塞、USB 客户端 JSON 类型处理错误；修复后共 25 项 Python 测试通过。固件函数的主机桩测试使用 AddressSanitizer 和 UBSan 重新编译运行通过。

本次仅修改电脑端服务、USB 控制工具及测试，没有修改固件源码，没有重新构建或烧录设备。测试服务仅监听本机临时端口，结束后关闭；未部署常驻服务。测试证书和令牌在临时目录生成并清理。

## 已复现并修复

1. **空闲 TCP 连接阻塞所有 HTTPS 上传**：原服务在监听套接字的 `accept()` 路径内完成 TLS 握手，客户端连接后不发送握手数据会阻止其他连接被处理。新增测试保持一个空闲 TCP 连接，同时上传另一个文件；修复前 3 秒握手超时。现在延迟握手到每连接工作线程，使用 15 秒连接超时，空闲连接期间上传成功。
2. **USB JSON 响应解析不严格**：数组会触发 `AttributeError`，字符串 `"false"` 被当作成功；事件对象也可能被误当成命令拒绝。现在只处理带布尔 `ok` 字段的响应对象，忽略事件和其他 JSON。有效的 `ok:false` 仍报告失败。这些是模拟串口输入测试，不表示当前设备已实际发出这些异常内容。

## Python 测试覆盖

运行入口（Python 3.10+）：

```sh
python3 -m unittest discover -s tools/https_poc -p 'test_*.py' -v
```

- 16 项接收端测试：认证和设备身份、重复提交、内容冲突、并发重复和并发冲突、重复 Content-Length、哈希错误、超限、路径限制、空文件、8 MiB 文件、上传中断及清理、重开服务后的幂等行为、错误 CA/域名、空闲握手隔离、回执及落盘字节一致性。
- 其中一项连续执行 20 次新建 TLS 1.2 连接上传，逐一核对哈希和落盘内容。不是 Clip 上的冷握手或内存泄漏测试。
- 9 项 USB 工具测试：分片响应、事件及非对象 JSON、非布尔 `ok`、设备拒绝、断开、超长响应、命令长度、等待超时和部分写入。I/O 使用 mock，不需要真实串口。

## 固件函数主机回归

核对现有归档测试中的 `header_fragment`、`receive_response`、`upload`、`tls_connect`、`network_event`、`wifi_init` 六个函数与当前源码完全一致，再重新编译执行：

```sh
clang -fsanitize=address,undefined -g -O1 \
  .analysis/https-prehardware-20260929/firmware-regression.c \
  -o .analysis/https-prehardware-20260929/firmware-regression
.analysis/https-prehardware-20260929/firmware-regression
```

以上路径相对本地项目父目录 `recording/`。测试归档在该目录，非源码仓库中的可移植测试入口。

通过 DHCP/断连/认证失败事件及接口过滤、响应头和响应体边界与分片超限、HTTP 回调返回值被忽略时的最终错误判定、连接失败/未完成握手、取消和任务剩余时间检查；Sanitizer 无报错。

该测试使用网络、HTTP、JSON 等主机桩，JSON 桩返回预设有效回执。它不验证真实 Zephyr JSON/TLS/驱动实现，也不覆盖真实线程竞态、硬件延迟或整个上传任务资源释放。

## 联调前发现的配置障碍

旧构建使用的测试 CA（SHA-256 `18d285be59210551faf803abfce1206179d62dfadc6c6130013189cefda71d6e`）域名为 `localhost`，有效期截至 **2026-09-27 06:45:44 UTC**。其哈希与最新留档构建报告一致。

设备联调时须生成设备实际可解析和访问域名的证书，使用匹配的公开 CA 重新构建，并注入正确时间。不能以关闭证书校验解决此问题。本次测试使用新生成的短期证书，不延长旧证书或替换旧归档。

## 仍需实机验证

外置 Wi-Fi 补丁写入/回读和 LittleFS 迁移、Wi-Fi/DHCP/DNS、设备 TLS 证书及时间验证、真实录音可解码、堆/栈峰值、取消实际时延、弱网恢复、录音上传交替及功耗。当前结果不构成设备端 HTTPS 直传验收。
