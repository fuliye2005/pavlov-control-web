> 更新：当前0.38.1，所有动作口令均由模型理解；最新说明见ALL-MODEL-VOICE.md。以下0.37.1说明为历史，接口字段仍可参考。

# 2026-11-05 语音动作规划接口更新

当前实机：ESP0.37.1 / STM0.6.6。单独说前进/后退/左转/右转无需称呼；locomotion_requires_name=false。精确的简单口令、本地可解析的连续动作走快速路径，复杂自然连续口令由DeepSeek输出结构化计划，经设备本地校验再顺序执行。

最多6个动作、预计总时长30秒。未知动作、字段/参数错误、嵌套计划、超量计划拒绝；否定/讨论不产生动作。每步等控制器完成才发下一步；任一步失败或网页/蓝牙停止会结束剩余步骤。诊断提示不作为模型聊天历史输入。该功能不是本地唤醒，也不支持执行/播音期间持续接收另一句话。

GET /api/v1/dialog/state 的 command_sequence 对象：
- active：计划是否正在运行。
- result：idle/running/finished/dry_run/invalid/rest_blocked/cancelled/aborted。
- source：none/local/model。
- model_planning：是否支持模型规划。
- planning_failures：模型返回格式无法通过校验并重试后仍失败的计数。
- total、completed、pending：计划总步数、已完成步数、待完成数（运行中含当前步；终止后pending为0）。
- max_actions=6、max_duration_ms=30000。
- actions：按顺序排列的动作名数组。
- steps：按顺序排列的参数对象数组，每项包含action、repeats、speed、strength。

结束后的actions/steps是最后一份计划摘要，不表示仍在排队。dry_run仅为诊断，不应显示为实物动作完成。active时可显示第completed+1步；已结束则不计算当前步。

已验证：28项实际C本地解析测试、37项实际C模型计划校验、7项设备端真实模型/本地路径诊断通过，包括自然多动作、分步快慢、非法混合及否定/讨论。诊断没有录音、播音或驱动舵机，不代表真人语音与实物连续动作已验收。跳跃、抖腿此前各一次实物通过记录仍保留。

注意语音上传偶发延迟仍是独立问题，不能把模型规划接入描述成实时打断或全部语音稳定性已解决。前端agent继续只做页面开发/模拟测试，不操作实物或自行升级。
