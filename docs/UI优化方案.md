# Harness ESP32-S3 UI 架构重构方案

> 基于油炸鸡 OV-Watch PageManager 框架，针对你现有代码的逐文件改造指南

---

## 一、现状诊断：你的代码现在是什么样的

### 1.1 现有目录结构
```
main/ui/
├── ui_disp.c/h          # 主框架：tabview + 下拉壁纸 + 全局对象定义
├── ui_splash.c/h        # 启动页：上滑进入主界面
├── ui_call.c/h          # Call Tab：语音通话页
├── ui_record.c/h        # Record Tab：录音页
├── ui_settings.c/h      # Sys Tab：系统信息 + 音量
├── ui_windows.c/h       # 弹窗：WAV播放窗口
└── ui_state.h           # 全局LVGL对象指针集中声明
```

### 1.2 你现在的代码模式（三个Tab页的共同点）
```c
// 每个Tab页都是这个模式：
void app_disp_lvgl_show_xxx(lv_obj_t *screen, lv_group_t *group)
{
    // 1. 直接在传入的screen上创建控件
    // 2. 颜色宏每个文件重复定义一遍
    // 3. 重要控件存到全局变量（ui_state.h里extern）
    // 4. 没有销毁函数
}
```

### 1.3 现有问题清单
| 问题 | 具体表现 |
|------|---------|
| 颜色重复 | COL_BG/COL_CARD/COL_CYAN等在4个文件里各定义一遍 |
| 全局控件太多 | rec_btn / play1_btn / play_btn等6个全局变量 |
| 无页面生命周期 | Tab创建后常驻，没法销毁释放内存 |
| 无页面管理 | 只有Tab切换，二级页面只能动态创建窗口再手动删 |
| 启动流程硬编码 | main.c直接调app_disp_lvgl_show_main() |

---

## 二、目标架构：改造后是什么样的

### 2.1 分层结构
```
┌─────────────────────────────────────────┐
│  UI页面层（业务页面）                    │
│  page_splash / page_main / page_wifi    │
│  page_bluetooth / page_rgb / page_system│
├─────────────────────────────────────────┤
│  页面管理层（PageManager）               │
│  栈式导航 / 生命周期 / 动画切换          │
├─────────────────────────────────────────┤
│  服务层（Service，你现有方案保留）       │
│  audio_service / fs_service              │
├─────────────────────────────────────────┤
│  BSP 层（esp-bsp，你现有方案保留）       │
└─────────────────────────────────────────┘
```

### 2.2 页面层级
```
栈底：Page_Splash（启动页）
  ↓ 上滑手势
栈中：Page_Main（主界面 TabView）
  ├── Tab 1: Call（语音通话）
  ├── Tab 2: Record（录音）
  └── Tab 3: Sys（系统入口）
       ↓ 点击 → PageManager_Load(&Page_Settings)
       └── 设置列表页
            ├── Row 1: WIFI       → Page_Wifi
            ├── Row 2: Bluetooth  → Page_Bluetooth
            ├── Row 3: RGB        → Page_Rgb
            └── Row 4: System     → Page_System

─────────────────────────────────────────────
Overlay 浮层层（lv_layer_top，不入页面栈）
  ├── overlay_wallpaper（壁纸层，主界面顶部下拉拉出）
  └── overlay_volume（音量弹窗，任意页面最右端左滑弹出）
```

> 说明：
> - 壁纸和音量条都是临时浮层，不改变当前页面上下文，关闭后回到原页面，所以都走 Overlay 模式，不走 PageManager 栈
> - 两个浮层由 `overlay_manager` 统一管理，互斥打开，避免触摸冲突
> - 音量弹窗：**任意页面的屏幕最右端左滑**即可弹出，不是只有主界面才能用

---

## 三、改造步骤 TODO List

### Phase 0：新增基础文件（不破坏现有功能）
- [ ] 新增 `ui_theme.h` —— 统一颜色定义，替代各文件重复的COL宏
- [ ] 新增 `page_manager.c/h` —— 页面栈管理框架
- [ ] 新增 `overlay_manager.c/h` —— 浮层管理器（壁纸+音量弹窗互斥调度）
- [ ] 新增 `ui_common.c/h` —— 通用控件工厂（标题栏、设置行等）

### Phase 1：改造启动流程
- [ ] main.c 改为 PageManager 初始化 + 加载 Splash 页
- [ ] ui_splash.c 封装为 Page_Splash（create/destroy）
- [ ] ui_disp.c 主界面封装为 Page_Main（create/destroy）
- [ ] 验证：启动 → 上滑 → 主界面，功能和现在完全一样

### Phase 2：迁移三个Tab页
- [ ] ui_call.c 改造：控件改为static，封装为Page_Main的内部模块
- [ ] ui_record.c 改造：rec_btn/play1_btn改为static，去掉ui_state.h的extern
- [ ] ui_settings.c 改造：整体改为设置列表入口页
- [ ] ui_windows.c WAV播放窗封装为 Page_WavPlayer
- [ ] 保留主界面下拉壁纸功能：继续放在 lv_layer_top()，和音量弹窗同级
- [ ] 验证：三个Tab正常切换，下拉壁纸正常滑出/收起，录音/播放功能正常

### Phase 3：新增四个资源页面
- [ ] 新增 page_settings.c —— 设置列表页（四大行）
- [ ] 新增 page_wifi.c —— WIFI 设置页
- [ ] 新增 page_bluetooth.c —— Bluetooth 设置页
- [ ] 新增 page_rgb.c —— RGB 灯效页
- [ ] 新增 page_system.c —— System 系统页

### Phase 4：Overlay 浮层
- [ ] 改造 overlay_wallpaper.c —— 从 ui_disp.c 里提取出来，封装成独立浮层组件
- [ ] 新增 overlay_volume_popup.c —— 右侧滑出音量条，**任意页面最右端左滑触发**
- [ ] 两个浮层通过 overlay_manager 互斥调度，互不干扰

---

## 四、逐文件改造指南（基于你的实际代码）

### 4.1 新增：ui_theme.h —— 统一颜色定义

**改造前**：每个文件都重复定义一遍
```c
// ui_call.c / ui_record.c / ui_settings.c 各写一遍
#define COL_BG        lv_color_hex(0x0F2547)
#define COL_CARD      lv_color_hex(0x1E3A5F)
#define COL_BORDER    lv_color_hex(0x3B5A82)
#define COL_CYAN      lv_color_hex(0x22D3EE)
// ... 重复N遍
```

**改造后**：统一放一个头文件
```c
// main/ui/ui_theme.h
#pragma once
#include "lvgl.h"

// AI dark-blue theme —— 从你现有代码提取
#define COL_BG        lv_color_hex(0x0F2547)
#define COL_CARD      lv_color_hex(0x1E3A5F)
#define COL_BORDER    lv_color_hex(0x3B5A82)
#define COL_CYAN      lv_color_hex(0x22D3EE)
#define COL_GREEN     lv_color_hex(0x34D399)
#define COL_RED       lv_color_hex(0xF87171)
#define COL_TEXT      lv_color_hex(0xE2E8F0)
#define COL_SUBTEXT   lv_color_hex(0x94A3B8)
#define COL_DARK      lv_color_hex(0x0B1120)
```

**改动量**：每个文件删掉重复的宏定义，改成 `#include "ui_theme.h"`

---

### 4.2 新增：page_manager.c/h —— 页面栈框架

> 参考油炸鸡实现，适配 ESP32 的 bsp_display_lock 线程安全

#### page_manager.h
```c
// main/ui/page_manager.h
#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAGE_STACK_MAX  8   // 页面栈最大深度

// 页面抽象结构体
typedef struct {
    lv_obj_t* (*create)(lv_obj_t* parent);  // 创建页面，返回根对象
    void (*destroy)(lv_obj_t* page);         // 销毁页面，释放资源
    const char* name;                        // 页面名（调试用）
} Page_t;

// 初始化页面管理器
void PageManager_Init(void);

// 压栈加载新页面（自动加锁）
void PageManager_Load(const Page_t* page);

// 返回上一页（自动加锁）
void PageManager_Back(void);

// 回到底层主页面（自动加锁）
void PageManager_BackToMain(void);

// 获取当前页面
const Page_t* PageManager_GetCurrent(void);

#ifdef __cplusplus
}
#endif
```

#### page_manager.c
```c
// main/ui/page_manager.c
#include "page_manager.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"

static const char* TAG = "PAGE_MGR";

typedef struct {
    const Page_t* page;
    lv_obj_t* obj;
} StackEntry_t;

static StackEntry_t s_stack[PAGE_STACK_MAX];
static uint8_t s_top = 0;

void PageManager_Init(void) {
    s_top = 0;
}

void PageManager_Load(const Page_t* page) {
    if (s_top >= PAGE_STACK_MAX) {
        ESP_LOGW(TAG, "page stack full!");
        return;
    }

    bsp_display_lock(0);

    // 创建新页面
    lv_obj_t* new_obj = page->create(lv_scr_act());

    // 入栈
    s_stack[s_top].page = page;
    s_stack[s_top].obj = new_obj;
    s_top++;

    // 淡入动画
    lv_scr_load_anim(new_obj, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);

    ESP_LOGI(TAG, "loaded: %s (depth=%d)", page->name, s_top);

    bsp_display_unlock();
}

void PageManager_Back(void) {
    if (s_top <= 1) {
        ESP_LOGW(TAG, "already at main page");
        return;
    }

    bsp_display_lock(0);

    // 弹出并销毁当前页面
    s_top--;
    s_stack[s_top].page->destroy(s_stack[s_top].obj);

    // 恢复上一页
    lv_obj_t* prev = s_stack[s_top - 1].obj;
    lv_scr_load_anim(prev, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);

    ESP_LOGI(TAG, "back to: %s (depth=%d)",
             s_stack[s_top - 1].page->name, s_top);

    bsp_display_unlock();
}

void PageManager_BackToMain(void) {
    while (s_top > 1) {
        s_top--;
        s_stack[s_top].page->destroy(s_stack[s_top].obj);
    }
    lv_scr_load(s_stack[0].obj);
}

const Page_t* PageManager_GetCurrent(void) {
    if (s_top == 0) return NULL;
    return s_stack[s_top - 1].page;
}
```

---

### 4.3 改造：ui_splash.c → page_splash.c

**改造前**（你现在的ui_splash.c）：
```c
// 对外暴露 ui_splash_show(cb)
void ui_splash_show(ui_splash_enter_cb_t cb) {
    s_enter_cb = cb;
    // ... 创建canvas、图片、提示文字 ...
}
```

**改造后**（封装成Page_t）：
```c
// main/ui/page_splash.c
#include "page_manager.h"
#include "ui_theme.h"
#include "bsp/esp-bsp.h"
#include "middleware/fs_service.h"

static ui_splash_enter_cb_t s_enter_cb;
static lv_obj_t* s_canvas;
static lv_obj_t* s_hint;
static int32_t s_press_start_y;
static bool s_entered;

// ... 原来的 splash_decode_into()、手势处理函数全部保留 ...

static lv_obj_t* splash_create(lv_obj_t* parent) {
    s_entered = false;

    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(page, COL_BG, 0);

    // ... 原来的 ui_splash_show() 里的创建逻辑搬过来 ...

    return page;
}

static void splash_destroy(lv_obj_t* page) {
    lv_obj_del(page);
    s_canvas = NULL;
    s_hint = NULL;
}

// 导出 Page_t 实例
const Page_t Page_Splash = {
    .create = splash_create,
    .destroy = splash_destroy,
    .name = "Splash",
};

// 保留原来的进入回调设置函数
void page_splash_set_enter_cb(ui_splash_enter_cb_t cb) {
    s_enter_cb = cb;
}
```

---

### 4.4 改造：ui_disp.c → page_main.c

**改造前**（你现在的ui_disp.c核心函数）：
```c
void app_disp_lvgl_show_main(void) {
    bsp_display_lock(0);
    lv_obj_clean(lv_scr_act());
    tabview = lv_tabview_create(lv_scr_act());
    // ... 三个tab、下拉壁纸、事件绑定 ...
    bsp_display_unlock();
}
```

**改造后**：
```c
// main/ui/page_main.c
#include "page_manager.h"
#include "ui_theme.h"
#include "bsp/esp-bsp.h"

// 原来的 s_wallpaper、s_pull_start_y 等全部保留static
static lv_obj_t *s_tabview;
static lv_obj_t *s_tab_btns;
static lv_group_t *s_filesystem_group;
static lv_group_t *s_recording_group;
static lv_group_t *s_settings_group;
static lv_obj_t *s_wallpaper;

// ... 原来的下拉手势函数、set_tab_group() 全部保留 ...

// 三个tab的内容函数从ui_call.c/ui_record.c/ui_settings.c引进来
extern void tab_call_build(lv_obj_t *tab, lv_group_t *group);
extern void tab_record_build(lv_obj_t *tab, lv_group_t *group);
extern void tab_settings_build(lv_obj_t *tab, lv_group_t *group);

static lv_obj_t* main_create(lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    // ... 原来的 app_disp_lvgl_show_main() 里的 tabview 创建逻辑搬过来 ...
    // 注意：原来用的是 lv_scr_act()，现在改成在 page 上创建

    s_tabview = lv_tabview_create(page);
    // ... 三个tab ...
    tab_call_build(tab_call, s_filesystem_group);
    tab_record_build(tab_rec, s_recording_group);
    tab_settings_build(tab_sys, s_settings_group);

    return page;
}

static void main_destroy(lv_obj_t* page) {
    // 删除group
    lv_group_del(s_filesystem_group);
    lv_group_del(s_recording_group);
    lv_group_del(s_settings_group);

    lv_obj_del(page);

    // 清空指针
    s_tabview = NULL;
    s_tab_btns = NULL;
}

const Page_t Page_Main = {
    .create = main_create,
    .destroy = main_destroy,
    .name = "Main",
};
```

---

### 4.5 改造：三个Tab页（改动最小）

**改造原则**：Tab页是主界面的子组件，不单独走Page_t，只是把函数名和变量私有化

#### ui_call.c 改造
```c
// 改造前：
void app_disp_lvgl_show_call(lv_obj_t *screen, lv_group_t *group) { ... }

// 改造后：函数名改成 tab_call_build，变量改static
static lv_obj_t *s_status_dot;
static lv_obj_t *s_status_label;

void tab_call_build(lv_obj_t *screen, lv_group_t *group) {
    // ... 原来的逻辑几乎不变 ...
    // 原来用 ui_call_get_screen() 的地方直接用 static 变量
}
```

#### ui_record.c 改造（重点：去掉全局控件）
```c
// 改造前：rec_btn、play1_btn、rec_stop_btn 都是 ui_state.h 里的全局变量
rec_btn = lv_btn_create(card);
play1_btn = lv_btn_create(card);
rec_stop_btn = lv_btn_create(card);

// 改造后：全部改成 static
static lv_obj_t *s_rec_btn;
static lv_obj_t *s_play_btn;
static lv_obj_t *s_stop_btn;

void tab_record_build(lv_obj_t *screen, lv_group_t *group) {
    s_rec_btn = lv_btn_create(card);
    s_play_btn = lv_btn_create(card);
    s_stop_btn = lv_btn_create(card);
}

// 原来在 ui_windows.c 里操作 rec_btn 的地方怎么办？
// 改成通过 audio_service 事件回调更新，或者提供对外接口
void tab_record_set_buttons_enabled(bool en) {
    if (en) {
        lv_obj_clear_state(s_rec_btn, LV_STATE_DISABLED);
        lv_obj_clear_state(s_play_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_rec_btn, LV_STATE_DISABLED);
        lv_obj_add_state(s_play_btn, LV_STATE_DISABLED);
    }
}
```

---

### 4.6 改造：ui_windows.c → page_wav_player.c

**改造前**：`show_window_wav()` 动态创建窗口，关闭时 `lv_obj_del(win)`

**改造后**：封装成完整Page_t
```c
// main/ui/page_wav_player.c
#include "page_manager.h"
#include "ui_theme.h"
#include "middleware/audio_service.h"

static lv_obj_t *s_play_btn;
static lv_obj_t *s_stop_btn;
static lv_obj_t *s_repeat_btn;
static char s_file_path[256];

// ... 原来的 play_event_cb / stop_event_cb 等事件函数全部保留 ...

static lv_obj_t* wav_player_create(lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(page, COL_BG, 0);

    // ... 原来的 show_window_wav() 里的创建逻辑搬过来 ...
    // 原来的 lv_win_create(lv_scr_act()) 改成在 page 上创建

    return page;
}

static void wav_player_destroy(lv_obj_t* page) {
    // 先停音频
    audio_cmd_t cmd = {.id = AUDIO_CMD_STOP};
    audio_service_post_cmd(&cmd);

    lv_obj_del(page);

    // 清空状态
    s_play_btn = NULL;
    s_stop_btn = NULL;
    s_file_path[0] = '\0';
}

// 对外提供打开函数
void Page_WavPlayer_Open(const char* path) {
    strncpy(s_file_path, path, sizeof(s_file_path) - 1);
    PageManager_Load(&Page_WavPlayer);
}

const Page_t Page_WavPlayer = {
    .create = wav_player_create,
    .destroy = wav_player_destroy,
    .name = "WavPlayer",
};
```

---

### 4.7 新增：page_settings.c —— 设置列表页

```c
// main/ui/page_settings.c
#include "page_manager.h"
#include "ui_theme.h"

// 页面私有控件
static lv_obj_t *s_row_wifi;
static lv_obj_t *s_row_bt;
static lv_obj_t *s_row_rgb;
static lv_obj_t *s_row_system;

// 四个资源页面的前置声明
extern const Page_t Page_Wifi;
extern const Page_t Page_Bluetooth;
extern const Page_t Page_Rgb;
extern const Page_t Page_System;

// 通用：创建一个设置行
static lv_obj_t* create_setting_row(lv_obj_t* parent,
                                    const char* icon, const char* text,
                                    uint32_t user_data) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, BSP_LCD_H_RES - 40, 56);
    lv_obj_set_style_bg_color(row, COL_CARD, 0);
    lv_obj_set_style_border_color(row, COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* left = lv_label_create(row);
    lv_label_set_text_fmt(left, "%s  %s", icon, text);
    lv_obj_set_style_text_color(left, COL_TEXT, 0);

    lv_obj_t* right = lv_label_create(row);
    lv_label_set_text_static(right, ">");
    lv_obj_set_style_text_color(right, COL_SUBTEXT, 0);

    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED,
                        (void*)(intptr_t)user_data);
    return row;
}

// 行点击事件
static void row_click_cb(lv_event_t* e) {
    uint32_t row_id = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    switch(row_id) {
        case 0: PageManager_Load(&Page_Wifi);      break;
        case 1: PageManager_Load(&Page_Bluetooth); break;
        case 2: PageManager_Load(&Page_Rgb);       break;
        case 3: PageManager_Load(&Page_System);    break;
    }
}

static lv_obj_t* settings_create(lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(page, COL_BG, 0);

    // 标题栏（带返回箭头）
    // ... 创建返回按钮，绑定 PageManager_Back() ...

    // 四行设置
    s_row_wifi    = create_setting_row(page, LV_SYMBOL_WIFI,  "WIFI",      0);
    s_row_bt      = create_setting_row(page, LV_SYMBOL_BLUETOOTH, "Bluetooth", 1);
    s_row_rgb     = create_setting_row(page, LV_SYMBOL_IMAGE, "RGB",       2);
    s_row_system  = create_setting_row(page, LV_SYMBOL_SETTINGS, "System",  3);

    return page;
}

static void settings_destroy(lv_obj_t* page) {
    lv_obj_del(page);
}

const Page_t Page_Settings = {
    .create = settings_create,
    .destroy = settings_destroy,
    .name = "Settings",
};
```

---

### 4.8 改造：main.c 启动流程

**改造前**：
```c
void app_main(void) {
    bsp_spiffs_mount();
    bsp_i2c_init();
    bsp_display_start();
    fs_service_init();
    audio_service_init();

    bsp_display_lock(0);
    ui_splash_show(on_splash_enter);
    bsp_display_unlock();
}

static void on_splash_enter(void) {
    app_disp_lvgl_show_main();
}
```

**改造后**：
```c
#include "page_manager.h"
#include "page_splash.h"
#include "page_main.h"

static void on_splash_enter(void) {
    PageManager_Load(&Page_Main);
}

void app_main(void) {
    // 硬件初始化（不变）
    bsp_spiffs_mount();
    bsp_i2c_init();
    bsp_display_start();
    fs_service_init();
    audio_service_init();

    // 页面管理器初始化
    PageManager_Init();

    // 设置Splash完成回调
    page_splash_set_enter_cb(on_splash_enter);

    // 加载启动页（第一个页面，作为栈底）
    PageManager_Load(&Page_Splash);
}
```

---

## 五、Overlay 浮层管理设计

### 5.1 为什么需要 Overlay Manager
壁纸和音量弹窗都是浮在 `lv_layer_top()` 上的临时层，两个叠在一起会有触摸冲突：
- 谁在最上面？谁先收到触摸事件？
- 能不能同时打开两个浮层？
- 主界面的下拉手势会不会和音量弹窗的滑块拖动打架？

**解决方案：统一浮层管理器，互斥打开 + 层级控制**

### 5.2 核心设计规则

| 规则 | 说明 |
|------|------|
| **互斥打开** | 同一时间最多只有一个浮层是激活状态，打开新的自动关掉旧的 |
| **后开的在上** | LVGL 默认 z-order，后创建的浮层自动放在最上面 |
| **遮罩只处理点击** | 浮层背景遮罩只监听点击关闭事件，不拦截滑动手势 |
| **手势加锁** | 有浮层打开时，下层页面的手势暂时失效 |

### 5.3 overlay_manager.c/h —— 浮层管理器

```c
// main/ui/overlay_manager.h
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_WALLPAPER,    // 壁纸层
    OVERLAY_VOLUME,       // 音量弹窗
} overlay_type_t;

// 打开浮层（自动关闭旧的）
void overlay_open(overlay_type_t type);

// 关闭指定浮层
void overlay_close(overlay_type_t type);

// 查询当前有没有浮层打开
bool overlay_any_active(void);

// 获取当前激活的浮层类型
overlay_type_t overlay_get_active(void);

#ifdef __cplusplus
}
#endif
```

```c
// main/ui/overlay_manager.c
#include "overlay_manager.h"
#include "overlay_wallpaper.h"
#include "overlay_volume_popup.h"
#include "esp_log.h"

static const char* TAG = "OVERLAY";
static overlay_type_t s_active = OVERLAY_NONE;

void overlay_open(overlay_type_t type) {
    if (s_active == type) return;  // 重复打开忽略

    // 先关旧浮层
    if (s_active != OVERLAY_NONE) {
        overlay_close(s_active);
    }

    s_active = type;

    switch(type) {
        case OVERLAY_WALLPAPER:
            overlay_wallpaper_show();
            break;
        case OVERLAY_VOLUME:
            overlay_volume_show();
            break;
        default:
            break;
    }

    ESP_LOGI(TAG, "overlay opened: %d", type);
}

void overlay_close(overlay_type_t type) {
    if (s_active != type) return;

    switch(type) {
        case OVERLAY_WALLPAPER:
            overlay_wallpaper_hide();
            break;
        case OVERLAY_VOLUME:
            overlay_volume_hide();
            break;
        default:
            break;
    }

    s_active = OVERLAY_NONE;
    ESP_LOGI(TAG, "overlay closed: %d", type);
}

bool overlay_any_active(void) {
    return s_active != OVERLAY_NONE;
}

overlay_type_t overlay_get_active(void) {
    return s_active;
}
```

### 5.4 音量弹窗触发：任意页面最右端左滑

**触发方式：**
- 从屏幕**最右端边缘**（约20px宽度区域）向左滑动
- **任意页面都能触发**（主界面、设置页、WIFI页、蓝牙页...）
- 不需要特定页面，全局手势

**实现要点：**
1. 在 `lv_layer_top()` 上放一个全屏幕的透明手势捕获层
2. 只监听从右边缘开始的滑动
3. 左滑距离超过阈值 → 弹出音量条
4. 音量条从右侧滑入，占屏幕宽度约 1/3

**手势冲突处理：**
- 音量弹窗打开时，手势捕获层让滑块正常拖动
- 点击左侧半透明遮罩 → 关闭音量弹窗
- 关闭后手势捕获层恢复监听

### 5.5 主界面下拉壁纸手势加锁

原来的下拉壁纸手势，加一层判断：
```c
static void main_pressing_cb(lv_event_t *e) {
    // 如果音量弹窗开着，不处理壁纸手势
    if (overlay_get_active() == OVERLAY_VOLUME) {
        return;
    }
    // ... 原来的壁纸下拉手势逻辑 ...
}
```

---

## 六、改造前后对比

| 维度 | 改造前 | 改造后 |
|------|--------|--------|
| 颜色定义 | 每个文件重复4-5次 | 统一在 ui_theme.h |
| 控件变量 | 6个全局extern在ui_state.h | 每个页面static，完全私有化 |
| 页面创建 | app_disp_lvgl_show_xxx()，无销毁 | create/destroy成对，生命周期完整 |
| 二级页面 | 动态创建窗口，手动lv_obj_del | PageManager栈自动管理 |
| 启动流程 | 硬编码调用show_main() | 统一走PageManager_Load |
| 新增页面 | 改一堆地方 | 实现create/destroy两个函数就行 |
| 内存管理 | Tab常驻，弹窗手动删 | 页面销毁自动释放，不会漏 |

---

## 七、目录结构（改造后，全部在 main/ui/ 下）

```
main/ui/                     ← UI层根目录，所有UI相关代码都在这
├── page_manager.c/h          # 页面栈管理框架
├── overlay_manager.c/h       # Overlay浮层管理器（互斥打开）
├── ui_theme.h                # 统一颜色/字体定义
├── ui_common.c/h             # 通用控件工厂
├── page_splash.c/h           # 启动页
├── page_main.c/h             # 主界面 TabView
│   ├── tab_call.c            # Call Tab（语音通话）
│   ├── tab_record.c          # Record Tab（录音）
│   └── tab_sys_entry.c       # Sys Tab 入口
├── overlay_wallpaper.c/h      # 下拉壁纸层（Overlay）
├── overlay_volume_popup.c/h  # 音量弹窗（Overlay，右滑左出）
├── page_wav_player.c/h       # WAV 播放页
├── page_settings.c/h         # 设置列表页
├── page_wifi.c/h             # WIFI 设置页（新增）
├── page_bluetooth.c/h        # Bluetooth 设置页（新增）
├── page_rgb.c/h              # RGB 灯效页（新增）
└── page_system.c/h           # System 系统页（新增）
```

> 说明：
> - `page_manager` 管理页面栈（二级页面导航）
> - `overlay_manager` 管理浮层（壁纸、音量弹窗），互斥打开，避免触摸冲突
> - `overlay_xxx` 开头的都是浮层组件，不走 PageManager 栈，直接操作 `lv_layer_top()`
> - 壁纸层和音量弹窗是同级的浮层组件，由 overlay_manager 统一调度

---

## 八、关键注意事项

1. **LVGL 线程安全**：PageManager 内部已经包了 `bsp_display_lock/unlock`，页面自己的定时器回调里操作LVGL也要加锁
2. **destroy 必须对称**：create 里创建了多少定时器、事件回调，destroy 里就要全清理
3. **Tab页不单独走 Page_t**：三个Tab是 Page_Main 的子组件，跟着主界面一起创建销毁
4. **音量弹窗是 Overlay**：不走 PageManager 栈，直接操作 `lv_layer_top()`
5. **全局变量逐步清理**：Phase 2 完成后，ui_state.h 里的 extern 可以全部删掉，改成各文件内部 static
#（注：内容由AI生成）
