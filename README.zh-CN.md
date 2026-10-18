# Remote Master

[English](README.md) | **简体中文**

基于 ESP32-C3 的红外转蓝牙 HID 桥接设备，内置 Web 管理界面。学习普通红外
遥控器的按键，把按键映射为蓝牙键盘 / 鼠标 / 媒体控制或红外重放，并通过浏览器
完成全部配置。

## 功能

- **红外学习与重放** —— 采集红外接收头的原始脉冲并按按键保存，支持协议/码值
  识别以及可选的 GPIO4 发射驱动模式。
- **BLE HID 输出** —— 作为低功耗蓝牙键盘、鼠标和媒体遥控器使用，最多可配对
  3 台主机并在 Web 界面中切换。
- **灵活的映射** —— 将学习到的按键绑定到 HID 组合键、鼠标动作、媒体键、红外
  重放或多步动作序列。
- **动作序列** —— 组合 HID 与红外步骤（按下、释放、文本、延时、红外重放），
  每一步均可单独设置时序。
- **实时反馈** —— 通过 WebSocket 推送配置变更、设备状态、输入事件和错误；
  按下实体遥控器按键时，界面中对应的按键卡片会高亮。
- **完整离线部署** —— Vue 前端直接由设备的 LittleFS 提供，浏览器无需联网。

## 硬件

| 项目 | 说明 |
| --- | --- |
| MCU / 模组 | ESP32-C3-MINI-1（Arduino ESP32 核心 3.x） |
| 红外接收数据 | GPIO5 |
| 红外接收供电 | GND GPIO6、VCC GPIO7 |
| 红外发射 | GPIO4 |
| 状态指示灯 | GPIO8 |
| 存储 | 片内 Flash 上的 LittleFS |
| 网络 | Wi-Fi 热点（默认 SSID `Remote Master`，地址 `192.168.4.1`） |

设备以独立热点方式运行，不需要路由器。可在 **系统设置 → Wi-Fi** 中设置
WPA2 密码（8–63 个字符）；留空则使用开放热点。

## 目录结构

| 路径 | 用途 |
| --- | --- |
| `remote-master.ino` | 固件入口，Wi-Fi/HTTP/WebSocket 初始化与学习流程 |
| `remote_store.cpp/.h` | 配置持久化与校验 |
| `ble_hid.cpp/.h` | BLE HID 设备、配对与主机管理 |
| `ir_tx.cpp/.h` | 基于 RMT 的红外发射器 |
| `bindings.cpp/.h` | 动作模型与分发 |
| `action_router.cpp/.h` | 将映射动作路由到 BLE HID / 红外输出 |
| `file_api.cpp/.h` | `/api/files` 文件上传、列表与删除 |
| `fallback_ui.h` | 前端缺失时的最小回退页面 |
| `web/` | Vue 3 + Vite 前端源码 |
| `data/frontend/` | 前端构建产物（设备文件系统镜像） |
| `deploy.mjs` | 通过 HTTP 把 `data/` 镜像到设备 |

## 快速开始

### 依赖

- Arduino IDE 或 `arduino-cli`，并安装 ESP32 开发板支持包 3.x
- 库：**ArduinoJson 7.x**、**IRremote 4.x**、**NimBLE-Arduino 2.5.x**、
  **ESPAsyncWebServer**（含 AsyncWebSocket）
- Node.js 18+（用于 Web 前端）

### 编译固件

1. 在 Arduino IDE 中打开 `remote-master.ino`。
2. 开发板选择 **ESP32-C3 Dev Module**。
3. 分区方案选择 **No OTA (2MB APP / 2MB SPIFFS)**。
4. 烧录固件。

默认的 1.2 MB 应用分区空间不足。修改已有设备的分区布局前，请先备份配置和
采样数据。

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=no_ota .
```

### Web 前端（开发模式）

```bash
cd web
npm install
npm run dev
```

开发服务器监听 `http://localhost:5173`，并把 `/api/*` 与 `/ws` 代理到
[`web/.env.development`](web/.env.development) 中配置的设备地址（默认
`http://192.168.4.1`）。请先让电脑连接到设备热点。`deploy.mjs` 读取的是同一个
文件，因此设备地址只需在一个地方修改。

### 构建与部署

```bash
cd web
npm run deploy
```

该命令会把前端构建到 `data/frontend/`，并把 `data/**` 上传到设备。若只想部署
而不重新构建，或需要指定其他地址：

```bash
node deploy.mjs http://192.168.4.1
node deploy.mjs --no-clean        # 保留设备上已有的文件
node deploy.mjs --dir ./data --remote /frontend
```

也可以使用 Arduino IDE 的 **Upload LittleFS data** 功能上传 `data/` 目录，
效果完全一致。

## Web 界面

| 页面 | 用途 |
| --- | --- |
| **遥控器与学习** | 添加、重命名、导出、删除遥控器；进入学习模式并命名采集到的按键；重放已保存的按键 |
| **动作与序列** | 创建并测试动作序列；导出单个序列 |
| **按键映射** | 将学习到的按键绑定到 BLE HID 动作、红外重放或动作序列 |
| **系统设置** | Wi-Fi 热点、蓝牙名称/配对/状态、红外发射模式、存储、恢复初始状态 |

典型流程：

1. 添加一个红外遥控器，选中并开始学习。
2. 将实体遥控器对准接收头按键，命名采集到的按键并保存。
3. 打开 **按键映射**，选择输入按键和输出目标，然后测试。
4. 在 **系统设置 → 蓝牙主机** 中配对一个电脑端主机，即可使用。

### 红外发射模式

在 **系统设置 → 红外** 中选择 GPIO4 的驱动方式：

| `irTxMode` | 输出 |
| --- | --- |
| `off`（默认） | 高阻态，无内部上拉/下拉 |
| `push-pull` | 高电平有效的载波，空闲为低电平 |
| `sink` | 低电平有效的开漏载波，空闲释放引脚 |

请使用限流电阻，并把 GPIO 电流控制在 ESP32-C3 的额定范围内；驱动更大电流的
红外管时请使用三极管。GPIO4 不耐 5 V，灌电流模式下不得将引脚电压拉高到
3.3 V 以上。

RMT 生成占空比 33% 的载波，并对单帧采样做接收端 mark 补偿后重放。若采样中
保存了 `carrierHz` 则优先使用；否则 Sony 使用 40 kHz，RC5/RC6 使用 36 kHz，
Panasonic/Kaseikyo 使用 37 kHz，Bang & Olufsen 使用 455 kHz，未知协议默认
38 kHz。接收端只记录包络而不记录载波；不执行自动重复、翻转位更新或多帧重建。

## 配置与存储

- 配置保存在 `/remote-config.json`，原始采样保存在 `/captures/`，前端页面
  保存在 `/frontend/`。
- 配置 JSON 共用 **32 KiB** 预算。上限为 **12 个遥控器**、**3 台 BLE 主机**、
  **50 个动作序列**。按键和映射没有固定条目数限制，但同样占用这份 JSON 预算。
- 每个动作序列最多 12 个步骤、512 字节 ASCII 文本和 60 秒。
- 写入采用原子方式（临时文件 + rename）。启动时若文件系统或配置无法读取/不
  兼容，设备会直接格式化，以保证始终能进入可用状态。
- **清除配置和数据** 会格式化整个文件系统，从设备中移除管理前端、所有蓝牙
  配对和全部配置项。此操作不可撤销，完成后需要重新部署前端。

## 设备接口

固件在 80 端口提供 HTTP 接口和位于 `/ws` 的 WebSocket。

| HTTP 接口 | 说明 |
| --- | --- |
| `GET /api/status` | 设备、BLE 与红外状态 |
| `GET /api/config/meta` | 设置、主机与 ID 计数器 |
| `GET /api/config/remotes` | 遥控器 |
| `GET /api/config/keys` | 学习到的按键 |
| `GET /api/config/macros` | 动作序列 |
| `GET /api/config/mappings` | 按键映射 |
| `GET /api/export?scope=…` | 以 JSON 导出 `remote`、`macro`、`macros` 或全部数据 |
| `GET/POST/DELETE /api/files` | 列出、上传或删除 LittleFS 中的文件 |

WebSocket 请求格式为 `{id, op, args}`，响应会回显 `id` 以及 `ok`，失败时附带
`error`。主要操作：`state.get`、`settings.save`、`remotes.save`、`keys.save`、
`keys.batchSave`、`keys.send`、`learn.start`、`learn.stop`、`mappings.save`、
`mappings.test`、`macros.save`、`macros.test`、`hosts.save`、`hosts.pair`、
`hosts.reconnect`、`hosts.delete`、`hid.stop` 和 `system.reset`。

服务端推送事件：`state`、`input`、`capture.error`、`ir.error` 和
`config.changed`。

## 许可证

MIT
