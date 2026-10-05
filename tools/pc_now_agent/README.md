# pc_now_agent 操作手册

> PC 音乐副屏推流代理 · 版本 v1（2026-09-23）
> 配套设备端：`display_audio_photo` 工程 `main/middleware/network/pcnow_service.c` + `ui/tabs/tab_pcnow.c`
> 协议规范：`docs/项目规划/通信接口规范_ICD.md` §6（V1.3）

---

## 1. 这是什么

在 Windows 上常驻运行的小程序，把你**正在播放的音乐**（网易云 / QQ 音乐 / foobar2000 / 浏览器等任何注册了 Windows 媒体会的播放器）实时推送给 Korvo 音箱，音箱作为副屏显示：曲名、歌手、专辑、进度条、逐句歌词、专辑封面（圆形旋转）。

**原理一句话**：Windows 系统有一条媒体总线（SMTC），所有播放器都会把"正在播放"注册上去——本程序订阅它拿曲名/歌手/进度，再从网易云服务器接口拿歌词和高清封面，通过局域网推给设备。**不需要连接、修改、配置任何播放器。**

> ⚠ **与统一上位机 pc_host v2 二选一**（2026-09-23 起）：`pc_host/`（PySide6，`python -m pc_host.main`）的"音乐副屏"页实现了完全相同的协议（ICD V1.3），且同时统筹通话对讲/录音库/系统页。两者**不可同时运行**（UDP 50004 / TCP 50005 绑定冲突，后启动方日志报错）。本 C# 版保留为命令行/无人值守场景使用。

---

## 2. 环境要求

| 项 | 要求 | 说明 |
|---|---|---|
| 系统 | Windows 10 19041+ / Windows 11 | SMTC 系统自带 |
| .NET | .NET 8 SDK（编译）或直接跑编译产物 | 本机便携版已装：`C:\Users\xiao1\.workbuddy\binaries\dotnet\`（无需安装到系统） |
| 网络 | PC 与设备连**同一路由器**（同一网段） | 路由器不能开 AP 隔离 |
| 端口 | UDP 50003/50004、TCP 50005 可用 | 首次运行若弹防火墙提示，勾选"专用网络"允许 |

---

## 3. 快速开始（3 步）

```bash
cd C:\Users\xiao1\Desktop\esp32\display_audio_photo\tools\pc_now_agent

:: ① 设置便携 SDK（已固化编译产物的话可跳过）
set DOTNET_ROOT=C:\Users\xiao1\.workbuddy\binaries\dotnet

:: ② 编译（首次或改代码后；产物在 bin\Release\...）
dotnet build -c Release

:: ③ 运行（常驻）
dotnet run -c Release
```

看到 `SMTC init ok` 和 `pc_now_agent started.` 即启动成功。之后：

1. 用网易云（或任何播放器）放一首歌；
2. 日志应依次出现 `track: 歌手 - 曲名` → `lyrics: N lines` → `cover ready: N B`；
3. 设备端进 **PC** tab，联上后状态点变绿，自动显示。

> 免编译直接跑：`bin\Release\net8.0-windows10.0.19041.0\pc_now_agent.exe`（双击即可，无需 SDK）。

---

## 4. 命令行参数

| 参数 | 作用 | 示例 |
|---|---|---|
| （无） | 常驻运行，监听 SMTC 自动推流 | `pc_now_agent.exe` |
| `--fake` | **自检模式**：合成一首"正在播放"的歌（含假歌词/封面），不依赖任何真实播放器 | `pc_now_agent.exe --fake` |
| `--device <ip>` | 跳过广播发现，直接向指定 IP 单播心跳 | `--device 192.168.1.23`；同机联调填 `127.0.0.1` |
| `--run-seconds <n>` | 运行 n 秒后自动退出（0=常驻），用于自动化测试 | `--fake --run-seconds 10` |
| `--ota <bin路径>` | **OTA 升级模式**：把该固件推给设备（自动切槽+重启） | `pc_now_agent.exe --ota build\display_audio_photo.bin` |
| `--ota-limit <N>` | OTA 破坏性测试：只发固件前 **N%**（1..99），验证设备端断点保留 + **续传** | `--ota xxx.bin --ota-limit 40` |
| `--ota-corrupt` | OTA 破坏性测试：固件流**中点翻 1 bit**，验证 `esp_ota_end` / 段哈希兜底 | `--ota xxx.bin --ota-corrupt` |
| `--force` | OTA：**忽略版本比对**（同版本/降级也照发） | `--ota xxx.bin --force` |

### `--ota` 升级模式（WiFi 局域网 OTA，详见方案 §4）

```bash
:: 一条命令完成：解析固件 → 版本比对 → 通报 → 设备拉取 → 校验 → 切槽 → 重启
pc_now_agent.exe --ota C:\...\build\display_audio_photo.bin
```

`--ota` 与推流模式**互斥**（二选一执行）。该模式自动完成：

1. **解析固件**：读 `esp_app_desc_t`（偏移 `0x20`，magic `0xABCD5432`）取版本号，
   现算大小与 SHA256。**不是 app bin 会直接拒绝退出**（挡住误选 bootloader.bin /
   partition-table.bin）；
2. **HTTP 路由**：在既有 `:50005` 上加 `GET /firmware.json`（`{ver,size,sha256,...}`）
   与 `GET /firmware.bin`（`Content-Length` + 64KB 分块流式输出，**支持 `Range` → 206**）；
3. **版本比对**：设备 `fw_ver >=` 固件版本 ⇒ **拒发**（除非 `--force`）；
4. **会话**：等设备心跳 → 发 `OTA_OFFER`（10s×3 重发）→ 设备 `OTA_ACCEPT` →
   打印 `[####----] 42% 312 KB/s` 进度 → `OTA_RESULT{ok}`；
5. **掉线保护**：会话期设备心跳丢失 > 8s ⇒ 主动发 `OTA_ABORT{reason:"peer_lost"}`；
6. **判定**：`ok:1` 后等设备重启回来，核对 `HEARTBEAT_ACK.fw_ver` 是否等于新版本
   （一致=升级成功；没变=已自动回滚）。

退出码：`0` 成功且版本一致 / `2` 设备未上线 / `3` 设备拒绝 / `4` 无 ACCEPT /
`5` 重启未回 / `6` 升级失败 / `7` 超时 / **`8` 版本未提升** / **`9` 版本不一致** /
`130` Ctrl-C。

> ⚠ **固件必须装得进备用槽**（main 分支 `ota_0`/`ota_1` 各 **5 MB**）。
> 设备若回 `OTA_REJECT reason=space` 就是 bin 超槽——需先瘦身。
> 也注意 `build/` 目录若残留其他分支的产物，bin 尺寸会不符（示例：人脸识别分支
> 的 bin 6.52MB，装不进 main 的 5MB 槽）。

> ★ 完整逐功能逐步骤验收流程见
> `docs/项目规划/WiFi_OTA_烧录验收与使用流程.md`（V2）。

参数可组合。典型联调组合：

```bash
:: PC 侧协议自检（不需要设备）：终端 A 开假音箱，终端 B 跑 agent
python fake_device.py --http
pc_now_agent.exe --fake --device 127.0.0.1 --run-seconds 10
```

---

## 5. 控制台日志判读（健康五信号）

启动后按顺序出现以下 **5 条即为全链路正常**：

```text
[16:30:01.012] SMTC init ok                          ← Windows 媒体总线接管成功
[16:30:05.210] device ONLINE at 192.168.1.23         ← 设备回 ACK，链路通
[16:30:08.441] track: 方大同 - Love Song             ← 换曲被捕获
[16:30:09.102] lyrics: 32 lines (hash 7bb9ba6d)      ← 歌词就绪（0 = 本曲无歌词）
[16:30:09.855] cover ready: 15230 B (hash 7bb9ba6d)  ← 封面就绪
```

之后的**静默是常态**：推流节奏为 META 每 3s、进度每 1s、歌词行变化时才推，不刷屏。换曲时重新出现 `track:` 一行并重新拉取资源。

红字（`NetEase lrc err: ...` / `cover err: ...`）是**单链降级**提示——只影响该曲的歌词或封面，曲名/进度照常推，程序不退出。

---

## 6. 故障排查 FAQ

| 现象 | 原因与处理 |
|---|---|
| 日志停在 `started`，放歌也无 `track:` | 播放器没向 Windows 注册媒体会话。按 `Win+G` 或点音量浮层，看有没有"正在播放"卡片；没有就换播放器（网易云桌面版/QQ 音乐/foobar 都支持） |
| 一直没有 `device ONLINE` | ① 设备没连上 WiFi / 连的不是同一路由器；② 路由器开了 AP 隔离；③ 防火墙拦了 UDP 50003（放行或临时关防火墙测试）。可先用 `python fake_device.py --http` + `--fake` 排除 PC 侧问题 |
| `lyrics: 0 lines` 频繁出现 | 网易云接口偶发限流（4xx）。解决：往本地歌词目录放同名 `.lrc` 文件（见 §7），本地优先不走网络 |
| `cover: none` | 网易云图片拉不到且 SMTC 缩略图也没给。仅此曲无封面，其它功能不受影响 |
| 设备突然 OFFLINE | PC 程序退出 / 设备断网 / 5s 无心跳的正常判离。恢复后自动重连（设备端会重新拉取） |
| 双开程序报端口占用 | 50003/50004/50005 被占——本程序只需运行一份 |

---

## 7. 本地歌词（可选的第一优先级）

查找顺序：**本地 `.lrc` → 网易云接口 → 无**。云端播放一般直接命中网易云接口，本地文件用于：网易云接口限流、本地下载的音乐、或想用自己的歌词版本。

放置规则：

- 目录：**"音乐"库文件夹**（`此电脑 → 音乐`）或本程序 exe 所在目录，**递归子目录**都会搜；
- 文件名包含"歌手"和"曲名"即可（推荐 `歌手 - 曲名.lrc`，与网易云下载的命名一致）；
- 标准增强 LRC 格式（`[mm:ss.xx]歌词`，一行可带多个时间标签）。

---

## 8. 代码级配置项（改后需重新编译）

| 位置 | 项 | 默认值 |
|---|---|---|
| `Program.cs` 顶部常量 | 心跳周期 / 离线阈值 | 200ms / 5s（与设备端 ICD 约定一致，**别单方面改**） |
| 同上 | 端口 50003/50004/50005 | 同上 |
| 同上 | `LyricDirs` 本地歌词搜索目录 | 音乐库 + exe 目录 |
| `NetEaseCoverAsync/LoadCoverAsync` | 封面尺寸/质量 | 240×240，JPEG q80 |
| `ParseLrc` | credits 行过滤词表 | 作词/作曲/编曲…（可按需增删） |

---

## 9. 协议速查（详见 ICD V1.3）

| 通道 | 方向 | 内容 |
|---|---|---|
| UDP :50003 | 双向 | 心跳（PC 200ms → 设备，设备回 ACK）；NOW_CMD 上行（预留） |
| UDP :50004 | PC → 设备 | `NOW_META`（换曲+3s 周期）/ `NOW_PROG`（1Hz）/ `NOW_LRC`（行变，回退通道） |
| TCP :50005 | 设备 → PC | `GET /cover/<hash>.jpg`（240×240 JPEG）、`GET /lyrics.json?k=<hash>`（归一化行数组） |

hash = `SHA1(曲名|歌手)` 前 8 位 hex；设备端只在 hash 变化时拉取，避免重复下载。

---

## 10. 常驻运行（可选）

想开机自动后台运行，二选一：

- **任务计划程序**：触发器"登录时"，操作指向 `pc_now_agent.exe`，勾选"不管用户是否登录都要运行"（无黑窗口）；
- **启动文件夹**：把 exe 的快捷方式放进 `shell:startup`（有控制台窗口，可加 `--device` 固定设备 IP）。

---

## 11. 关联文档

- 方案全貌：`docs/项目规划/PC音乐副屏局域网推流实施方案.md`（V2）
- 协议细节：`docs/项目规划/通信接口规范_ICD.md` §6（V1.3）
- 设备端说明：`pcnow_service.h` / `tab_pcnow.c` 文件头注释
- 联调脚本：`fake_device.py`（假音箱）
