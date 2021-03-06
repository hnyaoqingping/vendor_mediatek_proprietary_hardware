#include "AudioALSAPlaybackHandlerFast.h"

#include "AudioALSAHardwareResourceManager.h"
//#include "AudioALSAVolumeController.h"
//#include "AudioVolumeInterface.h"
#include "AudioVolumeFactory.h"
#include "AudioALSASampleRateController.h"

#include "AudioMTKFilter.h"
#include "AudioVUnlockDL.h"
#include "AudioALSADeviceParser.h"
#include "AudioALSADriverUtility.h"
#include "AudioSmartPaController.h"


#ifdef MTK_AURISYS_FRAMEWORK_SUPPORT
#include <audio_ringbuf.h>
#include <audio_pool_buf_handler.h>

#include <aurisys_controller.h>
#include <aurisys_lib_manager.h>
#endif

#include "AudioSmartPaController.h"

#ifdef MTK_LATENCY_DETECT_PULSE
#include "AudioDetectPulse.h"
#endif

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "AudioALSAPlaybackHandlerFast"

// Latency Detect
//#define DEBUG_LATENCY
#define THRESHOLD_FRAMEWORK   0.002
#define THRESHOLD_HAL         0.002
#define THRESHOLD_KERNEL      0.002

#define calc_time_diff(x,y) ((x.tv_sec - y.tv_sec )+ (double)( x.tv_nsec - y.tv_nsec ) / (double)1000000000)
static   const char PROPERTY_KEY_EXTDAC[PROPERTY_KEY_MAX]  = "af.resouce.extdac_support";

namespace android {

AudioALSAPlaybackHandlerFast::AudioALSAPlaybackHandlerFast(const stream_attribute_t *stream_attribute_source) :
    AudioALSAPlaybackHandlerBase(stream_attribute_source) {
    ALOGD("%s()", __FUNCTION__);
    mPlaybackHandlerType = PLAYBACK_HANDLER_FAST;

    memset(&mNewtime, 0, sizeof(mNewtime));
    memset(&mOldtime, 0, sizeof(mOldtime));
}


AudioALSAPlaybackHandlerFast::~AudioALSAPlaybackHandlerFast() {
}

uint32_t AudioALSAPlaybackHandlerFast::GetLowJitterModeSampleRate() {
    return 48000;
}

bool AudioALSAPlaybackHandlerFast::SetLowJitterMode(bool bEnable, uint32_t SampleRate) {

    enum mixer_ctl_type type;
    struct mixer_ctl *ctl;
    int retval = 0;

    // check need open low jitter mode
    if (SampleRate <= GetLowJitterModeSampleRate() && (AudioALSADriverUtility::getInstance()->GetPropertyValue(PROPERTY_KEY_EXTDAC)) == false) {
        ALOGD("%s(), bypass low jitter mode, bEnable = %d, SampleRate = %u", __FUNCTION__, bEnable, SampleRate);
        return false;
    }

    ctl = mixer_get_ctl_by_name(mMixer, "fast_dl_hd_Switch");

    if (ctl == NULL) {
        ALOGE("fast_dl_hd_Switch not support");
        return false;
    } else {
        ALOGD("%s() bEanble = %d SampleRate = %u, fast_dl_hd_Switch",
              __FUNCTION__, bEnable, SampleRate);
    }

    if (bEnable == true) {
        retval = mixer_ctl_set_enum_by_string(ctl, "On");
        ASSERT(retval == 0);
    } else {
        retval = mixer_ctl_set_enum_by_string(ctl, "Off");
        ASSERT(retval == 0);
    }
    return true;
}

bool AudioALSAPlaybackHandlerFast::DeviceSupportHifi(audio_devices_t outputdevice) {
    // modify this to let output device support hifi audio
    if (outputdevice == AUDIO_DEVICE_OUT_WIRED_HEADSET || outputdevice == AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
        return true;
    } else if (outputdevice & AUDIO_DEVICE_OUT_SPEAKER) {
        if (AudioSmartPaController::getInstance()->isSmartPAUsed()) {
            return AudioSmartPaController::getInstance()->getMaxSupportedRate() > 48000;
        } else {
            return true;
        }
    }
    return false;
}


uint32_t AudioALSAPlaybackHandlerFast::ChooseTargetSampleRate(uint32_t SampleRate, audio_devices_t outputdevice) {
    ALOGV("ChooseTargetSampleRate SampleRate = %d outputdevice = %d", SampleRate, outputdevice);
    uint32_t TargetSampleRate = 44100;
    if (SampleRate <=  192000 && SampleRate > 96000 && DeviceSupportHifi(outputdevice)) {
        TargetSampleRate = 192000;
    } else if (SampleRate <= 96000 && SampleRate > 48000 && DeviceSupportHifi(outputdevice)) {
        TargetSampleRate = 96000;
    } else if (SampleRate <= 48000 && SampleRate >= 32000) {
        TargetSampleRate = SampleRate;
    }
    return TargetSampleRate;
}

status_t AudioALSAPlaybackHandlerFast::open() {
    ALOGD("+%s(), mDevice = 0x%x", __FUNCTION__, mStreamAttributeSource->output_devices);

    AL_LOCK_MS(AudioALSADriverUtility::getInstance()->getStreamSramDramLock(), 3000);

    // debug pcm dump
    OpenPCMDump(LOG_TAG);
    // acquire pmic clk
    mHardwareResourceManager->EnableAudBufClk(true);
    int pcmindex = AudioALSADeviceParser::getInstance()->GetPcmIndexByString(keypcmDl2Meida);
    int cardindex = AudioALSADeviceParser::getInstance()->GetCardIndexByString(keypcmDl2Meida);

    //ListPcmDriver(cardindex, pcmindex);

    struct pcm_params *params;
    params = pcm_params_get(cardindex, pcmindex,  PCM_OUT);
    if (params == NULL) {
        ALOGD("Device does not exist.\n");
    }

    mStreamAttributeTarget.buffer_size = pcm_params_get_max(params, PCM_PARAM_BUFFER_BYTES);
    pcm_params_free(params);

    // HW attribute config // TODO(Harvey): query this
#ifdef PLAYBACK_USE_24BITS_ONLY
    mStreamAttributeTarget.audio_format = AUDIO_FORMAT_PCM_8_24_BIT;
#else
    mStreamAttributeTarget.audio_format = (mStreamAttributeSource->audio_format == AUDIO_FORMAT_PCM_32_BIT) ? AUDIO_FORMAT_PCM_8_24_BIT : AUDIO_FORMAT_PCM_16_BIT;
#endif
    mStreamAttributeTarget.audio_channel_mask = AUDIO_CHANNEL_IN_STEREO;
    mStreamAttributeTarget.num_channels = popcount(mStreamAttributeTarget.audio_channel_mask);

    mStreamAttributeTarget.sample_rate = ChooseTargetSampleRate(AudioALSASampleRateController::getInstance()->getPrimaryStreamOutSampleRate(),
                                                                mStreamAttributeSource->output_devices);

    // HW pcm config
    memset(&mConfig, 0, sizeof(mConfig));
    mConfig.channels = mStreamAttributeTarget.num_channels;
    mConfig.rate = mStreamAttributeTarget.sample_rate;

    // Buffer size: 1536(period_size) * 2(ch) * 4(byte) * 2(period_count) = 24 kb
    mConfig.period_count = 2;
    // audio low latency param - playback - interrupt rate
    mConfig.period_size = (mStreamAttributeSource->buffer_size / mConfig.channels / ((mStreamAttributeSource->audio_format == AUDIO_FORMAT_PCM_16_BIT) ? 2 : 4));

    // audio low latency param - playback - hw buffer size
    mStreamAttributeTarget.buffer_size = mConfig.period_size * mConfig.period_count * mConfig.channels * ((mStreamAttributeTarget.audio_format == AUDIO_FORMAT_PCM_16_BIT) ? 2 : 4);

    // ALPS02409284, change interrupt rate & hw buffer size depend target's sampling rate
    if (mStreamAttributeSource->sample_rate != mStreamAttributeTarget.sample_rate) {
        mConfig.period_size = mConfig.period_size * mStreamAttributeTarget.sample_rate / mStreamAttributeSource->sample_rate;
    }

    mConfig.format = transferAudioFormatToPcmFormat(mStreamAttributeTarget.audio_format);

    mConfig.start_threshold = mConfig.period_size * mConfig.period_count;
    mConfig.stop_threshold = 0;
    mConfig.silence_threshold = 0;
    ALOGD("%s(), mConfig: channels = %d, rate = %d, period_size = %d, period_count = %d, format = %d, buffer size %d %d",
          __FUNCTION__, mConfig.channels, mConfig.rate, mConfig.period_size, mConfig.period_count, mConfig.format,
          mStreamAttributeTarget.buffer_size, mStreamAttributeSource->buffer_size);

    mStreamAttributeTarget.mInterrupt = (mConfig.period_size + 0.0) / mStreamAttributeTarget.sample_rate;

#ifdef MTK_AURISYS_FRAMEWORK_SUPPORT
    if (get_aurisys_on()) {
        CreateAurisysLibManager();
    } else
#endif
    {
        // post processing
        initPostProcessing();

        // SRC
        initBliSrc();

        // bit conversion
        initBitConverter();

        initDataPending();
    }

    // disable lowjitter mode
    SetLowJitterMode(true, mStreamAttributeTarget.sample_rate);

    // open pcm driver
#if defined(PLAYBACK_MMAP)
    unsigned int flag = PCM_NOIRQ | PCM_MMAP | PCM_OUT | PCM_MONOTONIC;
    openPcmDriverWithFlag(pcmindex, flag);
#else
    openPcmDriver(pcmindex);
#endif

    AL_UNLOCK(AudioALSADriverUtility::getInstance()->getStreamSramDramLock());


#if defined(MTK_HYBRID_NLE_SUPPORT) // must be after pcm open
    mStreamAttributeTarget.output_devices = mStreamAttributeSource->output_devices;
    initNLEProcessing();
#endif

    // open codec driver
    mHardwareResourceManager->startOutputDevice(mStreamAttributeSource->output_devices, mStreamAttributeTarget.sample_rate);


    //============Voice UI&Unlock REFERECE=============
    AudioVUnlockDL *VUnlockhdl = AudioVUnlockDL::getInstance();
    if (VUnlockhdl != NULL) {
        VUnlockhdl->SetInputStandBy(false);
        VUnlockhdl-> GetSRCInputParameter(mStreamAttributeTarget.sample_rate, mStreamAttributeTarget.num_channels, mStreamAttributeTarget.audio_format);
        VUnlockhdl->GetFirstDLTime();
    }
    //===========================================


    ALOGD("-%s()", __FUNCTION__);
    return NO_ERROR;
}


status_t AudioALSAPlaybackHandlerFast::close() {
    ALOGD("+%s()", __FUNCTION__);

    //============Voice UI&Unlock REFERECE=============
    AudioVUnlockDL *VUnlockhdl = AudioVUnlockDL::getInstance();
    if (VUnlockhdl != NULL) {
        VUnlockhdl->SetInputStandBy(true);
    }
    //===========================================

#if defined(MTK_HYBRID_NLE_SUPPORT)
    // Must do this before close analog path
    deinitNLEProcessing();
#endif

    // close codec driver
    mHardwareResourceManager->stopOutputDevice();

    // close pcm driver
    AL_AUTOLOCK(*AudioALSADriverUtility::getInstance()->getStreamSramDramLock());
    closePcmDriver();

    // disable lowjitter mode
    SetLowJitterMode(false, mStreamAttributeTarget.sample_rate);

#ifdef MTK_AURISYS_FRAMEWORK_SUPPORT
    if (get_aurisys_on()) {
        DestroyAurisysLibManager();
    } else
#endif
    {
        DeinitDataPending();

        // bit conversion
        deinitBitConverter();

        // SRC
        deinitBliSrc();

        // post processing
        deinitPostProcessing();
    }

    // debug pcm dump
    ClosePCMDump();

    //release pmic clk
    mHardwareResourceManager->EnableAudBufClk(false);

    ALOGD("-%s()", __FUNCTION__);
    return NO_ERROR;
}


status_t AudioALSAPlaybackHandlerFast::routing(const audio_devices_t output_devices) {
    mHardwareResourceManager->changeOutputDevice(output_devices);
    if (mAudioFilterManagerHandler) { mAudioFilterManagerHandler->setDevice(output_devices); }
    return NO_ERROR;
}
int AudioALSAPlaybackHandlerFast::pause() {
    return -ENODATA;
}

int AudioALSAPlaybackHandlerFast::resume() {
    return -ENODATA;
}

int AudioALSAPlaybackHandlerFast::flush() {
    return 0;
}

status_t AudioALSAPlaybackHandlerFast::setVolume(uint32_t vol __unused) {
    return INVALID_OPERATION;
}


int AudioALSAPlaybackHandlerFast::drain(audio_drain_type_t type __unused) {
    return 0;
}


status_t AudioALSAPlaybackHandlerFast::setScreenState(bool mode __unused,
                                                      size_t buffer_size __unused,
                                                      size_t reduceInterruptSize __unused,
                                                      bool bforce __unused) {
    return NO_ERROR;
}

ssize_t AudioALSAPlaybackHandlerFast::write(const void *buffer, size_t bytes) {
    ALOGV("%s(), buffer = %p, bytes = %zu", __FUNCTION__, buffer, bytes);

    if (mPcm == NULL) {
        ALOGE("%s(), mPcm == NULL, return", __FUNCTION__);
        return bytes;
    }

    // const -> to non const
    void *pBuffer = const_cast<void *>(buffer);
    ASSERT(pBuffer != NULL);

#ifdef DEBUG_LATENCY
    clock_gettime(CLOCK_REALTIME, &mNewtime);
    latencyTime[0] = calc_time_diff(mNewtime, mOldtime);
    mOldtime = mNewtime;
#endif

    // stereo to mono for speaker
    doStereoToMonoConversionIfNeed(pBuffer, bytes);

    void *pBufferAfterPending = NULL;
    uint32_t bytesAfterpending = 0;

#ifdef MTK_AURISYS_FRAMEWORK_SUPPORT
    if (get_aurisys_on()) {
        audio_ringbuf_copy_from_linear(&mAudioPoolBufDlIn->ringbuf, (char *)pBuffer, bytes);

        // post processing + SRC + Bit conversion
        aurisys_process_dl_only(mAurisysLibManager, mAudioPoolBufDlIn, mAudioPoolBufDlOut);

        // data pending: sram is device memory, need word size align 64 byte for 64 bit platform
        uint32_t data_size = audio_ringbuf_count(&mAudioPoolBufDlOut->ringbuf);
        if (data_size > mTransferredBufferSize) {
            data_size = mTransferredBufferSize;
        }
        data_size &= 0xFFFFFFC0;
        audio_ringbuf_copy_to_linear(mLinearOut, &mAudioPoolBufDlOut->ringbuf, data_size);
        //ALOGD("aurisys process data_size: %u", data_size);

        // wrap to original playback handler
        pBufferAfterPending = (void *)mLinearOut;
        bytesAfterpending = data_size;
    } else
#endif
    {
        // post processing (can handle both Q1P16 and Q1P31 by audio_format_t)
        void *pBufferAfterPostProcessing = NULL;
        uint32_t bytesAfterPostProcessing = 0;
        doPostProcessing(pBuffer, bytes, &pBufferAfterPostProcessing, &bytesAfterPostProcessing);


        // SRC
        void *pBufferAfterBliSrc = NULL;
        uint32_t bytesAfterBliSrc = 0;
        doBliSrc(pBufferAfterPostProcessing, bytesAfterPostProcessing, &pBufferAfterBliSrc, &bytesAfterBliSrc);


        // bit conversion
        void *pBufferAfterBitConvertion = NULL;
        uint32_t bytesAfterBitConvertion = 0;
        doBitConversion(pBufferAfterBliSrc, bytesAfterBliSrc, &pBufferAfterBitConvertion, &bytesAfterBitConvertion);

        // data pending
        pBufferAfterPending = NULL;
        bytesAfterpending = 0;
        dodataPending(pBufferAfterBitConvertion, bytesAfterBitConvertion, &pBufferAfterPending, &bytesAfterpending);
    }

    // pcm dump
    WritePcmDumpData(pBufferAfterPending, bytesAfterpending);

#ifdef DEBUG_LATENCY
    clock_gettime(CLOCK_REALTIME, &mNewtime);
    latencyTime[1] = calc_time_diff(mNewtime, mOldtime);
    mOldtime = mNewtime;
#endif

#ifdef MTK_LATENCY_DETECT_PULSE
    AudioDetectPulse::doDetectPulse(TAG_PLAYERBACK_HANDLER, PULSE_LEVEL, 0, (void *)pBufferAfterPending,
                                    bytesAfterpending, mStreamAttributeTarget.audio_format,
                                    mStreamAttributeTarget.num_channels, mStreamAttributeTarget.sample_rate);
#endif

    dataTransferBeforeWrite(pBufferAfterPending, bytesAfterpending);

    // write data to pcm driver
    int retval = pcmWrite(mPcm, pBufferAfterPending, bytesAfterpending);

#ifdef DEBUG_LATENCY
    clock_gettime(CLOCK_REALTIME, &mNewtime);
    latencyTime[2] = calc_time_diff(mNewtime, mOldtime);
    mOldtime = mNewtime;
#endif

#ifndef DOWNLINK_LOW_LATENCY
    // TODO(Harvey, Wendy), temporary disable Voice Unlock until 24bit ready
    //============Voice UI&Unlock REFERECE=============
    AudioVUnlockDL *VUnlockhdl = AudioVUnlockDL::getInstance();
    if (VUnlockhdl != NULL) {
        // get remain time
        //VUnlockhdl->SetDownlinkStartTime(ret_ms);
        VUnlockhdl->GetFirstDLTime();

        //VUnlockhdl->SetInputStandBy(false);
        if (mStreamAttributeSource->output_devices & AUDIO_DEVICE_OUT_WIRED_HEADSET ||
            mStreamAttributeSource->output_devices & AUDIO_DEVICE_OUT_WIRED_HEADPHONE) {
            memset((void *)pBufferAfterPending, 0, bytesAfterpending);
        }
        VUnlockhdl->WriteStreamOutToRing(pBufferAfterPending, bytesAfterpending);
    }
    //===========================================
#endif


    if (retval != 0) {
        ALOGE("%s(), pcm_write() error, retval = %d", __FUNCTION__, retval);
    }

#ifdef DEBUG_LATENCY
    if (latencyTime[0] > THRESHOLD_FRAMEWORK || latencyTime[1] > THRESHOLD_HAL || latencyTime[2] > (mStreamAttributeTarget.mInterrupt - latencyTime[0] - latencyTime[1] + THRESHOLD_KERNEL)) {
        ALOGD("latency_in_s,%1.3lf,%1.3lf,%1.3lf, interrupt,%1.3lf", latencyTime[0], latencyTime[1], latencyTime[2], mStreamAttributeTarget.mInterrupt);
    }
#endif

    return bytes;
}


status_t AudioALSAPlaybackHandlerFast::setFilterMng(AudioMTKFilterManager *pFilterMng) {
    ALOGD("+%s() mAudioFilterManagerHandler [%p]", __FUNCTION__, pFilterMng);
    mAudioFilterManagerHandler = pFilterMng;
    return NO_ERROR;
}

status_t AudioALSAPlaybackHandlerFast::dataTransferBeforeWrite(void *addr, uint32_t size) {
    static bool bSupport = true;

    //ALOGD("+%s() addr 0x%x", __FUNCTION__, (long long) addr);

    if (bSupport) {
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(mMixer, "Audio_DL2_DataTransfer");
        if (ctl == NULL) {
            bSupport = false;
            return NO_ERROR;
        }

        int array[2] = {(int)((long long) addr), (int)size};
        int retval = mixer_ctl_set_array(ctl, array, 2);
        ASSERT(retval == 0);
    }
    return NO_ERROR;
}

} // end of namespace android
