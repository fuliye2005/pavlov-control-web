# 前端任务与接口交接

## 任务

在现有功能上重新梳理信息架构和视觉，先提出页面方案，再实现。突出连接状态、交互模式、动作、语音；高级校准/保护/更新等可收纳，但不要删掉既有能力或伪造可用状态。用户未指定新视觉风格，先给可讨论的方案。

## 真实设备状态

已装机ESP0.38.1 / STM0.6.6。跳跃jump、抖腿shake_leg通过API识别检查、ARM离线测试，并各取得一次明确实物正常观察。此前左转/握手已修复。当前不保证所有速度/幅度和持续运行均通过。

罗盘暂缓；RGB放弃；没有SD卡相关开发；微信小程序取消。“巴甫洛夫”本地唤醒仍缺自定义模型。语音偶发上传失败/延迟尚未闭环。前端不要掩盖这些限制。

## 运行与鉴权

正式页面由设备 `/` 或 `/control` 同源提供，API前缀 `/api/v1/`。鉴权为Authorization: Bearer <用户提供的口令>。口令不可公开。现有页面从URL片段key或sessionStorage读取，并立即清除片段。保留现有行为；此仓库不含真实口令或带口令快捷入口。

静态预览服务器没有API。另一台电脑也不必能连接小狗：通过本地模拟数据开展UI开发，最终由主agent集成。不要为跨域预览降低设备鉴权或放开CORS。正式CSP限制网络同源、禁止外部脚本/样式等；实现应兼容device_api.c中control_page的策略。

## 接口契约

以control.html现有请求及对应C实现为准，不凭名称猜测请求体或返回字段。

- GET device/state/capabilities：身份、启动boot_id、控制器与传感器状态和能力。
- GET/POST mode：rest/companion/action；rest拦截动作，不等于设备断电。
- GET/POST settings：音量、默认速度、默认幅度、聊天动作开关；动作幅度保留百分比。
- POST actions：携带唯一request_id及现有动作参数；GET requests/<id>轮询结果。收到受理或输出完成不代表实物动作已获用户验收。请求超时须先查结果，不自动重发。
- POST motion/stop：现有停止语义，保持可访问。PWM保持与关闭输出不同，不混用文案。
- dialog/state、dialog/control：语音收听/阶段等；勿自行启动真实麦克风。
- timer、game、profile、memory、pose、power、rtc、board、camera、ble及ota等：继续参考当前页面和各服务代码；高级功能不得以静态成功提示冒充真实结果。

动作name见shared/action_catalog.h，既有ID1..13保持，jump=14、shake_leg=15。参数以现有capabilities/校验代码为准。100%是当前动作轨迹包络，不是已测得的绝对机械极限。没有可靠电量百分比时只显示电压；供电采样不自动等于准确电池端电压。

## 交付

保留文件路径，以分支/PR提供变更。提供Windows宽屏与安卓窄屏预览，说明离线/模拟检查和待实机验收项目。不要提交真实用户数据或凭据。由主agent完成固件构建、OTA和实机测试。


最新连续动作字段及测试范围见 VOICE-PLANNING.md。它优先于本文件中旧的单动作描述。


最新语音路由与收音参数请优先阅读 docs/ALL-MODEL-VOICE.md（在docs内则读ALL-MODEL-VOICE.md）：单口令也统一模型，替代旧的快速本地动作分流描述。
