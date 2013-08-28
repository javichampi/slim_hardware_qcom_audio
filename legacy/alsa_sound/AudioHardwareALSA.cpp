/* AudioHardwareALSA.cpp
 **
 ** Copyright 2008-2010 Wind River Systems
 ** Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
 ** Not a Contribution.
 **
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 **
 **     http://www.apache.org/licenses/LICENSE-2.0
 **
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 */

#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

#define LOG_TAG "AudioHardwareALSA"
//#define LOG_NDEBUG 0
//#define LOG_NDDEBUG 0
#include <utils/Log.h>
#include <utils/String8.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <cutils/properties.h>
#include <media/AudioRecord.h>
#include <hardware_legacy/power.h>
#include <audio_utils/resampler.h>
#include <pthread.h>

#include "AudioHardwareALSA.h"
#ifdef QCOM_USBAUDIO_ENABLED
#include "AudioUsbALSA.h"
#endif
#include "AudioUtil.h"

//#define OUTPUT_BUFFER_LOG
#ifdef OUTPUT_BUFFER_LOG
    FILE *outputBufferFile1;
    char outputfilename [50] = "/data/output_proxy";
    static int number = 0;
#endif

extern "C"
{
    //
    // Function for dlsym() to look up for creating a new AudioHardwareInterface.
    //
    android_audio_legacy::AudioHardwareInterface *createAudioHardware(void) {
        return android_audio_legacy::AudioHardwareALSA::create();
    }
#ifdef QCOM_ACDB_ENABLED
    static int (*acdb_init)();
    static void (*acdb_deallocate)();
    static void (*acdb_send_audio_cal)(int, int);
#endif
#ifdef QCOM_CSDCLIENT_ENABLED
    static int (*csd_client_init)();
    static int (*csd_client_deinit)();
#ifdef NEW_CSDCLIENT
    static int (*csd_start_playback)(uint32_t);
    static int (*csd_stop_playback)(uint32_t);
    static int (*csd_standby_voice)(uint32_t);
    static int (*csd_resume_voice)(uint32_t);
#else
    static int (*csd_start_playback)();
    static int (*csd_stop_playback)();
#endif
#endif
}         // extern "C"

namespace android_audio_legacy
{

// ----------------------------------------------------------------------------

AudioHardwareInterface *AudioHardwareALSA::create() {

    AudioHardwareInterface * hardwareInterface = new AudioHardwareALSA();
    if(hardwareInterface->initCheck() != OK) {
        ALOGE("NULL - AHAL creation failed");
        delete hardwareInterface;
        hardwareInterface = NULL;
    }
    return hardwareInterface;
}

AudioHardwareALSA::AudioHardwareALSA() :
    mALSADevice(0),mVoipInStreamCount(0),mVoipOutStreamCount(0),mVoipMicMute(false),
    mVoipBitRate(0),mCallState(0),mAcdbHandle(NULL),mCsdHandle(NULL),mMicMute(0)
//XIAOMI_START
    ,mAudienceCodecInit(0)
//XIAOMI_END
{
    FILE *fp;
    char soundCardInfo[200];
    char platform[128], baseband[128], baseband_arch[128], audio_init[128], platformVer[128];
    int codec_rev = 2, verNum = 0;

    mDeviceList.clear();
    mVoiceCallState = CALL_INACTIVE;
    mVolteCallState = CALL_INACTIVE;
    mVoice2CallState = CALL_INACTIVE;
    mVSID = 0;
    mIsFmActive = 0;
    mDevSettingsFlag = 0;
    bool audio_init_done = false;
    int sleep_retry = 0;
#ifdef QCOM_USBAUDIO_ENABLED
    mAudioUsbALSA = new AudioUsbALSA();
    musbPlaybackState = 0;
    musbRecordingState = 0;
#endif
#ifdef USES_FLUENCE_INCALL
    mDevSettingsFlag |= TTY_OFF | DMIC_FLAG;
#else
    mDevSettingsFlag |= TTY_OFF;
#endif
    mBluetoothVGS = false;
    mFusion3Platform = false;

    mRouteAudioToExtOut = false;
    mA2dpDevice = NULL;
    mA2dpStream = NULL;
    mUsbDevice = NULL;
    mUsbStream = NULL;
    mExtOutStream = NULL;
    mResampler = NULL;
    mExtOutActiveUseCases = USECASE_NONE;
    mIsExtOutEnabled = false;
    mKillExtOutThread = false;
    mExtOutThreadAlive = false;
    mExtOutThread = NULL;
    mUcMgr = NULL;
    mCanOpenProxy=1;

#ifdef QCOM_ACDB_ENABLED
    acdb_deallocate = NULL;
#endif
    // MIUI MOD
    // mALSADevice = new ALSADevice();
	mALSADevice = new ALSADevice(this);
    if (!mALSADevice) {
        mStatus = NO_INIT;
        return;
    }
#ifdef QCOM_ACDB_ENABLED
    mAcdbHandle = ::dlopen("/system/lib/libacdbloader.so", RTLD_NOW);
    if (mAcdbHandle == NULL) {
        ALOGE("AudioHardware: DLOPEN not successful for ACDBLOADER");
    } else {
        ALOGD("AudioHardware: DLOPEN successful for ACDBLOADER");
        acdb_init = (int (*)())::dlsym(mAcdbHandle,"acdb_loader_init_ACDB");
        if (acdb_init == NULL) {
            ALOGE("dlsym:Error:%s Loading acdb_loader_init_ACDB");
        }else {
           acdb_init();
           acdb_deallocate = (void (*)())::dlsym(mAcdbHandle,"acdb_loader_deallocate_ACDB");
        }
    }
    mALSADevice->setACDBHandle(mAcdbHandle);
#endif

    if((fp = fopen("/proc/asound/cards","r")) == NULL) {
        ALOGE("Cannot open /proc/asound/cards file to get sound card info");
        mStatus = NO_INIT;
        return;
    } else {
        while((fgets(soundCardInfo, sizeof(soundCardInfo), fp) != NULL)) {
            ALOGV("SoundCardInfo %s", soundCardInfo);
            if (strstr(soundCardInfo, "msm8960-tabla1x-snd-card")) {
                codec_rev = 1;
                break;
            } else if (strstr(soundCardInfo, "msm-snd-card")) {
                codec_rev = 2;
                break;
            } else if (strstr(soundCardInfo, "apq8064-tabla-snd-card")) {
                codec_rev = 2;
                break;
            } else if (strstr(soundCardInfo, "msm8960-snd-card")) {
                codec_rev = 2;
                break;
            } else if (strstr(soundCardInfo, "msm8930-sitar-snd-card")) {
                codec_rev = 3;
                property_get("ro.baseband", baseband, "");
                if (!strcmp("sglte", baseband))
                    codec_rev = 31;
                break;
            } else if (strstr(soundCardInfo, "msm8974-taiko-mtp-snd-card")) {
                codec_rev = 40;
                break;
            } else if (strstr(soundCardInfo, "msm8974-taiko-cdp-snd-card")) {
                codec_rev = 41;
                break;
            } else if (strstr(soundCardInfo, "msm8974-taiko-fluid-snd-card")) {
                codec_rev = 42;
                break;
            } else if (strstr(soundCardInfo, "msm8974-taiko-liquid-snd-card")) {
                codec_rev = 43;
                break;
            } else if(strstr(soundCardInfo, "no soundcards")) {
                ALOGE("NO SOUND CARD DETECTED");
                if(sleep_retry < SOUND_CARD_SLEEP_RETRY) {
                    ALOGD("Sleeping for 100 ms");
                    usleep(SOUND_CARD_SLEEP_WAIT * 1000);
                    sleep_retry++;
                    fseek(fp, 0, SEEK_SET);
                    continue;
                }
                else {
                    ALOGE("Failed %d attempts for sound card detection", sleep_retry);
                    fclose(fp);
                    mStatus = NO_INIT;
                    return;
                }
            } else {
                ALOGE("ERROR - Sound card detection");
                fclose(fp);
                mStatus = NO_INIT;
                return;
            }
        }
        fclose(fp);
    }

    sleep_retry = 0;
    while (audio_init_done == false && sleep_retry < MAX_SLEEP_RETRY) {
        property_get("qcom.audio.init", audio_init, NULL);
        ALOGD("qcom.audio.init is set to %s\n",audio_init);
        if(!strncmp(audio_init, "complete", sizeof("complete"))) {
            audio_init_done = true;
        } else {
            ALOGD("Sleeping for 50 ms");
            usleep(AUDIO_INIT_SLEEP_WAIT*1000);
            sleep_retry++;
        }
    }

    if (codec_rev == 1) {
        ALOGV("Detected tabla 1.x sound card");
        snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm");
    } else if (codec_rev == 3) {
        ALOGV("Detected sitar 1.x sound card");
        snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_Sitar");
    } else if (codec_rev == 31) {
        ALOGV("Detected sitar 1.x sound card");
        snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_Sitar_Sglte");
    } else if (codec_rev == 40) {
        ALOGV("Detected taiko sound card");
        snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_Taiko");
    } else if (codec_rev == 41) {
        ALOGV("Detected taiko sound card");
        snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_Taiko_CDP");
    } else if (codec_rev == 42) {
        ALOGV("Detected taiko sound card");
        snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_Taiko_Fluid");
    } else if (codec_rev == 43) {
        ALOGV("Detected taiko liquid sound card");
        snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_Taiko_liquid");
    } else {
        property_get("ro.board.platform", platform, "");
        property_get("ro.baseband", baseband, "");
        property_get("ro.baseband.arch", baseband_arch, "");
        if (!strcmp("msm8960", platform) &&
            (!strcmp("mdm", baseband) || !strcmp("sglte2", baseband) ||
            !strcmp("mdm", baseband_arch))) {
            ALOGV("Detected Fusion tabla 2.x");
            mFusion3Platform = true;
            if((fp = fopen("/sys/devices/system/soc/soc0/platform_version","r")) == NULL) {
                ALOGE("Cannot open /sys/devices/system/soc/soc0/platform_version file");

                snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_2x_Fusion3");
            } else {
                while((fgets(platformVer, sizeof(platformVer), fp) != NULL)) {
                    ALOGV("platformVer %s", platformVer);

                    verNum = atoi(platformVer);
                    if (verNum == 0x10001) {
                        snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_I2SFusion");
                        break;
                    } else {
                        snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_2x_Fusion3");
                        break;
                    }
                }
            }
            fclose(fp);
        } else {
            ALOGV("Detected tabla 2.x sound card");
            snd_use_case_mgr_open(&mUcMgr, "snd_soc_msm_2x");
        }
    }

#ifdef QCOM_CSDCLIENT_ENABLED
    if (mFusion3Platform) {
        mCsdHandle = ::dlopen("/system/lib/libcsd-client.so", RTLD_NOW);
        if (mCsdHandle == NULL) {
            ALOGE("AudioHardware: DLOPEN not successful for CSD CLIENT");
        } else {
            ALOGD("AudioHardware: DLOPEN successful for CSD CLIENT");
            csd_client_init = (int (*)())::dlsym(mCsdHandle, "csd_client_init");
            csd_client_deinit = (int (*)())::dlsym(mCsdHandle,
                                                   "csd_client_deinit");
#ifdef NEW_CSDCLIENT
            csd_start_playback = (int (*)(uint32_t))::dlsym(mCsdHandle,
                                                   "csd_client_start_playback");
            csd_stop_playback = (int (*)(uint32_t))::dlsym(mCsdHandle,
                                                    "csd_client_stop_playback");
            csd_standby_voice = (int (*)(uint32_t))::dlsym(mCsdHandle,
                                                    "csd_client_standby_voice");
            csd_resume_voice = (int (*)(uint32_t))::dlsym(mCsdHandle,
                                                     "csd_client_resume_voice");
#else
            csd_start_playback = (int (*)())::dlsym(mCsdHandle,
                                               "csd_client_start_playback");
            csd_stop_playback = (int (*)())::dlsym(mCsdHandle,
                                               "csd_client_stop_playback");
#endif

            if (csd_client_init == NULL) {
                ALOGE("csd_client_init is NULL");
            } else {
                csd_client_init();
            }

        }
        mALSADevice->setCsdHandle(mCsdHandle);
    }
#endif

    if (mUcMgr < 0) {
        ALOGE("Failed to open ucm instance: %d", errno);
        mStatus = NO_INIT;
        return;
    } else {
        ALOGI("ucm instance opened: %u", (unsigned)mUcMgr);
        mUcMgr->isFusion3Platform = mFusion3Platform;
        if (mAcdbHandle) {
            mUcMgr->acdb_handle = static_cast<void*> (mAcdbHandle);
        }
//XIAOMI_START
    if (mFusion3Platform) {
        pthread_create(&CSDInitThread, NULL, CSDInitThreadWrapper, this);
        //csd_client_init();
    }
//XIAOMI_END
    }
//XIAOMI_START
    mLoopbackState = 0;
    ALOGE("doAudienceCodec_Init+");
    if (mAudienceCodecInit == 0) {
        Mutex::Autolock lock(mAudioCodecLock);
        doAudienceCodec_Init();
    }
    ALOGE("doAudienceCodec_Init-");
//XIAOMI_END

    //set default AudioParameters
    AudioParameter param;
    String8 key;
    String8 value;
#ifdef QCOM_FLUENCE_ENABLED
    //Set default AudioParameter for fluencetype
    key  = String8(AudioParameter::keyFluenceType);
    property_get("ro.qc.sdk.audio.fluencetype",mFluenceKey,"0");
    if (0 == strncmp("fluencepro", mFluenceKey, sizeof("fluencepro"))) {
        mDevSettingsFlag |= QMIC_FLAG;
        mDevSettingsFlag &= (~DMIC_FLAG);
        value = String8("fluencepro");
        ALOGD("FluencePro quadMic feature Enabled");
    } else if (0 == strncmp("fluence", mFluenceKey, sizeof("fluence"))) {
        mDevSettingsFlag |= DMIC_FLAG;
        mDevSettingsFlag &= (~QMIC_FLAG);
        value = String8("fluence");
        ALOGD("Fluence dualmic feature Enabled");
    } else if (0 == strncmp("none", mFluenceKey, sizeof("none"))) {
        mDevSettingsFlag &= (~DMIC_FLAG);
        mDevSettingsFlag &= (~QMIC_FLAG);
        value = String8("none");
        ALOGD("Fluence feature Disabled");
    }
    param.add(key, value);
    mALSADevice->setFlags(mDevSettingsFlag);
#endif

#ifdef QCOM_SSR_ENABLED
    //set default AudioParameters for surround sound recording
    char ssr_enabled[6] = "false";
    property_get("ro.qc.sdk.audio.ssr",ssr_enabled,"0");
    if (!strncmp("true", ssr_enabled, 4)) {
        ALOGD("surround sound recording is supported");
        param.add(String8(AudioParameter::keySSR), String8("true"));
    } else {
        ALOGD("surround sound recording is not supported");
        param.add(String8(AudioParameter::keySSR), String8("false"));
    }
#endif

    mStatus = OK;
}

AudioHardwareALSA::~AudioHardwareALSA()
{
//XIAOMI_START
    doAudienceCodec_DeInit();
//XIAOMI_END
    if (mUcMgr != NULL) {
        ALOGV("closing ucm instance: %u", (unsigned)mUcMgr);
        snd_use_case_mgr_close(mUcMgr);
    }
    if (mALSADevice) {
        delete mALSADevice;
    }
    for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
        it->useCase[0] = 0;
        mDeviceList.erase(it);
    }
    if (mResampler) {
        release_resampler(mResampler);
        mResampler = NULL;
    }
#ifdef QCOM_ACDB_ENABLED
     if (acdb_deallocate == NULL) {
        ALOGE("acdb_deallocate_ACDB is NULL");
     } else {
        acdb_deallocate();
     }
     if (mAcdbHandle) {
        ::dlclose(mAcdbHandle);
        mAcdbHandle = NULL;
     }
#endif
#ifdef QCOM_USBAUDIO_ENABLED
    delete mAudioUsbALSA;
#endif

#ifdef QCOM_CSDCLIENT_ENABLED
    if (mFusion3Platform) {
        if (mCsdHandle) {
            if (csd_client_deinit == NULL) {
                ALOGE("csd_client_deinit is NULL");
            } else {
                csd_client_deinit();
            }
            ::dlclose(mCsdHandle);
            mCsdHandle = NULL;
        }
    }
#endif
}

status_t AudioHardwareALSA::initCheck()
{
    return mStatus;
}

status_t AudioHardwareALSA::setVoiceVolume(float v)
{
    ALOGV("setVoiceVolume(%f)\n", v);
    if (v < 0.0) {
        ALOGW("setVoiceVolume(%f) under 0.0, assuming 0.0\n", v);
        v = 0.0;
    } else if (v > 1.0) {
        ALOGW("setVoiceVolume(%f) over 1.0, assuming 1.0\n", v);
        v = 1.0;
    }

    int newMode = mode();
    ALOGV("setVoiceVolume  newMode %d",newMode);
    int vol = lrint(v * 100.0);

    // Voice volume levels from android are mapped to driver volume levels as follows.
    // 0 -> 5, 20 -> 4, 40 ->3, 60 -> 2, 80 -> 1, 100 -> 0
    // So adjust the volume to get the correct volume index in driver
    vol = 100 - vol;

    if (mALSADevice) {
        if(newMode == AUDIO_MODE_IN_COMMUNICATION) {
            mALSADevice->setVoipVolume(vol);
        } else if (newMode == AUDIO_MODE_IN_CALL){
               if (mVoiceCallState == CALL_ACTIVE)
                   mALSADevice->setVoiceVolume(vol);
               else if (mVoice2CallState == CALL_ACTIVE)
                   mALSADevice->setVoice2Volume(vol);
               if (mVolteCallState == CALL_ACTIVE)
                   mALSADevice->setVoLTEVolume(vol);
        }
    }

    return NO_ERROR;
}

status_t AudioHardwareALSA::setMasterVolume(float volume)
{
    return NO_ERROR;
}

status_t AudioHardwareALSA::setMode(int mode)
{
    status_t status = NO_ERROR;
    ALOGE("mMode:%d, mode:%d", mMode, mode);
//XIAOMI_START
    if ((mMode == AUDIO_MODE_RINGTONE) &&
        (mode == AUDIO_MODE_NORMAL)) {
        enableAudienceloopback(0);
        doRouting_Audience_Codec( 0, 0, false);
    }
//XIAOMI_END

    ALOGV("%s() mode=%d mMode=%d", __func__, mode, mMode);

    if (mode != mMode) {
        status = AudioHardwareBase::setMode(mode);
    }

    if (mode == AUDIO_MODE_RINGTONE) {
//XIAOMI_START
        if (mAudienceCmd == CMD_AUDIENCE_READY)
        {
            mAudienceCmd = CMD_AUDIENCE_WAKEUP;
            pthread_cond_signal(&mAudienceCV);
        }
//XIAOMI_END
    }

    if (mode == AUDIO_MODE_IN_CALL) {
//XIAOMI_START
        if (mAudienceCmd == CMD_AUDIENCE_READY)
        {
            mAudienceCmd = CMD_AUDIENCE_WAKEUP;
            pthread_cond_signal(&mAudienceCV);
        }
//XIAOMI_END
        if (mCallState == CALL_INACTIVE) {
            ALOGV("%s() defaulting vsid and call state",__func__);
            mCallState = CALL_ACTIVE;
            mVSID = VOICE_SESSION_VSID;
        } else {
            ALOGV("%s no op",__func__);
        }
    } else if (mode == AUDIO_MODE_NORMAL) {
        mCallState = CALL_INACTIVE;
    }

    return status;
}

bool AudioHardwareALSA::isAnyCallActive() {

    bool ret = false;

    if ((mVoiceCallState == CALL_ACTIVE) ||
        (mVoiceCallState == CALL_HOLD) ||
        (mVoiceCallState == CALL_LOCAL_HOLD) ||
        (mVolteCallState == CALL_ACTIVE) ||
        (mVolteCallState == CALL_HOLD) ||
        (mVolteCallState == CALL_LOCAL_HOLD) ||
        (mVoice2CallState == CALL_ACTIVE) ||
        (mVoice2CallState == CALL_HOLD) ||
        (mVoice2CallState == CALL_LOCAL_HOLD)) {
        ret = true;
    }
    ALOGV("%s() ret=%d", __func__, ret);

    return ret;
}


status_t AudioHardwareALSA::setParameters(const String8& keyValuePairs)
{
    AudioParameter param = AudioParameter(keyValuePairs);
    String8 key;
    String8 value;
    float fm_volume;
    status_t status = NO_ERROR;
    int device;
    int btRate;
    int state;
    enum call_state  call_state = CALL_INVALID;
    uint32_t vsid = 0;

    ALOGV("%s() ,%s", __func__, keyValuePairs.string());

#ifdef QCOM_ADSP_SSR_ENABLED
    key = String8(AudioParameter::keyADSPStatus);
    if (param.get(key, value) == NO_ERROR) {
       if (value == "ONLINE") {
           ALOGV("ADSP online set SSRcomplete");
           mALSADevice->mSSRComplete = true;
           return status;
       }
       else if (value == "OFFLINE") {
           ALOGV("ADSP online re-set SSRcomplete");
           mALSADevice->mSSRComplete = false;
           if ( mRouteAudioToExtOut==true) {
               ALOGV("ADSP offline close EXT output");
               uint32_t activeUsecase = getExtOutActiveUseCases_l();
               stopPlaybackOnExtOut_l(activeUsecase);
           }
           return status;
       }
    }
#endif

    key = String8(TTY_MODE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        mDevSettingsFlag &= TTY_CLEAR;
        if (value == "full" || value == "tty_full") {
            mDevSettingsFlag |= TTY_FULL;
        } else if (value == "hco" || value == "tty_hco") {
            mDevSettingsFlag |= TTY_HCO;
        } else if (value == "vco" || value == "tty_vco") {
            mDevSettingsFlag |= TTY_VCO;
        } else {
            mDevSettingsFlag |= TTY_OFF;
        }
        ALOGI("Changed TTY Mode=%s", value.string());
        mALSADevice->setFlags(mDevSettingsFlag);
        if(mMode != AUDIO_MODE_IN_CALL){
           return NO_ERROR;
        }
        doRouting(0);
        param.remove(key);
    }
#ifdef QCOM_FLUENCE_ENABLED
    key = String8(AudioParameter::keyFluenceType);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "quadmic") {
            //Allow changing fluence type to "quadmic" only when fluence type is fluencepro
            if (0 == strncmp("fluencepro", mFluenceKey, sizeof("fluencepro"))) {
                mDevSettingsFlag |= QMIC_FLAG;
                mDevSettingsFlag &= (~DMIC_FLAG);
                ALOGV("Fluence quadMic feature Enabled");
            }
        } else if (value == "dualmic") {
            //Allow changing fluence type to "dualmic" only when fluence type is fluencepro or fluence
            if (0 == strncmp("fluencepro", mFluenceKey, sizeof("fluencepro")) ||
                0 == strncmp("fluence", mFluenceKey, sizeof("fluence"))) {
                mDevSettingsFlag |= DMIC_FLAG;
                mDevSettingsFlag &= (~QMIC_FLAG);
                ALOGV("Fluence dualmic feature Enabled");
            }
        } else if (value == "none") {
            mDevSettingsFlag &= (~DMIC_FLAG);
            mDevSettingsFlag &= (~QMIC_FLAG);
            ALOGV("Fluence feature Disabled");
        }
        mALSADevice->setFlags(mDevSettingsFlag);
        doRouting(0);
        param.remove(key);
    }
#endif

#ifdef QCOM_CSDCLIENT_ENABLED
    if (mFusion3Platform) {
        key = String8(INCALLMUSIC_KEY);
        if (param.get(key, value) == NO_ERROR) {
            if (value == "true") {
                ALOGV("Enabling Incall Music setting in the setparameter\n");
                if (csd_start_playback == NULL) {
                    ALOGE("csd_client_start_playback is NULL");
                    } else {
#ifdef NEW_CSDCLIENT
                        csd_start_playback(ALL_SESSION_VSID);
#else
                        csd_start_playback();
#endif
                    }
            } else {
                ALOGV("Disabling Incall Music setting in the setparameter\n");
                if (csd_stop_playback == NULL) {
                    ALOGE("csd_client_stop_playback is NULL");
                } else {
#ifdef NEW_CSDCLIENT
                    csd_stop_playback(ALL_SESSION_VSID);
#else
                    csd_stop_playback();
#endif
                }
            }
            param.remove(key);
        }
    }
#endif

#ifdef QCOM_ANC_HEADSET_ENABLED
    key = String8(ANC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "true") {
            ALOGV("Enabling ANC setting in the setparameter\n");
            mDevSettingsFlag |= ANC_FLAG;
        } else {
            ALOGV("Disabling ANC setting in the setparameter\n");
            mDevSettingsFlag &= (~ANC_FLAG);
        }
        mALSADevice->setFlags(mDevSettingsFlag);
        doRouting(0);
        param.remove(key);
    }
#endif

    key = String8(AudioParameter::keyRouting);
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore routing if device is 0.
        if(device) {
            doRouting(device);
        }
        param.remove(key);
    }

    key = String8(BT_SAMPLERATE_KEY);
    if (param.getInt(key, btRate) == NO_ERROR) {
        mALSADevice->setBtscoRate(btRate);
        param.remove(key);
    }

    key = String8(BTHEADSET_VGS);
    if (param.get(key, value) == NO_ERROR) {
        if (value == "on") {
            mBluetoothVGS = true;
        } else {
            mBluetoothVGS = false;
        }
        param.remove(key);
    }

    key = String8(WIDEVOICE_KEY);
    if (param.get(key, value) == NO_ERROR) {
        bool flag = false;
        if (value == "true") {
            flag = true;
        }

        if(mALSADevice) {
            mALSADevice->enableWideVoice(flag, ALL_SESSION_VSID);
        }
        param.remove(key);
    }

    key = String8("a2dp_connected");
    if (param.get(key, value) == NO_ERROR) {
        if (value == "true") {
            status_t err = openExtOutput(AudioSystem::DEVICE_OUT_ALL_A2DP);
        } else {
            status_t err = closeExtOutput(AudioSystem::DEVICE_OUT_ALL_A2DP);
        }
        param.remove(key);
    }

    key = String8("A2dpSuspended");
    if (param.get(key, value) == NO_ERROR) {
        if (mA2dpDevice != NULL) {
            mA2dpDevice->set_parameters(mA2dpDevice,keyValuePairs);
            if(value=="true"){
                 uint32_t activeUsecase = getExtOutActiveUseCases_l();
                 status_t err = suspendPlaybackOnExtOut_l(activeUsecase);
            }
        }
        param.remove(key);
    }

    key = String8(AUDIO_PARAMETER_KEY_FM_VOLUME);

    if (param.getFloat(key, fm_volume) == NO_ERROR) {
        if (fm_volume < 0.0) {
            ALOGW("set Fm Volume(%f) under 0.0, assuming 0.0\n", fm_volume);
            fm_volume = 0.0;
        } else if (fm_volume > 1.0) {
            ALOGW("set Fm Volume(%f) over 1.0, assuming 1.0\n", fm_volume);
            fm_volume = 1.0;
        }
        fm_volume = lrint((fm_volume * 0x2000) + 0.5);

        ALOGV("set Fm Volume(%f)\n", fm_volume);
        ALOGV("Setting FM volume to %d (available range is 0 to 0x2000)\n", fm_volume);

        mALSADevice->setFmVolume(fm_volume);
        param.remove(key);
    }

    key = String8("a2dp_sink_address");
    if (param.get(key, value) == NO_ERROR) {
        if (mA2dpStream != NULL) {
            mA2dpStream->common.set_parameters(&mA2dpStream->common,keyValuePairs);
        }
        param.remove(key);
    }

    key = String8("usb_connected");
    if (param.get(key, value) == NO_ERROR) {
        if (value == "true") {
            status_t err = openExtOutput(AUDIO_DEVICE_OUT_ALL_USB);
        } else {
            status_t err = closeExtOutput(AUDIO_DEVICE_OUT_ALL_USB);
        }
        param.remove(key);
    }

    key = String8("card");
    if (param.get(key, value) == NO_ERROR) {
        if (mUsbStream != NULL) {
            ALOGV("mUsbStream->common.set_parameters");
            mUsbStream->common.set_parameters(&mUsbStream->common,keyValuePairs);
        }
        param.remove(key);
    }

    key = String8(VOIPRATE_KEY);
    if (param.get(key, value) == NO_ERROR) {
            mVoipBitRate = atoi(value);
        param.remove(key);
    }

    key = String8(FENS_KEY);
    if (param.get(key, value) == NO_ERROR) {
        bool flag = false;
        if (value == "true") {
            flag = true;
        }
        if(mALSADevice) {
            mALSADevice->enableFENS(flag, ALL_SESSION_VSID);
        }
        param.remove(key);
    }

#ifdef QCOM_FM_ENABLED
    key = String8(AudioParameter::keyHandleFm);
    if (param.getInt(key, device) == NO_ERROR) {
        // Ignore if device is 0
        if(device) {
            handleFm(device);
        }
        param.remove(key);
    }
#endif

    key = String8(ST_KEY);
    if (param.get(key, value) == NO_ERROR) {
        bool flag = false;
        if (value == "true") {
            flag = true;
        }
        if(mALSADevice) {
            mALSADevice->enableSlowTalk(flag, ALL_SESSION_VSID);
        }
        param.remove(key);
    }

    key = String8(VSID_KEY);
    if (param.getInt(key, (int &)vsid) == NO_ERROR) {
        mVSID = vsid;
        param.remove(key);
        key = String8(CALL_STATE_KEY);
        if (param.getInt(key, (int &)call_state) == NO_ERROR) {
            param.remove(key);
            mCallState = call_state;
            ALOGV("%s() vsid:%x, callstate:%x", __func__, mVSID, call_state);

            if(isAnyCallActive())
                doRouting(0);
        }
        param.remove(key);
    }

    if (param.size()) {
        status = BAD_VALUE;
    }
    return status;
}

String8 AudioHardwareALSA::getParameters(const String8& keys)
{
    AudioParameter param = AudioParameter(keys);
    String8 value;
	String8 key;
    int device;
#ifdef QCOM_FLUENCE_ENABLED
    key = String8(DUALMIC_KEY);
    if (param.get(key, value) == NO_ERROR) {
        value = String8("false");
        param.add(key, value);
    }

    key = String8(AudioParameter::keyFluenceType);
    if (param.get(key, value) == NO_ERROR) {
    if ((mDevSettingsFlag & QMIC_FLAG) &&
                               (mDevSettingsFlag & ~DMIC_FLAG))
            value = String8("quadmic");
    else if ((mDevSettingsFlag & DMIC_FLAG) &&
                                (mDevSettingsFlag & ~QMIC_FLAG))
            value = String8("dualmic");
    else if ((mDevSettingsFlag & ~DMIC_FLAG) &&
                                (mDevSettingsFlag & ~QMIC_FLAG))
            value = String8("none");
        param.add(key, value);
    }
#endif

#ifdef QCOM_FM_ENABLED

    key = String8(AudioParameter::keyHandleA2dpDevice);
    if ( param.get(key,value) == NO_ERROR ) {
        param.add(key, String8("true"));
    }

    key = String8("Fm-radio");
    if ( param.get(key,value) == NO_ERROR ) {
        if ( mIsFmActive ) {
            param.addInt(String8("isFMON"), true );
        }
    }
#endif

    key = String8(BTHEADSET_VGS);
    if (param.get(key, value) == NO_ERROR) {
        if(mBluetoothVGS)
           param.addInt(String8("isVGS"), true);
    }
#ifdef QCOM_SSR_ENABLED
    key = String8(AudioParameter::keySSR);
    if (param.get(key, value) == NO_ERROR) {
        char ssr_enabled[6] = "false";
        property_get("ro.qc.sdk.audio.ssr",ssr_enabled,"0");
        if (!strncmp("true", ssr_enabled, 4)) {
            value = String8("true");
        }
        param.add(key, value);
    }
#endif

    key = String8("A2dpSuspended");
    if (param.get(key, value) == NO_ERROR) {
        if (mA2dpDevice != NULL) {
            value = mA2dpDevice->get_parameters(mA2dpDevice,key);
        }
        param.add(key, value);
    }

    key = String8("a2dp_sink_address");
    if (param.get(key, value) == NO_ERROR) {
        if (mA2dpStream != NULL) {
            value = mA2dpStream->common.get_parameters(&mA2dpStream->common,key);
        }
        param.add(key, value);
    }

    key = String8("tunneled-input-formats");
    if ( param.get(key,value) == NO_ERROR ) {
        ALOGD("Add tunnel AWB to audio parameter");
        param.addInt(String8("AWB"), true );
    }

    key = String8(AudioParameter::keyRouting);
    if (param.getInt(key, device) == NO_ERROR) {
        param.addInt(key, mCurDevice);
    }

    key = String8(AudioParameter::keyCanOpenProxy);
    if(param.get(key, value) == NO_ERROR) {
        param.addInt(key, mCanOpenProxy);
    }

    key = String8(ECHO_SUPRESSION);
    if (param.get(key, value) == NO_ERROR) {
        value = String8("yes");
        param.add(key, value);
    }

    ALOGV("AudioHardwareALSA::getParameters() %s", param.toString().string());
    return param.toString();
}

#ifdef QCOM_USBAUDIO_ENABLED
void AudioHardwareALSA::closeUSBPlayback()
{
    ALOGV("closeUSBPlayback, musbPlaybackState: %d", musbPlaybackState);
    musbPlaybackState = 0;
    mAudioUsbALSA->exitPlaybackThread(SIGNAL_EVENT_KILLTHREAD);
}

void AudioHardwareALSA::closeUSBRecording()
{
    ALOGV("closeUSBRecording");
    musbRecordingState = 0;
    mAudioUsbALSA->exitRecordingThread(SIGNAL_EVENT_KILLTHREAD);
}

void AudioHardwareALSA::closeUsbPlaybackIfNothingActive(){
    ALOGV("closeUsbPlaybackIfNothingActive, musbPlaybackState: %d", musbPlaybackState);
    if(!musbPlaybackState && mAudioUsbALSA != NULL) {
        setProxyProperty(1);
        mAudioUsbALSA->exitPlaybackThread(SIGNAL_EVENT_KILLTHREAD);
    }
}

void AudioHardwareALSA::closeUsbRecordingIfNothingActive(){
    ALOGV("closeUsbRecordingIfNothingActive, musbRecordingState: %d", musbRecordingState);
    if(!musbRecordingState && mAudioUsbALSA != NULL) {
        ALOGD("Closing USB Recording Session as no stream is active");
        mAudioUsbALSA->exitRecordingThread(SIGNAL_EVENT_KILLTHREAD);
    }
}

void AudioHardwareALSA::startUsbPlaybackIfNotStarted(){
    ALOGV("Starting the USB playback %d kill %d", musbPlaybackState,
             mAudioUsbALSA->getkillUsbPlaybackThread());
    if((!musbPlaybackState) || (mAudioUsbALSA->getkillUsbPlaybackThread() == true)) {
           setProxyProperty(0);
           mAudioUsbALSA->startPlayback();
    }
}

void AudioHardwareALSA::startUsbRecordingIfNotStarted(){
    ALOGV("Starting the recording musbRecordingState: %d killUsbRecordingThread %d",
          musbRecordingState, mAudioUsbALSA->getkillUsbRecordingThread());
    if((!musbRecordingState) || (mAudioUsbALSA->getkillUsbRecordingThread() == true)) {
        mAudioUsbALSA->startRecording();
    }
}
#endif

status_t AudioHardwareALSA::doRouting(int device)
{
    Mutex::Autolock autoLock(mLock);
    int newMode = mode();
    bool isRouted = false;

    if(device)
        mALSADevice->mCurDevice = device;
    if ((device == AudioSystem::DEVICE_IN_VOICE_CALL)
#if defined(QCOM_FM_ENABLED) || defined(STE_FM)
        || (device == AudioSystem::DEVICE_IN_FM_RX)
        || (device == AudioSystem::DEVICE_IN_FM_RX_A2DP)
#endif
        || (device == AudioSystem::DEVICE_IN_COMMUNICATION)
        ) {
        ALOGV("Ignoring routing for FM/INCALL/VOIP recording");
        return NO_ERROR;
    }
    ALOGV("device = 0x%x,mCurDevice 0x%x", device, mCurDevice);
    if (device == 0)
        device = mCurDevice;

    ALOGV("doRouting: device %#x newMode %d mVoiceCallState %x \
           mVolteCallActive %x mVoice2CallActive %x mIsFmActive %x",
          device, newMode, mVoiceCallState,
          mVolteCallState, mVoice2CallState, mIsFmActive);

    isRouted = routeCall(device, newMode, mVSID);

    if ((isAnyCallActive())&&
       (mFusion3Platform == true) &&
       (newMode == AudioSystem::MODE_RINGTONE)){
       /* 1st voice call on hold but still the call state will be active as
       * hold state is not propagated to audio stack, now if there is a MT
       * call, then consider the MODE_IN_RINGTONE as MODE_IN_CALL
       * */
       ALOGE(" Call active %d on fusion", isAnyCallActive());
       newMode = AudioSystem::MODE_IN_CALL;
    }
    if (!isRouted) {
#ifdef QCOM_USBAUDIO_ENABLED
        if(!(device & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET) &&
            !(device & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET) &&
            !(device & AudioSystem::DEVICE_IN_ANLG_DOCK_HEADSET) &&
             (musbPlaybackState || musbRecordingState)){
                // mExtOutStream should be initialized before calling route
                // when switching form USB headset to ExtOut device
                if( mRouteAudioToExtOut == true ) {
                    switchExtOut(device);
                }
                ALSAHandleList::iterator it = mDeviceList.end();
                it--;
                mALSADevice->route(&(*it), (uint32_t)device, newMode);
                ALOGD("USB UNPLUGGED, setting musbPlaybackState to 0");
                closeUSBRecording();
                closeUSBPlayback();
        } else if((device & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
                  (device & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
                    ALOGD("Routing everything to prox now");
                    // stopPlaybackOnExtOut should be called to close
                    // ExtOutthread when switching from ExtOut device to USB headset
                    if( mRouteAudioToExtOut == true ) {
                        uint32_t activeUsecase = getExtOutActiveUseCases_l();
                        status_t err = stopPlaybackOnExtOut_l(activeUsecase);
                        if(err) {
                            ALOGW("stopPlaybackOnExtOut_l failed = %d", err);
                            return err;
                        }
                    }
                    ALSAHandleList::iterator it = mDeviceList.end();
                    it--;
                    if (device != mCurDevice) {
                        if(musbPlaybackState)
                            closeUSBPlayback();
                    }
                    mALSADevice->route(&(*it), device, newMode);
                    for(it = mDeviceList.begin(); it != mDeviceList.end(); ++it) {
                         if((!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) ||
                            (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_LPA))) {
                                 ALOGV("doRouting: LPA device switch to proxy");
                                 startUsbPlaybackIfNotStarted();
                                 musbPlaybackState |= USBPLAYBACKBIT_LPA;
                                 break;
                         } else if((!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL)) ||
                                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL))) {
                                    ALOGD("doRouting: Tunnel Player device switch to proxy");
                                    startUsbPlaybackIfNotStarted();
                                    musbPlaybackState |= USBPLAYBACKBIT_TUNNEL;
                                    break;
                         } else if((!strcmp(it->useCase, SND_USE_CASE_VERB_VOICECALL)) ||
                                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOICE)) ||
                                   (!strcmp(it->useCase, SND_USE_CASE_VERB_VOLTE)) ||
                                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOLTE))) {
                                    ALOGV("doRouting: VOICE device switch to proxy");
                                    startUsbRecordingIfNotStarted();
                                    startUsbPlaybackIfNotStarted();
                                    musbPlaybackState |= USBPLAYBACKBIT_VOICECALL;
                                    musbRecordingState |= USBPLAYBACKBIT_VOICECALL;
                                    break;
                        }else if((!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) ||
                                 (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_FM))) {
                                    ALOGV("doRouting: FM device switch to proxy");
                                    startUsbPlaybackIfNotStarted();
                                    musbPlaybackState |= USBPLAYBACKBIT_FM;
                                    break;
                         }
                    }
        } else
#endif
        if ((isExtOutDevice(device)) && mRouteAudioToExtOut == true)  {
            ALOGV(" External Output Enabled - Routing everything to proxy now");
            switchExtOut(device);
            ALSAHandleList::iterator it = mDeviceList.end();
            it--;
            status_t err = NO_ERROR;
            uint32_t activeUsecase = useCaseStringToEnum(it->useCase);
            if (!((device & AudioSystem::DEVICE_OUT_ALL_A2DP) &&
                  (mCurRxDevice & AUDIO_DEVICE_OUT_ALL_USB))) {
                if ((activeUsecase == USECASE_HIFI_LOW_POWER) ||
                    (activeUsecase == USECASE_HIFI_TUNNEL)) {
                    if (device != mCurRxDevice) {
                        if((isExtOutDevice(mCurRxDevice)) &&
                           (isExtOutDevice(device))) {
                            activeUsecase = getExtOutActiveUseCases_l();
                            stopPlaybackOnExtOut_l(activeUsecase);
                            mRouteAudioToExtOut = true;
                        }
                        mALSADevice->route(&(*it),(uint32_t)device, newMode);
                    }
                    err = startPlaybackOnExtOut_l(activeUsecase);
                } else {
                    //WHY NO check for prev device here?
                    if (device != mCurRxDevice) {
                        if((isExtOutDevice(mCurRxDevice)) &&
                            (isExtOutDevice(device))) {
                            activeUsecase = getExtOutActiveUseCases_l();
                            stopPlaybackOnExtOut_l(activeUsecase);
                            mALSADevice->route(&(*it),(uint32_t)device, newMode);
                            mRouteAudioToExtOut = true;
                            startPlaybackOnExtOut_l(activeUsecase);
                        } else {
                           mALSADevice->route(&(*it),(uint32_t)device, newMode);
                        }
                    }
                    if (activeUsecase == USECASE_FM){
                        err = startPlaybackOnExtOut_l(activeUsecase);
                    }
                }
                if(err) {
                    ALOGW("startPlaybackOnExtOut_l for hardware output failed err = %d", err);
                    stopPlaybackOnExtOut_l(activeUsecase);
                    mALSADevice->route(&(*it),(uint32_t)mCurRxDevice, newMode);
                    return err;
                }
            }
        } else if((device & AudioSystem::DEVICE_OUT_ALL) &&
                  (!isExtOutDevice(device)) &&
                   mRouteAudioToExtOut == true ) {
            ALOGV(" ExtOut Disable on hardware output");
            ALSAHandleList::iterator it = mDeviceList.end();
            it--;
            status_t err;
            uint32_t activeUsecase = getExtOutActiveUseCases_l();
            err = stopPlaybackOnExtOut_l(activeUsecase);
            if(err) {
                ALOGW("stopPlaybackOnExtOut_l failed = %d", err);
                return err;
            }
            if (device != mCurDevice) {
                mALSADevice->route(&(*it),(uint32_t)device, newMode);
            }
        } else {
             setInChannels(device);
             ALSAHandleList::iterator it = mDeviceList.end();
             it--;
//XIAOMI_START
             ALOGD("ALSADevice->route mode:%d, device:0x%x, enable:%d", newMode, device, true);
//XIAOMI_END
             mALSADevice->route(&(*it), (uint32_t)device, newMode);
        }
    }
    mCurDevice = device;
    if (device & AudioSystem::DEVICE_OUT_ALL) {
        mCurRxDevice = device;
    }
    return NO_ERROR;
}

void AudioHardwareALSA::setInChannels(int device)
{
     ALSAHandleList::iterator it;

     if (device & AudioSystem::DEVICE_IN_BUILTIN_MIC) {
         for(it = mDeviceList.begin(); it != mDeviceList.end(); ++it) {
             if (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC,
                 strlen(SND_USE_CASE_VERB_HIFI_REC)) ||
                 !strncmp(it->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC,
                 strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC))) {
                 mALSADevice->setInChannels(it->channels);
                 return;
             }
         }
     }

     mALSADevice->setInChannels(1);
}

uint32_t AudioHardwareALSA::getVoipMode(int format)
{
    switch(format) {
    case AUDIO_FORMAT_PCM_16_BIT:
               return MODE_PCM;
         break;
    case AUDIO_FORMAT_AMR_NB:
               return MODE_AMR;
         break;
    case AUDIO_FORMAT_AMR_WB:
               return MODE_AMR_WB;
         break;

#ifdef QCOM_AUDIO_FORMAT_ENABLED
    case AUDIO_FORMAT_EVRC:
               return MODE_IS127;
         break;

    case AUDIO_FORMAT_EVRCB:
               return MODE_4GV_NB;
         break;
    case AUDIO_FORMAT_EVRCWB:
               return MODE_4GV_WB;
         break;
#endif
    default:
               return MODE_PCM;
    }
}

AudioStreamOut *
AudioHardwareALSA::openOutputStream(uint32_t devices,
                                    int *format,
                                    uint32_t *channels,
                                    uint32_t *sampleRate,
                                    status_t *status)
{
    Mutex::Autolock autoLock(mLock);

    audio_output_flags_t flags = static_cast<audio_output_flags_t> (*status);

    ALOGV("openOutputStream: devices 0x%x channels %d sampleRate %d flags %x",
         devices, *channels, *sampleRate, flags);

    status_t err = BAD_VALUE;
#ifdef QCOM_OUTPUT_FLAGS_ENABLED
    if (flags & (AUDIO_OUTPUT_FLAG_LPA | AUDIO_OUTPUT_FLAG_TUNNEL)) {
        int type = !(flags & AUDIO_OUTPUT_FLAG_LPA); //0 for LPA, 1 for tunnel
        AudioSessionOutALSA *out = new AudioSessionOutALSA(this, devices, *format, *channels,
                                                           *sampleRate, type, &err);
        if(err != NO_ERROR) {
            mLock.unlock();
            delete out;
            out = NULL;
            mLock.lock();
        }
        if (status) *status = err;
        return out;
    }
#endif
    AudioStreamOutALSA *out = 0;
    ALSAHandleList::iterator it;

    if (devices & (devices - 1)) {
        if (status) *status = err;
        ALOGE("openOutputStream called with bad devices");
        return out;
    }

    if(isExtOutDevice(devices)) {
        ALOGV("Set Capture from proxy true");
        mRouteAudioToExtOut = true;
    }

#ifdef QCOM_OUTPUT_FLAGS_ENABLED
    if((flags & AUDIO_OUTPUT_FLAG_DIRECT) && (flags & AUDIO_OUTPUT_FLAG_VOIP_RX)&&
       ((*sampleRate == VOIP_SAMPLING_RATE_8K) || (*sampleRate == VOIP_SAMPLING_RATE_16K))) {
        bool voipstream_active = false;
        for(it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
                if((!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
                    ALOGD("openOutput:  it->rxHandle %d it->handle %d",it->rxHandle,it->handle);
                    voipstream_active = true;
                    break;
                }
        }
      if(voipstream_active == false) {
         mVoipOutStreamCount = 0;
         mVoipMicMute = false;
         alsa_handle_t alsa_handle;
         unsigned long bufferSize;
         if(*sampleRate == VOIP_SAMPLING_RATE_8K) {
             bufferSize = VOIP_BUFFER_SIZE_8K;
         }
         else if(*sampleRate == VOIP_SAMPLING_RATE_16K) {
             bufferSize = VOIP_BUFFER_SIZE_16K;
         }
         else {
             ALOGE("unsupported samplerate %d for voip",*sampleRate);
             if (status) *status = err;
                 return out;
          }
          alsa_handle.module = mALSADevice;
          alsa_handle.bufferSize = bufferSize;
          alsa_handle.devices = devices;
          alsa_handle.handle = 0;
          if(*format == AUDIO_FORMAT_PCM_16_BIT)
              alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
          else
              alsa_handle.format = *format;
          alsa_handle.channels = VOIP_DEFAULT_CHANNEL_MODE;
          alsa_handle.sampleRate = *sampleRate;
          alsa_handle.latency = VOIP_PLAYBACK_LATENCY;
          alsa_handle.rxHandle = 0;
          alsa_handle.ucMgr = mUcMgr;
          mALSADevice->setVoipConfig(getVoipMode(*format), mVoipBitRate);
          char *use_case;
          snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
          if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
              strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_IP_VOICECALL, sizeof(alsa_handle.useCase));
          } else {
              strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOIP, sizeof(alsa_handle.useCase));
          }
          free(use_case);
          mDeviceList.push_back(alsa_handle);
          it = mDeviceList.end();
          it--;
          ALOGV("openoutput: mALSADevice->route useCase %s mCurDevice %d mVoipOutStreamCount %d mode %d", it->useCase,mCurDevice,mVoipOutStreamCount, mode());
          if((mCurDevice & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
             (mCurDevice & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)
#ifdef QCOM_PROXY_DEVICE_ENABLED
             ||(mCurDevice & AudioSystem::DEVICE_OUT_PROXY)
#endif
          ){
              ALOGD("Routing to proxy for normal voip call in openOutputStream");
              mALSADevice->route(&(*it), mCurDevice, AUDIO_MODE_IN_COMMUNICATION);
#ifdef QCOM_USBAUDIO_ENABLED
                ALOGD("enabling VOIP in openoutputstream, musbPlaybackState: %d", musbPlaybackState);
              startUsbPlaybackIfNotStarted();
              musbPlaybackState |= USBPLAYBACKBIT_VOIPCALL;
              ALOGD("Starting recording in openoutputstream, musbRecordingState: %d", musbRecordingState);
              startUsbRecordingIfNotStarted();
              musbRecordingState |= USBRECBIT_VOIPCALL;
#endif
           } else{
              mALSADevice->route(&(*it), mCurDevice, AUDIO_MODE_IN_COMMUNICATION);
          }
          if(!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) {
              snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_IP_VOICECALL);
          } else {
              snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_VOIP);
          }
          err = mALSADevice->startVoipCall(&(*it));
          if (err) {
              ALOGE("Device open failed");
              return NULL;
          }
      }
      if(mVoipOutStreamCount>=1) {
          ALOGE("Trying to OPen Multiple Output streams: Not Supported %d",mVoipOutStreamCount);
          return out;
      }
      out = new AudioStreamOutALSA(this, &(*it));
      err = out->set(format, channels, sampleRate, devices);
      if(err == NO_ERROR) {
          mVoipOutStreamCount++;   //increment VoipOutputstreamCount only if success
          ALOGD("openoutput mVoipOutStreamCount %d",mVoipOutStreamCount);
      } else {
          mLock.unlock();
          delete out;
          out = NULL;
          ALOGE("AudioStreamOutALSA->set() failed, return NULL");
          mLock.lock();
      }
      if (status) *status = err;
      return out;
    } else
#endif
    if ((flags & AUDIO_OUTPUT_FLAG_DIRECT) &&
        (devices == AUDIO_DEVICE_OUT_AUX_DIGITAL)) {
        ALOGD("Multi channel PCM");
        alsa_handle_t alsa_handle;
        EDID_AUDIO_INFO info = { 0 };

        alsa_handle.module = mALSADevice;
        alsa_handle.devices = devices;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;

#ifdef TARGET_8974
        char hdmiEDIDData[MAX_SHORT_AUDIO_DESC_CNT + 1];
                              // additional 1 byte for length of the EDID
        if (mALSADevice->getEDIDData(hdmiEDIDData) == NO_ERROR) {
            if (!AudioUtil::getHDMIAudioSinkCaps(&info, hdmiEDIDData)) {
                ALOGE("openOutputStream: Failed to get HDMI sink capabilities");
                return NULL;
            }
        }
#else
        if (!AudioUtil::getHDMIAudioSinkCaps(&info)) {
            ALOGE("openOutputStream: Failed to get HDMI sink capabilities");
            return NULL;
        }
#endif
        if (0 == *channels) {
            alsa_handle.channels = info.AudioBlocksArray[info.nAudioBlocks-1].nChannels;
            if (alsa_handle.channels > 8) {
                alsa_handle.channels = 8;
            }
            *channels = audio_channel_out_mask_from_count(alsa_handle.channels);
        } else {
            alsa_handle.channels = AudioSystem::popCount(*channels);
        }
        if (alsa_handle.channels > 2) {
            alsa_handle.bufferSize = DEFAULT_MULTI_CHANNEL_BUF_SIZE;
        } else {
            alsa_handle.bufferSize = DEFAULT_BUFFER_SIZE;
        }
        if (0 == *sampleRate) {
            alsa_handle.sampleRate = info.AudioBlocksArray[info.nAudioBlocks-1].nSamplingFreq;
            *sampleRate = alsa_handle.sampleRate;
        } else {
            alsa_handle.sampleRate = *sampleRate;
        }
        alsa_handle.latency = PLAYBACK_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.ucMgr = mUcMgr;
        ALOGD("alsa_handle.channels %d alsa_handle.sampleRate %d",alsa_handle.channels,alsa_handle.sampleRate);

        char *use_case;
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI2 , sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_MUSIC2, sizeof(alsa_handle.useCase));
        }
        free(use_case);
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        out = new AudioStreamOutALSA(this, &(*it));
        err = out->set(format, channels, sampleRate, devices);
        if (status) *status = err;
        return out;
    } else {
      alsa_handle_t alsa_handle;
      unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

      for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
          bufferSize &= ~b;

      alsa_handle.module = mALSADevice;
      alsa_handle.bufferSize = bufferSize;
      alsa_handle.devices = devices;
      alsa_handle.handle = 0;
      alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
      alsa_handle.channels = DEFAULT_CHANNEL_MODE;
      alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
      alsa_handle.latency = PLAYBACK_LATENCY;
      alsa_handle.rxHandle = 0;
      alsa_handle.ucMgr = mUcMgr;
#ifdef QCOM_TUNNEL_LPA_ENABLED
      alsa_handle.session = NULL;
#endif
      alsa_handle.isFastOutput = false;

      char *use_case;
      snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);

#ifdef QCOM_OUTPUT_FLAGS_ENABLED
      if (flags & AUDIO_OUTPUT_FLAG_FAST) {
          alsa_handle.bufferSize = PLAYBACK_LOW_LATENCY_BUFFER_SIZE;
          alsa_handle.latency = PLAYBACK_LOW_LATENCY;
          alsa_handle.isFastOutput = true;
          if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
               strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_LOWLATENCY_MUSIC, sizeof(alsa_handle.useCase));
          } else {
               strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_LOWLATENCY_MUSIC, sizeof(alsa_handle.useCase));
          }
      } else
#endif
      {
          if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
               strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI, sizeof(alsa_handle.useCase));
          } else {
               strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_MUSIC, sizeof(alsa_handle.useCase));
          }
      }
      free(use_case);
      mDeviceList.push_back(alsa_handle);
      ALSAHandleList::iterator it = mDeviceList.end();
      it--;
      ALOGD("useCase %s", it->useCase);
      mALSADevice->route(&(*it), devices, mode());
#ifdef QCOM_OUTPUT_FLAGS_ENABLED
      if (flags & AUDIO_OUTPUT_FLAG_FAST) {
          if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOWLATENCY_MUSIC)) {
             snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI_LOWLATENCY_MUSIC);
          } else {
             snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_LOWLATENCY_MUSIC);
          }
      } else
#endif
      {
          if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI)) {
             snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI);
          } else {
             snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_MUSIC);
          }
      }
      err = mALSADevice->open(&(*it));
      if (err) {
          ALOGE("Device open failed");
      } else {
          out = new AudioStreamOutALSA(this, &(*it));
          err = out->set(format, channels, sampleRate, devices);
      }

      if (status) *status = err;
      return out;
    }
}

void
AudioHardwareALSA::closeOutputStream(AudioStreamOut* out)
{
    delete out;
}

#ifdef QCOM_TUNNEL_LPA_ENABLED
AudioStreamOut *
AudioHardwareALSA::openOutputSession(uint32_t devices,
                                     int *format,
                                     status_t *status,
                                     int sessionId,
                                     uint32_t samplingRate,
                                     uint32_t channels)
{
    Mutex::Autolock autoLock(mLock);
    ALOGD("openOutputSession = %d" ,sessionId);
    AudioStreamOutALSA *out = 0;
    status_t err = BAD_VALUE;

    alsa_handle_t alsa_handle;
    unsigned long bufferSize = DEFAULT_BUFFER_SIZE;

    for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;

    alsa_handle.module = mALSADevice;
    alsa_handle.bufferSize = bufferSize;
    alsa_handle.devices = devices;
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = DEFAULT_CHANNEL_MODE;
    alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
    alsa_handle.latency = VOICE_LATENCY;
    alsa_handle.rxHandle = 0;
    alsa_handle.ucMgr = mUcMgr;

    char *use_case;
    if(sessionId == TUNNEL_SESSION_ID) {
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_TUNNEL, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_TUNNEL, sizeof(alsa_handle.useCase));
        }
    } else {
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_LPA, sizeof(alsa_handle.useCase));
        }
    }
    free(use_case);
    mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mDeviceList.end();
    it--;
    ALOGD("useCase %s", it->useCase);
#ifdef QCOM_USBAUDIO_ENABLED
    if((devices & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
       (devices & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
        ALOGE("Routing to proxy for LPA in openOutputSession");
        mALSADevice->route(&(*it), devices, mode());
        devices = AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET;
        ALOGD("Starting USBPlayback for LPA");
        startUsbPlaybackIfNotStarted();
        musbPlaybackState |= USBPLAYBACKBIT_LPA;
    } else
#endif
    {
        mALSADevice->route(&(*it), devices, mode());
    }
    if(sessionId == TUNNEL_SESSION_ID) {
        if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL)) {
            snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI_TUNNEL);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_TUNNEL);
        }
    }
    else {
        if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER)) {
            snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_HIFI_LOW_POWER);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_LPA);
        }
    }
    err = mALSADevice->open(&(*it));
    out = new AudioStreamOutALSA(this, &(*it));

    if (status) *status = err;
       return out;
}

void
AudioHardwareALSA::closeOutputSession(AudioStreamOut* out)
{
    delete out;
}
#endif

AudioStreamIn *
AudioHardwareALSA::openInputStream(uint32_t devices,
                                   int *format,
                                   uint32_t *channels,
                                   uint32_t *sampleRate,
                                   status_t *status,
                                   AudioSystem::audio_in_acoustics acoustics)
{
    Mutex::Autolock autoLock(mLock);
    char *use_case;
    int newMode = mode();
    uint32_t route_devices;

    status_t err = BAD_VALUE;
    AudioStreamInALSA *in = 0;
    ALSAHandleList::iterator it;

    ALOGD("openInputStream: devices 0x%x format 0x%x channels %d sampleRate %d", devices, *format, *channels, *sampleRate);
    if (devices & (devices - 1)) {
        if (status) *status = err;
        ALOGE("openInputStream failed error:0x%x devices:%x",err,devices);
        return in;
    }

    if((devices == AudioSystem::DEVICE_IN_COMMUNICATION) &&
       ((*sampleRate == VOIP_SAMPLING_RATE_8K) || (*sampleRate == VOIP_SAMPLING_RATE_16K))) {
        bool voipstream_active = false;
        for(it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
                if((!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) ||
                   (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_VOIP))) {
                    ALOGD("openInput:  it->rxHandle %p it->handle %p",it->rxHandle,it->handle);
                    voipstream_active = true;
                    break;
                }
        }
        if(voipstream_active == false) {
           mVoipInStreamCount = 0;
           mVoipMicMute = false;
           alsa_handle_t alsa_handle;
           unsigned long bufferSize;
           if(*sampleRate == VOIP_SAMPLING_RATE_8K) {
               bufferSize = VOIP_BUFFER_SIZE_8K;
           }
           else if(*sampleRate == VOIP_SAMPLING_RATE_16K) {
               bufferSize = VOIP_BUFFER_SIZE_16K;
           }
           else {
               ALOGE("unsupported samplerate %d for voip",*sampleRate);
               if (status) *status = err;
               return in;
           }
           alsa_handle.module = mALSADevice;
           alsa_handle.bufferSize = bufferSize;
           alsa_handle.devices = devices;
           alsa_handle.handle = 0;
          if(*format == AUDIO_FORMAT_PCM_16_BIT)
              alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
          else
              alsa_handle.format = *format;
           alsa_handle.channels = VOIP_DEFAULT_CHANNEL_MODE;
           alsa_handle.sampleRate = *sampleRate;
           alsa_handle.latency = VOIP_RECORD_LATENCY;
           alsa_handle.rxHandle = 0;
           alsa_handle.ucMgr = mUcMgr;
          mALSADevice->setVoipConfig(getVoipMode(*format), mVoipBitRate);
           snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
           if ((use_case != NULL) && (strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_VOIP, sizeof(alsa_handle.useCase));
           } else {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_IP_VOICECALL, sizeof(alsa_handle.useCase));
           }
           free(use_case);
           mDeviceList.push_back(alsa_handle);
           it = mDeviceList.end();
           it--;
           ALOGD("mCurrDevice: %d", mCurDevice);
#ifdef QCOM_USBAUDIO_ENABLED
           if((mCurDevice == AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
              (mCurDevice == AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
              ALOGD("Routing everything from proxy for voipcall");
              mALSADevice->route(&(*it), AudioSystem::DEVICE_IN_PROXY, AUDIO_MODE_IN_COMMUNICATION);
              ALOGD("enabling VOIP in openInputstream, musbPlaybackState: %d", musbPlaybackState);
              startUsbPlaybackIfNotStarted();
              musbPlaybackState |= USBPLAYBACKBIT_VOIPCALL;
              ALOGD("Starting recording in openoutputstream, musbRecordingState: %d", musbRecordingState);
              startUsbRecordingIfNotStarted();
              musbRecordingState |= USBRECBIT_VOIPCALL;
           } else
#endif
           {
               mALSADevice->route(&(*it),mCurDevice, AUDIO_MODE_IN_COMMUNICATION);
           }
           if(!strcmp(it->useCase, SND_USE_CASE_VERB_IP_VOICECALL)) {
               snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_IP_VOICECALL);
           } else {
               snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_VOIP);
           }
           if(sampleRate) {
               it->sampleRate = *sampleRate;
           }
           if(channels) {
               it->channels = AudioSystem::popCount(*channels);
               setInChannels(devices);
           }
           err = mALSADevice->startVoipCall(&(*it));
           if (err) {
               ALOGE("Error opening pcm input device");
               return NULL;
           }
        }
        if(mVoipInStreamCount>=1){
            ALOGE("Trying to Open Multiple inpust Stream: Not supported %d",mVoipInStreamCount);
            return in;
        }
        in = new AudioStreamInALSA(this, &(*it), acoustics);
        err = in->set(format, channels, sampleRate, devices);
        if(err == NO_ERROR) {
            mVoipInStreamCount++;   //increment VoipInputstreamCount only if success
            ALOGD("OpenInput mVoipInStreamCount %d",mVoipInStreamCount);
        } else {
            mLock.unlock();
            delete in;
            in = NULL;
            ALOGE("AudioStreamInALSA->set() failed, return NULL");
            mLock.lock();
        }
        ALOGD("openInput: After Get alsahandle");
        if (status) *status = err;
        return in;
      } else {
//XIAOMI_START
WORKAROUND:
//XIAOMI_END
        alsa_handle_t alsa_handle;
        unsigned long bufferSize = MIN_CAPTURE_BUFFER_SIZE_PER_CH;

        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = devices;
        alsa_handle.handle = 0;
        if(*format == AUDIO_FORMAT_PCM_16_BIT)
            alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        else
            alsa_handle.format = *format;
        alsa_handle.channels = VOICE_CHANNEL_MODE;
        alsa_handle.sampleRate = android::AudioRecord::DEFAULT_SAMPLE_RATE;
        alsa_handle.latency = RECORD_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.ucMgr = mUcMgr;
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case != NULL) && (strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AUDIO_MODE_IN_CALL)) {
                ALOGD("openInputStream: into incall recording, channels %d", *channels);
                mIncallMode = *channels;
                if ((*channels & AUDIO_CHANNEL_IN_VOICE_UPLINK) &&
                    (*channels & AUDIO_CHANNEL_IN_VOICE_DNLINK)) {
                    if (mFusion3Platform) {
                        mALSADevice->setVocRecMode(INCALL_REC_STEREO);
                        strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE,
                                sizeof(alsa_handle.useCase));
                    } else {
                        if (*format == AUDIO_FORMAT_AMR_WB) {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_COMPRESSED_VOICE_UL_DL,
                                    sizeof(alsa_handle.useCase));
                        } else {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_UL_DL,
                                    sizeof(alsa_handle.useCase));
                        }
                    }
                } else if (*channels & AUDIO_CHANNEL_IN_VOICE_DNLINK) {
                    if (mFusion3Platform) {
                        mALSADevice->setVocRecMode(INCALL_REC_MONO);
                        strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE,
                                sizeof(alsa_handle.useCase));
                    } else {
                        if (*format == AUDIO_FORMAT_AMR_WB) {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_COMPRESSED_VOICE_DL,
                                    sizeof(alsa_handle.useCase));
                        } else {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_VOICE_DL,
                                    sizeof(alsa_handle.useCase));
                        }
                    }
                }
#if defined(QCOM_FM_ENABLED) || defined(STE_FM)
            } else if((devices == AudioSystem::DEVICE_IN_FM_RX)) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_FM, sizeof(alsa_handle.useCase));
            } else if(devices == AudioSystem::DEVICE_IN_FM_RX_A2DP) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_A2DP_FM, sizeof(alsa_handle.useCase));
#endif
            } else {
                char value[128];
                property_get("persist.audio.lowlatency.rec",value,"0");
                if (!strcmp("true", value)) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_LOWLATENCY_MUSIC, sizeof(alsa_handle.useCase));
                } else if (*format == AUDIO_FORMAT_AMR_WB) {
                    ALOGV("Format AMR_WB, open compressed capture");
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC_COMPRESSED, sizeof(alsa_handle.useCase));
                } else {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, sizeof(alsa_handle.useCase));
                }
            }
        } else {
            if ((devices == AudioSystem::DEVICE_IN_VOICE_CALL) &&
                (newMode == AUDIO_MODE_IN_CALL)) {
                ALOGD("openInputStream: incall recording, channels %d", *channels);
                mIncallMode = *channels;
                if ((*channels & AUDIO_CHANNEL_IN_VOICE_UPLINK) &&
                    (*channels & AUDIO_CHANNEL_IN_VOICE_DNLINK)) {
                    if (mFusion3Platform) {
                        mALSADevice->setVocRecMode(INCALL_REC_STEREO);
                        strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_INCALL_REC,
                                sizeof(alsa_handle.useCase));
                    } else {
                        if (*format == AUDIO_FORMAT_AMR_WB) {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_CAPTURE_COMPRESSED_VOICE_UL_DL,
                                    sizeof(alsa_handle.useCase));
                        } else {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_UL_DL_REC,
                                    sizeof(alsa_handle.useCase));
                        }
                    }
                } else if (*channels & AUDIO_CHANNEL_IN_VOICE_DNLINK) {
                    if (mFusion3Platform) {
                        mALSADevice->setVocRecMode(INCALL_REC_MONO);
                        strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_INCALL_REC,
                                sizeof(alsa_handle.useCase));
                    } else {
                        if (*format == AUDIO_FORMAT_AMR_WB) {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_CAPTURE_COMPRESSED_VOICE_DL,
                                    sizeof(alsa_handle.useCase));
                        } else {
                            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_DL_REC,
                                    sizeof(alsa_handle.useCase));
                        }
                    }
                }
#if defined(QCOM_FM_ENABLED) || defined(STE_FM)
            } else if(devices == AudioSystem::DEVICE_IN_FM_RX) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_FM_REC, sizeof(alsa_handle.useCase));
            } else if (devices == AudioSystem::DEVICE_IN_FM_RX_A2DP) {
                strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_FM_A2DP_REC, sizeof(alsa_handle.useCase));
#endif
            } else {
                char value[128];
                property_get("persist.audio.lowlatency.rec",value,"0");
                if (!strcmp("true", value)) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_LOWLATENCY_REC, sizeof(alsa_handle.useCase));
                } else if (*format == AUDIO_FORMAT_AMR_WB) {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_REC_COMPRESSED, sizeof(alsa_handle.useCase));
                } else {
                    strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_HIFI_REC, sizeof(alsa_handle.useCase));
                }
            }
        }
        free(use_case);
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;
        //update channel info before do routing
        if(channels) {
            it->channels = AudioSystem::popCount((*channels) &
                      (AUDIO_CHANNEL_IN_STEREO
                       | AUDIO_CHANNEL_IN_MONO
#ifdef QCOM_SSR_ENABLED
                       | AUDIO_CHANNEL_IN_5POINT1
#endif
                       ));
            ALOGV("updated channel info: channels=%d", it->channels);
            setInChannels(devices);
        }
        if (devices == AudioSystem::DEVICE_IN_VOICE_CALL){
           /* Add current devices info to devices to do route */
#ifdef QCOM_USBAUDIO_ENABLED
            if(mCurDevice == AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET ||
               mCurDevice == AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET){
                ALOGD("Routing everything from proxy for VOIP call");
                route_devices = devices | AudioSystem::DEVICE_IN_PROXY;
            } else
#endif
            {
            route_devices = devices | mCurDevice;
            }
            mALSADevice->route(&(*it), route_devices, mode());
        } else {
#ifdef QCOM_USBAUDIO_ENABLED
            if(devices & AudioSystem::DEVICE_IN_ANLG_DOCK_HEADSET ||
               devices & AudioSystem::DEVICE_IN_PROXY) {
                devices |= AudioSystem::DEVICE_IN_PROXY;
                ALOGD("routing everything from proxy");
            mALSADevice->route(&(*it), devices, mode());
            } else
#endif
            {
                mALSADevice->route(&(*it), devices, mode());
            }
        }

        if(!strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOWLATENCY_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC_COMPRESSED) ||
#if defined(QCOM_FM_ENABLED) || defined(STE_FM)
           !strcmp(it->useCase, SND_USE_CASE_VERB_FM_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_FM_A2DP_REC) ||
#endif
           !strcmp(it->useCase, SND_USE_CASE_VERB_DL_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_UL_DL_REC) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_CAPTURE_COMPRESSED_VOICE_DL) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_CAPTURE_COMPRESSED_VOICE_UL_DL) ||
           !strcmp(it->useCase, SND_USE_CASE_VERB_INCALL_REC)) {
            snd_use_case_set(mUcMgr, "_verb", it->useCase);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", it->useCase);
        }
        if(sampleRate) {
            it->sampleRate = *sampleRate;
        }
#ifdef CAF_LEGACY_INPUT_BUFFER_SIZE
	if (6 == it->channels) {
#endif
        if (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC, strlen(SND_USE_CASE_VERB_HIFI_REC))
            || !strncmp(it->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC))) {
            ALOGV("OpenInoutStream: getInputBufferSize sampleRate:%d format:%d, channels:%d", it->sampleRate,*format,it->channels);
            it->bufferSize = getInputBufferSize(it->sampleRate,*format,it->channels);
        }
#ifdef CAF_LEGACY_INPUT_BUFFER_SIZE
	}
#endif

#ifdef QCOM_SSR_ENABLED
        if (6 == it->channels) {
            if (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC, strlen(SND_USE_CASE_VERB_HIFI_REC))
                || !strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_REC_COMPRESSED, strlen(SND_USE_CASE_VERB_HIFI_REC_COMPRESSED))
                || !strncmp(it->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC, strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC))
                || !strncmp(it->useCase, SND_USE_CASE_MOD_CAPTURE_MUSIC_COMPRESSED, strlen(SND_USE_CASE_MOD_CAPTURE_MUSIC_COMPRESSED))) {
                //Check if SSR is supported by reading system property
                char ssr_enabled[6] = "false";
                property_get("ro.qc.sdk.audio.ssr",ssr_enabled,"0");
                if (strncmp("true", ssr_enabled, 4)) {
                    if (status) *status = err;
                    ALOGE("openInputStream: FAILED:%d. Surround sound recording is not supported",*status);
                }
            }
        }
#endif
        err = mALSADevice->open(&(*it));
        if (*format == AUDIO_FORMAT_AMR_WB) {
             ALOGV("### Setting bufsize to 61");
             it->bufferSize = 61;
        }
        if (err) {
           ALOGE("Error opening pcm input device");
           mDeviceList.erase(it);
        } else {
           in = new AudioStreamInALSA(this, &(*it), acoustics);
           err = in->set(format, channels, sampleRate, devices);
        }
        if (status) *status = err;
        return in;
      }
}

void
AudioHardwareALSA::closeInputStream(AudioStreamIn* in)
{
    delete in;
}

//XIAOMI_START
#define BUFSIZE_UART 1024
#define VOICEPROC_MAX_FW_SIZE	(32 * 4096)

static ES310_PathID dwOldPath = ES310_PATH_SUSPEND;
static ES310_PathID dwNewPath = ES310_PATH_SUSPEND;
static int AudiencePrevMode = AudioSystem::MODE_NORMAL;
static unsigned int dwOldPreset = -1;
static unsigned int dwNewPreset = -1;

//Call this API after enabling the Audience, Also call this API before disabling the Audience
void AudioHardwareALSA::enableAudienceloopback(int enable)
{
    if (mAudienceCodecInit != 1) {
        ALOGE("Audience Codec not initialized.\n");
        return;
    }

    if (mLoopbackState == enable)
        return;

    ALOGV("enableAudienceloopback enable:%d", enable);
    if(enable ==1 ) {
        mALSADevice->setMixerControl("IIR1 INP1 MUX", "DEC2");
    }
    else {
        mALSADevice->setMixerControl("RX1 Digital Volume",1,0);
        mALSADevice->setMixerControl("RX2 Digital Volume",1,0);
        mALSADevice->setMixerControl("RX3 Digital Volume",1,0);
        mALSADevice->setMixerControl("RX4 Digital Volume",1,0);
        mALSADevice->setMixerControl("IIR1 INP1 MUX", "ZERO");
    }

    mLoopbackState = enable;
}

static void setHigherBaudrate(int uart_fd, int baud)
{
	struct termios2 ti2;
       struct termios ti;
	/* Flush non-transmitted output data,
	 * non-read input data or both
	 */
	tcflush(uart_fd, TCIFLUSH);

	/* Set the UART flow control */

	ti.c_cflag |= 1;

	/* ti.c_cflag |= (CLOCAL | CREAD | CSTOPB); */
	/*	ti.c_cflag &= ~(CRTSCTS | PARENB); */

	/*
	 * Enable the receiver and set local mode + 2 STOP bits
	 */
	ti.c_cflag |= (CLOCAL | CREAD | CSTOPB);
	/* 8 data bits */
	ti.c_cflag &= ~CSIZE;
	ti.c_cflag |= CS8;
	/* diable HW flow control and parity check */
	ti.c_cflag &= ~(CRTSCTS | PARENB);

	/* choose raw input */
	ti.c_lflag &= ~(ICANON | ECHO);
	/* choose raw output */
	ti.c_oflag &= ~OPOST;
	/* ignore break condition, CR and parity error */
	ti.c_iflag |= (IGNBRK | IGNPAR);
	ti.c_iflag &= ~(IXON | IXOFF | IXANY);
	ti.c_cc[VMIN] = 0;
	ti.c_cc[VTIME] = 10;

	/*
	 * Set the parameters associated with the UART
	 * The change will occur immediately by using TCSANOW
	 */
	if (tcsetattr(uart_fd, TCSANOW, &ti) < 0) {
		printf("Can't set port settings\n");
		return;
	}

	tcflush(uart_fd, TCIFLUSH);

	/* Set the actual baud rate */
	ioctl(uart_fd, TCGETS2, &ti2);
	ti2.c_cflag &= ~CBAUD;
	ti2.c_cflag |= BOTHER;
	ti2.c_ospeed = baud;
	ti2.c_ispeed = baud;
	ioctl(uart_fd, TCSETS2, &ti2);
}

int sendDownloadCmd(int uart_fd)
{
	unsigned char respBuffer = 0xcc, tmp;
	int nretry = 10, rc;
	int BytesWritten, readCnt;

	tmp = 0x00;
	BytesWritten = write(uart_fd, &tmp, 1);
	if (BytesWritten == -1)
		ALOGE("error writing synccmd to comm port: %s\n", strerror(errno));
	else
		ALOGE("Uart_write BytesWritten = %i\n", BytesWritten);

	usleep(1000);
	readCnt = read(uart_fd, &respBuffer, 1);
	if (readCnt == -1)
		ALOGE("error reading bootcmd from comm port: %s\n", strerror(errno));
	else
		ALOGE("readCnt = %d, respBuffer = %.2x\n", readCnt, respBuffer);
	usleep(1000);

	tmp = 0x01;
	BytesWritten = write(uart_fd, &tmp, 1);
	if (BytesWritten == -1)
		ALOGE("error writing bootcmd to comm port: %s\n", strerror(errno));
	else
		ALOGE("Uart_write BytesWritten = %i\n", BytesWritten);

	usleep(1000);
	readCnt = read(uart_fd, &respBuffer, 1);
	if (readCnt == -1)
		ALOGE("error reading bootcmd from comm port: %s\n", strerror(errno));
	else
		ALOGE("readCnt = %d, respBuffer = %.2x\n", readCnt, respBuffer);

	if (respBuffer == 1)
		return 0;

	return -1;
}

int uartSendBinaryFile(int uart_fd, const char* img)
{
	int ret = -1, write_size;
	int i = 0;
	ALOGE("voiceproc_uart_sendImg %s\n", img);
	struct voiceproc_img fwimg;
	char char_tmp = 0;
	unsigned char local_vpimg_buf[VOICEPROC_MAX_FW_SIZE], *ptr = local_vpimg_buf;
	int rc = 0, fw_fd = -1;
	ssize_t nr;
	size_t remaining;
	struct stat fw_stat;

	fw_fd = open(img, O_RDONLY);
	if (fw_fd < 0) {
		ALOGE("Fail to open %s\n", img);
              rc = -1;
		goto ld_img_error;
	}

	rc = fstat(fw_fd, &fw_stat);
	if (rc < 0) {
		ALOGE("Cannot stat file %s: %s\n", img, strerror(errno));
              rc = -1;
		goto ld_img_error;
	}

	remaining = (int)fw_stat.st_size;

	ALOGV("Firmware %s size %d\n", img, remaining);

	if (remaining > sizeof(local_vpimg_buf)) {
		ALOGE("File %s size %d exceeds internal limit %d\n",
			 img, remaining, sizeof(local_vpimg_buf));
              rc = -1;
		goto ld_img_error;
	}

	while (remaining) {
		nr = read(fw_fd, ptr, remaining);
		if (nr < 0) {
			ALOGE("Error reading firmware: %s\n", strerror(errno));
                     rc=-1;
			goto ld_img_error;
		} else if (!nr) {
			if (remaining)
				ALOGV("EOF reading firmware %s while %d bytes remain\n",
					 img, remaining);
			break;
		}
		remaining -= nr;
		ptr += nr;
	}

	close (fw_fd);
	fw_fd = -1;

	fwimg.buf = local_vpimg_buf;
	fwimg.img_size = (int)(fw_stat.st_size - remaining);
	ALOGV("voiceproc_uart_sendImg firmware Total %d bytes\n", fwimg.img_size);
	i = 0;
	write_size = 0;

	while (i < fwimg.img_size) {
		ret = write(uart_fd, fwimg.buf+i,
			(fwimg.img_size - i) < BUFSIZE_UART ? (fwimg.img_size-i) : BUFSIZE_UART);
		if (ret == -1)
              {
			ALOGV("Error, voiceproc uart write: %s\n", strerror(errno));
                     rc = -1;
                     goto ld_img_error;
              }

		write_size += ret;
		i += BUFSIZE_UART;
	}
	if (write_size != fwimg.img_size)
       {
		ALOGE("Error, UART writeCnt %d != img_size %d\n", write_size, fwimg.img_size);
              rc = -1;
              goto ld_img_error;
       }
	else
		ALOGV("UART writeCnt is %d verus img_size is %d\n", write_size, fwimg.img_size);

ld_img_error:
	if (fw_fd >= 0)
		close(fw_fd);
	return rc;
}

#define UART_INIT_IMAGE "/system/etc/firmware/voiceproc_init.img"
#define UART_DEV_NAME "/dev/ttyHS2"
int uart_load_binary(int fd, char *firmware_path)
{
    int ret = 0;
    int uart_fd = 0;
    int i;
    int retry_count = 100;
    struct termios options;
    struct termios2 options2;

    while (retry_count) {
        ret = ioctl(fd, ES310_RESET_CMD);
        if (!ret)
            ALOGV("ES310: voiceproc_reset ES310_RESET_CMD OK\n");
        else
            ALOGE("ES310: voiceproc_reset ES310_RESET_CMD error %s\n", strerror(errno));

        /* init uart port */
        uart_fd = open(UART_DEV_NAME, O_RDWR | O_NOCTTY | O_NDELAY);
        if (uart_fd < 0) {
            ALOGE("fail to open uart port %s\n", UART_DEV_NAME);
            return -1;
        }
        fcntl(uart_fd, F_SETFL, 0);

        /* First stage download */
        setHigherBaudrate(uart_fd, 28800);

        /* reset voice processor */
        usleep(10000);

        ret = sendDownloadCmd(uart_fd);
        if (ret) {
            ALOGE("error sending 1st download command on 1st stage\n");
            retry_count--;
            close(uart_fd);
            continue;
        }

        uartSendBinaryFile(uart_fd, UART_INIT_IMAGE);
        ALOGV("Send init image done\n");

        /* Second stage download */
        if (tcgetattr(uart_fd, &options) < 0) {
            ALOGE("Can't get port settings\n");
            retry_count--;
            close(uart_fd);
            continue;
        } else if (tcsetattr(uart_fd, TCSADRAIN, &options) < 0) {
            ALOGE("Can't set port settings\n");
            retry_count--;
            close(uart_fd);
            continue;
        }
        if (ioctl(uart_fd, TCGETS2, &options2) < 0) {
            ALOGE("Can't get port settings 2\n");
            retry_count--;
            close(uart_fd);
            continue;
        } else {
            options2.c_cflag &= ~CBAUD;
            options2.c_cflag |= BOTHER;
            options2.c_ospeed = 3000000;
            options2.c_ispeed = 3000000;
            if (ioctl(uart_fd, TCSETS2, &options2) < 0) {
                ALOGE("Can't set port settings 2\n");
                retry_count--;
                close(uart_fd);
                continue;
            }
        }

        usleep(10000);
        ret = sendDownloadCmd(uart_fd);
        if (ret) {
            ALOGE("ES310: error sending download command, abort. \n");
            ALOGE("ES310: retry_count:%d", 100 - retry_count);
            retry_count--;
            close(uart_fd);
            continue;
        } else {
            ALOGV("ES310: init send command done");
            break;
        }
    }

    if (ret) {
        ALOGE("ES310: initial codec command error");
        ret = -1;
        goto ERROR;
    }

    usleep(1000);
    ret = uartSendBinaryFile(uart_fd, firmware_path);

ERROR:
    close(uart_fd);

    return ret;
}

void *AudioHardwareALSA::CSDInitThreadWrapper(void *me) {
    ALOGV("AudioHardwareALSA::CSDInitThread+");
    csd_client_init();
    ALOGV("AudioHardwareALSA::CSDInitThread-");
    return NULL;
}

void *AudioHardwareALSA::AudienceThreadWrapper(void *me) {
    static_cast<AudioHardwareALSA *>(me)->AudienceThreadEntry();
    return NULL;
}

void AudioHardwareALSA::AudienceThreadEntry() {
    ALOGV("AudioHardwareALSA::AudienceThreadEntry +");
    pid_t tid  = gettid();
    int err;
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    mAudienceCmd = CMD_AUDIENCE_READY;
    while(!mKillAudienceThread) {
        switch (mAudienceCmd)
        {
            case CMD_AUDIENCE_WAKEUP:
            {
                ALOGV("AudioHardwareALSA::AudienceThreadEntry, doAudienceCodec_Wakeup");
                err = doAudienceCodec_Wakeup();
                if (err < 0) {
                    ALOGE("doAudienceCodec_Wakeup: error %d\n", err);
                }
                break;
            }
            default:
                mAudienceCmd = CMD_AUDIENCE_READY;
        }
        mAudienceCmd = CMD_AUDIENCE_READY;
        pthread_mutex_lock(&mAudienceMutex);
        pthread_cond_wait(&mAudienceCV, &mAudienceMutex);
        pthread_mutex_unlock(&mAudienceMutex);
        ALOGV("Audience command:%d", mAudienceCmd);
        continue;
    }
    ALOGV("ALSADevice::csdThreadEntry -");
}

status_t AudioHardwareALSA::doAudienceCodec_Init(void)
{
    int fd_codec = -1;
    int rc = 0;
    int Audio_path = ES310_PATH_SUSPEND;
    int retry_count = 20;
    static const char *const path = "/dev/audience_es310";

    fd_codec = open("/dev/audience_es310", O_RDWR | O_NONBLOCK, 0);

    if (fd_codec < 0) {
        ALOGE("Cannot open %s %d.\n", path, fd_codec);
            return fd_codec;
    }

    while(retry_count)
    {
        ALOGV("start loading the voiceproc.img file, retry:%d +", 20 - retry_count);
        ALOGV("set codec reset command");
        rc = uart_load_binary(fd_codec, "/etc/firmware/voiceproc.img");
        if (rc != 0)
        {
            ALOGE("uart_load_binary fail, rc:%d", rc);
            retry_count--;
            continue;
        }
        ALOGV("start loading the voiceproc.img file -");
        usleep(11000);
        ALOGV("ES310 SYNC CMD +");
        rc = ioctl(fd_codec, ES310_SYNC_CMD, NULL);
        ALOGV("ES310 SYNC CMD, rc:%d-", rc);
        if (rc != 0)
        {
            ALOGE("ES310 SYNC CMD fail, rc:%d", rc);
            retry_count--;
            continue;
        }

        if (rc == 0)
            break;
    }

    pthread_mutex_init(&mAudienceMutex, NULL);
    pthread_cond_init (&mAudienceCV, NULL);
    mKillAudienceThread = false;
    mAudienceCmd = CMD_AUDIENCE_READY;
    ALOGV("Creating Audience Thread");
    pthread_create(&AudienceThread, NULL, AudienceThreadWrapper, this);

    if (rc == 0) {
        ALOGV("audience codec init OK\n");
        mAudienceCodecInit = 1;
    } else
        ALOGE("audience codec init failed\n");

    close(fd_codec);
    return rc;
}

status_t AudioHardwareALSA::doAudienceCodec_DeInit(void)
{
    mKillAudienceThread = true;
    pthread_cond_signal(&mAudienceCV);
    pthread_join(AudienceThread,NULL);
    ALOGV("Audience Thread Killed");
    return 0;
}

status_t AudioHardwareALSA::doAudienceCodec_Wakeup()
{
    int fd_codec = -1;
    int retry = 4;
    int rc = 0;
    ALOGV("Pre Wakeup Audience Codec ++");
    Mutex::Autolock lock(mAudioCodecLock);
    if (fd_codec < 0) {
        fd_codec = open("/dev/audience_es310", O_RDWR);
        if (fd_codec < 0) {
            ALOGE("Cannot open either audience_es310 device (%d)\n", fd_codec);
            return -1;
        }
    }

    retry = 4;
    do {
        ALOGV("ioctl ES310_SET_CONFIG retry:%d", 4-retry);
        rc = ioctl(fd_codec, ES310_WAKEUP_CMD, NULL);
        if (rc == 0) {
            break;
        }
        else {
            ALOGE("ERROR: ES310_SET_CONFIG rc=%d", rc);
        }
    } while (--retry);

    close(fd_codec);
    fd_codec = -1;

    ALOGV("Pre Wakeup Audience Codec --");
    return rc;
}

status_t AudioHardwareALSA::doRouting_Audience_Codec(int mode, int device, bool enable)
{
    int rc = 0;
    int retry = 4;
    int fd_codec = -1;
    bool bVideoRecord_NS = false;
    bool bPresetAgain = false;
    bool bForcePathAgain = false;
    bool bVRMode = false;
    char cVRMode[255]="0";

    ALOGD("doRouting_Audience_Codec mode:%d Routes:0x%x Enable:%d.\n", mode, device, enable);

    if (mAudienceCodecInit != 1) {
        ALOGE("Audience Codec not initialized.\n");
        return -1;
    }

    Mutex::Autolock lock(mAudioCodecLock);
    if ((mode < AudioSystem::MODE_CURRENT) || (mode >= AudioSystem::NUM_MODES)) {
        ALOGW("Illegal value: doRouting_Audience_Codec(%d, 0x%x, %d)", mode, device, enable);
        return -1;
    }

    if (enable == 0)
    {
        dwNewPath = ES310_PATH_SUSPEND;
        goto ROUTE;
    }

    if (((device & AUDIO_DEVICE_OUT_ALL) != 0) && (mode == AudioSystem::MODE_NORMAL))
    {
        ALOGV("doRouting_Audience_Codec: Normal mode, RX no routing ");
        return 0;
    }

    property_get("audio.record.vrmode",cVRMode,"0");
    if (!strncmp("1", cVRMode, 1)) {
        bVRMode = 1;
    }
    else {
        bVRMode = 0;
    }
    ALOGV("doRouting_Audience_Codec  -> VRMode:%d", bVRMode);

    if (mode == AudioSystem::MODE_IN_CALL ||
         mode == AudioSystem::MODE_RINGTONE) {
        switch (device & AudioSystem::DEVICE_OUT_ALL) {
            case AudioSystem::DEVICE_OUT_EARPIECE:
                 dwNewPath = ES310_PATH_HANDSET;
                 dwNewPreset = ES310_PRESET_HANDSET_INCALL_NB;
                 break;
            case AudioSystem::DEVICE_OUT_SPEAKER:
                 dwNewPath = ES310_PATH_HANDSFREE;
                 dwNewPreset = ES310_PRESET_HANDSFREE_INCALL_NB;
                 break;
            case AudioSystem::DEVICE_OUT_WIRED_HEADSET:
                 dwNewPath = ES310_PATH_HEADSET;
                 dwNewPreset = ES310_PRESET_HEADSET_INCALL_NB;
                 break;
            case AudioSystem::DEVICE_OUT_WIRED_HEADPHONE:
                 dwNewPath = ES310_PATH_HANDSET;
                 dwNewPreset = ES310_PRESET_HANDSET_INCALL_NB;
                 break;
            case AudioSystem::DEVICE_OUT_BLUETOOTH_SCO:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER:
                 dwNewPath = ES310_PATH_HANDSET;
                 dwNewPreset = ES310_PRESET_HANDSET_INCALL_NB;
                 break;
            case AudioSystem::DEVICE_OUT_AUX_DIGITAL:
            case AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET:
            case AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET:
                 dwNewPath = ES310_PATH_HEADSET;
                 dwNewPreset = ES310_PRESET_HEADSET_INCALL_NB;
                 break;
            default:
                 dwNewPath = ES310_PATH_HANDSET;
                 dwNewPreset = ES310_PRESET_HANDSET_INCALL_NB;
                 break;
        }
        goto ROUTE;
    }
    else if (mode == AudioSystem::MODE_IN_COMMUNICATION) {
        switch (device & AudioSystem::DEVICE_OUT_ALL) {
            case AudioSystem::DEVICE_OUT_EARPIECE:
                 dwNewPath = ES310_PATH_HANDSET;
                 dwNewPreset = ES310_PRESET_HANDSET_VOIP_WB;
                 break;
            case AudioSystem::DEVICE_OUT_SPEAKER:
                 dwNewPath = ES310_PATH_HANDSFREE;
                 dwNewPreset = ES310_PRESET_HANDSFREE_VOIP_WB;
                 break;
            case AudioSystem::DEVICE_OUT_WIRED_HEADSET:
                 dwNewPath = ES310_PATH_HEADSET;
                 dwNewPreset = ES310_PRESET_HEADSET_VOIP_WB;
                 break;
            case AudioSystem::DEVICE_OUT_WIRED_HEADPHONE:
                 dwNewPath = ES310_PATH_HANDSET;
                 dwNewPreset = ES310_PRESET_HANDSET_VOIP_WB;
                 break;
            case AudioSystem::DEVICE_OUT_BLUETOOTH_SCO:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_HEADSET:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_SCO_CARKIT:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES:
            case AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER:
                 dwNewPath = ES310_PATH_HANDSET;
                 dwNewPreset = ES310_PRESET_HANDSET_VOIP_WB;
                 break;
            case AudioSystem::DEVICE_OUT_AUX_DIGITAL:
            case AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET:
            case AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET:
                 dwNewPath = ES310_PATH_HEADSET;
                 dwNewPreset = ES310_PRESET_HEADSET_VOIP_WB;
                 break;
            default:
                 dwNewPath = ES310_PATH_HANDSET;
                 dwNewPreset = ES310_PRESET_HANDSET_VOIP_WB;
                 break;
        }
        goto ROUTE;
    }
    else {
        switch (device & AudioSystem::DEVICE_IN_ALL)
        {
                //TX
                case AudioSystem::DEVICE_IN_COMMUNICATION:
                     dwNewPath = ES310_PATH_HANDSFREE;
                     dwNewPreset = ES310_PRESET_HANDSFREE_REC_WB;
                     break;
                case AudioSystem::DEVICE_IN_AMBIENT:
                     dwNewPath = ES310_PATH_HANDSFREE;
                     dwNewPreset = ES310_PRESET_HANDSFREE_REC_WB;
                     break;
                case AudioSystem::DEVICE_IN_BUILTIN_MIC:
                     {
                         dwNewPath = ES310_PATH_HANDSET;
                         dwNewPreset = ES310_PRESET_HANDSFREE_REC_WB;
                         char process[128];
                         property_get("sys.foreground_process", process, "");
                         if (!strcmp(process, "com.eg.android.AlipayGphone")) {
                             ALOGV("Alipay, using bypass mode");
                             dwNewPreset = ES310_PRESET_ANALOG_BYPASS;
                         }
                     }
                     if (bVRMode)
                     {
                         dwNewPreset = ES310_PRESET_VOICE_RECOGNIZTION_WB;
                     }
                     break;
                case AudioSystem::DEVICE_IN_BLUETOOTH_SCO_HEADSET:
                     dwNewPath = ES310_PATH_HEADSET;
                     dwNewPreset = ES310_PRESET_HANDSFREE_REC_WB;
                     break;
                case AudioSystem::DEVICE_IN_WIRED_HEADSET:
                     dwNewPath = ES310_PATH_HEADSET;
                     dwNewPreset = ES310_PRESET_HEADSET_REC_WB;
                     char process[128];
                     property_get("sys.foreground_process", process, "");
                     if (!strcmp(process, "com.jawbone.up")) {
                         ALOGV("jawbone, using bypass mode");
                         dwNewPreset = ES310_PRESET_HEADSET_MIC_ANALOG_BYPASS;
                     }
                     break;
                case AudioSystem::DEVICE_IN_AUX_DIGITAL:
                case AudioSystem::DEVICE_IN_VOICE_CALL:
                case AudioSystem::DEVICE_IN_BACK_MIC:
                     dwNewPath = ES310_PATH_HANDSET;
                     dwNewPreset = ES310_PRESET_HANDSFREE_REC_WB;
                     break;
                default:
                     dwNewPath = ES310_PATH_HANDSET;
                     dwNewPreset = ES310_PRESET_HANDSFREE_REC_NB;
                     break;
        }
    }

ROUTE:

    ALOGV("doRouting_Audience_Codec: dwOldPath=%d, dwNewPath=%d, prevMode=%d, mode=%d",
                dwOldPath, dwNewPath, AudiencePrevMode, mode);

    if (AudiencePrevMode != mode)
    {
        bForcePathAgain = true;
        AudiencePrevMode = mode;
    }

    if (bForcePathAgain ||
        (dwOldPath != dwNewPath) ||
        (dwOldPreset != dwNewPreset)) {

        if (fd_codec < 0) {
            fd_codec = open("/dev/audience_es310", O_RDWR);
            if (fd_codec < 0) {
                ALOGE("Cannot open either audience_es310 device (%d)\n", fd_codec);
                return -1;
            }
        }

        if (bForcePathAgain ||
            (dwOldPath != dwNewPath)) {
            bPresetAgain = true;
            retry = 4;
            do {
                ALOGV("ioctl ES310_SET_CONFIG newPath:%d, retry:%d", dwNewPath, (4-retry));
                rc = ioctl(fd_codec, ES310_SET_CONFIG, &dwNewPath);

                if (rc == 0) {
                    dwOldPath = dwNewPath;
                    break;
                }
                else
                {
                    ALOGE("ERROR: ES310_SET_CONFIG rc=%d", rc);
                }
            } while (--retry);
            /*Close driver first incase we need to do audience HW reset when ES310_SET_CONFIG failed.*/
        }

        if (bPresetAgain)
            ALOGV("doRouting_Audience_Codec: dwOldPreset:%s, dwNewPreset:%s", getNameByPresetID(dwOldPreset), getNameByPresetID(dwNewPreset));

        if (bPresetAgain && (dwNewPath != ES310_PATH_SUSPEND)) {
            retry = 4;
            do {
                ALOGV("ioctl ES310_SET_PRESET newPreset:0x%x, retry:%d", dwNewPreset, (4-retry));
                rc = ioctl(fd_codec, ES310_SET_PRESET, &dwNewPreset);

                if (rc == 0) {
                    dwOldPreset = dwNewPreset;
                    break;
                }
                else
                {
                    ALOGE("ERROR: ES310_SET_PRESET rc=%d", rc);
                }
            } while (--retry);
            /*Close driver first incase we need to do audience HW reset when ES310_SET_CONFIG failed.*/
        }

        close(fd_codec);
        fd_codec = -1;

RECOVER:
        if (rc < 0) {
            ALOGE("E310 do hard reset to recover from error!\n");
            rc = doAudienceCodec_Init(); /* A1026 needs to do hard reset! */
            if (!rc) {
                fd_codec = open("/dev/audience_es310", O_RDWR);
                if (fd_codec >= 0) {
                    rc = ioctl(fd_codec, ES310_SET_CONFIG, &dwNewPath);
                    if (rc == NO_ERROR)
                        dwOldPath = dwNewPath;
                    else
                        ALOGE("Audience Codec Fatal Error! rc %d\n", rc);
                    close(fd_codec);
                } else
                    ALOGE("Audience Codec Fatal Error: Re-init Audience Codec open driver fail!! rc %d\n", fd_codec);
            } else
                ALOGE("Audience Codec Fatal Error: Re-init A1026 Failed. rc %d\n", rc);
        }
    }

    return NO_ERROR;
}
char* AudioHardwareALSA::getNameByPresetID(int presetID)
{
     switch(presetID){
        case ES310_PRESET_HANDSET_INCALL_NB:
              return "ES310_PRESET_HANDSET_INCALL_NB";
        case ES310_PRESET_HEADSET_INCALL_NB:
              return "ES310_PRESET_HEADSET_INCALL_NB";
        case ES310_PRESET_HANDSFREE_REC_NB:
              return "ES310_PRESET_HANDSFREE_REC_NB";
        case ES310_PRESET_HANDSFREE_INCALL_NB:
              return "ES310_PRESET_HANDSFREE_INCALL_NB";
        case ES310_PRESET_HANDSET_INCALL_WB:
              return "ES310_PRESET_HANDSET_INCALL_WB";
        case ES310_PRESET_HEADSET_INCALL_WB:
              return "ES310_PRESET_HEADSET_INCALL_WB";
        case ES310_PRESET_AUDIOPATH_DISABLE:
              return "ES310_PRESET_AUDIOPATH_DISABLE";
        case ES310_PRESET_HANDSFREE_INCALL_WB:
              return "ES310_PRESET_HANDSFREE_INCALL_WB";
        case ES310_PRESET_HANDSET_VOIP_WB:
              return "ES310_PRESET_HANDSET_VOIP_WB";
        case ES310_PRESET_HEADSET_VOIP_WB:
              return "ES310_PRESET_HEADSET_VOIP_WB";
        case ES310_PRESET_HANDSFREE_REC_WB:
              return "ES310_PRESET_HANDSFREE_REC_WB";
        case ES310_PRESET_HANDSFREE_VOIP_WB:
              return "ES310_PRESET_HANDSFREE_VOIP_WB";
        case ES310_PRESET_VOICE_RECOGNIZTION_WB:
              return "ES310_PRESET_VOICE_RECOGNIZTION_WB";
        case ES310_PRESET_HEADSET_REC_WB:
              return "ES310_PRESET_HEADSET_REC_WB";
        default:
            return "Unknown";
     }
     return "Unknown";
}
//XIAOMI_END

status_t AudioHardwareALSA::setMicMute(bool state)
{
    int newMode = mode();
    ALOGD("setMicMute  newMode %d state:%d",newMode,state);
    if(newMode == AUDIO_MODE_IN_COMMUNICATION) {
        if (mVoipMicMute != state) {
             mVoipMicMute = state;
            ALOGD("setMicMute: mVoipMicMute %d", mVoipMicMute);
            if(mALSADevice) {
                mALSADevice->setVoipMicMute(state);
            }
        }
    } else {
        if (mMicMute != state) {
              mMicMute = state;
              ALOGD("setMicMute: mMicMute %d", mMicMute);
              if(mALSADevice) {
                 if(mVoiceCallState == CALL_ACTIVE ||
                    mVoiceCallState == CALL_LOCAL_HOLD)
                     mALSADevice->setMicMute(state);

                 if(mVoice2CallState == CALL_ACTIVE ||
                    mVoice2CallState == CALL_LOCAL_HOLD)
                     mALSADevice->setVoice2MicMute(state);

                 if(mVolteCallState == CALL_ACTIVE ||
                    mVolteCallState == CALL_LOCAL_HOLD)
                     mALSADevice->setVoLTEMicMute(state);
              }
        }
    }
    return NO_ERROR;
}

status_t AudioHardwareALSA::getMicMute(bool *state)
{
    int newMode = mode();
    if(newMode == AUDIO_MODE_IN_COMMUNICATION) {
        *state = mVoipMicMute;
    } else {
        *state = mMicMute;
    }
    return NO_ERROR;
}

status_t AudioHardwareALSA::dump(int fd, const Vector<String16>& args)
{
    return NO_ERROR;
}

size_t AudioHardwareALSA::getInputBufferSize(uint32_t sampleRate, int format, int channelCount)
{
    size_t bufferSize = 0;
    if (format == AUDIO_FORMAT_PCM_16_BIT
#ifdef QCOM_AUDIO_FORMAT_ENABLED
        || format == AUDIO_FORMAT_EVRC
        || format == AUDIO_FORMAT_EVRCB
        || format == AUDIO_FORMAT_EVRCWB
#endif
        || format == AUDIO_FORMAT_AMR_NB
        || format == AUDIO_FORMAT_AMR_WB) {
        if(sampleRate == 8000 || sampleRate == 16000 || sampleRate == 32000) {
#ifdef TARGET_8974
            bufferSize = DEFAULT_IN_BUFFER_SIZE;
#else
            bufferSize = (sampleRate * channelCount * 20 * sizeof(int16_t)) / 1000;
#endif
        } else if (sampleRate == 11025 || sampleRate == 12000) {
            bufferSize = 256 * sizeof(int16_t)  * channelCount;
        } else if (sampleRate == 22050 || sampleRate == 24000) {
            bufferSize = 512 * sizeof(int16_t)  * channelCount;
        } else if (sampleRate == 44100 || sampleRate == 48000) {
            bufferSize = 1024 * sizeof(int16_t) * channelCount;
        }
    } else {
        bufferSize = DEFAULT_IN_BUFFER_SIZE * channelCount;
        ALOGE("getInputBufferSize bad format: %x use default input buffersize:%d", format, bufferSize);
    }
    return bufferSize;
}

#ifdef QCOM_FM_ENABLED
void AudioHardwareALSA::handleFm(int device)
{
    int newMode = mode();
    uint32_t activeUsecase = USECASE_NONE;
    char ident[70];
    int tx_dev_id, capability;

    if(device & AUDIO_DEVICE_OUT_FM && mIsFmActive == 0) {
        // Start FM Radio on current active device
        unsigned long bufferSize = FM_BUFFER_SIZE;
        alsa_handle_t alsa_handle;
        char *use_case;
        ALOGV("Start FM");
        snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
        if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_VERB_DIGITAL_RADIO, sizeof(alsa_handle.useCase));
        } else {
            strlcpy(alsa_handle.useCase, SND_USE_CASE_MOD_PLAY_FM, sizeof(alsa_handle.useCase));
        }
        free(use_case);

        for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
        bufferSize &= ~b;
        alsa_handle.module = mALSADevice;
        alsa_handle.bufferSize = bufferSize;
        alsa_handle.devices = device;
        alsa_handle.handle = 0;
        alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
        alsa_handle.channels = DEFAULT_CHANNEL_MODE;
        alsa_handle.sampleRate = DEFAULT_SAMPLING_RATE;
        alsa_handle.latency = VOICE_LATENCY;
        alsa_handle.rxHandle = 0;
        alsa_handle.ucMgr = mUcMgr;
        mIsFmActive = 1;
        mDeviceList.push_back(alsa_handle);
        ALSAHandleList::iterator it = mDeviceList.end();
        it--;

        // Get the FM device ACDB ID and capability
        memset(&ident,0,sizeof(ident));
        strlcpy(ident, "ACDBID/", sizeof(ident));
        strlcat(ident, "FM TX PREPROC", sizeof(ident));
        tx_dev_id = snd_use_case_get(alsa_handle.ucMgr, ident, NULL);

        memset(&ident,0,sizeof(ident));
        strlcpy(ident, "CAPABILITY/", sizeof(ident));
        strlcat(ident, "FM TX PREPROC", sizeof(ident));
        capability = snd_use_case_get(alsa_handle.ucMgr, ident, NULL);

        if ((tx_dev_id < 0) || (capability < 0)) {
            ALOGD("No pre-proc calibration applied on receiving FM stream\n");
        } else {
            if (mAcdbHandle == NULL) {
                ALOGE("mAcdbHandle is NULL\n");
            } else {
                acdb_send_audio_cal =
                    (void (*)(int, int))::dlsym(mAcdbHandle,"acdb_loader_send_audio_cal");
                if (acdb_send_audio_cal == NULL) {
                    ALOGE("acdb_send_audio_cal is NULL\n");
                } else {
                    acdb_send_audio_cal(tx_dev_id, capability);
                }
            }
        }
        mALSADevice->route(&(*it), (uint32_t)device, newMode);
        if(!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) {
            snd_use_case_set(mUcMgr, "_verb", SND_USE_CASE_VERB_DIGITAL_RADIO);
        } else {
            snd_use_case_set(mUcMgr, "_enamod", SND_USE_CASE_MOD_PLAY_FM);
        }
        mALSADevice->startFm(&(*it));
        activeUsecase = useCaseStringToEnum(it->useCase);
#ifdef QCOM_USBAUDIO_ENABLED
        if((device & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
           (device & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
            ALOGD("Starting FM, musbPlaybackState %d", musbPlaybackState);
            startUsbPlaybackIfNotStarted();
            musbPlaybackState |= USBPLAYBACKBIT_FM;
        }
#endif
        if(isExtOutDevice(device)) {
            status_t err = NO_ERROR;
            mRouteAudioToExtOut = true;
            err = startPlaybackOnExtOut_l(activeUsecase);
            if(err) {
                ALOGE("startPlaybackOnExtOut_l for hardware output failed err = %d", err);
                stopPlaybackOnExtOut_l(activeUsecase);
            }
        }

    } else if (!(device & AUDIO_DEVICE_OUT_FM) && mIsFmActive == 1) {
        //i Stop FM Radio
        ALOGV("Stop FM");
        for(ALSAHandleList::iterator it = mDeviceList.begin();
            it != mDeviceList.end(); ++it) {
            if((!strcmp(it->useCase, SND_USE_CASE_VERB_DIGITAL_RADIO)) ||
              (!strcmp(it->useCase, SND_USE_CASE_MOD_PLAY_FM))) {
                mALSADevice->close(&(*it));
                activeUsecase = useCaseStringToEnum(it->useCase);
                //mALSADevice->route(&(*it), (uint32_t)device, newMode);
                mDeviceList.erase(it);
                break;
            }
        }
        mIsFmActive = 0;
#ifdef QCOM_USBAUDIO_ENABLED
        musbPlaybackState &= ~USBPLAYBACKBIT_FM;
        if((device & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
           (device & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
            closeUsbPlaybackIfNothingActive();
        }
#endif
        if(mRouteAudioToExtOut == true) {
            status_t err = NO_ERROR;
            err = stopPlaybackOnExtOut_l(activeUsecase);
            if(err)
                ALOGE("stopPlaybackOnExtOut_l for hardware output failed err = %d", err);
        }

    }
}
#endif

void AudioHardwareALSA::disableVoiceCall(int mode, int device, uint32_t vsid)
{
    char *verb = getUcmVerbForVSID(vsid);
    char *modifier = getUcmModForVSID(vsid);

    if (verb == NULL || modifier == NULL) {
        ALOGE("%s(): Error, verb=%p or modifier=%p is NULL",
              __func__, verb, modifier);

            return;
    }

    for(ALSAHandleList::iterator it = mDeviceList.begin();
         it != mDeviceList.end(); ++it) {
        if((!strncmp(it->useCase, verb,
                     MAX(strlen(verb), strlen(it->useCase)))) ||
           (!strncmp(it->useCase, modifier,
                     MAX(strlen(modifier), strlen(it->useCase))))) {
            ALOGV("Disabling voice call vsid:%x", vsid);
            mALSADevice->setInChannels(0);
            mALSADevice->close(&(*it), vsid);
            mALSADevice->route(&(*it), (uint32_t)device, mode);
            mDeviceList.erase(it);
            break;
        }
    }

#ifdef QCOM_USBAUDIO_ENABLED
   if(musbPlaybackState & USBPLAYBACKBIT_VOICECALL) {
          ALOGD("Voice call ended on USB");
          musbPlaybackState &= ~USBPLAYBACKBIT_VOICECALL;
          musbRecordingState &= ~USBRECBIT_VOICECALL;
          closeUsbRecordingIfNothingActive();
          closeUsbPlaybackIfNothingActive();
   }
#endif
}

status_t  AudioHardwareALSA::enableVoiceCall(int mode, int device, uint32_t vsid)
{
    // Start voice call
    status_t status;
    unsigned long bufferSize = DEFAULT_VOICE_BUFFER_SIZE;
    alsa_handle_t alsa_handle;
    char *use_case;
    char *verb = getUcmVerbForVSID(vsid);
    char *modifier = getUcmModForVSID(vsid);

    if (verb == NULL || modifier == NULL) {
        ALOGE("%s(): Error, verb=%p or modifier=%p is NULL",
              __func__, verb, modifier);

            return NO_INIT;
    }

    snd_use_case_get(mUcMgr, "_verb", (const char **)&use_case);
    if ((use_case == NULL) || (!strcmp(use_case, SND_USE_CASE_VERB_INACTIVE))) {
        strlcpy(alsa_handle.useCase, verb, sizeof(alsa_handle.useCase));
    } else {
        strlcpy(alsa_handle.useCase, modifier, sizeof(alsa_handle.useCase));
    }
    free(use_case);

    if (device == AUDIO_DEVICE_OUT_BLUETOOTH_A2DP) {
        ALOGE("Returning error as BTA2DP device is not compatible for Voice call");
        return NO_INIT;
    }

    for (size_t b = 1; (bufferSize & ~b) != 0; b <<= 1)
    bufferSize &= ~b;
    alsa_handle.module = mALSADevice;
    alsa_handle.bufferSize = bufferSize;
    alsa_handle.devices = device;
    alsa_handle.handle = 0;
    alsa_handle.format = SNDRV_PCM_FORMAT_S16_LE;
    alsa_handle.channels = VOICE_CHANNEL_MODE;
    alsa_handle.sampleRate = VOICE_SAMPLING_RATE;
    alsa_handle.latency = VOICE_LATENCY;
    alsa_handle.rxHandle = 0;
    alsa_handle.ucMgr = mUcMgr;
    mDeviceList.push_back(alsa_handle);
    ALSAHandleList::iterator it = mDeviceList.end();
    it--;
    setInChannels(device);

    ALOGV("%s: enable Voice call voice_vsid:%x", __func__, vsid);

    mALSADevice->route(&(*it), (uint32_t)device, mode);
    if (!strcmp(it->useCase, verb)) {
        snd_use_case_set(mUcMgr, "_verb", verb);
    } else {
        snd_use_case_set(mUcMgr, "_enamod", modifier);
    }

    status = mALSADevice->startVoiceCall(&(*it), vsid);

    if (status == NO_ERROR) {
        ALOGV("AudioHardwareAlsa: voice call succesfully started");
    }
    else {
        ALOGE("AudioHardwareAlsa: voice call setup was unsuccesfull");
        mDeviceList.erase(it);
        return NO_INIT;
    }

#ifdef QCOM_USBAUDIO_ENABLED
    if((device & AudioSystem::DEVICE_OUT_ANLG_DOCK_HEADSET)||
       (device & AudioSystem::DEVICE_OUT_DGTL_DOCK_HEADSET)){
       startUsbRecordingIfNotStarted();
       startUsbPlaybackIfNotStarted();
       musbPlaybackState |= USBPLAYBACKBIT_VOICECALL;
       musbRecordingState |= USBRECBIT_VOICECALL;
    }
#endif

    return NO_ERROR;
}

char *AudioHardwareALSA::getUcmVerbForVSID(uint32_t vsid)
{
    char *verb = NULL;

    switch (vsid) {
       case VOLTE_SESSION_VSID:
           verb = SND_USE_CASE_VERB_VOLTE;
           break;

       case VOICE2_SESSION_VSID:
           verb = SND_USE_CASE_VERB_VOICE2;
           break;

       case VOICE_SESSION_VSID:
           verb = SND_USE_CASE_VERB_VOICECALL;
           break;

        default:
            ALOGE("%s: Invalid vsid:%x", __func__, vsid);

    }

    return verb;
}

char *AudioHardwareALSA::getUcmModForVSID(uint32_t vsid)
{
    char *mod = NULL;

    switch (vsid) {
    case VOLTE_SESSION_VSID:
        mod = SND_USE_CASE_MOD_PLAY_VOLTE;
        break;

    case VOICE2_SESSION_VSID:
        mod = SND_USE_CASE_MOD_PLAY_VOICE2;
        break;

    case VOICE_SESSION_VSID:
        mod = SND_USE_CASE_MOD_PLAY_VOICE;
        break;

    default:
        ALOGE("%s: Invalid vsid:%x", __func__, vsid);
    }

    return mod;
}

int *AudioHardwareALSA::getCallStateForVSID(uint32_t vsid)
{
    int *callState = NULL;

    switch (vsid) {
    case VOLTE_SESSION_VSID:
        callState = &mVolteCallState;
        break;

    case VOICE2_SESSION_VSID:
        callState = &mVoice2CallState;
        break;

    case VOICE_SESSION_VSID:
        callState = &mVoiceCallState;
        break;

    default:
       ALOGE("%s: Invalid vsid:%x", __func__, vsid);
       callState = NULL;
    }

    return callState;
}

alsa_handle_t *AudioHardwareALSA::getALSADeviceHandleForVSID(uint32_t vsid)
{
    alsa_handle_t *handle = NULL;
    char *ucmVerbForCall = getUcmVerbForVSID(vsid);
    char *ucmModForCall =  getUcmModForVSID(vsid);
    ALSAHandleList::iterator vt_it;

    if (ucmVerbForCall == NULL || ucmModForCall == NULL ) {
        ALOGE("%s: Error, ucmVerbForCall=%p or ucmModForCall=%p is NULL",
              __func__, ucmVerbForCall, ucmModForCall);

        return handle;
    }

    for(vt_it = mDeviceList.begin();
         vt_it != mDeviceList.end(); ++vt_it) {
        if((!strncmp(vt_it->useCase, ucmVerbForCall,
                     MAX(strlen(ucmVerbForCall), strlen(vt_it->useCase)))) ||
           (!strncmp(vt_it->useCase, ucmModForCall,
                     MAX(strlen(ucmModForCall), strlen(vt_it->useCase))))) {
            handle = (alsa_handle_t *)(&(*vt_it));
            break;
        }
    }

    return handle;
}

bool AudioHardwareALSA::routeCall(int device, int newMode, uint32_t vsid)
{
    bool isRouted = false;
    alsa_handle_t *handle = NULL;
    int err = 0;
    int *curCallState = getCallStateForVSID(vsid);
    int newCallState = mCallState;
    status_t status;

    if (curCallState == NULL) {
        ALOGE("%s(): Error, mCurCallState=%p is NULL",
              __func__, curCallState);

        return isRouted;
    }

    ALOGV("%s: CurCallState=%x newCallState=%x, vsid =%x",
          __func__, *curCallState, newCallState, vsid);


    switch (newCallState) {
    case CALL_INACTIVE:
        if (*curCallState != CALL_INACTIVE) {
            ALOGV("%s: Disabling call for vsid:%x", __func__, vsid);

            disableVoiceCall(newMode, device, vsid);
            isRouted = true;
            *curCallState = CALL_INACTIVE;
        } else {
            ALOGV("%s(): NO-OP in CALL_INACTIVE for vsid:%x", __func__, vsid);
        }
        break;

    case CALL_ACTIVE:
        if (*curCallState == CALL_INACTIVE) {
            ALOGV("%s(): Start call for vsid:%x ",__func__, vsid);

            status = enableVoiceCall(newMode, device, vsid);
            if (status == NO_INIT) {
                isRouted = false;
                *curCallState = CALL_INACTIVE;
                ALOGW("%s(): enableVoiceCall for VSID : %x is failed", __func__,vsid);
            }
            else {
                isRouted = true;
                *curCallState = CALL_ACTIVE;
            }
        } else if (*curCallState == CALL_HOLD) {
            ALOGV("%s(): Resume call from hold state for vsid:%x",
                  __func__, vsid);

            *curCallState = CALL_ACTIVE;
            isRouted = true;
        } else if(*curCallState == CALL_LOCAL_HOLD) {
            /* Not doing Anything here since we don't hit this block*/
            ALOGV("%s: Resume call from local call hold state \
                   for vsid:%x",__func__, vsid);

            handle = getALSADeviceHandleForVSID(vsid);
            if (handle) {
                *curCallState = CALL_ACTIVE;
                isRouted = true;
            } else {
                ALOGE("%s: AlsaHandle for vsid=%d is NULL", __func__, vsid);
            }
        } else {
            ALOGV("%s():NO-OP in CALL_ACTIVE for vsid:%x", __func__, vsid);
        }
        break;

    case CALL_HOLD:
        if (*curCallState == CALL_ACTIVE ||
            *curCallState == CALL_LOCAL_HOLD) {
            ALOGV("%s(): Call going to Hold for vsid:%x",__func__, vsid);

            handle = getALSADeviceHandleForVSID(vsid);
            if (handle) {
                if (*curCallState == CALL_LOCAL_HOLD) {
                }
                *curCallState = CALL_HOLD;
                isRouted = true;
            } else {
                ALOGE("%s(): AlsaHandle for vsid=%d is NULL", __func__, vsid);
            }
        } else {
            ALOGV("%s(): NO-OP in CALL_HOLD for vsid:%x", __func__, vsid);
        }
        break;

    case CALL_LOCAL_HOLD:
            /* Not doing Anything here since we don't hit this block */
            if (*curCallState == CALL_ACTIVE || *curCallState == CALL_HOLD) {
            ALOGV("%s(): Call going to local Hold for vsid:%x", __func__, vsid);

            handle = getALSADeviceHandleForVSID(vsid);
            if (handle) {
                *curCallState = CALL_LOCAL_HOLD;
                isRouted = true;
            } else {
                ALOGE("%s(): AlsaHandle for vsid=%x is NULL",__func__, vsid);
            }
        } else {
            ALOGV("%s(): NO-OP in CALL_LOCAL_HOLD for vsid:%x",__func__, vsid);
        }
        break;

       default:
       case CALL_INVALID:
           ALOGE("%s(): Invalid Call state=%d vsid=%x",
                 __func__, newCallState, vsid );
           break;
    }

    return isRouted;
}

#ifdef QCOM_TUNNEL_LPA_ENABLED
void AudioHardwareALSA::pauseIfUseCaseTunnelOrLPA() {
    for (ALSAHandleList::iterator it = mDeviceList.begin();
           it != mDeviceList.end(); it++) {
        if((!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                strlen(SND_USE_CASE_VERB_HIFI_TUNNEL))) ||
            (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                strlen(SND_USE_CASE_MOD_PLAY_TUNNEL))) ||
            (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
                strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER))) ||
            (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_LPA,
                strlen(SND_USE_CASE_MOD_PLAY_LPA)))) {
                it->session->pause_l();
        }
    }
}

void AudioHardwareALSA::resumeIfUseCaseTunnelOrLPA() {
    for (ALSAHandleList::iterator it = mDeviceList.begin();
           it != mDeviceList.end(); it++) {
        if((!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                strlen(SND_USE_CASE_VERB_HIFI_TUNNEL))) ||
            (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                strlen(SND_USE_CASE_MOD_PLAY_TUNNEL))) ||
            (!strncmp(it->useCase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
                strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER))) ||
            (!strncmp(it->useCase, SND_USE_CASE_MOD_PLAY_LPA,
                strlen(SND_USE_CASE_MOD_PLAY_LPA)))) {
                it->session->resume_l();
        }
    }
}
#endif

status_t AudioHardwareALSA::startPlaybackOnExtOut(uint32_t activeUsecase) {

    Mutex::Autolock autoLock(mLock);
    status_t err = startPlaybackOnExtOut_l(activeUsecase);
    if(err) {
        ALOGE("startPlaybackOnExtOut_l  = %d", err);
    }
    return err;
}
status_t AudioHardwareALSA::startPlaybackOnExtOut_l(uint32_t activeUsecase) {

    ALOGV("startPlaybackOnExtOut_l::usecase = %d ", activeUsecase);
    status_t err = NO_ERROR;
    if (!mExtOutStream) {
        ALOGE("Unable to open ExtOut stream");
        return err;
    }
    if (activeUsecase != USECASE_NONE && !mIsExtOutEnabled) {
        Mutex::Autolock autolock1(mExtOutMutex);
         err = setProxyProperty(0);
         if(err) {
            ALOGE("Proxy Property Set Failedd");
        }
        int ProxyOpenRetryCount=PROXY_OPEN_RETRY_COUNT;
        while(ProxyOpenRetryCount){
            err = mALSADevice->openProxyDevice();
            if(err) {
                 ProxyOpenRetryCount --;
                 usleep(PROXY_OPEN_WAIT_TIME * 1000);
                 ALOGV("openProxyDevice failed retrying = %d", ProxyOpenRetryCount);
            }
            else
               break;
        }
        if(err) {
            ALOGE("openProxyDevice failed = %d", err);
        }

        mKillExtOutThread = false;
        err = pthread_create(&mExtOutThread, (const pthread_attr_t *) NULL,
                extOutThreadWrapper,
                this);
        if(err) {
            ALOGE("thread create failed = %d", err);
            return err;
        }
        mExtOutThreadAlive = true;
        mIsExtOutEnabled = true;

#ifdef OUTPUT_BUFFER_LOG
    sprintf(outputfilename, "%s%d%s", outputfilename, number,".pcm");
    outputBufferFile1 = fopen (outputfilename, "ab");
    number++;
#endif
    }

    setExtOutActiveUseCases_l(activeUsecase);
    mALSADevice->resumeProxy();

    Mutex::Autolock autolock1(mExtOutMutex);
    ALOGV("ExtOut signal");
    mExtOutCv.signal();
    return err;
}

status_t AudioHardwareALSA::setProxyProperty(uint32_t value) {
     status_t err=NO_ERROR;
     if(value){
        //property_set("proxy.can.open", "true");
        mCanOpenProxy = 1;
     }
     else{
        //property_set("proxy.can.open", "false");
        mCanOpenProxy = 0;
     }
     return err;
}


status_t AudioHardwareALSA::stopPlaybackOnExtOut(uint32_t activeUsecase) {
     Mutex::Autolock autoLock(mLock);
     status_t err = stopPlaybackOnExtOut_l(activeUsecase);
     if(err) {
         ALOGE("stopPlaybackOnExtOut = %d", err);
     }
     return err;
}

status_t AudioHardwareALSA::stopPlaybackOnExtOut_l(uint32_t activeUsecase) {

     ALOGV("stopPlaybackOnExtOut  = %d", activeUsecase);
     status_t err = NO_ERROR;
     suspendPlaybackOnExtOut_l(activeUsecase);
     {
         Mutex::Autolock autolock1(mExtOutMutex);
         ALOGV("stopPlaybackOnExtOut  getExtOutActiveUseCases_l = %d",
                getExtOutActiveUseCases_l());

         if(!getExtOutActiveUseCases_l()) {
             mIsExtOutEnabled = false;

             mExtOutMutex.unlock();
             err = stopExtOutThread();
             if(err) {
                 ALOGE("stopExtOutThread Failed :: err = %d" ,err);
             }
             mExtOutMutex.lock();

             if (mExtOutStream != NULL) {
                 ALOGV(" External Output Stream Standby called");
                 mExtOutStream->common.standby(&mExtOutStream->common);
             }

             err = mALSADevice->closeProxyDevice();
             if(err) {
                 ALOGE("closeProxyDevice failed = %d", err);
             }

             mExtOutActiveUseCases = 0x0;
             mRouteAudioToExtOut = false;

#ifdef OUTPUT_BUFFER_LOG
    ALOGV("close file output");
    fclose (outputBufferFile1);
#endif
         }
     }
     return err;
}

status_t AudioHardwareALSA::openExtOutput(int device) {

    ALOGV("openExtOutput");
    status_t err = NO_ERROR;
    Mutex::Autolock autolock1(mExtOutMutex);
    if (device & AudioSystem::DEVICE_OUT_ALL_A2DP) {
        err= openA2dpOutput();
        if(err) {
            ALOGE("openA2DPOutput failed = %d",err);
            return err;
        }
        if(!mExtOutStream) {
            mExtOutStream = mA2dpStream;
        }
    } else if (device & AudioSystem::DEVICE_OUT_ALL_USB) {
        err= openUsbOutput();
        if(err) {
            ALOGE("openUsbPOutput failed = %d",err);
            return err;
        }
        if(!mExtOutStream) {
            mExtOutStream = mUsbStream;
        }
    }
    return err;
}

status_t AudioHardwareALSA::closeExtOutput(int device) {

    ALOGV("closeExtOutput");
    status_t err = NO_ERROR;
    Mutex::Autolock autolock1(mExtOutMutex);
    if (device & AudioSystem::DEVICE_OUT_ALL_A2DP) {
        if(mExtOutStream == mA2dpStream)
            mExtOutStream = NULL;
        err= closeA2dpOutput();
        if(err) {
            ALOGE("closeA2DPOutput failed = %d",err);
            return err;
        }
    } else if (device & AUDIO_DEVICE_OUT_ALL_USB) {
        if(mExtOutStream == mUsbStream)
            mExtOutStream = NULL;
        err= closeUsbOutput();
        if(err) {
            ALOGE("closeUsbPOutput failed = %d",err);
            return err;
        }
    }
    return err;
}

status_t AudioHardwareALSA::openA2dpOutput()
{
    hw_module_t *mod;
    int      format = AUDIO_FORMAT_PCM_16_BIT;
    uint32_t channels = AUDIO_CHANNEL_OUT_STEREO;
    uint32_t sampleRate = AFE_PROXY_SAMPLE_RATE;
    status_t status;
    ALOGV("openA2dpOutput");
    struct audio_config config;
    config.sample_rate = AFE_PROXY_SAMPLE_RATE;
    config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    config.format = AUDIO_FORMAT_PCM_16_BIT;

    //TODO : Confirm AUDIO_HARDWARE_MODULE_ID_A2DP ???
    int rc = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID/*_A2DP*/, (const char*)"a2dp",
                                    (const hw_module_t**)&mod);
    if (rc) {
        ALOGE("Could not get a2dp hardware module");
        return NO_INIT;
    }

    rc = audio_hw_device_open(mod, &mA2dpDevice);
    if(rc) {
        ALOGE("couldn't open a2dp audio hw device");
        return NO_INIT;
    }
    //TODO: unique id 0?
    status = mA2dpDevice->open_output_stream(mA2dpDevice, 0,((audio_devices_t)(AudioSystem::DEVICE_OUT_BLUETOOTH_A2DP)),
                                    (audio_output_flags_t)AUDIO_OUTPUT_FLAG_NONE, &config, &mA2dpStream);
    if(status != NO_ERROR) {
        ALOGE("Failed to open output stream for a2dp: status %d", status);
    }
    return status;
}

status_t AudioHardwareALSA::closeA2dpOutput()
{
    ALOGV("closeA2dpOutput");
    if(!mA2dpDevice){
        ALOGE("No Aactive A2dp output found");
        return NO_ERROR;
    }

    mA2dpDevice->close_output_stream(mA2dpDevice, mA2dpStream);
    mA2dpStream = NULL;

    audio_hw_device_close(mA2dpDevice);
    mA2dpDevice = NULL;
    return NO_ERROR;
}

status_t AudioHardwareALSA::openUsbOutput()
{
    hw_module_t *mod;
    int      format = AUDIO_FORMAT_PCM_16_BIT;
    uint32_t channels = AUDIO_CHANNEL_OUT_STEREO;
    uint32_t sampleRate = AFE_PROXY_SAMPLE_RATE;
    status_t status;
    ALOGV("openUsbOutput");
    struct audio_config config;
    config.sample_rate = AFE_PROXY_SAMPLE_RATE;
    config.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    config.format = AUDIO_FORMAT_PCM_16_BIT;

    int rc = hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID/*_USB*/, (const char*)"usb",
                                    (const hw_module_t**)&mod);
    if (rc) {
        ALOGE("Could not get usb hardware module");
        return NO_INIT;
    }

    rc = audio_hw_device_open(mod, &mUsbDevice);
    if(rc) {
        ALOGE("couldn't open Usb audio hw device");
        return NO_INIT;
    }

    status = mUsbDevice->open_output_stream(mUsbDevice, 0,((audio_devices_t)(AUDIO_DEVICE_OUT_USB_ACCESSORY)),
                                    (audio_output_flags_t)AUDIO_OUTPUT_FLAG_NONE, &config, &mUsbStream);
    if(status != NO_ERROR) {
        ALOGE("Failed to open output stream for USB: status %d", status);
    }

    return status;
}

status_t AudioHardwareALSA::closeUsbOutput()
{
    ALOGV("closeUsbOutput");
    if(!mUsbDevice){
        ALOGE("No Aactive Usb output found");
        return NO_ERROR;
    }

    mUsbDevice->close_output_stream(mUsbDevice, mUsbStream);
    mUsbStream = NULL;

    audio_hw_device_close(mUsbDevice);
    mUsbDevice = NULL;
    return NO_ERROR;
}

status_t AudioHardwareALSA::stopExtOutThread()
{
    ALOGV("stopExtOutThread");
    status_t err = NO_ERROR;
    if (!mExtOutThreadAlive) {
        ALOGD("Return - thread not live");
        return NO_ERROR;
    }
    mExtOutMutex.lock();
    mKillExtOutThread = true;
    err = mALSADevice->exitReadFromProxy();
    if(err) {
        ALOGE("exitReadFromProxy failed = %d", err);
    }
    mExtOutCv.signal();
    mExtOutMutex.unlock();
    int ret = pthread_join(mExtOutThread,NULL);
    ALOGD("ExtOut thread killed = %d", ret);
    return err;
}

void AudioHardwareALSA::switchExtOut(int device) {

    ALOGV("switchExtOut");
    uint32_t sampleRate;
    Mutex::Autolock autolock1(mExtOutMutex);
    if (device & AudioSystem::DEVICE_OUT_ALL_A2DP) {
        mExtOutStream = mA2dpStream;
    } else if (device & AUDIO_DEVICE_OUT_ALL_USB) {
        mExtOutStream = mUsbStream;
    } else {
        mExtOutStream = NULL;
    }
    if ((mExtOutStream == mUsbStream) && mExtOutStream != NULL) {
        sampleRate = mExtOutStream->common.get_sample_rate(&mExtOutStream->common);
        if (sampleRate > AFE_PROXY_SAMPLE_RATE) {
            ALOGW(" Requested samplerate %d is greater than supported so fall back to %d ",
                  sampleRate,AFE_PROXY_SAMPLE_RATE);
            sampleRate = AFE_PROXY_SAMPLE_RATE;
        }
        if (mResampler != NULL) {
            release_resampler(mResampler);
            mResampler = NULL;
        }
        if (sampleRate != AFE_PROXY_SAMPLE_RATE) {
            mResampler = (struct resampler_itfe *)calloc(1, sizeof(struct resampler_itfe));
            if (mResampler != NULL ) {
                status_t err = create_resampler(AFE_PROXY_SAMPLE_RATE,
                                                sampleRate,
                                                2, //channel count
                                                RESAMPLER_QUALITY_DEFAULT,
                                                NULL,
                                                &mResampler);
                ALOGV(" sampleRate %d mResampler %p",sampleRate,mResampler);
                if (err) {
                    ALOGE(" Failed to create resampler");
                    free(mResampler);
                    mResampler = NULL;
                }
            } else{
                ALOGE(" Failed to allocate memory for mResampler = %p",mResampler);
            }
        }
    }
}

status_t AudioHardwareALSA::isExtOutDevice(int device) {
    return ((device & AudioSystem::DEVICE_OUT_ALL_A2DP) ||
            (device & AUDIO_DEVICE_OUT_ALL_USB)) ;
}

void *AudioHardwareALSA::extOutThreadWrapper(void *me) {
    static_cast<AudioHardwareALSA *>(me)->extOutThreadFunc();
    return NULL;
}

void AudioHardwareALSA::extOutThreadFunc() {
    if(!mExtOutStream) {
        ALOGE("No valid External output stream found");
        return;
    }
    if(!mALSADevice->isProxyDeviceOpened()) {
        ALOGE("No valid mProxyPcmHandle found");
        return;
    }

    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_URGENT_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"ExtOutThread", 0, 0, 0);

    int ionBufCount = 0;
    int32_t bytesWritten = 0;
    uint32_t numBytesRemaining = 0;
    uint32_t bytesAvailInBuffer = 0;
    uint32_t proxyBufferTime = 0;
    void  *data;
    status_t err = NO_ERROR;
    ssize_t size = 0;
    void * outbuffer= malloc(AFE_PROXY_PERIOD_SIZE);

    mALSADevice->resetProxyVariables();

    ALOGV("mKillExtOutThread = %d", mKillExtOutThread);
    while(!mKillExtOutThread) {
        {
            Mutex::Autolock autolock1(mExtOutMutex);
            if (mKillExtOutThread) {
                break;
            }
            if (!mExtOutStream || !mIsExtOutEnabled ||
                !mALSADevice->isProxyDeviceOpened() ||
                (mALSADevice->isProxyDeviceSuspended()) ||
                (err != NO_ERROR)) {
                ALOGD("ExtOutThreadEntry:: proxy opened = %d,\
                        proxy suspended = %d,err =%d,\
                        mExtOutStream = %p mIsExtOutEnabled = %d",\
                        mALSADevice->isProxyDeviceOpened(),\
                        mALSADevice->isProxyDeviceSuspended(),err,mExtOutStream, mIsExtOutEnabled);
                ALOGD("ExtOutThreadEntry:: Waiting on mExtOutCv");
                mExtOutCv.wait(mExtOutMutex);
                ALOGD("ExtOutThreadEntry:: received signal to wake up");
                mExtOutMutex.unlock();
                continue;
            }
        }
        err = mALSADevice->readFromProxy(&data, &size);
        if(err == (status_t) FAILED_TRANSACTION) {
            ALOGE("readFromProxy returned an error, mostly a flush or an under run continuing");
            err = NO_ERROR;
            continue;
        }
        if(err < 0) {
           ALOGE("ALSADevice readFromProxy returned err = %d,data = %p,\
                    size = %ld", err, data, size);
           continue;
        }

#ifdef OUTPUT_BUFFER_LOG
    if (outputBufferFile1)
    {
        fwrite (data,1,size,outputBufferFile1);
    }
#endif
        void *copyBuffer = data;
        numBytesRemaining = size;
        proxyBufferTime = mALSADevice->mProxyParams.mBufferTime;
        {
            Mutex::Autolock autolock1(mExtOutMutex);
            if (mResampler != NULL) {
                uint32_t inFrames = size/(AFE_PROXY_CHANNEL_COUNT*2);
                uint32_t outFrames = inFrames;
                mResampler->resample_from_input(mResampler,
                                               (int16_t *)data,
                                               &inFrames,
                                               (int16_t *)outbuffer,
                                               &outFrames);
                copyBuffer = outbuffer;
                numBytesRemaining = outFrames*(AFE_PROXY_CHANNEL_COUNT*2);
                ALOGV("inFrames %d outFrames %d",inFrames,outFrames);
            }
        }
        while (err == OK && (numBytesRemaining  > 0) && !mKillExtOutThread
                && mIsExtOutEnabled ) {
            {
                Mutex::Autolock autolock1(mExtOutMutex);
                if(mExtOutStream != NULL ) {
                    bytesAvailInBuffer = mExtOutStream->common.get_buffer_size(&mExtOutStream->common);
                    uint32_t writeLen = bytesAvailInBuffer > numBytesRemaining ?
                                    numBytesRemaining : bytesAvailInBuffer;
                    ALOGV("Writing %d bytes to External Output ", writeLen);
                    bytesWritten = mExtOutStream->write(mExtOutStream,copyBuffer, writeLen);
                } else {
                    ALOGV(" No External output to write  ");
                    usleep(proxyBufferTime*1000);
                    bytesWritten = numBytesRemaining;
                }
            }
            //If the write fails make this thread sleep and let other
            //thread (eg: stopA2DP) to acquire lock to prevent a deadlock.
            if(bytesWritten == -1) {
                ALOGV("bytesWritten = %d",bytesWritten);
                usleep(10000);
                break;
            }
            //Need to check warning here - void used in arithmetic
            copyBuffer = (char *)copyBuffer + bytesWritten;
            numBytesRemaining -= bytesWritten;
            ALOGV("@_@bytes To write2:%d",numBytesRemaining);
        }
    }

    mALSADevice->resetProxyVariables();
    mExtOutThreadAlive = false;
    ALOGD("ExtOut Thread is dying");
}

void AudioHardwareALSA::setExtOutActiveUseCases_l(uint32_t activeUsecase)
{
   mExtOutActiveUseCases |= activeUsecase;
   ALOGV("mExtOutActiveUseCases = %u, activeUsecase = %u", mExtOutActiveUseCases, activeUsecase);
}

uint32_t AudioHardwareALSA::getExtOutActiveUseCases_l()
{
   ALOGV("getExtOutActiveUseCases_l: mExtOutActiveUseCases = %u", mExtOutActiveUseCases);
   return mExtOutActiveUseCases;
}

void AudioHardwareALSA::clearExtOutActiveUseCases_l(uint32_t activeUsecase) {

   mExtOutActiveUseCases &= ~activeUsecase;
   ALOGV("clear - mExtOutActiveUseCases = %u, activeUsecase = %u", mExtOutActiveUseCases, activeUsecase);

}

uint32_t AudioHardwareALSA::useCaseStringToEnum(const char *usecase)
{
   ALOGV("useCaseStringToEnum usecase:%s",usecase);
   uint32_t activeUsecase = USECASE_NONE;

   if ((!strncmp(usecase, SND_USE_CASE_VERB_HIFI_LOW_POWER,
                    strlen(SND_USE_CASE_VERB_HIFI_LOW_POWER))) ||
       (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_LPA,
                    strlen(SND_USE_CASE_MOD_PLAY_LPA)))) {
       activeUsecase = USECASE_HIFI_LOW_POWER;
   } else if ((!strncmp(usecase, SND_USE_CASE_VERB_HIFI_TUNNEL,
                           strlen(SND_USE_CASE_VERB_HIFI_TUNNEL))) ||
              (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_TUNNEL,
                           strlen(SND_USE_CASE_MOD_PLAY_TUNNEL)))) {
       activeUsecase = USECASE_HIFI_TUNNEL;
   } else if ((!strncmp(usecase, SND_USE_CASE_VERB_HIFI_LOWLATENCY_MUSIC,
                           strlen(SND_USE_CASE_VERB_HIFI_LOWLATENCY_MUSIC )))||
               (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_LOWLATENCY_MUSIC,
                           strlen(SND_USE_CASE_MOD_PLAY_LOWLATENCY_MUSIC)))) {
       activeUsecase = USECASE_HIFI_LOWLATENCY;
   } else if ((!strncmp(usecase, SND_USE_CASE_VERB_DIGITAL_RADIO,
                           strlen(SND_USE_CASE_VERB_DIGITAL_RADIO))) ||
               (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_FM,
                           strlen(SND_USE_CASE_MOD_PLAY_FM)))||
               (!strncmp(usecase, SND_USE_CASE_VERB_FM_REC,
                           strlen(SND_USE_CASE_VERB_FM_REC)))||
               (!strncmp(usecase, SND_USE_CASE_MOD_CAPTURE_FM,
                           strlen(SND_USE_CASE_MOD_CAPTURE_FM)))){
       activeUsecase = USECASE_FM;
    } else if ((!strncmp(usecase, SND_USE_CASE_VERB_HIFI,
                           strlen(SND_USE_CASE_VERB_HIFI)))||
               (!strncmp(usecase, SND_USE_CASE_MOD_PLAY_MUSIC,
                           strlen(SND_USE_CASE_MOD_PLAY_MUSIC)))) {
       activeUsecase = USECASE_HIFI;
    }
    return activeUsecase;
}

bool  AudioHardwareALSA::suspendPlaybackOnExtOut(uint32_t activeUsecase) {

    Mutex::Autolock autoLock(mLock);
    suspendPlaybackOnExtOut_l(activeUsecase);
    return NO_ERROR;
}

bool  AudioHardwareALSA::suspendPlaybackOnExtOut_l(uint32_t activeUsecase) {

    Mutex::Autolock autolock1(mExtOutMutex);
    ALOGD("suspendPlaybackOnExtOut_l activeUsecase = %d, mRouteAudioToExtOut = %d",\
            activeUsecase, mRouteAudioToExtOut);
    clearExtOutActiveUseCases_l(activeUsecase);
    if((!getExtOutActiveUseCases_l()) && mIsExtOutEnabled )
        return mALSADevice->suspendProxy();
    return NO_ERROR;
}

}       // namespace android_audio_legacy