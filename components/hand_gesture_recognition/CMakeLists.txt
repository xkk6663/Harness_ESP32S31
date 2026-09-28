# 本地 vendor 的模型组件（源: espressif/esp-dl master models/hand_gesture_recognition）。
# 本工程固定 SDCARD 模式，上游的 .espdl 打包块在本模式下不生效 ⇒ 裁掉。
idf_component_register(SRC_DIRS "." INCLUDE_DIRS "." REQUIRES esp-dl)
