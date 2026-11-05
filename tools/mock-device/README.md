# 本地模拟后端（仅供前端开发）

> 全部为模拟数据，只监听本机 `localhost`，不连接、不扫描、不操作任何实物机器狗。
> **模拟检查通过 ≠ 实机通过**，实机验收仍由主 agent 完成。

只用 Windows 自带的 PowerShell 5.1，不需要安装 Python、Node 或其他依赖。

## 启动

双击 `start-mock.cmd`，或在仓库根目录运行：

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File tools\mock-device\mock-device.ps1
```

可选参数：`-Port 8787`（默认）、`-Token mock-token`（默认）。

然后在浏览器打开：

- 控制页（自动填入模拟口令）：http://localhost:8787/#key=mock-token
- 模拟场景开关：http://localhost:8787/__mock

页面顶部有一条棕色横幅，标明这是模拟后端。真实固件不会注入这条横幅。

场景页还能触发**连续动作**（`dialog/state.command_sequence`）：本地解析 3 步、模型规划 4 步、诊断演练、校验未通过、让当前步失败、清空；运行中点停止会变成“已取消”。

## 它做了什么

- 在 `/` 和 `/control` 提供 `robot-dog/firmware/device-service/main/control.html`，每次请求都重新读取文件，改完 HTML 刷新浏览器即可。
- 响应头带上与 `device_api.c` 相同的 CSP，能提前发现违反 CSP 的写法，比如外部脚本、样式或图片。
- `/api/v1/*` 模拟固件接口：
  - 需要 `Authorization: Bearer <口令>`，口令错误返回 401。
  - 带 `?` 的查询返回 400。
- 请求体校验和错误码参照 C 代码，例如：
  - 休息模式下发动作返回 `409 rest_mode_motion_blocked`；
  - `boot_id` 不一致返回 `409 device_boot_changed`；
  - 校准、硬件检查、RTC、保护等写操作要求“休息 + 暂停收听”，否则返回 409。
- 动作回执按时间推进：`gateway_received → controller_accepted → running → output_finished`。时长按 `shared/action_catalog.h` 的帧时间计算。
- 计时、猜谜、记忆、偏好、设置、供电、姿态与磁校准流程、摄像头拍照（生成一张写着“模拟照片”的 JPEG）、蓝牙配对窗口、无线升级（校验 SHA-256 后模拟重启并更换 boot_id）都有状态。

## 模拟场景（/__mock）

| 开关 | 选项 | 用途 |
|---|---|---|
| API 连接 | ok / down | down 时所有接口返回 503，检查“连接已中断 / 状态已过期” |
| 请求延迟 | 0 / 800 / 3000 / 7000 ms | 7000 超过页面 6 秒超时，检查“执行结果尚未确认” |
| 下层控制器 | online / offline | 控制器未连接、传感器不可用 |
| 动作结果 | finish / fail / hang | 动作失败；或一直进行中，检查超时后自动发送停止 |
| 供电读数 | ok / low / unavailable | 低电、读数不可用（不显示百分比） |
| 语音 | ok / error | 上传失败、重试中 |
| Wi-Fi 信号 | -50 / -68 / -82 | 良好 / 一般 / 较弱 |

另外还能：

- 模拟“最近一句”听到的话；
- 模拟设备重启（更换 `boot_id`）。

## 与真实设备的差异

- 字段只覆盖页面用到的部分和主要状态字段，诊断类字段没有全部模拟。
- 语音不会真的收听：开启收听后只显示“在听”，“最近一句”在场景页手动注入。
- 姿态保护、低电限制的锁定条件只做了简化模拟。
- 罗盘航向在模拟中校准后会显示固定值 37.5°，仅用于界面开发。真实设备上罗盘仍处于暂缓状态。
- 返回的 JSON 带有 `"mock": true` 标记（`device`、`state`、`dialog/state`、`capabilities`）。
