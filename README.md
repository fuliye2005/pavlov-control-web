# 巴甫洛夫机器狗前端交接包

这是供另一台电脑、另一位 agent 开发网页界面的最小源码包，不是完整固件工程。

## 开始

先阅读 AGENTS.md、docs/HANDOFF.md，再修改 `robot-dog/firmware/device-service/main/control.html`。
现有页面是内联 HTML/CSS/JavaScript，无 npm 依赖。可在仓库根目录运行：

```sh
python -m http.server 8080 --bind 127.0.0.1
```

浏览器打开 http://127.0.0.1:8080/robot-dog/firmware/device-service/main/control.html 。
此命令仅作静态预览，没有设备后端；未连接状态正常，不代表页面功能失败。
需要演示已连接界面时，请建立明确标识的本地模拟后端，不连接或操作实物。

## 版本与边界

2026-11-05：上层0.38.1，下层0.6.6。跳跃、抖腿已各获一次明确实物正常反馈；非所有速度/装配环境完整验收。“巴甫洛夫”本地唤醒尚未实现，罗盘暂缓。

保留现有接口和真实状态语义。使用中文，适配Windows与安卓浏览器。暂不做小程序或原生App。
仅开发前端，不自行操作小狗、不发动作、不开启收听、不执行升级。

## 文件说明

- `control.html`：当前完整控制页，优先编辑。
- 同目录的C/H文件：只读接口参考，不是完整可构建固件。
- `shared/`：动作目录和数据策略参考。
- `docs/HANDOFF.md`：交接任务与接口注意事项。
- `PUBLICATION-MANIFEST.json`：发布文件清单和摘要。

未包含Wi-Fi密码、控制口令、云密钥、用户聊天/记忆、私人配置、照片、录音、厂家整包或指导书PDF。未附完整固件及真实启动快捷入口。

完成前端后请通过分支/PR交回变更，由主agent审阅并完成固件集成、无线升级和实机验收。


最新语音路由与收音参数请优先阅读 docs/ALL-MODEL-VOICE.md（在docs内则读ALL-MODEL-VOICE.md）：单口令也统一模型，替代旧的快速本地动作分流描述。
