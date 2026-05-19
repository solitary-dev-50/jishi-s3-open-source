#ifndef _TLV320_SIMPLEX_AUDIO_CODEC_H_
#define _TLV320_SIMPLEX_AUDIO_CODEC_H_

#include "audio_codec.h"
#include "i2c_device.h"

class Tlv320SimplexAudioCodec : public AudioCodec, private I2cDevice {
public:
    Tlv320SimplexAudioCodec(i2c_master_bus_handle_t i2c_bus);
    virtual ~Tlv320SimplexAudioCodec();

    virtual void Start() override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
    virtual void SetOutputVolume(int volume) override;

private:
    static constexpr uint8_t kInputWarmupReads = 6;
    int output_volume_cached_ = 100;    uint32_t read_counter_ = 0;
    uint32_t last_read_log_ms_ = 0;
    uint32_t last_route_log_ms_ = 0;
    uint8_t input_warmup_reads_left_ = 0;
    bool route_mono_from_right_ = false;

    void InitializeCodec();
    void CreateVoiceHardware();
    void LogInputLevel(int samples,
                       int32_t left_peak,
                       float left_rms,
                       int32_t right_peak,
                       float right_rms,
                       bool using_right);

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;
};

#endif // _TLV320_SIMPLEX_AUDIO_CODEC_H_


