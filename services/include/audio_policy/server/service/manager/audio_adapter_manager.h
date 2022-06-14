/*
 * Copyright (c) 2021-2022 Huawei Device Co., Ltd.
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

#ifndef ST_AUDIO_ADAPTER_MANAGER_H
#define ST_AUDIO_ADAPTER_MANAGER_H

#include <list>
#include <unordered_map>

#include "audio_service_adapter.h"
#include "distributed_kv_data_manager.h"
#include "iaudio_policy_interface.h"
#include "types.h"
#include "audio_log.h"

namespace OHOS {
namespace AudioStandard {
using namespace OHOS::DistributedKv;

class AudioAdapterManager : public IAudioPolicyInterface {
public:
    static constexpr std::string_view HDI_SINK = "libmodule-hdi-sink.z.so";
    static constexpr std::string_view HDI_SOURCE = "libmodule-hdi-source.z.so";
    static constexpr std::string_view PIPE_SINK = "libmodule-pipe-sink.z.so";
    static constexpr std::string_view PIPE_SOURCE = "libmodule-pipe-source.z.so";
    static constexpr uint32_t KVSTORE_CONNECT_RETRY_COUNT = 5;
    static constexpr uint32_t KVSTORE_CONNECT_RETRY_DELAY_TIME = 200000;
    static constexpr float MAX_VOLUME = 1.0f;
    static constexpr float MIN_VOLUME = 0.0f;

    bool Init();
    void Deinit(void);
    void InitKVStore();
    bool ConnectServiceAdapter();

    std::string GetPolicyManagerName();

    static IAudioPolicyInterface& GetInstance()
    {
        static AudioAdapterManager audioAdapterManager;
        return audioAdapterManager;
    }

    int32_t SetStreamVolume(AudioStreamType streamType, float volume);

    float GetStreamVolume(AudioStreamType streamType);

    int32_t SetStreamMute(AudioStreamType streamType, bool mute);

    bool GetStreamMute(AudioStreamType streamType);

    bool IsStreamActive(AudioStreamType streamType);

    std::vector<SinkInput> GetAllSinkInputs();

    std::vector<SourceOutput> GetAllSourceOutputs();

    AudioIOHandle OpenAudioPort(const AudioModuleInfo &audioModuleInfo);

    int32_t CloseAudioPort(AudioIOHandle ioHandle);

    int32_t SetDeviceActive(AudioIOHandle ioHandle, InternalDeviceType deviceType, std::string name, bool active);

    int32_t SetRingerMode(AudioRingerMode ringerMode);

    AudioRingerMode GetRingerMode(void) const;

    int32_t SetAudioSessionCallback(AudioSessionCallback *callback);

    int32_t SuspendAudioDevice(std::string &name, bool isSuspend);

    virtual ~AudioAdapterManager() {}

private:
    struct UserData {
        AudioAdapterManager *thiz;
        AudioStreamType streamType;
        float volume;
        bool mute;
        bool isCorked;
        uint32_t idx;
    };

    AudioAdapterManager()
        : mRingerMode(RINGER_MODE_NORMAL),
          mAudioPolicyKvStore(nullptr)
    {
        mVolumeMap[STREAM_MUSIC] = MAX_VOLUME;
        mVolumeMap[STREAM_RING] = MAX_VOLUME;
        mVolumeMap[STREAM_VOICE_CALL] = MAX_VOLUME;
        mVolumeMap[STREAM_VOICE_ASSISTANT] = MAX_VOLUME;
    }

    bool ConnectToPulseAudio(void);
    std::string GetModuleArgs(const AudioModuleInfo &audioModuleInfo) const;
    std::string GetStreamNameByStreamType(AudioStreamType streamType);
    AudioStreamType GetStreamIDByType(std::string streamType);
    AudioStreamType GetStreamForVolumeMap(AudioStreamType streamType);
    bool InitAudioPolicyKvStore(bool& isFirstBoot);
    void InitVolumeMap(bool isFirstBoot);
    bool LoadVolumeMap(void);
    void WriteVolumeToKvStore(AudioStreamType streamType, float volume);
    bool LoadVolumeFromKvStore(AudioStreamType streamType);
    void InitRingerMode(bool isFirstBoot);
    bool LoadRingerMode(void);
    void WriteRingerModeToKvStore(AudioRingerMode ringerMode);
    void InitMuteStatusMap(bool isFirstBoot);
    bool LoadMuteStatusMap(void);
    bool LoadMuteStatusFromKvStore(AudioStreamType streamType);
    void WriteMuteStatusToKvStore(AudioStreamType streamType, bool muteStatus);
    std::string GetStreamTypeKeyForMute(AudioStreamType streamType);
    std::unique_ptr<AudioServiceAdapter> mAudioServiceAdapter;
    std::unordered_map<AudioStreamType, float> mVolumeMap;
    std::unordered_map<AudioStreamType, int> mMuteStatusMap;
    AudioRingerMode mRingerMode;
    std::shared_ptr<SingleKvStore> mAudioPolicyKvStore;

    AudioSessionCallback *sessionCallback_;
    friend class PolicyCallbackImpl;
};

class PolicyCallbackImpl : public AudioServiceAdapterCallback {
public:
    explicit PolicyCallbackImpl(std::unique_ptr<AudioAdapterManager> &audioAdapterManager)
    {
        audioAdapterManager_ = std::move(audioAdapterManager);
    }

    ~PolicyCallbackImpl()
    {
        audioAdapterManager_ = nullptr;
    }

    float OnGetVolumeCb(std::string streamType)
    {
        AudioStreamType streamForVolumeMap = audioAdapterManager_->GetStreamForVolumeMap(
            audioAdapterManager_->GetStreamIDByType(streamType));
        if (audioAdapterManager_->mRingerMode != RINGER_MODE_NORMAL) {
            if (streamForVolumeMap == STREAM_RING) {
                return AudioAdapterManager::MIN_VOLUME;
            }
        }

        if (audioAdapterManager_->GetStreamMute(streamForVolumeMap)) {
            return AudioAdapterManager::MIN_VOLUME;
        }
        return audioAdapterManager_->mVolumeMap[streamForVolumeMap];
    }

    void OnSessionRemoved(const uint32_t sessionID)
    {
        AUDIO_DEBUG_LOG("AudioAdapterManager: PolicyCallbackImpl OnSessionRemoved: Session ID %{public}d", sessionID);
        if (audioAdapterManager_->sessionCallback_ == nullptr) {
            AUDIO_DEBUG_LOG("AudioAdapterManager: PolicyCallbackImpl audioAdapterManager_->sessionCallback_ == nullptr"
                            "not firing OnSessionRemoved");
        } else {
            audioAdapterManager_->sessionCallback_->OnSessionRemoved(sessionID);
        }
    }
private:
    std::unique_ptr<AudioAdapterManager> audioAdapterManager_;
};
} // namespace AudioStandard
} // namespace OHOS
#endif // ST_PULSEAUDIO_ADAPTER_MANAGER_H
