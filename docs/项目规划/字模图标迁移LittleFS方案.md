# 字模与图标迁移到 LittleFS 方案（待审阅）

## 1. 现状盘点

### 1.1 当前固件占用
| 项 | 大小 |
|---|---|
| app bin | 3.18 MB |
| ota_0 / ota_1 | 各 4 MB（0x400000） |
| 当前 app 余量 | ~0.82 MB |
| LittleFS 分区 | 8 MB（0x7E0000） |
| LittleFS 已用 | ~130 KB（xwkkk.jpg 32KB + boot_tone.wav 98KB） |
| LittleFS 剩余 | ~7.9 MB |

### 1.2 拟迁移资源（全部编译进 app .rodata）

**字体（4 个，源码 ~900 KB）**
| 文件 | 源码大小 | 用途 |
|---|---|---|
| lv_font_digital_96.c | 103 KB | 大时间数字 |
| lv_font_cn_20.c | 119 KB | 风向/日出日落 |
| lv_font_cn_30.c | 225 KB | 主要中文文字 |
| lv_font_cn_45.c | 462 KB | 温度数字+城市名 |

**图标（8 个，源码 ~7.2 MB）**
| 文件 | 源码大小 | 用途 |
|---|---|---|
| image_sun.c | 1296 KB | 晴天 |
| image_rain.c | 1127 KB | 雨天 |
| image_cloud.c | 1106 KB | 多云 |
| image_wifi.c | 1065 KB | WiFi 状态 |
| image_humidity.c | 750 KB | 湿度 |
| image_sunrise.c | 747 KB | 日出日落 |
| image_wind.c | 716 KB | 风 |
| image_temp.c | 568 KB | 温度计 |

> 注：.c 源码是 C 数组文本，编译后在 flash 里是二进制。8 个图标源码合计 7.2 MB，但当前 bin 只有 3.18 MB，说明链接器实际只链接了被引用的部分，或部分数组被 GC。**迁移前需用 `idf.py size-components` 确认 .rodata 真实占用**。

## 2. 迁移目标

把字体和图标数据从 app 固件（.rodata）搬到 LittleFS，app 只保留加载逻辑。

| 项 | 迁移前 | 迁移后 |
|---|---|---|
| app bin | 3.18 MB | 预计 1.5~2.0 MB |
| ota 余量 | 0.82 MB | ~2 MB |
| LittleFS 占用 | 130 KB | ~1.5 MB |
| LittleFS 剩余 | 7.9 MB | ~6.5 MB |

## 3. 技术路径

### 3.1 图标迁移（简单，推荐先做）

LVGL v9 直接支持文件系统图片：

```c
// 旧：编译进 flash
LV_IMG_DECLARE(image_wifi);
lv_image_set_src(img, &image_wifi);

// 新：从 LittleFS 读
lv_image_set_src(img, "L:/littlefs/icons/wifi.png");
```

**步骤**：
1. 用 LVGL 在线图片转换器（https://lvgl.io/tools/imageconverter）把每个 .c 源数组导出为 PNG/BIN 文件，放 `littlefs_content/icons/`
2. CMakeLists 删除 8 个 `ui/icons/image_*.c`
3. 代码里 `LV_IMG_DECLARE` 删除，`lv_image_set_src` 改成路径字符串
4. 重烧 littlefs（`idf.py -p COM9 storage-flash`）
5. LVGL 需启用 `LV_USE_FS_POSIX` 并注册盘符 `L:` → `/littlefs`

**风险**：首次显示时从 flash 读 PNG 有 ~50ms 延迟，但图标小且只加载一次，无感。

### 3.2 字体迁移（稍复杂，可选）

LVGL v9 支持运行时加载字模：

```c
// 旧：编译进 flash
LV_FONT_DECLARE(lv_font_cn_30);
lv_obj_set_style_text_font(label, &lv_font_cn_30, 0);

// 新：从文件加载
lv_font_t *cn30 = lv_font_load("L:/littlefs/fonts/cn_30.bin");
lv_obj_set_style_text_font(label, cn30, 0);
```

**步骤**：
1. 用 LVGL Font Converter 把每个字模生成 `.bin`（不是 .c），放 `littlefs_content/fonts/`
2. CMakeLists 删除 4 个 `ui/fonts/lv_font_*.c`
3. `overlay_wallpaper_init` 里先 `lv_font_load` 拿到指针，存全局变量
4. 所有 `&lv_font_cn_XX` 改成全局指针

**风险**：
- 字模加载后常驻内存（PSRAM），4 个字模合计 ~900 KB，PSRAM 充足
- `lv_font_load` 失败要处理降级（用内置字体）
- digital_96 纯数字字体最小，可考虑保留在 flash 不迁移

## 4. 建议分阶段

**阶段 A（低风险，收益大）**：先迁 8 个图标
- 图标数据最大，迁移后 bin 预计降 1~1.5 MB
- 改代码少（只改 `lv_image_set_src` 调用）
- 不动字体，UI 文字渲染不变

**阶段 B（可选）**：再迁字体
- 字体迁移要改所有 `lv_obj_set_style_text_font` 调用点
- 收益约 0.5~0.9 MB
- 如果阶段 A 后 bin 已经 <2MB，ota 余量够，字体可以不迁

## 5. 不迁移的理由

- 当前 bin 3.18MB，ota 4MB，还有 0.82MB 余量
- 后续加 YOLO/摄像头/OTA 固件可能再涨
- 迁移后每次改图标/字模都要重烧 littlefs，不如改 .c 直接编译方便
- 代码复杂度上升（错误处理、加载时机）

## 6. 决策点

请审阅后选择：
- [ ] 只迁图标（阶段 A）
- [ ] 图标+字体都迁（阶段 A+B）
- [ ] 不迁移，保持现状
