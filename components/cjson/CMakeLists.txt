# cJSON 本地组件（vendored）
#
# 背景：ESP-IDF v6.1 已不再内置 json/cJSON 组件，而本机访问乐鑫组件注册表
#       返回 403（见 docs/工程规范/11_遇到的问题.md F12），无法经组件管理器获取。
#       因此按 esp_littlefs 的同样做法，把 cJSON 源码直接放进工程本地 components/，
#       由 IDF 构建系统自动识别（本地组件优先，无需联网求解依赖）。
#
# 版本：cJSON v1.7.18（https://github.com/DaveGamble/cJSON，MIT License）

idf_component_register(SRCS "cJSON.c"
                       INCLUDE_DIRS "include")
