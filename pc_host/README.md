# Korvo 统一上位机（pc_host v2）

一个 PySide6 应用统筹设备端全部 PC 侧功能：**通话对讲 / 录音库 / 音乐副屏 / 系统**，
左侧边栏切页，暗色主题。原"低延迟传声机"工程即本包 v1，v2 在其基础上融合了
原 `tools/pc_now_agent`（C#）的音乐副屏能力（协议 ICD V1.3 完全一致，两者二选一运行）。

## 启动

**日常：双击桌面 `ESP32_Voice_Host.bat` 就够了。** 它是个 3 行包装器，只负责转发到
工程内的唯一入口：

```
pc_host\start_all.bat          <- 唯一真源，改脚本改这里
```

`start_all.bat` 一键做三件事：

| 步骤 | 动作 | 说明 |
|---|---|---|
| 1/3 | 网易云带 `--remote-debugging-port=9222` 起来 | **幂等**：先探 9222，已在就完全不动网易云（不会打断正在放的歌）；需要重启时会提示"当前播放会被打断" |
| 2/3 | 前置检查 | venv python、`pc_host\main.py`、工程根（`%~dp0..` 自推导，不依赖 cwd） |
| 3/3 | `python -m pc_host.main %*` | PySide6 上位机；多余参数原样转发 |

```bash
# 等效的手工方式
cd C:\Users\xiao1\Desktop\esp32\display_audio_photo
C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe -m pc_host.main            # 广播自动发现设备
... -m pc_host.main --device 192.168.4.1                                          # 指定设备 IP
... -m pc_host.main --no-pcnow                                                    # 不占 50004/50005
```

可选开关与环境：

- `start_all.bat` 里 `set "SKIP_NETEASE=1"` ⇒ 完全不碰网易云（例如今天用 Spotify 时）。
- `pc_host\restart_cloudmusic_debug.bat` 保留，仅用于"只想单独重启网易云、不起上位机"。
- 两步都要走 loopback 探测，而 **Clash/TUN 会设 `http_proxy` 劫持 127.0.0.1** ⇒ 两个脚本里的
  `curl` 都带 `--noproxy "*"`，删掉就会误报"CDP 不通"。
- `start_all.bat` 开头把 `System32` 提到 `PATH` 最前：万一从 Git Bash 启动，
  MSYS 的 `find.exe` / `timeout.exe` 会顶掉 Windows 版本，`/I`、`/t` 全部失效。

## 五个页面

| 页面 | 功能 | 协议 |
|---|---|---|
| 通话对讲 | 设备麦克风上行电平、WAV 推流到设备扬声器、sherpa-onnx 实时转写 | UDP 50000/50001（会话由设备端按键发起） |
| 录音库 | 设备录音自动上传后的本地库：列表/时长/删除/打开目录 | TCP 50002（分块+CRC16，断点续传） |
| 音乐副屏 | 采集 Windows 正在播放（SMTC）→ 网易云歌词/封面 → 推送设备 | UDP 50004 推流 + TCP 50005 HTTP 拉取 |
| 固件升级 | WiFi OTA：选 bin（解析 esp_app_desc_t + SHA256）→ OFFER → 进度条 → 自证版本核对；内置 --force / --ota-limit / --ota-corrupt 联调开关 | UDP 50003 OFFER + TCP 50005 /firmware.bin（Range 续传） |
| 壁纸管理 | 独立页：**大图预览**（与设备 800x480 同比例）→ 本地预校验 → 上传/取消，实时会话日志 | UDP 50003 WP_OFFER/WP_RESULT + TCP 50005 /wallpaper/<name> |
| 系统 | 固件版本/协议版本/设备 IP、本机环境自检、端口总表、OTA 入口 | UDP 50003 心跳 |

> v0.3（2026-10-07）起 OTA 面板收编进本应用（`pc_host/ota/`），**C# agent 不再是唯一带 OTA 的端**；
> 设备回包（OTA_ACCEPT/PROGRESS/RESULT）固定到 PC:50003（`link_manager_send_json` 端口覆写），
> 本应用由 link 分发器路由给 OTA 会话——比 C# 版临时 UDP 口收包的写法更贴合设备语义。
>
> v0.4（2026-10-07）起壁纸上传收编进系统页（`wallpaper.py`，§76 设备拉模型）：
> 选 800x480 JPEG（≤2MB）→ 本地预校验（SOF 解析）→ WP_OFFER×3 → 设备 HTTP 拉取
> `/wallpaper/<name>`（就绪才宣告）→ 设备落池 + 自动设为当前 + crossfade → WP_RESULT。
> 设备端回包同样固定 PC:50003，由 link 分发器路由（`WP_*` → 壁纸会话）。
>
> v0.4.1（2026-10-07）壁纸管理独立成左侧「壁纸管理」页：大图预览（800x480 同比例）+
> 校验结果着色 + 取消上传 + 会话日志面板；系统页只保留信息卡片。

## 依赖

运行于 ESP-IDF 的 Python venv（`C:\Espressif\tools\python\v6.1\venv`，3.12）：

- 核心：标准库 + requests
- 桌面：PySide6、sounddevice、pyaudiowpatch、numpy（已装）
- 音乐副屏：**winsdk**（SMTC 采集，2026-09-23 已装入该 venv）
- 转写：sherpa-onnx（可选，无模型时自动跳过）

补装桌面全家桶：`pip install -e .[desktop]`

## 防火墙

首次运行放行"专用网络"：UDP 50000-50004、TCP 50002/50005。

## 注意

- 音乐副屏与 C# 版 `tools/pc_now_agent` **不要同时开**（50004/50005 冲突，后启动方绑定失败并日志提示）。
- OTA 面板与 C# agent 的 `--ota` 模式**也不要同时开**（50005 冲突）；OTA 已收编进本应用，日常直接用「固件升级」页。
- `--no-pcnow` 只是不起副屏采集（50004 不占）；50005 仍由本应用占用供 OTA 固件下发。
- 歌词/封面来自网易云非官方接口（约 2 次/首），纯音乐或未收录歌曲会显示"无歌词"，不影响推流。
- v1 的单窗口 UI 已被 `ui/` 多页框架取代；`net/` `record/` `audio/` `asr/` 模块保持原实现。
