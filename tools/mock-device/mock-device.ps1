<#
  巴甫洛夫 本地模拟后端（仅供前端开发）
  - 只在本机 localhost 监听，不连接、不扫描、不操作任何实物机器狗。
  - 所有返回均为模拟数据；模拟通过不代表实机通过。
  - 接口路径、请求体校验和返回字段参照 device_api.c 等只读参考代码，力求一致，但不保证完全等同。
  用法：powershell -ExecutionPolicy Bypass -File tools\mock-device\mock-device.ps1 [-Port 8787] [-Token mock-token]
#>
param(
    [int]$Port = 8787,
    [string]$Token = 'mock-token'
)
$ErrorActionPreference = 'Stop'
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$PagePath = Join-Path $RepoRoot 'robot-dog\firmware\device-service\main\control.html'
$Csp = "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src blob:; base-uri 'none'; frame-ancestors 'none'; form-action 'self'"

function Now-Ms { [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }
function New-BootId { -join ((1..16) | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) }) }

# ---------- 动作目录（与 shared/action_catalog.h 一致：每帧 [过渡ms, 保持ms]） ----------
$Catalog = [ordered]@{
    backward   = @(@(100,50),@(100,50),@(100,50),@(100,50),@(100,50))
    turn_left  = @(@(100,50),@(100,75),@(100,50))
    turn_right = @(@(100,50),@(100,75),@(100,50))
    stand      = @(,@(200,300))
    squat      = @(@(300,100),@(200,400))
    lie_down   = @(@(300,100),@(200,400))
    stretch    = @(@(200,100),@(200,150),@(200,250),@(200,100),@(200,100))
    handshake  = @(@(300,100),@(200,400),@(200,0),@(300,0))
    rock_fb    = @(@(200,200),@(200,200))
    rock_lr    = @(@(200,300),@(200,300))
    step       = @(@(100,50),@(100,25),@(100,50),@(100,25))
    wag        = @(@(200,200),@(200,200))
    forward    = @(@(100,50),@(100,50),@(100,50),@(100,50),@(100,50))
    jump       = @(@(200,700),@(200,300),@(200,100))
    shake_leg  = @(@(200,50),@(100,50),@(100,50),@(100,50),@(100,50),@(100,50),@(100,50),@(100,50),@(200,100))
}
function Step-Time([int]$ms, [int]$speed, [int]$minimum) {
    $s = if ($speed -eq 1) { $ms * 2 } elseif ($speed -eq 3) { [math]::Floor($ms * 3 / 4) } else { $ms }
    if ($s -lt $minimum) { $s = $minimum }
    return [int]([math]::Floor(($s + 9) / 10) * 10)
}
function Action-Duration([string]$name, [int]$repeats, [int]$speed) {
    $cycle = 0
    foreach ($f in $Catalog[$name]) { $cycle += (Step-Time $f[0] $speed 100) + (Step-Time $f[1] $speed 0) }
    return 1000 + $cycle * $repeats
}

# ---------- 模拟场景开关（可在 /__mock 页面切换） ----------
$Scenario = [ordered]@{
    api        = 'ok'        # ok | down        整个 API 返回 503，模拟连接中断
    latency_ms = 0           # 每个 API 请求额外延迟
    controller = 'online'    # online | offline 下层控制器离线
    action     = 'finish'    # finish | fail | hang  动作回执结果
    supply     = 'ok'        # ok | low | unavailable
    voice      = 'ok'        # ok | error       语音上传失败
    wifi_rssi  = -58
    stage      = 'auto'      # auto | recognizing | thinking | playing  收听开启时强制语音阶段（看狗脸用）
    posture    = 'upright'   # upright | side | inverted  姿态分类
}

# ---------- 设备状态 ----------
$S = @{
    boot_id = New-BootId; started = Now-Ms
    esp_version = '0.35.0-mock'; ctrl_version = '0.6.6-mock'
    mode = 'companion'
    dialog_enabled = $false; dialog_active_until = 0; turns = 0
    last_heard = ''; last_reply = ''; last_motion_state = 'none'; last_motion_id = ''; reply_emotion = 'auto'
    routine = $null
    settings = [ordered]@{ volume = 35; default_speed = 'normal'; default_strength = 25; chat_actions = $true }
    profile = [ordered]@{ nickname = ''; reply_style = 'natural'; revision = 0 }
    memory = New-Object System.Collections.ArrayList; memory_rev = 0
    requests = [ordered]@{}; active_id = $null; stop_pending_until = 0
    timer = @{ state = 'idle'; duration = 0; ends = 0; generation = 0 }
    game = @{ active = $false; round = 0; attempts = 0; correct = 0; hinted = $false; revealed = $false; solved = $false; idx = -1; last_reply = '' }
    power = @{ source = 'unknown'; calibrated = $false; reference_mv = 0; alerts = $false; low_mv = 7000; recover_mv = 7200; settings_id = 1 }
    limit = @{ enabled = $false; observing = $false; latched = $false; reason = ''; accepted_settings = 0; observed = 0 }
    guard = @{ enabled = $false; observing = $false; latched = $false; reason = ''; accepted = $false; stop_requests = 0 }
    pose = @{ calibrated = $false; active = $false; stage = 0; collecting_until = 0; error = '' }
    compass = @{ calibrated = $false; active = $false; start = 0; error = '' }
    camera = @{ host_started = $false; phase = 'disabled'; phase_until = 0; next_phase = ''; photo = $null; photo_seq = 0; photo_at = 0 }
    display_refreshes = 120
    ble = @{ pairing_until = 0 }
    ota = @{ reboot_at = 0; up_at = 0; target = '' }
    board = @{ probed = $false; eeprom_checked = $false; sd_checked = $false; presses = 0; stops = 0 }
    rtc = @{ offset_ms = $null; sync_status = 'none' }
}
$Riddles = @(
    @{ q = '有头没有颈，身上冷冰冰，有翅不能飞，无脚也能行。打一动物'; h = '生活在水里'; a = '鱼' },
    @{ q = '小小一间房，只有一扇窗，什么都能看，就是不能摸。打一物品'; h = '家里客厅常见'; a = '电视' },
    @{ q = '身穿绿衣裳，肚里水汪汪，生的子儿多，个个黑脸膛。打一水果'; h = '夏天最常吃'; a = '西瓜' }
)

# ---------- 工具函数 ----------
function Send-Bytes($ctx, [int]$status, [byte[]]$bytes, [string]$type) {
    $r = $ctx.Response
    $r.StatusCode = $status; $r.ContentType = $type
    $r.Headers['Cache-Control'] = 'no-store'
    $r.ContentLength64 = $bytes.Length
    $r.OutputStream.Write($bytes, 0, $bytes.Length); $r.OutputStream.Close()
}
function Send-Json($ctx, [int]$status, $obj) {
    $obj['boot_id'] = $S.boot_id
    $json = $obj | ConvertTo-Json -Depth 12 -Compress
    Send-Bytes $ctx $status ($Utf8.GetBytes($json)) 'application/json'
}
function Send-Error($ctx, [int]$status, [string]$code) { Send-Json $ctx $status ([ordered]@{ error = $code }) }
function Has-Only($body, [string[]]$allowed) {
    if ($null -eq $body -or $body -isnot [psobject]) { return $false }
    foreach ($p in $body.PSObject.Properties) { if ($allowed -notcontains $p.Name) { return $false } }
    return $true
}
function Has-Prop($body, [string]$name) { return $null -ne $body -and $null -ne $body.PSObject.Properties[$name] }
function Get-Int($v, [int]$lo, [int]$hi) {
    if ($null -eq $v -or $v -is [string] -or $v -is [bool]) { return $null }
    try { $d = [double]$v } catch { return $null }
    if ($d -ne [math]::Floor($d) -or $d -lt $lo -or $d -gt $hi) { return $null }
    return [int]$d
}
function Rest-Idle { return ($S.mode -eq 'rest') -and (-not $S.dialog_enabled) -and ((Now-Ms) -ge $S.dialog_active_until) }
function Controller-Online { return $Scenario.controller -eq 'online' }

# ---------- 时间推进 ----------
function Receipt-State($r) {
    if ($r.final) { return $r.final }
    $t = (Now-Ms) - $r.start
    if ($t -lt 150) { return 'gateway_received' }
    if ($t -lt 400) { return 'controller_accepted' }
    if ($t -lt 400 + $r.duration -or $Scenario.action -eq 'hang') { return 'running' }
    if ($Scenario.action -eq 'fail') { $r.final = 'failed'; $r.error = 'mock_controller_reported_failure' } else { $r.final = 'output_finished' }
    return $r.final
}
function Update-World {
    $now = Now-Ms
    if ($S.active_id) {
        $st = Receipt-State $S.requests[$S.active_id]
        if (@('output_finished','failed','cancelled','unknown') -contains $st) { $S.last_motion_state = $st; $S.active_id = $null }
    }
    if ($S.routine -and $now -ge $S.routine.ends) {
        $S.turns++; $S.last_motion_state = 'routine_finished'; $S.routine = $null
        $S.last_reply = '（模拟）组合动作流程已结束。'
    }
    if ($S.timer.state -eq 'running' -and $now -ge $S.timer.ends) { $S.timer.state = 'finished' }
    if ($S.pose.active -and $S.pose.collecting_until -gt 0 -and $now -ge $S.pose.collecting_until) {
        $S.pose.collecting_until = 0; $S.pose.stage++
        if ($S.pose.stage -ge 3) { $S.pose.active = $false; $S.pose.calibrated = $true; $S.guard.accepted = $false }
    }
    if ($S.camera.next_phase -and $now -ge $S.camera.phase_until) { $S.camera.phase = $S.camera.next_phase; $S.camera.next_phase = '' }
    if ($S.ota.reboot_at -gt 0 -and $now -ge $S.ota.reboot_at) {
        $S.ota.reboot_at = 0; $S.boot_id = New-BootId; $S.started = $now; $S.esp_version = $S.ota.target
        $S.dialog_enabled = $false; $S.requests = [ordered]@{}; $S.active_id = $null
        $S.timer = @{ state = 'idle'; duration = 0; ends = 0; generation = 0 }
    }
}
# ---------- 连续动作（对应 dialog/state.command_sequence，见 docs/VOICE-PLANNING.md） ----------
$SeqDemos = @{
    local = @{ source = 'local'; steps = @(@('stand',1,'normal',25), @('wag',2,'normal',25), @('handshake',1,'slow',25)) }
    model = @{ source = 'model'; steps = @(@('squat',1,'slow',25), @('stand',1,'normal',25), @('rock_lr',1,'fast',30), @('wag',1,'normal',25)) }
}
function Seq-Update {
    $q = $S.seq; if (-not $q -or $q.result -ne 'running') { return }
    $done = [math]::Min($q.steps.Count, [int][math]::Floor(((Now-Ms) - $q.start) / $q.per))
    $q.completed = $done
    if ($done -ge $q.steps.Count) { $q.result = $(if ($q.dry) { 'dry_run' } else { 'finished' }); $S.last_motion_state = 'sequence_finished' }
}
function Seq-Json {
    Seq-Update; $q = $S.seq
    $base = [ordered]@{ active = $false; result = 'idle'; source = 'none'; model_planning = $true; planning_failures = 0; total = 0; completed = 0; pending = 0; max_actions = 6; max_duration_ms = 30000; actions = @(); steps = @() }
    if (-not $q) { return $base }
    $active = $q.result -eq 'running'
    $base.active = $active; $base.result = $q.result; $base.source = $q.source; $base.total = $q.steps.Count; $base.completed = $q.completed
    $base.pending = $(if ($active) { $q.steps.Count - $q.completed } else { 0 })
    $base.actions = @($q.steps | ForEach-Object { $_[0] })
    $base.steps = @($q.steps | ForEach-Object { [ordered]@{ action = $_[0]; repeats = $_[1]; speed = $_[2]; strength = $_[3] } })
    return $base
}
function Seq-Running { Seq-Update; return ($S.seq -and $S.seq.result -eq 'running') }
function Is-Moving {
    if ($S.routine) { return $true }
    if (Seq-Running) { return $true }
    if ($S.active_id) { return @('controller_accepted','running') -contains (Receipt-State $S.requests[$S.active_id]) }
    return $false
}
function Stop-All {
    Stop-Motion
    $now = Now-Ms
    $S.dialog_enabled = $false
    if ($S.dialog_active_until -lt $now + 300) { $S.dialog_active_until = $now + 300 }
}
function Stop-Motion {
    $now = Now-Ms
    if ($S.active_id) {
        $r = $S.requests[$S.active_id]
        if (-not $r.final) { $r.final = 'cancelled'; $S.stop_pending_until = $now + 400 }
        $S.last_motion_state = 'cancelled'; $S.active_id = $null
    }
    if ($S.routine) { $S.routine = $null; $S.turns++; $S.last_motion_state = 'cancelled'; $S.stop_pending_until = $now + 400 }
    if (Seq-Running) { $S.seq.result = 'cancelled'; $S.last_motion_state = 'sequence_aborted'; $S.stop_pending_until = $now + 400 }
}

# ---------- JSON 构造 ----------
function Supply-Mv {
    if ($Scenario.supply -eq 'low') { return 6820 + (Get-Random -Maximum 30) }
    $base = 7640 - [int](((Now-Ms) - $S.started) / 60000) * 3
    if (Is-Moving) { $base -= 180 }
    return $base + (Get-Random -Maximum 25)
}
function Power-Json {
    $fresh = $Scenario.supply -ne 'unavailable'; $mv = Supply-Mv
    $p = $S.power; $low = $fresh -and $p.alerts -and $mv -lt $p.low_mv
    $status = if (-not $fresh) { 'unavailable' } elseif ($p.source -eq 'unknown') { 'source_required' } elseif ($p.source -eq 'usb') { 'usb_source' } elseif (-not $p.calibrated) { 'reference_required' } elseif (-not $p.alerts) { 'disabled' } elseif ($low) { 'low' } else { 'monitoring' }
    $j = [ordered]@{ source = $p.source; calibrated = $p.calibrated; persisted = $true
        calibration_method = $(if ($p.calibrated) { 'user_measured_reference_single_point' } else { 'nominal_divider_uncalibrated' })
        reference_accuracy_verified_by_device = $false; alerts_enabled = $p.alerts; low = $low
        low_mv = $p.low_mv; recover_mv = $p.recover_mv; notifications = 0; motion_limit_enabled = $S.limit.enabled; status = $status }
    if ($p.calibrated) { $j['reference_millivolts'] = $p.reference_mv }
    return $j
}
function Supply-Json {
    $fresh = $Scenario.supply -ne 'unavailable'; $mv = Supply-Mv
    $cls = if (-not $fresh) { 'unavailable' } elseif ($S.power.source -eq 'unknown') { 'source_unconfirmed' } elseif ($mv -lt 7000) { 'estimated_low' } else { 'estimated_ok' }
    return [ordered]@{ supported = $true; available = $fresh; calibrated = $S.power.calibrated; measurement = 'switched_supply_pc3_adc1'
        battery_terminal_measurement = $false; battery_percent = $null; age_ms = 180; classification = $cls
        millivolts = $(if ($fresh) { $mv } else { $null }); monitor = (Power-Json) }
}
function Pose-Angles { if (Is-Moving) { return $null }; return @{ roll = 1.2 + (Get-Random -Maximum 10) / 20; pitch = -2.4 + (Get-Random -Maximum 10) / 20 } }
function Compass-Json {
    $c = $S.compass; $samples = 0
    if ($c.active) { $samples = [math]::Min(256, [int](((Now-Ms) - $c.start) / 100)) }
    $status = if ($c.active) { 'calibration_in_progress' } elseif (-not $c.calibrated) { 'calibration_required' } elseif (-not $S.pose.calibrated) { 'mount_calibration_required' } elseif (Is-Moving) { 'motion_in_progress' } elseif (-not (Controller-Online)) { 'sample_unavailable' } else { 'available' }
    return [ordered]@{ calibrated = $c.calibrated; persisted = $true; available = ($status -eq 'available'); status = $status
        heading_deg = $(if ($status -eq 'available') { 37.5 } else { $null }); absolute_reference_verified = $false
        reference = 'local_magnetic_north_not_true_north'; active = $c.active; collecting = ($c.active -and $samples -lt 256)
        samples = $samples; minimum_samples = 96; maximum_samples = 256; rejected_samples = 0; error = $c.error }
}
function Sensors-Json {
    $on = Controller-Online; $a = Pose-Angles; $p = $S.pose
    $imu = [ordered]@{ supported = $true; available = $on; calibrated = $false; error = 0; gravity_tilt_available = ($on -and $null -ne $a) }
    if ($on -and $a) { $imu['roll_deg'] = $a.roll; $imu['pitch_deg'] = $a.pitch }
    $pose = [ordered]@{ available = ($on -and $p.calibrated -and $null -ne $a); mount_calibrated = $p.calibrated; posture = $(if ($on) { $Scenario.posture } else { 'unknown' })
        motion_protection_enabled = $S.guard.enabled
        calibration = [ordered]@{ active = $p.active; collecting = ($p.collecting_until -gt 0); stage = $p.stage; samples = $(if ($p.collecting_until -gt 0) { [math]::Max(0, 40 - [int](($p.collecting_until - (Now-Ms)) / 50)) } else { 0 }); samples_required = 40; calibrated = $p.calibrated; error = $p.error } }
    if ($pose.available) { $pose['roll_deg'] = $a.roll - 1.0; $pose['pitch_deg'] = $a.pitch + 2.0 }
    return [ordered]@{ imu = $imu; magnetometer = [ordered]@{ supported = $true; available = $on; calibrated = $false; error = 0 }; compass = (Compass-Json); pose = $pose }
}
function State-Json {
    $now = Now-Ms; $on = Controller-Online
    return [ordered]@{ mock = $true; network_online = $true; calibration_active = ($S.pose.active -or $S.compass.active)
        controller_version = $S.ctrl_version; controller_online = $on; controller_status_age_ms = $(if ($on) { 120 } else { 9000 })
        baseline_set = $true; armed = $true; controller_moving = ($on -and (Is-Moving)); stop_pending = ($now -lt $S.stop_pending_until)
        supply = (Supply-Json); sensors = (Sensors-Json); wifi_rssi_dbm = $Scenario.wifi_rssi; wifi_channel = 6; ip = '127.0.0.1' }
}
function Interaction-Json {
    return [ordered]@{ interaction_mode = $S.mode; persisted = $true; autonomous_motion = $false
        chat_expression_allowed = ($S.mode -eq 'companion' -and $S.settings.chat_actions); autonomous_locomotion = $false; rest_requires_cloud_asr = $true }
}
function Dialog-Json {
    $now = Now-Ms; $seqOn = Seq-Running; $active = ($null -ne $S.routine) -or $seqOn -or ($now -lt $S.dialog_active_until)
    $stage = if ($S.routine -or $seqOn) { 'executing_motion' } elseif (-not $S.dialog_enabled) { 'paused' } elseif ($Scenario.voice -eq 'error') { 'retry_wait' } elseif ($Scenario.stage -ne 'auto') { $Scenario.stage } else { 'listening' }
    return [ordered]@{ name = '巴甫洛夫'; mode = 'standalone_cloud'; mock = $true; enabled = $S.dialog_enabled; wifi_online = $true
        active = $active; stage = $stage; last_error = $(if ($Scenario.voice -eq 'error') { 1 } else { 0 }); turns = $S.turns
        last_failure_stage = $(if ($Scenario.voice -eq 'error') { 'asr_upload' } else { '' })
        last_heard = $S.last_heard; last_reply = $S.last_reply; reply_emotion = $S.reply_emotion; wake_word_required = $false
        voice_routines = @('greet','show'); interaction = (Interaction-Json); voice_strength_limit = 100
        last_motion_request_id = $S.last_motion_id; last_motion_state = $S.last_motion_state
        locomotion_requires_name = $false; command_sequence = (Seq-Json) }
}
function Settings-Json {
    $j = [ordered]@{}; foreach ($k in $S.settings.Keys) { $j[$k] = $S.settings[$k] }
    $j['persisted'] = $true; $j['voice_strength_limit'] = 100; $j['volume_apply'] = 'next_playback'; return $j
}
function Profile-Json {
    return [ordered]@{ nickname = $S.profile.nickname; reply_style = $S.profile.reply_style
        has_preferences = [bool]($S.profile.nickname -or $S.profile.reply_style -ne 'natural'); persisted = $true; revision = $S.profile.revision }
}
function Memory-Json { return [ordered]@{ notes = @($S.memory.ToArray()); capacity = 8; revision = $S.memory_rev; persisted = $true } }
function Timer-Json {
    $t = $S.timer; $rem = 0
    if ($t.state -eq 'running') { $rem = [int][math]::Ceiling(($t.ends - (Now-Ms)) / 1000) }
    return [ordered]@{ state = $t.state; duration_seconds = $t.duration; remaining_seconds = $rem; generation = $t.generation
        notification_pending = $false; notified = ($t.state -eq 'finished'); notifications = 0; alert_error = 0
        survives_restart = $false; offline_tone = $true; offline_voice = $false }
}
function Game-Json {
    $g = $S.game; $r = $null; if ($g.idx -ge 0) { $r = $Riddles[$g.idx] }
    return [ordered]@{ active = $g.active; round = $g.round; attempts = $g.attempts; correct_answers = $g.correct
        hinted = $g.hinted; revealed = $g.revealed; solved = $g.solved
        question = $(if ($g.active -and $r) { $r.q } else { '' }); hint = $(if ($g.hinted -and $r) { $r.h } else { '' })
        answer = $(if (($g.revealed -or $g.solved) -and $r) { $r.a } else { '' }); last_reply = $g.last_reply
        automatic_motion = $false; survives_restart = $false }
}
function Request-Json($r) {
    return [ordered]@{ request_id = $r.id; action = $r.action; argument = $r.repeats; repeats = $r.repeats; speed = $r.speed_name
        strength = $r.strength; duration_ms = $r.duration; state = (Receipt-State $r)
        completion_evidence = $(if ((Receipt-State $r) -eq 'output_finished') { 'pwm_sequence' } else { 'none' })
        physical_position_verified = $false; error = $r.error }
}
function PowerLimit-Json {
    $p = $S.power; $l = $S.limit; $fresh = $Scenario.supply -ne 'unavailable'; $mv = Supply-Mv
    $ready = ($p.source -eq 'battery_2s' -and $p.calibrated)
    return [ordered]@{ enabled = $l.enabled; observing = $l.observing; latched = $l.latched; reason = $l.reason
        source_reference_ready = $ready; sample_fresh = $fresh; millivolts = $(if ($fresh) { $mv } else { $null })
        recovered_ready = ($ready -and $fresh -and $mv -ge $p.recover_mv); low_mv = $p.low_mv; recover_mv = $p.recover_mv
        settings_id = $p.settings_id; settings_match_acceptance = ($l.accepted_settings -eq $p.settings_id)
        observed_updates = $l.observed; minimum_observed_mv = $(if ($l.observed) { $mv - 210 } else { $null }); maximum_observed_mv = $(if ($l.observed) { $mv + 15 } else { $null }) }
}
function Guard-Json {
    $g = $S.guard; $on = Controller-Online
    return [ordered]@{ enabled = $g.enabled; observing = $g.observing; latched = $g.latched; reason = $g.reason
        mount_calibrated = $S.pose.calibrated; sensor_fresh = $on; upright = ($on -and -not (Is-Moving))
        observed_side_or_inverted_during_motion = $false; stop_requests = $g.stop_requests; calibration_matches_acceptance = $g.accepted }
}
function Camera-Json {
    $c = $S.camera; $connected = @('ready','capturing','photo_ready') -contains $c.phase
    $j = [ordered]@{ phase = $c.phase; host_started = $c.host_started; busy = (@('starting','enumerating','capturing') -contains $c.phase)
        connected = $connected; streaming = $false; cloud_upload = $false; vid = 0x0c45; pid = 0x6366; last_error = 0; error_name = 'ESP_OK'
        photo_bytes = $(if ($c.photo -and $c.phase -eq 'photo_ready') { $c.photo.Length } else { 0 }); photo_sequence = $c.photo_seq
        photo_width = 640; photo_height = 480; capture_format = 'MJPEG'; photo_content_type = 'image/jpeg'
        formats = $(if ($connected) { @([ordered]@{ format = 'MJPEG'; width = 640; height = 480; default_interval = 333333 }) } else { @() }) }
    return $j
}
function Rtc-Json {
    $valid = $null -ne $S.rtc.offset_ms
    $utc = if ($valid) { [DateTimeOffset]::FromUnixTimeMilliseconds((Now-Ms) + $S.rtc.offset_ms).UtcDateTime.ToString('yyyy-MM-ddTHH:mm:ssZ') } else { '' }
    return [ordered]@{ available = (Controller-Online); valid = $valid; utc = $utc; sync_pending = $false; sync_status = $S.rtc.sync_status }
}
function Board-Json {
    $b = $S.board
    $eeprom = if ($b.eeprom_checked) { [ordered]@{ pending = $false; available = $true; all_ff = $false; non_ff_bytes = 37 } } else { [ordered]@{ pending = $false; available = $false } }
    return [ordered]@{ esp_button_presses = $b.presses; esp_button_pressed = $false; stop_requests = $b.stops; esp_led_on = $true
        esp_sd = [ordered]@{ checked = $b.sd_checked; responded = $false; result = $(if ($b.sd_checked) { 'no_card_response_storage_skipped' } else { 'not_checked' }); mounted = $false; files_written = $false }
        button_action = 'pause_listening_and_stop'; physical_button_verified = $false; physical_stop_verified = $false
        stm32 = [ordered]@{ available = (Controller-Online); stm32_sd_detect_present = $false; probed = $b.probed; eeprom_acknowledged = $true; rtc_acknowledged = $true }
        rtc = (Rtc-Json); eeprom = $eeprom }
}
function Ota-Json {
    $blocked = ''
    if ($S.dialog_enabled -or (Now-Ms) -lt $S.dialog_active_until) { $blocked = 'ota_pause_listening' }
    elseif (Is-Moving) { $blocked = 'ota_motion_busy' }
    elseif ($S.timer.state -eq 'running') { $blocked = 'ota_timer_active' }
    elseif ($S.ota.reboot_at -gt 0) { $blocked = 'ota_in_progress' }
    return [ordered]@{ supported = $true; version = $S.esp_version; running_partition = 'ota_0'; next_partition = 'ota_1'
        max_image_bytes = 1966080; rollback_enabled = $true; minimum_version = '0.17.0'; preserves_settings = $true
        image_state = 'valid'; busy = ($S.ota.reboot_at -gt 0); phase = $(if ($S.ota.reboot_at -gt 0) { 'rebooting' } else { 'idle' }); error = ''
        target_version = $S.ota.target; boot_check = 'confirmed'; ready = ($blocked -eq ''); blocked_reason = $blocked
        last_invalid_partition = ''; last_invalid_version = '' }
}
function Capabilities-Json {
    $motion = [ordered]@{}
    foreach ($n in $Catalog.Keys) {
        $motion["motion.$n"] = [ordered]@{ min_repeats = 1; max_repeats = 3; min_strength = 25; max_strength = 100; default_strength = 25
            speeds = 'slow,normal,fast'; default_duration_ms = (Action-Duration $n 1 2); max_duration_ms = (Action-Duration $n 3 1)
            auto_return_to_baseline = $true; physical_motion_verified = $false }
    }
    $motion['stop_behavior'] = 'hold_current_pwm'; $motion['completion_evidence'] = 'pwm_sequence'; $motion['physical_position_verified'] = $false
    return [ordered]@{ mock = $true; modules = [ordered]@{ motion = $true; telemetry = $true; audio = $true; dialog = $true; config = $true; diagnostics_export = $false; ota = $true; ble = $true; discovery = $false }
        motion = $motion; dedup_history_records = 16; dialog = (Dialog-Json); timer = (Timer-Json) }
}
function Mock-Photo {
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap 640, 480
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::FromArgb(70, 110, 95))
    $font = New-Object System.Drawing.Font 'Microsoft YaHei', 30
    $g.DrawString('模拟照片 #' + $S.camera.photo_seq, $font, [System.Drawing.Brushes]::White, 40, 180)
    $g.DrawString((Get-Date).ToString('HH:mm:ss'), $font, [System.Drawing.Brushes]::White, 40, 240)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Jpeg); $bmp.Dispose()
    return $ms.ToArray()
}

# ---------- API 路由 ----------
function Handle-Api($ctx, [string]$path, [string]$method, $body, [byte[]]$raw) {
    $now = Now-Ms
    switch -Exact ($path) {
        '/api/v1/device' {
            return Send-Json $ctx 200 ([ordered]@{ mock = $true; device_id = 'MOCK-DOG'; api_version = '1'; esp32_firmware = $S.esp_version
                project = 'dog_device_service'; hostname = 'localhost'; control_url = "http://localhost:$Port/"; controller_version = $S.ctrl_version; uptime_ms = $now - $S.started })
        }
        '/api/v1/capabilities' { return Send-Json $ctx 200 (Capabilities-Json) }
        '/api/v1/state' { return Send-Json $ctx 200 (State-Json) }
        '/api/v1/dialog/state' { return Send-Json $ctx 200 (Dialog-Json) }
        '/api/v1/mode' {
            if ($method -eq 'POST') {
                if (-not (Has-Only $body @('mode')) -or $body.mode -isnot [string]) { return Send-Error $ctx 422 'invalid_mode' }
                if (@('companion','action','rest') -notcontains $body.mode) { return Send-Error $ctx 422 'mode_not_saved' }
                $S.mode = $body.mode
                if ($S.mode -eq 'rest' -and $S.routine) { Stop-All }
            }
            return Send-Json $ctx 200 (Interaction-Json)
        }
        '/api/v1/settings' {
            if ($method -eq 'POST') {
                if (-not (Has-Only $body @('volume','default_speed','default_strength','chat_actions'))) { return Send-Error $ctx 422 'settings_not_saved' }
                if (Has-Prop $body 'volume') { $v = Get-Int $body.volume 0 100; if ($null -eq $v) { return Send-Error $ctx 422 'settings_not_saved' }; $S.settings.volume = $v }
                if (Has-Prop $body 'default_speed') { if (@('slow','normal','fast') -notcontains $body.default_speed) { return Send-Error $ctx 422 'settings_not_saved' }; $S.settings.default_speed = $body.default_speed }
                if (Has-Prop $body 'default_strength') { $v = Get-Int $body.default_strength 25 100; if ($null -eq $v) { return Send-Error $ctx 422 'settings_not_saved' }; $S.settings.default_strength = $v }
                if (Has-Prop $body 'chat_actions') { if ($body.chat_actions -isnot [bool]) { return Send-Error $ctx 422 'settings_not_saved' }; $S.settings.chat_actions = $body.chat_actions }
            }
            return Send-Json $ctx 200 (Settings-Json)
        }
        '/api/v1/profile' {
            if ($method -eq 'POST') {
                if (-not (Has-Only $body @('nickname','reply_style','clear'))) { return Send-Error $ctx 422 'profile_not_saved' }
                if ($body.clear -eq $true) { $S.profile.nickname = ''; $S.profile.reply_style = 'natural' }
                else {
                    if (Has-Prop $body 'nickname') { if ($body.nickname -isnot [string] -or $body.nickname.Length -gt 24) { return Send-Error $ctx 422 'profile_not_saved' }; $S.profile.nickname = $body.nickname }
                    if (Has-Prop $body 'reply_style') { if (@('natural','brief') -notcontains $body.reply_style) { return Send-Error $ctx 422 'profile_not_saved' }; $S.profile.reply_style = $body.reply_style }
                }
                $S.profile.revision++
            }
            return Send-Json $ctx 200 (Profile-Json)
        }
        '/api/v1/memory' {
            if ($method -eq 'POST') {
                $cmd = $body.command; $text = "$($body.text)".Trim()
                switch ($cmd) {
                    'clear' { $S.memory.Clear() }
                    'remember' {
                        if (-not $text -or $text.Length -gt 50) { return Send-Error $ctx 422 'memory_not_saved' }
                        if ($S.memory.Count -ge 8) { return Send-Error $ctx 409 'memory_not_saved' }
                        [void]$S.memory.Add($text)
                    }
                    'forget' { $i = $S.memory.IndexOf($text); if ($i -lt 0) { return Send-Error $ctx 404 'memory_not_saved' }; $S.memory.RemoveAt($i) }
                    'replace' {
                        $rep = "$($body.replacement)".Trim(); $i = $S.memory.IndexOf($text)
                        if (-not $rep -or $rep.Length -gt 50) { return Send-Error $ctx 422 'memory_not_saved' }
                        if ($i -lt 0) { return Send-Error $ctx 404 'memory_not_saved' }
                        $S.memory[$i] = $rep
                    }
                    default { return Send-Error $ctx 422 'memory_not_saved' }
                }
                $S.memory_rev++
            }
            return Send-Json $ctx 200 (Memory-Json)
        }
        '/api/v1/timer' {
            if ($method -eq 'POST') {
                if (-not (Has-Only $body @('seconds','command'))) { return Send-Error $ctx 422 'invalid_timer' }
                if ($body.command -eq 'cancel' -and -not (Has-Prop $body 'seconds')) { $S.timer.state = 'idle'; $S.timer.generation++ }
                else {
                    $sec = Get-Int $body.seconds 1 86400
                    if ((Has-Prop $body 'command') -or $null -eq $sec) { return Send-Error $ctx 422 'invalid_timer' }
                    $S.timer.state = 'running'; $S.timer.duration = $sec; $S.timer.ends = $now + $sec * 1000; $S.timer.generation++
                }
            }
            return Send-Json $ctx 200 (Timer-Json)
        }
        '/api/v1/game' {
            if ($method -eq 'POST') {
                $g = $S.game; $cmd = $body.command
                if (-not (Has-Only $body @('command','text'))) { return Send-Error $ctx 422 'invalid_game_command' }
                switch ($cmd) {
                    'start' { $g.active = $true; $g.round = 1; $g.correct = 0; $g.attempts = 0; $g.idx = 0; $g.hinted = $g.revealed = $g.solved = $false; $g.last_reply = '好呀，第一题来了。' }
                    'next' { if (-not $g.active) { return Send-Error $ctx 422 'invalid_game_command' }; $g.round++; $g.idx = ($g.idx + 1) % $Riddles.Count; $g.attempts = 0; $g.hinted = $g.revealed = $g.solved = $false; $g.last_reply = '下一题。' }
                    'hint' { if (-not $g.active) { return Send-Error $ctx 422 'invalid_game_command' }; $g.hinted = $true; $g.last_reply = '提示：' + $Riddles[$g.idx].h }
                    'answer' { if (-not $g.active) { return Send-Error $ctx 422 'invalid_game_command' }; $g.revealed = $true; $g.last_reply = '答案是：' + $Riddles[$g.idx].a }
                    'end' { $g.active = $false; $g.idx = -1; $g.last_reply = '猜谜结束啦。' }
                    'guess' {
                        if (-not $g.active -or -not $body.text) { return Send-Error $ctx 422 'invalid_game_command' }
                        $g.attempts++
                        if ("$($body.text)".Contains($Riddles[$g.idx].a)) { $g.solved = $true; $g.correct++; $g.last_reply = '答对了！' } else { $g.last_reply = '不对哦，再想想。' }
                    }
                    default { return Send-Error $ctx 422 'invalid_game_command' }
                }
            }
            return Send-Json $ctx 200 (Game-Json)
        }
        '/api/v1/dialog/control' {
            if ($method -ne 'POST') { break }
            if (-not (Has-Only $body @('command','text','execute_motion')) -or $body.command -isnot [string]) { return Send-Error $ctx 422 'invalid_dialog_command' }
            switch ($body.command) {
                'start' { if ($S.routine) { return Send-Error $ctx 409 'dialog_command_rejected' }; $S.dialog_enabled = $true }
                'stop' { Stop-All }
                'routine' {
                    if (@('greet','show') -notcontains $body.text -or $S.mode -eq 'rest' -or $S.dialog_enabled -or -not (Controller-Online) -or (Is-Moving)) { return Send-Error $ctx 409 'dialog_command_rejected' }
                    $S.routine = @{ ends = $now + $(if ($body.text -eq 'greet') { 4200 } else { 6500 }) }; $S.last_motion_state = 'routine_running'
                }
                default { return Send-Error $ctx 409 'dialog_command_rejected' }
            }
            return Send-Json $ctx 202 (Dialog-Json)
        }
        '/api/v1/actions' {
            if ($method -ne 'POST') { break }
            if ($S.mode -eq 'rest') { return Send-Error $ctx 409 'rest_mode_motion_blocked' }
            if (-not (Has-Only $body @('request_id','action','args','ttl_ms','boot_id'))) { return Send-Error $ctx 400 'invalid_json_fields' }
            if ($body.request_id -isnot [string] -or $body.request_id -notmatch '^[A-Za-z0-9._-]{1,48}$' -or $body.action -isnot [string]) { return Send-Error $ctx 400 'request_id_and_action_required' }
            if ((Has-Prop $body 'boot_id') -and $body.boot_id -ne $S.boot_id) { return Send-Error $ctx 409 'device_boot_changed' }
            if ($S.requests.Contains($body.request_id)) { return Send-Json $ctx 202 (Request-Json $S.requests[$body.request_id]) }
            if ((Has-Prop $body 'ttl_ms') -and $null -eq (Get-Int $body.ttl_ms 1 5000)) { return Send-Error $ctx 422 'invalid_ttl_ms' }
            $name = $body.action -replace '^motion\.', ''
            if ($body.action -notlike 'motion.*' -or -not $Catalog.Contains($name)) { return Send-Error $ctx 501 'action_not_supported' }
            $a = $body.args; $rep = 1; $spd = 'normal'; $str = 25
            if ($null -ne $a) {
                if (-not (Has-Only $a @('repeats','speed','strength'))) { return Send-Error $ctx 422 'invalid_action_arguments' }
                if (Has-Prop $a 'repeats') { $rep = Get-Int $a.repeats 1 3 }
                if (Has-Prop $a 'speed') { $spd = $a.speed }
                if (Has-Prop $a 'strength') { $str = Get-Int $a.strength 25 100 }
                if ($null -eq $rep -or $null -eq $str -or @('slow','normal','fast') -notcontains $spd) { return Send-Error $ctx 422 'invalid_action_arguments' }
            }
            if (-not (Controller-Online)) { return Send-Error $ctx 503 'controller_offline' }
            if ((Is-Moving) -or $now -lt $S.stop_pending_until) { return Send-Error $ctx 409 'controller_busy' }
            $speedNum = @{ slow = 1; normal = 2; fast = 3 }[$spd]
            $r = @{ id = $body.request_id; action = $body.action; repeats = $rep; speed_name = $spd; strength = $str; start = $now; duration = (Action-Duration $name $rep $speedNum); final = $null; error = '' }
            $S.requests[$r.id] = $r; $S.active_id = $r.id; $S.last_motion_id = $r.id
            while ($S.requests.Count -gt 16) { $S.requests.RemoveAt(0) }
            return Send-Json $ctx 202 (Request-Json $r)
        }
        '/api/v1/motion/stop' {
            if ($method -ne 'POST') { break }
            if ($raw.Length -ne 0) { return Send-Error $ctx 400 'stop_requires_empty_body' }
            Stop-All   # 0.37.1 起与 dialog/control stop 相同：停止动作、连续动作与收听
            return Send-Json $ctx 202 ([ordered]@{ state = 'stop_requested'; controller_acknowledged = $false })
        }
        '/api/v1/power' {
            if ($method -eq 'POST') {
                $p = $S.power
                switch ($body.command) {
                    'source' { if (@('unknown','usb','battery_2s') -notcontains $body.source) { return Send-Error $ctx 422 'invalid_power_settings' }; $p.source = $body.source; $p.calibrated = $false; $p.alerts = $false; $p.settings_id++ }
                    'reference' {
                        $mv = Get-Int $body.millivolts 6000 9000
                        if ($p.source -ne 'battery_2s' -or $body.reference_verified -ne $true -or $null -eq $mv) { return Send-Error $ctx 422 'invalid_power_settings' }
                        $p.calibrated = $true; $p.reference_mv = $mv; $p.settings_id++
                    }
                    'thresholds' {
                        $lo = Get-Int $body.low_mv 6400 7400; $hi = Get-Int $body.recover_mv 6550 7800
                        if ($null -eq $lo -or $null -eq $hi -or $hi -lt $lo + 150) { return Send-Error $ctx 422 'invalid_power_settings' }
                        $p.low_mv = $lo; $p.recover_mv = $hi; $p.settings_id++
                    }
                    'alerts' { if ($body.enabled -eq $true -and -not ($p.calibrated -and $p.source -eq 'battery_2s')) { return Send-Error $ctx 409 'power_alerts_require_reference' }; $p.alerts = [bool]$body.enabled }
                    'clear' { $p.source = 'unknown'; $p.calibrated = $false; $p.alerts = $false; $p.low_mv = 7000; $p.recover_mv = 7200; $p.settings_id++ }
                    default { return Send-Error $ctx 422 'invalid_power_settings' }
                }
            }
            return Send-Json $ctx 200 (Power-Json)
        }
        '/api/v1/power/history' {
            $samples = New-Object System.Collections.ArrayList
            $up = $now - $S.started
            if ($Scenario.supply -ne 'unavailable') {
                for ($age = [math]::Min(600000, [int]($up / 10000) * 10000); $age -ge 0; $age -= 10000) {
                    if ($age -ge 230000 -and $age -le 270000) { continue }   # 模拟一段缺采样
                    $mv = 7640 + [int]($age / 60000) * 3 + (Get-Random -Maximum 20)
                    [void]$samples.Add(@($age, $mv))
                }
            }
            $trend = if ($samples.Count -eq 0) { 'unavailable' } elseif ($S.power.source -eq 'unknown') { 'source_required' } elseif (-not $S.power.calibrated) { 'reference_required' } elseif ($up -lt 120000) { 'collecting' } else { 'falling' }
            return Send-Json $ctx 200 ([ordered]@{ calibrated = $S.power.calibrated; trend = $trend; interval_ms = 10000; window_ms = 600000; samples = @($samples.ToArray()) })
        }
        '/api/v1/power/limit' {
            if ($method -eq 'POST') {
                if (-not (Rest-Idle)) { return Send-Error $ctx 409 'power_limit_requires_rest' }
                $l = $S.limit; $pl = PowerLimit-Json
                switch ($body.command) {
                    'observe' { if (-not $pl.source_reference_ready) { return Send-Error $ctx 409 'power_limit_reference_required' }; $l.observing = [bool]$body.enabled; if ($l.observing) { $l.observed = 0 } else { $l.observed = 0 } }
                    'enable' {
                        if ($body.acceptance_confirmed -ne $true) { return Send-Error $ctx 422 'invalid_power_limit_settings' }
                        if (-not $pl.source_reference_ready) { return Send-Error $ctx 409 'power_limit_reference_required' }
                        if ($body.expected_settings -ne $S.power.settings_id) { return Send-Error $ctx 409 'power_limit_settings_changed' }
                        if (-not $pl.recovered_ready) { return Send-Error $ctx 409 'power_limit_recovery_required' }
                        $l.enabled = $true; $l.observing = $false; $l.latched = $false; $l.accepted_settings = $S.power.settings_id
                    }
                    'disable' { $l.enabled = $false; $l.latched = $false }
                    'clear' {
                        if ($body.supply_confirmed -ne $true) { return Send-Error $ctx 422 'invalid_power_limit_settings' }
                        if (-not $pl.recovered_ready) { return Send-Error $ctx 409 'power_limit_recovery_required' }
                        $l.latched = $false; $l.reason = ''
                    }
                    default { return Send-Error $ctx 422 'invalid_power_limit_settings' }
                }
            }
            return Send-Json $ctx 200 (PowerLimit-Json)
        }
        '/api/v1/pose/protection' {
            if ($method -eq 'POST') {
                if (-not (Rest-Idle)) { return Send-Error $ctx 409 'pose_protection_requires_rest' }
                $g = $S.guard
                switch ($body.command) {
                    'observe' { $g.observing = [bool]$body.enabled }
                    'enable' {
                        if ($body.acceptance_confirmed -ne $true) { return Send-Error $ctx 422 'invalid_pose_protection_settings' }
                        if (-not $S.pose.calibrated) { return Send-Error $ctx 409 'pose_protection_reconfirm_required' }
                        $g.enabled = $true; $g.observing = $false; $g.accepted = $true; $g.latched = $false
                    }
                    'disable' { $g.enabled = $false; $g.latched = $false }
                    'clear' { if ($body.upright_confirmed -ne $true) { return Send-Error $ctx 422 'invalid_pose_protection_settings' }; $g.latched = $false; $g.reason = '' }
                    default { return Send-Error $ctx 422 'invalid_pose_protection_settings' }
                }
            }
            return Send-Json $ctx 200 (Guard-Json)
        }
        '/api/v1/pose/calibration' {
            if ($method -eq 'POST') {
                $cmd = $body.command; $p = $S.pose
                if ($cmd -ne 'cancel' -and -not (Rest-Idle)) { return Send-Error $ctx 409 'pose_calibration_requires_rest' }
                switch ($cmd) {
                    'cancel' { if ($p.active) { $p.active = $false; $p.collecting_until = 0; $p.error = 'cancelled' } }
                    'level' { $p.active = $true; $p.stage = 0; $p.error = ''; $p.collecting_until = $now + 2000 }
                    'forward' { if (-not $p.active -or $p.stage -ne 1 -or $p.collecting_until) { return Send-Error $ctx 409 'calibration_step_out_of_order' }; $p.collecting_until = $now + 2000 }
                    'left' { if (-not $p.active -or $p.stage -ne 2 -or $p.collecting_until) { return Send-Error $ctx 409 'calibration_step_out_of_order' }; $p.collecting_until = $now + 2000 }
                    default { return Send-Error $ctx 422 'invalid_calibration_command' }
                }
                return Send-Json $ctx 202 ((Sensors-Json).pose)
            }
            return Send-Json $ctx 200 ((Sensors-Json).pose)
        }
        '/api/v1/compass' {
            if ($method -eq 'POST') {
                $cmd = $body.command; $c = $S.compass
                if (@('cancel','clear') -notcontains $cmd -and -not (Rest-Idle)) { return Send-Error $ctx 409 'compass_requires_rest' }
                switch ($cmd) {
                    'start' { $c.active = $true; $c.start = $now; $c.error = '' }
                    'finish' {
                        $n = (Compass-Json).samples
                        if (-not $c.active) { return Send-Error $ctx 409 'compass_not_collecting' }
                        $c.active = $false
                        if ($n -lt 96) { $c.error = 'need_more_rotations' } else { $c.calibrated = $true; $c.error = '' }
                    }
                    'cancel' { if ($c.active) { $c.active = $false; $c.error = 'cancelled' } }
                    'clear' { $c.active = $false; $c.calibrated = $false; $c.error = '' }
                    default { return Send-Error $ctx 422 'invalid_compass_command' }
                }
            }
            return Send-Json $ctx 200 (Compass-Json)
        }
        '/api/v1/board' {
            if ($method -eq 'POST') {
                $cmd = $body.command; $b = $S.board
                if (-not (Rest-Idle)) { if ($cmd -eq 'inspect_eeprom') { return Send-Error $ctx 409 'eeprom_requires_rest' }; return Send-Error $ctx 409 'board_aux_requires_rest' }
                if ((Is-Moving)) { return Send-Error $ctx 409 'eeprom_requires_idle' }
                switch ($cmd) {
                    'inspect_eeprom' { $b.eeprom_checked = $true }
                    'probe_storage' { $b.sd_checked = $true }
                    'probe' { $b.probed = $true }
                    'beep' { if ($null -eq (Get-Int $body.value 0 300)) { return Send-Error $ctx 422 'invalid_board_command' } }
                    'led' { if ($null -eq (Get-Int $body.value 0 2)) { return Send-Error $ctx 422 'invalid_board_command' } }
                    default { return Send-Error $ctx 422 'invalid_board_command' }
                }
                return Send-Json $ctx 202 (Board-Json)
            }
            return Send-Json $ctx 200 (Board-Json)
        }
        '/api/v1/rtc' {
            if ($method -eq 'POST') {
                if (-not (Has-Only $body @('command','utc')) -or $body.utc -isnot [string]) { return Send-Error $ctx 422 'invalid_rtc_command' }
                if (-not (Rest-Idle)) { return Send-Error $ctx 409 'rtc_requires_rest' }
                $t = [DateTimeOffset]::MinValue
                if (-not [DateTimeOffset]::TryParse($body.utc, [ref]$t)) { return Send-Error $ctx 422 'invalid_rtc_utc' }
                if ($body.command -eq 'sync') { $S.rtc.offset_ms = $t.ToUnixTimeMilliseconds() - $now; $S.rtc.sync_status = 'confirmed'; return Send-Json $ctx 202 (Rtc-Json) }
                if ($body.command -eq 'remind') {
                    if ($null -eq $S.rtc.offset_ms) { return Send-Error $ctx 409 'rtc_reminder_requires_valid_time' }
                    if ($S.timer.state -eq 'running') { return Send-Error $ctx 409 'timer_already_active' }
                    $sec = [int](($t.ToUnixTimeMilliseconds() - ($now + $S.rtc.offset_ms)) / 1000)
                    if ($sec -lt 5 -or $sec -gt 86400) { return Send-Error $ctx 422 'rtc_reminder_range' }
                    $S.timer.state = 'running'; $S.timer.duration = $sec; $S.timer.ends = $now + $sec * 1000; $S.timer.generation++
                    return Send-Json $ctx 200 (Timer-Json)
                }
                return Send-Error $ctx 422 'invalid_rtc_command'
            }
            return Send-Json $ctx 200 (Rtc-Json)
        }
        '/api/v1/display/state' {
            $S.display_refreshes += 3
            return Send-Json $ctx 200 ([ordered]@{ controller = 'ST7789'; initialized = $true; width = 240; height = 240; spi_hz = 40000000
                refreshes = $S.display_refreshes; last_error = 0; error_name = 'ESP_OK'; expression = 'calm'; emotion = 'auto'; touch_initialized = $true })
        }
        '/api/v1/camera/state' { return Send-Json $ctx 200 (Camera-Json) }
        '/api/v1/camera/control' {
            if ($method -ne 'POST') { break }
            $c = $S.camera
            if ($body.command -eq 'start' -and -not $c.host_started) { $c.host_started = $true; $c.phase = 'enumerating'; $c.next_phase = 'ready'; $c.phase_until = $now + 1200 }
            elseif ($body.command -eq 'capture' -and @('ready','photo_ready') -contains $c.phase) { $c.photo_seq++; $c.photo = Mock-Photo; $c.phase = 'capturing'; $c.next_phase = 'photo_ready'; $c.phase_until = $now + 1000 }
            else { return Send-Error $ctx 409 'camera_command_rejected' }
            return Send-Json $ctx 202 (Camera-Json)
        }
        '/api/v1/camera/photo' {
            if (-not $S.camera.photo -or $S.camera.phase -ne 'photo_ready') { return Send-Error $ctx 404 'photo_unavailable' }
            return Send-Bytes $ctx 200 $S.camera.photo 'image/jpeg'
        }
        '/api/v1/ble/state' {
            return Send-Json $ctx 200 ([ordered]@{ supported = $true; name = 'Pavlov-DOG001'; initialized = $true; connected = $false; authenticated = $false
                pairing_window = ($now -lt $S.ble.pairing_until); confirmation_pending = $false; passkey = 0; last_error = 0; busy = $false })
        }
        '/api/v1/ble/control' {
            if ($method -ne 'POST') { break }
            if ($body.command -ne 'pair') { return Send-Error $ctx 409 'ble_pairing_unavailable' }
            $S.ble.pairing_until = $now + 60000
            return Send-Json $ctx 202 ([ordered]@{ supported = $true; name = 'Pavlov-DOG001'; initialized = $true; connected = $false; authenticated = $false; pairing_window = $true; confirmation_pending = $false })
        }
        '/api/v1/ota/state' { return Send-Json $ctx 200 (Ota-Json) }
        '/api/v1/ota/upload' {
            if ($method -ne 'POST') { break }
            $o = Ota-Json
            if (-not $o.ready) { return Send-Error $ctx 409 $o.blocked_reason }
            $hash = ([System.Security.Cryptography.SHA256]::Create().ComputeHash($raw) | ForEach-Object { $_.ToString('x2') }) -join ''
            if ($hash -ne $ctx.Request.Headers['X-Firmware-SHA256']) { return Send-Error $ctx 422 'ota_sha256_mismatch' }
            $ver = ''
            if ($raw.Length -ge 80) { $ver = $Utf8.GetString($raw, 48, 32).Split([char]0)[0] }
            if (-not $ver) { return Send-Error $ctx 422 'ota_incompatible_image' }
            $S.ota.target = $ver; $S.ota.reboot_at = $now + 2500; $S.ota.up_at = $now + 6000
            return Send-Json $ctx 202 ([ordered]@{ accepted = $true; target_version = $ver; rebooting = $true })
        }
    }
    if ($path -like '/api/v1/requests/*' -and $method -eq 'GET') {
        $id = $path.Substring(17)
        if ($id -notmatch '^[A-Za-z0-9._-]{1,48}$') { return Send-Error $ctx 400 'invalid_request_id' }
        if (-not $S.requests.Contains($id)) { return Send-Error $ctx 404 'request_not_found' }
        return Send-Json $ctx 200 (Request-Json $S.requests[$id])
    }
    return Send-Error $ctx 501 'endpoint_not_implemented'
}

# ---------- 模拟场景面板 ----------
$ScenarioOptions = [ordered]@{
    api        = @('ok','down')
    latency_ms = @('0','800','3000','7000')
    controller = @('online','offline')
    action     = @('finish','fail','hang')
    supply     = @('ok','low','unavailable')
    voice      = @('ok','error')
    wifi_rssi  = @('-50','-68','-82')
    stage      = @('auto','recognizing','thinking','playing')
    posture    = @('upright','side','inverted')
}
$ScenarioLabels = @{ api = 'API 连接（down=全部返回503）'; latency_ms = '每个请求额外延迟（7000会触发页面6秒超时）'; controller = '下层控制器'
    action = '动作回执结果（hang=一直进行中）'; supply = '供电读数'; voice = '语音（error=上传失败重试中）'; wifi_rssi = 'Wi-Fi 信号 dBm'
    stage = '语音阶段（需先开启收听）'; posture = '姿态分类（side/inverted 时狗脸疑惑）' }
function Mock-Panel($ctx, [string]$query) {
    if ($query) {
        foreach ($pair in $query.TrimStart('?').Split('&')) {
            $kv = $pair.Split('=', 2)
            if ($kv.Count -ne 2) { continue }
            $k = [Uri]::UnescapeDataString($kv[0]); $v = [Uri]::UnescapeDataString($kv[1].Replace('+', ' '))
            if ($k -eq 'say') { $S.last_heard = $v; $S.last_reply = '（模拟回复）我听到了：' + $v; $S.turns++ }
            elseif ($k -eq 'sequence') {
                $now = Now-Ms
                switch ($v) {
                    { $_ -in 'local','model','dry_run' } {
                        $demo = $SeqDemos[$(if ($v -eq 'dry_run') { 'local' } else { $v })]
                        if ($S.mode -eq 'rest') { $S.seq = @{ steps = $demo.steps; source = $demo.source; start = $now; per = 2200; completed = 0; result = 'rest_blocked'; dry = $false } }
                        else { $S.seq = @{ steps = $demo.steps; source = $demo.source; start = $now; per = 2200; completed = 0; result = 'running'; dry = ($v -eq 'dry_run') }; $S.last_heard = $(if ($v -eq 'model') { '蹲下慢一点，然后站起来，再快速左右摇一摇，最后摇尾巴' } else { '先站起来，然后摇两下尾巴，最后慢慢握手' }); $S.last_reply = '（模拟回复）好的，按顺序来。'; $S.turns++ }
                    }
                    'invalid' { $S.seq = @{ steps = @(); source = 'local'; start = $now; per = 2200; completed = 0; result = 'invalid'; dry = $false }; $S.last_heard = '先飞起来，然后翻跟头'; $S.last_reply = '这次不执行这些动作。'; $S.turns++ }
                    'abort'   { if (Seq-Running) { $S.seq.result = 'aborted' } }
                    'clear'   { $S.seq = $null }
                }
            }
            elseif ($k -eq 'emotion' -and @('auto','calm','happy','curious','puzzled','low') -contains $v) { $S.reply_emotion = $v; $S.last_reply = "（模拟回复，情绪 $v）"; $S.turns++ }
            elseif ($k -eq 'reset') { $S.boot_id = New-BootId; $S.started = Now-Ms; $S.requests = [ordered]@{}; $S.active_id = $null; $S.dialog_enabled = $false }
            elseif ($Scenario.Contains($k) -and $ScenarioOptions[$k] -contains $v) {
                if ($k -eq 'latency_ms' -or $k -eq 'wifi_rssi') { $Scenario[$k] = [int]$v } else { $Scenario[$k] = $v }
            }
        }
        $ctx.Response.Redirect('/__mock'); $ctx.Response.Close(); return
    }
    $rows = foreach ($k in $ScenarioOptions.Keys) {
        $links = foreach ($o in $ScenarioOptions[$k]) {
            if ("$($Scenario[$k])" -eq $o) { "<b>[$o]</b>" } else { "<a href='/__mock?$k=$o'>$o</a>" }
        }
        "<tr><td>$($ScenarioLabels[$k])</td><td>$($links -join ' &nbsp; ')</td></tr>"
    }
    $html = @"
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>模拟场景</title>
<style>body{font:15px/1.6 system-ui,'Microsoft YaHei';margin:0;padding:16px;background:#fff8ef;color:#3a2600}table{border-collapse:collapse;width:100%;max-width:760px}td{border-bottom:1px solid #e8d6bd;padding:10px 6px;vertical-align:top}a{color:#8a4b00}h1{font-size:20px}.warn{background:#7a3e00;color:#fff;padding:10px 14px;border-radius:10px;max-width:760px}input{font:inherit;padding:6px;width:60%}</style></head><body>
<p class="warn">本地模拟后端 · 全部为假数据 · 不连接也不操作实物机器狗</p>
<h1>模拟场景开关</h1><p>当前 boot_id：$($S.boot_id) · 模式：$($S.mode) · 收听：$($S.dialog_enabled)</p>
<table>$($rows -join '')</table>
<form action="/__mock" method="get" style="margin-top:16px"><label>模拟“最近一句”：<input name="say" placeholder="例如：今天天气怎么样"></label> <button>发送</button></form>
<p>连续动作（每步约 2.2 秒；休息模式下会变成“休息拦截”）：<a href="/__mock?sequence=local">本地解析 3 步</a> · <a href="/__mock?sequence=model">模型规划 4 步</a> · <a href="/__mock?sequence=dry_run">诊断演练</a> · <a href="/__mock?sequence=invalid">校验未通过</a> · <a href="/__mock?sequence=abort">让当前步失败</a> · <a href="/__mock?sequence=clear">清空</a>（停止按钮会变成“已取消”）</p>
<p>模拟一轮回复并带情绪（狗脸保持约 6 秒）：<a href="/__mock?emotion=happy">happy</a> · <a href="/__mock?emotion=curious">curious</a> · <a href="/__mock?emotion=puzzled">puzzled</a> · <a href="/__mock?emotion=low">low</a> · <a href="/__mock?emotion=calm">calm</a></p>
<p><a href="/__mock?reset=1">模拟设备重启（更换 boot_id）</a> · <a href="/">返回控制页</a></p></body></html>
"@
    Send-Bytes $ctx 200 ($Utf8.GetBytes($html)) 'text/html; charset=utf-8'
}

# ---------- 页面 ----------
$Banner = '<div style="background:#7a3e00;color:#fff;text-align:center;font:600 13px/1.5 system-ui,sans-serif;padding:6px 12px">本地模拟后端 · 数据均为模拟，不连接也不操作实物机器狗 · <a href="/__mock" target="_blank" rel="noopener" style="color:#ffe7b8">切换模拟场景</a></div>'
function Serve-Page($ctx) {
    $html = [IO.File]::ReadAllText($PagePath, $Utf8)
    $i = $html.IndexOf('<body>')
    if ($i -ge 0) { $html = $html.Insert($i + 6, $Banner) }
    $r = $ctx.Response
    $r.Headers['Content-Security-Policy'] = $Csp
    $r.Headers['Referrer-Policy'] = 'no-referrer'
    $r.Headers['X-Frame-Options'] = 'DENY'
    Send-Bytes $ctx 200 ($Utf8.GetBytes($html)) 'text/html; charset=utf-8'
}

# ---------- 主循环 ----------
function Handle($ctx) {
    $req = $ctx.Request; $path = $req.Url.AbsolutePath; $method = $req.HttpMethod
    if ($method -eq 'GET' -and ($path -eq '/' -or $path -eq '/control')) { return Serve-Page $ctx }
    if ($path -eq '/__mock') { return Mock-Panel $ctx $req.Url.Query }
    if ($path -notlike '/api/v1/*') { return Send-Bytes $ctx 404 ($Utf8.GetBytes('not found')) 'text/plain; charset=utf-8' }

    if ($Scenario.latency_ms -gt 0) { Start-Sleep -Milliseconds $Scenario.latency_ms }
    Update-World
    $now = Now-Ms
    if ($Scenario.api -eq 'down' -or ($S.ota.up_at -gt $now -and $S.ota.reboot_at -eq 0)) { return Send-Error $ctx 503 'mock_device_unreachable' }
    if ($req.Headers['Authorization'] -ne "Bearer $Token") { $ctx.Response.AddHeader('WWW-Authenticate', 'Bearer'); return Send-Error $ctx 401 'authentication_required' }
    if ($req.RawUrl.Contains('?')) { return Send-Error $ctx 400 'query_not_supported' }

    $raw = [byte[]]@()
    if ($req.HasEntityBody) { $ms = New-Object System.IO.MemoryStream; $req.InputStream.CopyTo($ms); $raw = $ms.ToArray() }
    $body = $null
    if ($method -eq 'POST' -and $raw.Length -gt 0 -and $path -ne '/api/v1/ota/upload') {
        try { $body = $Utf8.GetString($raw) | ConvertFrom-Json } catch { return Send-Error $ctx 400 'invalid_json_fields' }
    }
    if ($method -eq 'POST' -and $null -eq $body -and $path -notin @('/api/v1/motion/stop', '/api/v1/ota/upload')) { return Send-Error $ctx 422 'body_required' }
    Handle-Api $ctx $path $method $body $raw
}

$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://localhost:$Port/")
try { $listener.Start() } catch {
    $running = $false
    try { $null = Invoke-WebRequest "http://localhost:$Port/__mock" -UseBasicParsing -TimeoutSec 2; $running = $true } catch { }
    Write-Host ''
    if ($running) {
        Write-Host "  端口 $Port 上已经有一份模拟后端在运行，不需要再启动一次。" -ForegroundColor Yellow
        Write-Host "  直接打开：http://localhost:$Port/#key=$Token"
        Write-Host "  如果想重新启动，先关掉之前那个黑色窗口，再双击 start-mock.cmd。"
    } else {
        Write-Host "  端口 $Port 被其他程序占用了，换一个端口试试，例如：" -ForegroundColor Red
        Write-Host "  powershell -NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Port 8788"
        Write-Host "  （原始错误：$($_.Exception.Message)）"
    }
    Write-Host ''
    exit 1
}
Write-Host ''
Write-Host '  巴甫洛夫 本地模拟后端已启动（假数据，不连接实物）' -ForegroundColor Yellow
Write-Host "  控制页（自动填入模拟口令）：http://localhost:$Port/#key=$Token"
Write-Host "  模拟场景开关：            http://localhost:$Port/__mock"
Write-Host '  按 Ctrl+C 停止。'
Write-Host ''
try {
    while ($listener.IsListening) {
        $task = $listener.GetContextAsync()
        while (-not $task.AsyncWaitHandle.WaitOne(200)) { }
        $ctx = $task.GetAwaiter().GetResult()
        $t0 = Now-Ms
        try { Handle $ctx }
        catch {
            Write-Host "  [错误] $($ctx.Request.HttpMethod) $($ctx.Request.Url.AbsolutePath): $($_.Exception.Message) @ line $($_.InvocationInfo.ScriptLineNumber)" -ForegroundColor Red
            try { Send-Error $ctx 500 'mock_internal_error' } catch { }
        }
        $p = $ctx.Request.Url.AbsolutePath
        if ($ctx.Request.HttpMethod -eq 'POST' -or $ctx.Response.StatusCode -ge 400) {
            Write-Host ("  {0} {1,-5} {2} -> {3} ({4}ms)" -f (Get-Date -Format 'HH:mm:ss'), $ctx.Request.HttpMethod, $p, $ctx.Response.StatusCode, ((Now-Ms) - $t0))
        }
    }
} finally { $listener.Stop(); $listener.Close() }
