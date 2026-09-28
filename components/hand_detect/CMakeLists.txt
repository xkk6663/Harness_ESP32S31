# 本地 vendor 的模型组件（源: espressif/esp-dl master models/hand_detect）。
# 本工程固定 SDCARD 模式（模型在 /littlefs/models，见 littlefs_content/models/），
# 上游 CMakeLists 的"打包 .espdl 进固件"整块在本模式下本来就不生效 ⇒ 裁掉。
idf_component_register(SRC_DIRS "." INCLUDE_DIRS "." REQUIRES esp-dl)
