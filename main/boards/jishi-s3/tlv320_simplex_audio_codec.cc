#include "tlv320_simplex_audio_codec.h"

#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr uint8_t kRegWake = 0x02;
constexpr uint8_t kRegAsiStatus = 0x15;
constexpr uint8_t kRegDeviceStatus = 0x76;
constexpr TickType_t kReadTimeoutTicks = pdMS_TO_TICKS(200);
constexpr float kMonoRouteSwitchRatio = 1.35f;
constexpr float kMonoRouteSwitchMargin = 8.0f;
constexpr float kMonoRouteMinRms = 24.0f;
}

static const char TAG[] = "Tlv320AudioCodec";

Tlv320SimplexAudioCodec::Tlv320SimplexAudioCodec(i2c_master_bus_handle_t i2c_bus)
    : I2cDevice(i2c_bus, AUDIO_CODEC_TLV320_ADDR) {
    duplex_ = false;
    input_channels_ = 1;
    output_channels_ = 1;
    input_sample_rate_ = AUDIO_INPUT_SAMPLE_RATE;
    output_sample_rate_ = AUDIO_OUTPUT_SAMPLE_RATE;

    CreateVoiceHardware();
    InitializeCodec();
    ESP_LOGI(TAG, "TLV320 simplex codec initialized");
}

Tlv320SimplexAudioCodec::~Tlv320SimplexAudioCodec() {
    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(rx_handle_));
    }
    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(tx_handle_));
    }
}

void Tlv320SimplexAudioCodec::Start() {
    AudioCodec::Start();
    output_volume_cached_ = std::clamp(output_volume_, 0, 100);
    ESP_LOGI(TAG, "Apply persisted output volume: %d", output_volume_cached_);
}

void Tlv320SimplexAudioCodec::CreateVoiceHardware() {
    i2s_chan_config_t tx_chan_config = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)0, I2S_ROLE_MASTER);
    tx_chan_config.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
    tx_chan_config.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
    tx_chan_config.auto_clear = true;

    i2s_chan_config_t rx_chan_config = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)1, I2S_ROLE_MASTER);
    rx_chan_config.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
    rx_chan_config.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
    rx_chan_config.auto_clear = true;

    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_config, &tx_handle_, nullptr));
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_config, nullptr, &rx_handle_));

    i2s_std_config_t tx_config = {
        .clk_cfg = {
            .sample_rate_hz = static_cast<uint32_t>(output_sample_rate_),
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_SPK_GPIO_BCLK,
            .ws = AUDIO_I2S_SPK_GPIO_LRCK,
            .dout = AUDIO_I2S_SPK_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    tx_config.slot_cfg.slot_mask = AUDIO_I2S_SPK_SLOT_MASK;

    i2s_std_config_t rx_config = {
        .clk_cfg = {
            .sample_rate_hz = static_cast<uint32_t>(input_sample_rate_),
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_MIC_GPIO_SCK,
            .ws = AUDIO_I2S_MIC_GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_I2S_MIC_GPIO_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    rx_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    rx_config.slot_cfg.ws_width = 32;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_config));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &rx_config));
    ESP_LOGI(TAG, "Voice hardware created: spk=16bit mono-left, mic=16bit stereo slot32");
}

void Tlv320SimplexAudioCodec::InitializeCodec() {
    ESP_LOGI(TAG, "Initialize TLV320ADC5140");

    WriteReg(kRegWake, 0x81);
    WriteReg(0x46, 0x40);  // CH3_CFG0 -> PDM
    WriteReg(0x4B, 0x40);  // CH4_CFG0 -> PDM
    WriteReg(0x22, 0x41);  // GPO1 -> PDMCLK
    WriteReg(0x2B, 0x05);  // GPI2 -> PDMDIN2 for CH3/CH4
    WriteReg(0x07, 0x40);  // ASI 16-bit I2S
    WriteReg(0x0D, 0x00);  // CH3 -> Left
    WriteReg(0x0E, 0x20);  // CH4 -> Right
    WriteReg(0x48, 0xFF);  // CH3 gain
    WriteReg(0x4D, 0xFF);  // CH4 gain
    WriteReg(0x73, 0x30);  // Power up CH3/CH4
    WriteReg(0x74, 0x30);  // Enable ASI CH3/CH4
    WriteReg(0x75, 0x60);  // Start ADC + PLL

    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "TLV320 ASI_STS=0x%02X DEV_STS0=0x%02X", ReadReg(kRegAsiStatus), ReadReg(kRegDeviceStatus));
}

void Tlv320SimplexAudioCodec::SetOutputVolume(int volume) {
    output_volume_cached_ = std::clamp(volume, 0, 100);
    AudioCodec::SetOutputVolume(output_volume_cached_);
}

void Tlv320SimplexAudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        input_warmup_reads_left_ = kInputWarmupReads;
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
        input_warmup_reads_left_ = 0;
    }
    AudioCodec::EnableInput(enable);
}

void Tlv320SimplexAudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
    AudioCodec::EnableOutput(enable);
}

void Tlv320SimplexAudioCodec::LogInputLevel(int samples,
                                            int32_t left_peak,
                                            float left_rms,
                                            int32_t right_peak,
                                            float right_rms,
                                            bool using_right) {
    if (samples <= 0) {
        return;
    }
    ESP_LOGI(TAG,
             "MIC self-check: mono_samples=%d route=%s peakL=%ld rmsL=%.1f peakR=%ld rmsR=%.1f",
             samples,
             using_right ? "right" : "left",
             static_cast<long>(left_peak),
             left_rms,
             static_cast<long>(right_peak),
             right_rms);
}

int Tlv320SimplexAudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || dest == nullptr || samples <= 0) {
        return 0;
    }

    std::vector<int16_t> stereo_buffer(samples * 2);
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(
        rx_handle_,
        stereo_buffer.data(),
        stereo_buffer.size() * sizeof(int16_t),
        &bytes_read,
        kReadTimeoutTicks);
    if (err != ESP_OK || bytes_read == 0) {
        return 0;
    }

    const int stereo_samples = static_cast<int>(bytes_read / sizeof(int16_t));
    const int frames = stereo_samples / 2;
    const int mono_samples = std::min(samples, frames);

    int32_t left_peak = 0;
    int32_t right_peak = 0;
    int32_t slot_diff_peak = 0;
    int slot_equal_count = 0;
    double left_sum_sq = 0.0;
    double right_sum_sq = 0.0;
    for (int i = 0; i < mono_samples; ++i) {
        const int32_t left = stereo_buffer[i * 2];
        const int32_t right = stereo_buffer[i * 2 + 1];
        const int32_t diff = left - right;
        const int32_t diff_abs = std::abs(diff);
        left_peak = std::max<int32_t>(left_peak, std::abs(left));
        right_peak = std::max<int32_t>(right_peak, std::abs(right));
        slot_diff_peak = std::max<int32_t>(slot_diff_peak, diff_abs);
        if (diff == 0) {
            ++slot_equal_count;
        }
        left_sum_sq += static_cast<double>(left) * static_cast<double>(left);
        right_sum_sq += static_cast<double>(right) * static_cast<double>(right);
    }
    const float left_rms = mono_samples > 0 ? std::sqrt(left_sum_sq / static_cast<double>(mono_samples)) : 0.0f;
    const float right_rms = mono_samples > 0 ? std::sqrt(right_sum_sq / static_cast<double>(mono_samples)) : 0.0f;

    const bool right_clearly_stronger =
        right_rms >= kMonoRouteMinRms &&
        right_rms > (left_rms * kMonoRouteSwitchRatio + kMonoRouteSwitchMargin);
    const bool left_clearly_stronger =
        left_rms >= kMonoRouteMinRms &&
        left_rms > (right_rms * kMonoRouteSwitchRatio + kMonoRouteSwitchMargin);

    bool desired_route_from_right = route_mono_from_right_;
    if (right_clearly_stronger) {
        desired_route_from_right = true;
    } else if (left_clearly_stronger) {
        desired_route_from_right = false;
    }

    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    if (desired_route_from_right != route_mono_from_right_) {
        route_mono_from_right_ = desired_route_from_right;
        if (last_route_log_ms_ == 0 || now_ms - last_route_log_ms_ >= 10000) {
            last_route_log_ms_ = now_ms;
            ESP_LOGI(TAG,
                     "Mono route switched to %s: peakL=%ld rmsL=%.1f peakR=%ld rmsR=%.1f",
                     route_mono_from_right_ ? "right" : "left",
                     static_cast<long>(left_peak),
                     left_rms,
                     static_cast<long>(right_peak),
                     right_rms);
        }
    }

    for (int i = 0; i < mono_samples; ++i) {
        dest[i] = stereo_buffer[i * 2 + (route_mono_from_right_ ? 1 : 0)];
    }

    if (input_warmup_reads_left_ > 0) {
        std::memset(dest, 0, mono_samples * sizeof(int16_t));
        --input_warmup_reads_left_;
    }

    ++read_counter_;
    if (read_counter_ <= 3 || now_ms - last_read_log_ms_ >= 30000) {
        last_read_log_ms_ = now_ms;
        if (read_counter_ <= 3) {
            ESP_LOGI(TAG,
                     "Stereo slot mapping: slot0=%s slot1=%s mono_route=%s equal=%d/%d diff_peak=%ld s0=%d,%d,%d,%d s1=%d,%d,%d,%d",
                     "left",
                     "right",
                     route_mono_from_right_ ? "right" : "left",
                     slot_equal_count,
                     mono_samples,
                     static_cast<long>(slot_diff_peak),
                     mono_samples > 0 ? stereo_buffer[0] : 0,
                     mono_samples > 1 ? stereo_buffer[2] : 0,
                     mono_samples > 2 ? stereo_buffer[4] : 0,
                     mono_samples > 3 ? stereo_buffer[6] : 0,
                     mono_samples > 0 ? stereo_buffer[1] : 0,
                     mono_samples > 1 ? stereo_buffer[3] : 0,
                     mono_samples > 2 ? stereo_buffer[5] : 0,
                     mono_samples > 3 ? stereo_buffer[7] : 0);
        }
        LogInputLevel(mono_samples, left_peak, left_rms, right_peak, right_rms, route_mono_from_right_);
    }

    return mono_samples;
}

int Tlv320SimplexAudioCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_ || data == nullptr || samples <= 0) {
        return 0;
    }

    std::vector<int16_t> output_data(samples);
    for (int i = 0; i < samples; ++i) {
        output_data[i] = static_cast<int16_t>(static_cast<float>(data[i]) * (static_cast<float>(output_volume_cached_) / 100.0f));
    }

    size_t bytes_written = 0;
    ESP_ERROR_CHECK(i2s_channel_write(
        tx_handle_,
        output_data.data(),
        output_data.size() * sizeof(int16_t),
        &bytes_written,
        portMAX_DELAY));
    return static_cast<int>(bytes_written / sizeof(int16_t));
}





