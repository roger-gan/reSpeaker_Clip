# HTTPS 最小验证版

目标：停止录音后，由 USB 命令触发设备连接 WPA2 Wi-Fi，直接通过 HTTPS 上传一个已有片段。不是开机自动同步，也不是生产服务器。

## 当前边界

- 无样机：电脑端协议测试与固件编译不代表设备验收通过。
- 继承 `55ef94c` 外置 Wi-Fi 补丁方案。刷机前必须完成补丁写入、回读和 LittleFS 分区迁移；不能只刷应用包。
- BLE 保留；实验构建替换 AP 为 Station，USB 仅 CDC，不暴露录音磁盘。应用运行期间禁用 OTA 和非必要 AT 命令，bootloader 恢复入口不在本次修改范围。
- 只允许空闲时上传，上传期间不能录音或操作其他文件传输。最多 8 MiB，保留本地文件，不改变原手机同步标记。
- Wi-Fi 密码和令牌仅存在 RAM。CA 公钥编入固件；证书链、域名和有效期验证开启。USB 注入当前时间，重启须重新配置。
- TLS 收发记录缓冲各 4 KiB，只针对受控测试服务器，不能据此承诺兼容任意互联网服务器或长证书链。
- 取消不是强制释放线程。DNS 等底层调用可能延迟退出，须实测；上传中长按关机先取消，清理后需要再次发起关机。

## 1. 电脑端测试

需要 Python 3.10+ 和支持 `req -addext` 的 OpenSSL，无额外 Python 包。

```sh
python3 -m unittest discover -s tools/https_poc -p 'test_*.py' -v
```

覆盖认证、设备身份、长度和哈希校验、8 MiB、重复／冲突请求、并发提交、中断清理、重新打开存储、错误 CA 和主机名。测试用电脑 TLS 客户端，**不是固件 TLS 栈测试**。

## 2. 生成测试凭据并启动接收服务

在隔离目录生成有效期 7 天的测试证书；目录必须不存在。实际设备测试需使用设备可解析、可访问的域名，不能使用 `localhost`。

```sh
python3 tools/https_poc/make_credentials.py tools/https_poc/.local/lab --hostname clip-lab.example.net
python3 tools/https_poc/server.py \
  --bind 127.0.0.1 --port 8443 \
  --cert tools/https_poc/.local/lab/ca.pem \
  --key tools/https_poc/.local/lab/server.key \
  --token-file tools/https_poc/.local/lab/token \
  --device clip-test --directory tools/https_poc/.local/recordings
```

上述域名是占位符。电脑自测保持 loopback；设备联调另行绑定受控 LAN 地址并限制访问来源，不直接开放公网。服务仅为测试夹具，未实现生产级连接限流、配额、密钥轮换和监控。

接收端按设备／会话／片段生成对象名，验证后原子保存。同内容重传返回 200，新对象返回 201，不同内容冲突返回 409。正常中断会清理临时文件；进程被强制终止可能留下 `.incoming-*`，须停服后核对清理，不作为成功录音读取。

私钥、令牌和客户端配置不得提交或打印到日志。证书到期或更换后，重新生成并用对应公开 CA 重编译固件。

## 3. 服务器编译

先按项目规则同步到独立的 `reSpeaker_Clip-https` 目录。只传公开 CA 给构建过程，不传服务端私钥。Ubuntu 上执行：

```sh
source /home/ubuntu/firmware/.venv/bin/activate
python /home/ubuntu/firmware/reSpeaker_Clip-https/tools/https_poc/build.py \
  --ncs /home/ubuntu/firmware/ncs \
  --sdk /home/ubuntu/firmware/zephyr-sdk-0.17.0 \
  --build /home/ubuntu/firmware/ncs/build-clip-https \
  --ca /home/ubuntu/firmware/https-poc-credentials/ca.pem
```

`build.py` 仅构建，不刷机。应用配置为 `https.conf`，不把 TLS 配置施加到 MCUboot／网络核。完整输出为构建目录旁的 `.log`。

## 4. 样机到货后的控制方式

先完成外置补丁启动验证，再连接 USB CDC。建立权限为 `0600` 的私有 JSON 配置，字段为 `ssid`、`password`、`host`、`port`、`device`、`token`。`host` 必须与证书 DNS 名称一致，`device` 与接收端一致。SSID 不支持逗号；首版只支持 2.4 GHz WPA2-Personal。

```sh
python3 tools/https_poc/device.py --port /dev/cu.usbmodemXXXX --config /private/path/clip-test.json
python3 tools/https_poc/device.py --port /dev/cu.usbmodemXXXX --upload SESSION_ID:1
python3 tools/https_poc/device.py --port /dev/cu.usbmodemXXXX
python3 tools/https_poc/device.py --port /dev/cu.usbmodemXXXX --cancel
```

会话 ID 和正整数片段号来自现有录音列表，不是文件路径。工具不负责创建录音、刷机或补丁初始化；先用现有录音控制完成录音并停止。返回 `ok:true` 只代表命令被接受，必须查询至 `active:false` 且 `stage:succeeded` 才代表本次上传成功。

对应命令：`AT+NETCFG=ssid,password`、`AT+UPLOADCFG=host,port,device,token`、`AT+UPTIME=Unix秒`、`AT+UPLOAD=session:片段号`、`AT+UPLOAD?`、`AT+UPLOADCANCEL`。配置和上传控制仅接受 USB，响应不返回凭据。

## 5. 尚需样机完成的验收

外置补丁冷启动；Station/DHCP/DNS；正确证书与错误 CA／域名／时间；真实录音哈希及解码；取消、断网和重复上传；至少 20 次冷握手；录音／上传交替；堆与线程栈峰值；失败后恢复录音。

静态 RAM 剩余量不是 TLS 可用内存。TLS 与原业务共用系统堆，是否足够必须在上述压力场景中测量。
