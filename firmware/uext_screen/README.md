# uext 副屏固件（预编译烧录包）

本目录存放 **USB 扩展屏固件（uext_screen）的预编译产物**，供**不维护源码、只要快速点亮副屏**的使用者直接烧录。

> 源码工程不在本仓库内，位于 `Desktop/esp32/uext_screen/`（独立 IDF 工程，含 7 个离线 vendor 组件）。
> 构建细节见 [`docs/项目规划/USB副屏集成技术方案.md`](../../docs/项目规划/USB副屏集成技术方案.md)。

## 包内文件

| 文件 | 说明 |
|---|---|
| `uext_screen.bin` | 副屏应用固件（烧到 uext 槽 `0xA20000`） |
| `xfz1986_usb_graphic_250224_rc_sign.exe` | Espressif 官方已签名 Windows IDD 显示驱动（Win10/11，免测试模式） |

当前固件参数：**JPEG 质量 9 / 单帧上限 450KB**（2026-10-05 版，md5 `e16b94d0...`，446,752 字节）。

## 前置条件

1. **主固件（display_audio_photo）已烧录**，分区表为 V1.3 版（uext 槽 = `ota_2 @ 0xA20000`，2.875MB）。
   旧 V1.1/V1.2 分区表无法容纳本固件，请先烧录主工程。
2. ESP32-S31-Korvo-1 的 **USB Type-A 口**接 PC（这是 OTG High-Speed 数据口）；
   **Type-C (UART) 口**接 PC 用于烧录和日志。

## 烧录步骤

⚠️ **只能用下面的地址烧录，绝对不要对 uext 工程跑 `idf.py flash`**（会把主固件槽覆盖）。

1. 安装 ESP-IDF v6.1 环境后，在 IDF 终端执行（COM 口以设备管理器为准）：

   ```
   python -m esptool -p COM9 write-flash 0xA20000 firmware/uext_screen/uext_screen.bin
   ```

2. 烧录完成后设备仍运行主固件；在主界面**按 SET 键（单击）→ 屏幕出现 2 秒倒计时确认卡 → 点确认**，设备重启进入副屏模式。

## Windows 驱动与使用

1. 双击 `xfz1986_usb_graphic_250224_rc_sign.exe` 安装已签名 IDD 驱动；
2. 设备进入副屏模式后，Windows 枚举复合设备 `USB\VID_303A&PID_2986`；
3. `Win+P` → "扩展这些显示器"，出现一台 800×480 显示器；
4. 触摸屏需在 Windows "平板设置" 里做屏幕映射。

## 切回主固件

副屏模式下**单击 SET 键**：固件回写 `ota_0` 槽引导标志并重启，回到主固件（无需重新烧录）。
