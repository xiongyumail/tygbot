# TYGBot

ESP32-C3 门禁测试客户端。

**仅供授权测试使用，非官方项目。** 仅使用本人或明确获授权的账号、设备和门禁；遵守服务条款与适用法律，不用于绕过权限或访问未授权资源。测试用途声明不替代必要授权。

## 功能

- 在手机网页上开门
- 门禁来电时提醒，并可选择联动开门
- 接入 Home Assistant
- 兼作 Home Assistant 蓝牙网关

## 安装

需要 ESP32-C3 开发板（4 MB Flash）、USB 数据线、电脑上的 Chrome 或 Edge，以及 [Releases](https://github.com/xiongyumail/tygbot/releases) 中最新的 `tygbot-esp32c3-<版本>.factory.bin`。

### 1. 烧录

1. 用数据线连接开发板和电脑。
2. 打开 https://web.esphome.io/ ，点击“连接”，选择开发板的串口。
3. 点击“安装”，选择下载的 `factory.bin` 并确认。不要点“准备首次使用”，那会安装 ESPHome 默认固件。

也可使用 esptool：

```sh
esptool.py --chip esp32c3 write_flash 0x0 tygbot-esp32c3-<版本>.factory.bin
```

### 2. 连接 Wi-Fi

任选一种：

- **热点**：手机连接 `tygbot-setup`，在弹出的页面选择 Wi-Fi 并填写密码；未弹出时访问 `http://192.168.4.1/`。
- **蓝牙**：用 Chrome / Edge 打开 https://www.improv-wifi.com/ 连接设备并填写 Wi-Fi。iPhone 浏览器不支持。
- **USB**：在 https://web.esphome.io/ 连接设备后选择“配置 Wi-Fi”。经 CH340、CP2102 等转串口芯片连接的开发板不支持。

### 3. 登录

1. 手机连回同一个 Wi-Fi，打开 `http://tygbot.local/`。打不开时，在路由器的设备列表中查找 `tygbot` 的 IP 地址。
2. 按提示完成手机号、图形验证码和短信验证码登录。

## 使用

### 开门

在设备网页点击开门按钮。请求只发送一次，不自动重试；显示“成功”表示服务器已确认，不代表检测到门已打开。

### 门禁来电

收到门禁来电时，设备会匹配当前门禁，并在网页和 Home Assistant 中显示门禁名称。设备不接听，也不传输音视频。

“来电联动开门”默认关闭，仅在来电能唯一匹配当前门禁时可开启。开启后，每通来电只尝试开门一次；服务器确认成功后，设备结束来电。匹配失败、来电已结束或开门未成功时，不重试，也不结束来电。更换账号或当前门禁会自动关闭此功能。

- 从不支持门禁来电的版本升级后，需要重新短信登录一次。
- 来电配置保存在设备中，也可手动刷新。
- 来电服务使用 UDP（默认端口 6000）。与其他客户端共用账号时，其他客户端的来电可能受到影响。

## 接入 Home Assistant

设备连上 Wi-Fi 后，Home Assistant 会自动发现，点击“添加”即可，使用 Release 固件时无需密钥。用 ESPHome Device Builder 管理时，先在 Builder 中“接管”设备再添加，两边使用同一个密钥。

添加后提供：

| 类别 | 实体 |
| --- | --- |
| 开门 | 开门、门禁检查（不开门）、最近开门结果、登录状态、服务状态、当前住房、当前门禁 |
| 来电 | 门禁来电、来电来源、来电标识（诊断）、来电服务状态、来电联动开门、重新获取来电配置 |
| 诊断 | 服务就绪、开门处理中、来电服务已连接、内存、运行时间、Wi-Fi 信号 |
| 其他 | 蓝牙网关（在蓝牙集成中出现）、设备页的“访问”入口 |

实体名称为“英文 中文”格式，如 `Door Call 门禁来电`。需要纯中文时在实体设置中修改显示名称；不要修改固件中的名称，否则 Home Assistant 会当作新实体。设备信息中的固件版本带构建提交号，如 `1.2.1+a1b2c`。

呼叫通知可用自动化实现（实体 ID 以 Home Assistant 中显示的为准）：

```yaml
automation:
  - alias: 门禁来电通知
    triggers:
      - trigger: state
        entity_id: binary_sensor.tygbot_door_call_men_jin_lai_dian
        from: "off"
        to: "on"
    actions:
      - action: notify.notify
        data:
          message: "门禁来电：{{ states('sensor.tygbot_caller_lai_dian_lai_yuan') }}"
```

## 升级

以下方式保留 Wi-Fi 和登录：

| 方式 | 操作 |
| --- | --- |
| 设备网页 | 展开“固件升级”，选择新版本的 `ota.bin`，点击“上传” |
| 热点页面 | 设备开着配网热点时同上；连上 Wi-Fi 后此入口关闭 |
| ESPHome Device Builder | 把配置中的版本号改为新版本，点击“安装”。由 Builder 管理的设备只用这种方式，否则会提示配置未同步 |
| esptool | 写入 `factory.bin` 到 0x0，不擦除其他数据 |

通过 https://web.esphome.io/ 安装会擦除整个设备，之后需要重新配网和登录，可用于恢复初始状态。

## 更换 Wi-Fi

原 Wi-Fi 不可用时（如换了路由器），设备约 90 秒后自动开放配网，按“连接 Wi-Fi”重新设置。原 Wi-Fi 仍可用时，也可通过 USB 在 https://web.esphome.io/ 修改。

## 安全说明

- Release 固件不含密码：网页、接口、固件升级和配网热点都没有密码，只适合可信的家庭局域网，不要暴露到公网。
- 设备未连上 Wi-Fi 时开放蓝牙和热点配网，附近的人可以连接、修改 Wi-Fi 或上传固件。
- 登录信息、对讲账号（密码的 MD5）和对讲配置保存在设备中，未加密。

## 常见问题

**找不到热点？** 热点只在设备未连上 Wi-Fi 时开启：未配网时上电即开；已配网的设备连不上 Wi-Fi 约 90 秒后开启。

**找不到设备网页？** 确认手机和设备在同一个 Wi-Fi；`tygbot.local` 打不开时改用 IP 地址。

**USB 配网没反应？** 开发板可能经转串口芯片连接，改用热点或蓝牙。

**提示“多个授权住房”或“门禁不唯一”？** 在设备网页选择当前住房和门禁，见“多个住房或门禁”。

**对讲状态不是“对讲已注册”？** 显示“尚无对讲账号”时重新短信登录；显示注册超时时检查网络是否允许访问对讲平台的 UDP 端口；其他情况可按“重新获取对讲配置”。

## 进阶

### 自行编译

自行编译可以给 API、固件升级和热点设置密码。

1. 将 `tygbot.example.yaml`（Release 中也附有）复制为 ESPHome 配置目录中的 `tygbot.yaml`。
2. 将 `secrets.example.yaml` 中的三项加入 `secrets.yaml`。API 密钥可用 `openssl rand -base64 32` 生成。
3. 在 ESPHome Device Builder 中导入 `tygbot.yaml` 并安装。

设备名可在 substitutions 中修改，网页地址随之变为 `http://<设备名>.local/`。含个人密码的固件不要公开分享。

### 多个住房或门禁

只有一个有效住房和门禁时，设备会自动选择。存在多个目标时，在设备网页的“当前门禁”旁点击“选择”，按名称选择住房和门禁；设备会复用最近一次连接取得的授权列表，切换到其他住房时按需更新，并在保存前重新验证授权。选择结果重启后继续使用，不需要写入 ESPHome 配置。

### Release 文件

| 文件 | 用途 |
| --- | --- |
| `tygbot-esp32c3-<版本>.factory.bin` | 完整固件，USB 写入地址 0x0 |
| `tygbot-esp32c3-<版本>.ota.bin` | 仅应用程序，用于网络升级，不能用 USB 写入 0x0 |
| `tygbot.yaml` | ESPHome Device Builder 配置 |
| `SHA256SUMS` | 文件校验值 |

也可用命令行做网络升级：`esphome upload tygbot.yaml --file tygbot-esp32c3-<版本>.ota.bin --device <设备 IP>`。

### 接口

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/`、`/icon.png` | 网页与图标 |
| GET | `/health` | 最小存活状态、版本和构建提交号 |
| GET | `/status`、`/diagnostics` | 设备状态与诊断，含对讲状态和构建提交号 |
| GET | `/events` | 事件推送，见下节 |
| GET | `/households`、`/doors`、`/target` | 授权列表与当前选择 |
| POST | `/login/send`、`/login/captcha`、`/login/sms` | 登录 |
| POST | `/door?dry=1` | 门禁检查，不开门 |
| POST | `/door?from=web` | 开门 |
| POST | `/settings` | 保存“来电联动开门”开关 |
| POST | `/target` | 验证并保存当前住房和门禁 |
| POST | `/update` | 上传 `ota.bin`（multipart 表单）；只接受本设备网页的同源请求，配网热点上不限 |

### 事件推送

`GET /events` 返回 SSE 事件流，连上后先发一条 `snapshot`（与 `/status` 内容相同），之后按变化推送：

| 事件 | 时机 | 主要字段 |
| --- | --- | --- |
| `door_start` | 开始处理开门请求 | `source`、`ts` |
| `door` | 开门结束 | `ok`、`detail`、`seconds`、`at`、`source` |
| `login` | 登录状态变化 | `ok`、`at` |
| `call` | 门禁来电开始或结束 | `ringing`、`source`、`matched`、`ended_by`、`ts` |
| `target` | 当前住房或门禁变化 | `selected`、`automatic_supported`、`household`、`door` |
| `settings` | 来电联动设置变化 | `call_linked_open` |

```
event: call
data: {"ringing":true,"source":"一号门","matched":true,"ts":1234}
```

- 无事件时每 2 秒发送一行 `: ping`。
- 断线重连时带上 `Last-Event-ID` 请求头或 `?last_event_id=` 参数，可补收最近 8 条事件。
- 最多同时保持 2 条事件流，打开的设备网页也占用一条。
- 不保持连接时可轮询 `/status` 的 `call.ringing`，呼叫可能只持续十几秒，间隔需要足够短。

用命令行查看：`curl -N http://tygbot.local/events`。在脚本中接收呼叫：

```python
import json, urllib.request

with urllib.request.urlopen("http://tygbot.local/events") as stream:
    event = None
    for raw in stream:
        line = raw.decode().strip()
        if line.startswith("event:"):
            event = line[6:].strip()
        elif line.startswith("data:") and event == "call":
            call = json.loads(line[5:])
            if call["ringing"]:
                print("门禁来电", call["source"], "已匹配" if call["matched"] else "未匹配")
```

### 开发

核心和网页源文件在 `core/`，修改后执行同步与离线测试。测试使用模拟数据，不访问云端，也不开门。

```sh
python3 sync_core.py
bash test.sh
node --test tests/test_*.js
python3 -m unittest discover -s tests -v
```

离线测试需要 C 编译器、cJSON / mbedTLS 开发包、Node.js 和 Python 3。GitHub Actions 在每次推送时运行测试并编译固件，推送 `v*` 标签时发布 Release。

## 兼容性

开门和来电依赖门禁服务的云端接口，这些接口并非官方公开，服务方调整后可能无法使用。

固件使用 ESPHome 2026.9 编译和测试，其他版本的 ESPHome 可能无法编译，编译时会给出提示。

## 许可证

[Apache-2.0](LICENSE)。
