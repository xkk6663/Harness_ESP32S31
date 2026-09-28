#include "hand_detect.hpp"
#include "esp_log.h"
#include <filesystem>

/* 本工程只走 SDCARD（littlefs）模式；FLASH_RODATA/PARTITION 分支已裁掉，
 * 模型文件在 /littlefs/models/（littlefs_content/models/ 烧录到 storage 分区（S5 v1.2 起偏移 0x720000））。 */

namespace hand_detect {
ESPDet::ESPDet(const char *model_name, float score_thr, float nms_thr)
{
    auto sd_path = std::filesystem::path(CONFIG_BSP_SD_MOUNT_POINT) / CONFIG_HAND_DETECT_MODEL_SDCARD_DIR / model_name;
    m_model = new dl::Model(sd_path.c_str(), fbs::MODEL_LOCATION_IN_SDCARD);
    m_model->minimize();
    m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255});
    m_image_preprocessor->enable_letterbox({114, 114, 114});
    m_postprocessor = new dl::detect::ESPDetPostProcessor(
        m_model, m_image_preprocessor, score_thr, nms_thr, 10, {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}});
}

} // namespace hand_detect

HandDetect::HandDetect(model_type_t model_type, bool lazy_load) : m_model_type(model_type)
{
    switch (model_type) {
    case model_type_t::ESPDET_PICO_224_224_HAND:
        m_score_thr[0] = hand_detect::ESPDet::default_score_thr;
        m_nms_thr[0] = hand_detect::ESPDet::default_nms_thr;
        break;
    }
    if (lazy_load) {
        m_model = nullptr;
    } else {
        load_model();
    }
}

void HandDetect::load_model()
{
    switch (m_model_type) {
    case model_type_t::ESPDET_PICO_224_224_HAND:
        m_model = new hand_detect::ESPDet("espdet_pico_224_224_hand.espdl", m_score_thr[0], m_nms_thr[0]);
        break;
    }
}
