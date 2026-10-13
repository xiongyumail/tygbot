# TYGBot

ESP32-C3 门禁测试客户端：用手机网页开门，也可接入 Home Assistant，并兼作蓝牙网关。

**仅供授权测试使用，非官方项目。** 仅使用本人或明确获授权的账号、设备和门禁；遵守服务条款与适用法律，不用于绕过权限或访问未授权资源。测试用途声明不替代必要授权。

## 准备

- ESP32-C3 开发板（4 MB Flash）和 USB 数据线
- 电脑上的 Chrome 或 Edge 浏览器
- 固件：在 [Releases](https://github.com/xiongyumail/tygbot/releases) 下载最新的 `tygbot-esp32c3-<版本>.factory.bin`

## 第一次使用

### 1. 烧录

1. 用数据线连接开发板和电脑。
2. 打开 https://web.esphome.io/ ，点击“连接”，选择开发板的串口。
3. 点击上传图标（安装），选择下载的 `factory.bin`，再点“安装”，等待完成。不要点“准备首次使用”，那会安装 ESPHome 自带的默认固件。

也可使用 esptool：

```sh
esptool.py --chip esp32c3 write_flash 0x0 tygbot-esp32c3-<版本>.factory.bin
```

### 2. 连接 Wi-Fi

任选一种方式：

- **蓝牙**：用 Chrome / Edge 打开 https://www.improv-wifi.com/ ，连接设备并填写 Wi-Fi，完成后点击链接进入设备网页。iPhone 浏览器不支持，请用热点方式。
- **热点**：手机连接 `tygbot-setup`，在自动弹出的页面选择 Wi-Fi 并填写密码。未弹出时访问 `http://192.168.4.1/`。
- **USB**：在 https://web.esphome.io/ 连接设备后选择“配置 Wi-Fi”。开发板经 CH340、CP2102 等转串口芯片连接时不支持，请用蓝牙或热点。

### 3. 登录并开门

1. 手机连回同一个 Wi-Fi，打开 `http://tygbot.local/`。打不开时，在路由器的设备列表中查找 `tygbot` 的 IP 地址。
2. 按提示完成手机号、图形验证码和短信验证码登录。
3. 点击开门按钮。

## 升级

以下方式会保留 Wi-Fi 和登录：

- **设备网页**：在设备网页展开“固件升级”，选择新版本的 `ota.bin` 后点击“上传”，完成后设备自动重启。
- **Home Assistant**：在 ESPHome Device Builder 中接管后，把配置里的版本号改为新版本，点击“安装”。用 Builder 管理的设备建议只用这种方式，否则 Builder 会提示配置未同步。
- **esptool**：执行 `esptool.py --chip esp32c3 write_flash 0x0 tygbot-esp32c3-<版本>.factory.bin`。只写入固件，不擦除其他数据。
- **热点页面**：设备开着配网热点时，在热点页面展开“固件升级”，选择新版本的 `ota.bin` 后点击“上传”。设备连上 Wi-Fi 后此入口关闭。

用 https://web.esphome.io/ 安装会先擦除整个设备，Wi-Fi 和登录都会清除，之后需要重新配网和登录。需要恢复初始状态时可以用这种方式。

## 更换 Wi-Fi

- 原 Wi-Fi 不可用时（如换了路由器或地点），设备约 90 秒后自动开放配网，按“连接 Wi-Fi”的方式重新设置。
- 原 Wi-Fi 仍可用时，也可通过 USB 在 https://web.esphome.io/ 修改。

已保存的 Wi-Fi 和登录在升级后保留（web.esphome.io 安装除外）。

## 接入 Home Assistant

设备连上 Wi-Fi 后，Home Assistant 会自动发现，点击“添加”即可。使用 Release 固件时不需要输入密钥。添加后提供：

- 开门按钮、只读检查按钮
- 登录状态、最近开门结果、运行状态
- 蓝牙网关（在蓝牙集成中自动出现）
- 设备页的“访问”入口，打开设备网页

使用 ESPHome Device Builder 管理时，建议先在 Builder 中“接管”设备，再添加集成，两边会使用同一个密钥。

## 安全说明

- Release 固件不含任何密码：网页、接口、固件升级和配网热点均无密码，只适合在可信的家庭局域网使用，不要暴露到公网。
- 设备未连上 Wi-Fi 时会开放蓝牙和热点配网，附近的人都能连接，修改 Wi-Fi 或上传固件。
- 登录信息保存在设备中，未加密。
- 开门请求只发送一次，不自动重试；网页显示的“成功”是服务器的确认结果，不代表已检测到门被打开。

## 常见问题

**USB 配网没反应？** 开发板可能经转串口芯片连接，请改用蓝牙或热点配网。

**找不到热点？** 热点只在设备没有连上 Wi-Fi 时开启：未配网时上电即有；已配网的设备在连不上 Wi-Fi 约 90 秒后开启。

**找不到设备网页？** 确认手机和设备在同一个 Wi-Fi；`tygbot.local` 打不开时改用 IP 地址。

**提示“多个授权住房”或“门禁不唯一”？** 账号下有多个住房或门禁时，需要在配置中指定，见“指定住房和门禁”。

## 进阶

### 自行编译

自行编译可以给 API、固件升级和热点设置密码。

1. 将 `tygbot.example.yaml` 复制为 ESPHome 配置目录中的 `tygbot.yaml`（Release 中也附有该文件）。
2. 将 `secrets.example.yaml` 中的三项加入 `secrets.yaml`。API 密钥可用 `openssl rand -base64 32` 生成。
3. 在 ESPHome Device Builder 中导入 `tygbot.yaml` 并安装。

设备名可在配置的 substitutions 中修改，网页地址随之变为 `http://<设备名>.local/`。含个人密码的固件不要公开分享。

### Release 文件

| 文件 | 用途 |
| --- | --- |
| `tygbot-esp32c3-<版本>.factory.bin` | 完整固件，USB 写入地址 0x0 |
| `tygbot-esp32c3-<版本>.ota.bin` | 仅应用程序，用于设备网页、热点页面或 ESPHome OTA 工具的网络升级，不能用 USB 从 0x0 写入 |
| `tygbot.yaml` | ESPHome Device Builder 配置 |
| `SHA256SUMS` | 文件校验值 |

也可用 ESPHome 命令行做网络升级：`esphome upload tygbot.yaml --file tygbot-esp32c3-<版本>.ota.bin --device <设备 IP>`。

### 指定住房和门禁

在浏览器打开 `http://tygbot.local/households` 和 `http://tygbot.local/doors` 查看可用的住房和门禁，然后在 `tygbot.yaml` 中加入对应的值，用 ESPHome Device Builder 安装：

```yaml
tygbot:
  organization: "<organization>"
  household: "<household>"
  door_number: "<number>"
```

只需填写区分所需的项；同一门禁有多个编号时填 `door_number`，多个门禁时填 `door_code`。

### 接口

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/`、`/icon.png` | 网页与图标 |
| GET | `/health`、`/status`、`/diagnostics` | 状态与诊断 |
| GET | `/events` | SSE 事件推送 |
| GET | `/households`、`/doors` | 授权住房与门禁列表 |
| POST | `/login/send`、`/login/captcha`、`/login/sms` | 登录 |
| POST | `/door?dry=1` | 只读检查，不开门 |
| POST | `/door?from=web` | 开门 |
| POST | `/update` | 上传 `ota.bin` 升级固件（multipart 表单）；只接受本设备网页发起的同源请求，配网热点上不限 |

### 开发

核心和网页源文件在 `core/`，修改后执行同步与离线测试。测试使用模拟数据，不访问云端，也不开门。

```sh
python3 sync_core.py
bash test.sh
node --test tests/test_*.js
python3 -m unittest discover -s tests -v
```

离线测试需要 C 编译器、cJSON / mbedTLS 开发包、Node.js 和 Python 3。GitHub Actions 在每次推送时运行测试并编译固件；推送 `v*` 标签时自动发布 Release。

## 兼容性

开门依赖门禁服务的云端接口，该接口并非官方公开。服务方调整接口后，TYGBot 可能无法使用，需要等待更新。

固件使用 ESPHome 2026.9 编译和测试。用 ESPHome Device Builder 安装时，其他版本的 ESPHome 可能无法编译，编译时会给出提示。

## 许可证

[Apache-2.0](LICENSE)。
