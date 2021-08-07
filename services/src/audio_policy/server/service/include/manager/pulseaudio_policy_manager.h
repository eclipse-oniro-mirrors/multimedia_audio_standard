/*
 * Copyright (C) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ST_PULSEAUDIO_POLICY_MANAGER_H
#define ST_PULSEAUDIO_POLICY_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif
#include <pulse/pulseaudio.h>
#ifdef __cplusplus
}
#endif

#include <list>
#include <unordered_map>

#include "distributed_kv_data_manager.h"
#include "iaudio_policy_interface.h"
#include "types.h"

namespace OHOS {
namespace AudioStandard {
using namespace OHOS::DistributedKv;

class PulseAudioPolicyManager : public IAudioPolicyInterface {
public:
    static constexpr char HDI_SINK[] = "libmodule-hdi-sink.z.so";
    static constexpr char HDI_SOURCE[] = "libmodule-hdi-source.z.so";
    static constexpr char PIPE_SINK[] = "libmodule-pipe-sink.z.so";
    static constexpr char PIPE_SOURCE[] = "libmodule-pipe-source.z.so";
    static constexpr float MAX_VOLUME = 1.0f;
    static constexpr float MIN_VOLUME = 0.0f;
    static constexpr uint32_t PA_CONNECT_RETRY_SLEEP_IN_MICRO_SECONDS = 500000;

    bool Init();
    void Deinit(void);

    std::string GetPolicyManagerName();

    static IAudioPolicyInterface& GetInstance()
    {
        static PulseAudioPolicyManager policyManager;
        return policyManager;
    }

    int32_t SetStreamVolume(AudioStreamType streamType, float volume);

    float GetStreamVolume(AudioStreamType streamType);

    int32_t SetStreamMute(AudioStreamType streamType, bool mute);

    bool GetStreamMute(AudioStreamType streamType);

    bool IsStreamActive(AudioStreamType streamType);

    AudioIOHandle OpenAudioPort(std::shared_ptr<AudioPortInfo> audioPortInfo);

    int32_t CloseAudioPort(AudioIOHandle ioHandle);

    int32_t SetDeviceActive(AudioIOHandle ioHandle, DeviceType deviceType, std::string name, bool active);

    int32_t SetRingerMode(AudioRingerMode ringerMode);

    AudioRingerMode GetRingerMode(void);

    // Static Member functions
    static void ContextStateCb(pa_context *c, void *userdata);

    static void SubscribeCb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata);

    static void ModuleLoadCb(pa_context *c, uint32_t idx, void *userdata);

    static void GetSinkInputInfoVolumeCb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata);

    static void GetSinkInputInfoCb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata);

    static void GetSinkInputInfoMuteCb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata);

    static void GetSinkInputInfoMuteStatusCb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata);

    static void GetSinkInputInfoCorkStatusCb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata);
private:
    struct UserData {
        PulseAudioPolicyManager* thiz;
        AudioStreamType streamType;
        float volume;
        bool mute;
        bool isCorked;
        uint32_t idx;
    };

    PulseAudioPolicyManager()
        : mContext(nullptr),
          mMainLoop(nullptr),
          mRingerMode(RINGER_MODE_NORMAL),
          mAudioPolicyKvStore(nullptr)
    {
        mVolumeMap[STREAM_MUSIC] = MAX_VOLUME;
        mVolumeMap[STREAM_RING] = MAX_VOLUME;
    }

    virtual ~PulseAudioPolicyManager() {}

    bool ConnectToPulseAudio(void);
    void HandleSinkInputEvent(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata);
    std::string GetModuleArgs(std::shared_ptr<AudioPortInfo> audioPortInfo);
    std::string GetStreamNameByStreamType(AudioStreamType streamType);
    AudioStreamType GetStreamIDByType(std::string streamType);
    bool InitAudioPolicyKvStore(bool& isFirstBoot);
    void InitVolumeMap(bool isFirstBoot);
    bool LoadVolumeMap(void);
    void WriteVolumeToKvStore(AudioStreamType streamType, float volume);
    bool LoadVolumeFromKvStore(AudioStreamType streamType);
    void InitRingerMode(bool isFirstBoot);
    bool LoadRingerMode(void);
    void WriteRingerModeToKvStore(AudioRingerMode ringerMode);

    pa_context* mContext;
    pa_threaded_mainloop* mMainLoop;
    std::unordered_map<AudioStreamType, float> mVolumeMap;
    AudioRingerMode mRingerMode;
    std::unique_ptr<SingleKvStore> mAudioPolicyKvStore;
};
} // namespace AudioStandard
} // namespace OHOS
#endif // ST_PULSEAUDIO_POLICY_MANAGER_H
