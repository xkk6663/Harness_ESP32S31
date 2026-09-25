/* ov3660_fmt.c - OV3660 运行时格式档位表实现（含供应商私有寄存器表的引用）
 *
 * ★ 本文件是整个工程与 `espressif__esp_cam_sensor` **私有内部结构**的**唯一**耦合点。
 *   改动/升级组件时，只需要看这一个文件。文件头的长注释在 ov3660_fmt.h 里（务必先读）。
 */

#include "ov3660_fmt.h"

/* ---------------------------------------------------------------------------
 * 供应商私有寄存器表（private_include，由 main/CMakeLists.txt 的
 * target_include_directories 引入）
 *
 * ★ 只 include 这一个头就够了：`ov3660_settings.h` 在**尾部无条件**包含了全部 5 个格式表头
 *   （第 405-409 行：#include "ov3660_dvp_8bit_10Minput_1280x720_jpeg_12fps.h" 等）。
 *   ⇒ **不要再单独 include 那两张表头** —— 它们是 `static const` 数组，
 *     重复 include 会直接 "redefinition" 报错（2026-09-25 实测踩过）。
 *
 * ★ 那两个我们需要的表，展开后各自只有 3 行（基础寄存器组宏 + 格式寄存器组宏 + tail）：
 *     static const ov3660_reginfo_t ov3660_dvp_8bit_20Minput_240x240_rgb565_be_24fps[] = {
 *         ov3660_settings_240X240
 *         ov3660_settings_fmt_rgb565_be
 *         {OV3660_REGLIST_TAIL, 0x00}, // tail
 *     };
 *   下面的代码直接引用这两个数组名，**不要手抄寄存器值**。
 *
 * ★ pragma 说明：ov3660_settings.h 还会定义若干 `static const` 数组
 *   （ov3660_sensor_default_regs / sensor_saturation_levels / sensor_special_effects），
 *   本文件都用不到 ⇒ gcc 报 -Wunused-const-variable（本工程 -Werror=all 会变错误）。
 *   那是**无害的**，局部关掉即可；**不要**为了消警告去"引用"它们
 *   （那会误导读者以为我们依赖它们）。
 * ------------------------------------------------------------------------- */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "ov3660_settings.h"
#pragma GCC diagnostic pop

/* 与 ov3660_format_info[] 里的取值一致（ov3660.c:67-151）。XCLK 由 BSP 提供 20 MHz
 * （bsp_camera.c:29 BSP_CAMERA_XCLK_CLOCK_MHZ），BSP 那边也是 20 MHz，两处必须一致。 */
#define OV3660_XCLK_HZ      20000000

#define ARRAY_SIZE_(a)      (sizeof(a) / sizeof((a)[0]))

/* ---- 档位 1：240x240 RGB565 BE @24fps —— CAM tab 预览 / 跑模型 / 人脸检测 ---------- */
static const esp_cam_sensor_format_t s_fmt_rgb565_240x240 = {
    .name      = "DVP_8bit_20Minput_RGB565_BE_240x240_24fps",
    .format    = ESP_CAM_SENSOR_PIXFORMAT_RGB565_BE,
    .port      = ESP_CAM_SENSOR_DVP,
    .xclk      = OV3660_XCLK_HZ,
    .width     = 240,
    .height    = 240,
    .regs      = ov3660_dvp_8bit_20Minput_240x240_rgb565_be_24fps,
    .regs_size = (int)ARRAY_SIZE_(ov3660_dvp_8bit_20Minput_240x240_rgb565_be_24fps),
    .fps       = 24,
    .isp_info  = NULL,
};

/* ---- 档位 2：1280x720 JPEG @12fps —— VIDEO tab 拍照 / 录像（传感器内部硬件编码） ---
 *
 * ★ 这里**没有** ESP32 侧的软件编码，也没有 esp_video 的 M2M JPEG 设备：
 *   本工程 `CONFIG_ESP_VIDEO_ENABLE_JPEG_DEVICE` **未开**（sdkconfig 里只有 DVP 设备），
 *   而 M2M 路线输入只能是 RGB565/UYVY/RGB24/GREY，且 OV3660 根本没提供 1280x720 RGB565 档
 *   （看 ov3660_format_info[]：RGB565 只有 240x240 与 640x480 两档）
 *   ⇒ M2M 路线**做不到 720p**。720p 只能靠传感器自己的 JPEG 编码器，即本档位。
 *
 * ★ 待判（2026-09-25，**未动**）：本表文件名是 `..._10Minput_...`（按 10 MHz XCLK 设计），
 *   而 `.xclk` 写的是 20 MHz；.name 字符串又写着 "20Minput"。三者不一致。
 *   台账里 `0x303b/0x303d/0x3824` 三个 PLL 寄存器的值确实与 240x240（20Minput）档不同，
 *   说明"输入时钟"这个维度是真实存在的。**但颜色问题已由下面的 0x4514 解释清楚，
 *   不要和它混在一起改**（一次只改一件事）；若要动 XCLK，先按下述判据复测 fps。 */

/* ---------------------------------------------------------------------------
 * ★★★ F49（2026-09-25）修"照片/视频整体发紫"：0x4514（读出顺序）没跟着镜像一起改
 *
 * 【症状】CAM 页 240x240 预览颜色正常，但拍照/录像存到 SD 卡的 JPEG **整体偏品红**。
 *   实测样张（1280x720，50,964 B）解出来的通道均值是 R=122.7 / G=92.6 / B=122.2
 *   —— G 比 R、B 低 ~30 灰阶，而 R≈B。
 *   ★ 这条数据本身就排除了"红蓝/RB 通道互换"：互换是**双向重标号**，把中性灰映射成
 *     中性灰，**不可能**把一面中性墙变成品红。⇒ 不是通道顺序，是**读出相位错位**。
 *
 * 【根因】esp32-camera（老驱动，用户基数极大）的 `ov3660.c::set_image_options()` 把下面
 *   三个寄存器当**一个不可分的整体**写，并按 (binning, vflip, hmirror) 查表：
 *       reg20 (0x3820) = compression / binning / vflip 位
 *       reg21 (0x3821) = compression / binning / hmirror 位
 *       0x4514         = 读出顺序补偿值（**必须与上面两位同步**）
 *   它的表：
 *       不 binning + 不镜像        → 0x88   ← 供应商 720p 表给的就是这个
 *       不 binning + V翻 + H镜像   → 0xbb   ← 本工程实际需要这个
 *       binning    + 不镜像        → 0xaa   ← 供应商 240x240 表给的就是这个
 *       binning    + V翻 + H镜像   → 0xaa   ★ 与上一行**同值**！
 *
 * 【为什么"预览正常、拍照发紫"】本工程在 `camera_service.c` 里**总是**开 HFLIP+VFLIP
 *   （它是被搬到传感器里的 CPU 180° 旋转的等价物，见那个文件里的长注释）：
 *     · 240x240 档 = binning ⇒ 需要「binning+V翻+H镜像」= **0xaa**，而供应商表也是 0xaa
 *       ⇒ **两个 case 恰好同值**，漏洞被掩盖 ⇒ 预览颜色正常。
 *     · 1280x720 档 = 不 binning ⇒ 需要「no-bin+V翻+H镜像」= **0xbb**，而供应商表是 0x88
 *       ⇒ 读出相位错位 ⇒ 颜色错位 ⇒ 发紫。
 *   ⇒ **供应商表本身没错**（它给的是"不翻不镜像"的参考设计值），
 *     错在"我们加了镜像却没同步 0x4514"。
 *
 * 【修法】只给 720p 这张表补一条覆盖项，放在格式组**之后**（`ov3660_write_array()`
 *   按顺序写、遇 `OV3660_REGLIST_TAIL` 结束 ⇒ 后写者生效 ⇒ 覆盖掉前面的 0x88）。
 *   240x240 那条**故意不动**：它的 0xaa 已经等于正确值，再加一条只会制造"两个真源"。
 *
 * 【★ 不变式 / 回归防线】
 *   (1) 本工程一旦**不再**开 HFLIP+VFLIP，这里的 0xbb 就必须改回 0x88（反之亦然）；
 *   (2) `camera_service.c` 起流后会**读回** 0x3820/0x3821/0x4514 打到日志
 *       （搜 `sensor reg 0x4514`）—— 那行就是这条不变式的**上机判据**：
 *       240x240 应读到 0xaa，720p 应读到 0xbb，两次都读不到就说明镜像没生效；
 *   (3) 下面的 `_Static_assert` 保证"供应商表条目数一变就必须重新核对本覆盖项",
 *       避免组件升级后覆盖项静默失效。
 *
 * 【★ 已核实的写入路径】`VIDIOC_S_SENSOR_FMT` → `dvp_*_set_sensor_format()` →
 *   `esp_cam_sensor_set_format()` → `dev->ops->set_format(dev, format)`
 *   （esp_cam_sensor.c:51-56，**原样透传**，不做"按 name 重新解析成驱动自带表"）
 *   → `ov3660_set_format()` 把 `format->regs` 强转后逐条写（ov3660.c:567）
 *   ⇒ 本数组一定会被原样写进传感器。 */
#define OV3660_REG_READOUT_ORDER            0x4514
/* 读出顺序补偿：非 binning + V翻转 + H镜像（= 本工程固定配置，见上面的证据链） */
#define OV3660_RDORD_NOBIN_VFLIP_HMIRROR    0xbb

/* 供应商 720p JPEG 表 + 本工程的一条读出顺序覆盖项。
 * ★ 仍然**引用供应商宏**、不手抄寄存器值（见 ov3660_fmt.h 顶部"引用 vs 复制"的取舍）；
 *   ★ 末尾的 `{OV3660_REGLIST_TAIL, 0x00}` **不能省** —— `ov3660_write_array()` 是
 *     靠 `reg == OV3660_REGLIST_TAIL` 结束循环的（ov3660.c:190），不是靠 regs_size。 */
static const ov3660_reginfo_t s_jpeg_1280x720_regs[] = {
    ov3660_settings_1280X720
    ov3660_settings_fmt_jpeg
    {OV3660_REG_READOUT_ORDER, OV3660_RDORD_NOBIN_VFLIP_HMIRROR},
    {OV3660_REGLIST_TAIL, 0x00}, // tail
};

/* 组件升级后供应商表条目数一变，这里就编译失败 ⇒ 逼人回来重新核对覆盖项位置。
 *
 * ★★ 断言/诊断文字**必须是纯 ASCII**：GCC 会把消息里的非 ASCII 字节按八进制转义打印
 *    （2026-09-25 负向自测实测：中文变成 `\37777777744\37777777676...`），
 *    真触发时完全读不懂 ⇒ 等于把"响亮失败"又变回"静默失败"。
 *    ⇒ **断言、`#error` 用英文；解释用中文注释**（注释不进编译输出，不受影响）。 */
_Static_assert(ARRAY_SIZE_(s_jpeg_1280x720_regs) ==
               ARRAY_SIZE_(ov3660_dvp_8bit_10Minput_1280x720_jpeg_12fps) + 1,
               "OV3660 720p JPEG vendor reg-table entry count changed: "
               "re-check the 0x4514 readout-order override in ov3660_fmt.c");

static const esp_cam_sensor_format_t s_fmt_jpeg_1280x720 = {
    .name      = "DVP_8bit_20Minput_JPEG_1280x720_12fps",
    .format    = ESP_CAM_SENSOR_PIXFORMAT_JPEG,
    .port      = ESP_CAM_SENSOR_DVP,
    .xclk      = OV3660_XCLK_HZ,
    .width     = 1280,
    .height    = 720,
    /* ★ 不是供应商那张原表，而是"原表 + 读出顺序覆盖"（F49，见上面的长注释）。
     *   两张表只差一条 {0x4514, 0xbb}，其余逐字节相同（由上面的 _Static_assert 保证）。 */
    .regs      = s_jpeg_1280x720_regs,
    .regs_size = (int)ARRAY_SIZE_(s_jpeg_1280x720_regs),
    .fps       = 12,
    .isp_info  = NULL,
};

const esp_cam_sensor_format_t *ov3660_fmt_lookup(uint32_t width, uint32_t height, bool want_jpeg)
{
    /* 只认本工程收录的两档。**故意不做"向下兼容/自动缩放"** ——
     * 传进来一个没收录的尺寸却拿到别的档，会变成"日志说 640x480、实际 240x240"这类
     * 极难查的问题；宁可在调用处直接失败。 */
    if (want_jpeg) {
        if (width == s_fmt_jpeg_1280x720.width && height == s_fmt_jpeg_1280x720.height) {
            return &s_fmt_jpeg_1280x720;
        }
        return NULL;
    }

    if (width == s_fmt_rgb565_240x240.width && height == s_fmt_rgb565_240x240.height) {
        return &s_fmt_rgb565_240x240;
    }
    return NULL;
}
