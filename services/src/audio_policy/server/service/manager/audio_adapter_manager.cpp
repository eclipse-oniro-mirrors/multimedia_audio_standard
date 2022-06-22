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

#include <memory>
#include <unistd.h>

#include "audio_errors.h"
#include "audio_log.h"

#include "audio_adapter_manager.h"

using namespace std;

namespace OHOS {
namespace AudioStandard {
bool AudioAdapterManager::Init()
{
    return true;
}

bool AudioAdapterManager::ConnectServiceAdapter()
{
    std::unique_ptr<AudioAdapterManager> audioAdapterManager(this);
    std::unique_ptr<PolicyCallbackImpl> policyCallbackImpl = std::make_unique<PolicyCallbackImpl>(audioAdapterManager);
    mAudioServiceAdapter = AudioServiceAdapter::CreateAudioAdapter(std::move(policyCallbackImpl));
    if (!mAudioServiceAdapter) {
        AUDIO_ERR_LOG("[AudioAdapterManager] Error in audio adapter initialization");
        return false;
    }

    bool result = mAudioServiceAdapter->Connect();
    if (!result) {
        AUDIO_ERR_LOG("[AudioAdapterManager] Error in connecting audio adapter");
        return false;
    }

    return true;
}


void AudioAdapterManager::InitKVStore()
{
    bool isFirstBoot = false;
    InitAudioPolicyKvStore(isFirstBoot);
    InitVolumeMap(isFirstBoot);
    InitRingerMode(isFirstBoot);
    InitMuteStatusMap(isFirstBoot);
}

void AudioAdapterManager::Deinit(void)
{
    if (!mAudioServiceAdapter) {
        AUDIO_ERR_LOG("[AudioAdapterManager] audio adapter null");
        return;
    }

    return  mAudioServiceAdapter->Disconnect();
}

int32_t AudioAdapterManager::SetAudioSessionCallback(AudioSessionCallback *callback)
{
    if (callback == nullptr) {
        AUDIO_ERR_LOG("[AudioAdapterManager] SetAudioSessionCallback callback == nullptr");
        return ERR_INVALID_PARAM;
    }

    sessionCallback_ = callback;
    return SUCCESS;
}

int32_t AudioAdapterManager::SetStreamVolume(AudioStreamType streamType, float volume)
{
    if (!mAudioServiceAdapter) {
        AUDIO_ERR_LOG("[AudioAdapterManager] audio adapter null");
        return ERR_OPERATION_FAILED;
    }

    // Incase if KvStore didnot connect during  bootup
    if (mAudioPolicyKvStore == nullptr) {
        bool isFirstBoot = false;
        InitAudioPolicyKvStore(isFirstBoot);
    }

    if (GetStreamMute(streamType) && volume > 0.0f) {
        SetStreamMute(streamType, 0);
    }
    AudioStreamType streamForVolumeMap = GetStreamForVolumeMap(streamType);
    mVolumeMap[streamForVolumeMap] = volume;
    WriteVolumeToKvStore(streamType, volume);

    return mAudioServiceAdapter->SetVolume(streamType, volume);
}

float AudioAdapterManager::GetStreamVolume(AudioStreamType streamType)
{
    AudioStreamType streamForVolumeMap = GetStreamForVolumeMap(streamType);

    return mVolumeMap[streamForVolumeMap];
}

int32_t AudioAdapterManager::SetStreamMute(AudioStreamType streamType, bool mute)
{
    if (!mAudioServiceAdapter) {
        AUDIO_ERR_LOG("[AudioAdapterManager] audio adapter null");
        return ERR_OPERATION_FAILED;
    }
    AudioStreamType streamForVolumeMap = GetStreamForVolumeMap(streamType);
    mMuteStatusMap[streamForVolumeMap] = mute;
    WriteMuteStatusToKvStore(streamType, mute);
    return mAudioServiceAdapter->SetMute(streamType, mute);
}

bool AudioAdapterManager::GetStreamMute(AudioStreamType streamType)
{
    if (!mAudioServiceAdapter) {
        AUDIO_ERR_LOG("[AudioAdapterManager] audio adapter null");
        return false;
    }
    AudioStreamType streamForVolumeMap = GetStreamForVolumeMap(streamType);
    return mMuteStatusMap[streamForVolumeMap];
}

bool AudioAdapterManager::IsStreamActive(AudioStreamType streamType)
{
    if (!mAudioServiceAdapter) {
        AUDIO_ERR_LOG("[AudioAdapterManager] audio adapter null");
        return false;
    }

    return mAudioServiceAdapter->IsStreamActive(streamType);
}

vector<SinkInput> AudioAdapterManager::GetAllSinkInputs()
{
    if (!mAudioServiceAdapter) {
        AUDIO_ERR_LOG("[AudioAdapterManager] audio adapter null");
        vector<SinkInput> sinkInputList;
        return sinkInputList;
    }

    return mAudioServiceAdapter->GetAllSinkInputs();
}

vector<SourceOutput> AudioAdapterManager::GetAllSourceOutputs()
{
    if (!mAudioServiceAdapter) {
        AUDIO_ERR_LOG("[AudioAdapterManager] audio adapter null");
        vector<SourceOutput> sourceOutputList;
        return sourceOutputList;
    }

    return mAudioServiceAdapter->GetAllSourceOutputs();
}

int32_t AudioAdapterManager::SuspendAudioDevice(std::string &portName, bool isSuspend)
{
    if (!mAudioServiceAdapter) {
        AUDIO_ERR_LOG("[AudioAdapterManager] audio adapter null");
        return ERR_OPERATION_FAILED;
    }

    return mAudioServiceAdapter->SuspendAudioDevice(portName, isSuspend);
}

int32_t AudioAdapterManager::SetDeviceActive(AudioIOHandle ioHandle, InternalDeviceType deviceType,
    std::string name, bool active)
{
    if (!mAudioServiceAdapter) {
        AUDIO_ERR_LOG("[AudioAdapterManager] audio adapter null");
        return ERR_OPERATION_FAILED;
    }

    switch (deviceType) {
        case InternalDeviceType::DEVICE_TYPE_SPEAKER:
        case InternalDeviceType::DEVICE_TYPE_FILE_SINK:
        case InternalDeviceType::DEVICE_TYPE_WIRED_HEADSET:
        case InternalDeviceType::DEVICE_TYPE_USB_HEADSET:
        case InternalDeviceType::DEVICE_TYPE_BLUETOOTH_A2DP:
        case InternalDeviceType::DEVICE_TYPE_BLUETOOTH_SCO: {
            AUDIO_INFO_LOG("SetDefaultSink %{public}d", deviceType);
            return mAudioServiceAdapter->SetDefaultSink(name);
        }
        case InternalDeviceType::DEVICE_TYPE_FILE_SOURCE:
        case InternalDeviceType::DEVICE_TYPE_MIC: {
            AUDIO_INFO_LOG("SetDefaultSource %{public}d", deviceType);
            return mAudioServiceAdapter->SetDefaultSource(name);
        }
        default:
            break;
    }
    return SUCCESS;
}

int32_t AudioAdapterManager::SetRingerMode(AudioRingerMode ringerMode)
{
    mRingerMode = ringerMode;

    // Incase if KvStore didnot connect during  bootup
    if (mAudioPolicyKvStore == nullptr) {
        bool isFirstBoot = false;
        InitAudioPolicyKvStore(isFirstBoot);
    }

    WriteRingerModeToKvStore(ringerMode);
    return SUCCESS;
}

AudioRingerMode AudioAdapterManager::GetRingerMode() const
{
    return mRingerMode;
}

AudioIOHandle AudioAdapterManager::OpenAudioPort(const AudioModuleInfo &audioModuleInfo)
{
    std::string moduleArgs = GetModuleArgs(audioModuleInfo);
    AUDIO_INFO_LOG("[Adapter load-module] %{public}s %{public}s", audioModuleInfo.lib.c_str(), moduleArgs.c_str());
    if (audioModuleInfo.lib == PIPE_SOURCE || audioModuleInfo.lib == PIPE_SINK) {
        if (!audioModuleInfo.fileName.empty() && access(audioModuleInfo.fileName.c_str(), F_OK) == 0) {
            int32_t ret = std::remove(audioModuleInfo.fileName.c_str());
            if (ret) {
                AUDIO_ERR_LOG("Removing pipe file failed!. Ret val: %{public}d", ret);
                return ERR_OPERATION_FAILED;
            }
        }
    }

    CHECK_AND_RETURN_RET_LOG(mAudioServiceAdapter != nullptr, ERR_OPERATION_FAILED, "ServiceAdapter is null");
    return mAudioServiceAdapter->OpenAudioPort(audioModuleInfo.lib, moduleArgs.c_str());
}

int32_t AudioAdapterManager::CloseAudioPort(AudioIOHandle ioHandle)
{
    CHECK_AND_RETURN_RET_LOG(mAudioServiceAdapter != nullptr, ERR_OPERATION_FAILED, "ServiceAdapter is null");
    return mAudioServiceAdapter->CloseAudioPort(ioHandle);
}

void UpdateCommonArgs(const AudioModuleInfo &audioModuleInfo, std::string &args)
{
    if (!audioModuleInfo.rate.empty()) {
        args = "rate=";
        args.append(audioModuleInfo.rate);
    }

    if (!audioModuleInfo.channels.empty()) {
        args.append(" channels=");
        args.append(audioModuleInfo.channels);
    }

    if (!audioModuleInfo.bufferSize.empty()) {
        args.append(" buffer_size=");
        args.append(audioModuleInfo.bufferSize);
    }

    if (!audioModuleInfo.format.empty()) {
        args.append(" format=");
        args.append(audioModuleInfo.format);
        AUDIO_INFO_LOG("[PolicyManager] format: %{public}s", args.c_str());
    }

    if (!audioModuleInfo.fixedLatency.empty()) {
        args.append(" fixed_latency=");
        args.append(audioModuleInfo.fixedLatency);
    }

    if (!audioModuleInfo.renderInIdleState.empty()) {
        args.append(" render_in_idle_state=");
        args.append(audioModuleInfo.renderInIdleState);
    }

    if (!audioModuleInfo.OpenMicSpeaker.empty()) {
        args.append(" open_mic_speaker=");
        args.append(audioModuleInfo.OpenMicSpeaker);
    }
}

// Private Members
std::string AudioAdapterManager::GetModuleArgs(const AudioModuleInfo &audioModuleInfo) const
{
    std::string args;

    if (audioModuleInfo.lib == HDI_SINK) {
        UpdateCommonArgs(audioModuleInfo, args);
        if (!audioModuleInfo.name.empty()) {
            args.append(" sink_name=");
            args.append(audioModuleInfo.name);
        }
 
        if (!audioModuleInfo.adapterName.empty()) {
            args.append(" adapter_name=");
            args.append(audioModuleInfo.adapterName);
        }

        if (!audioModuleInfo.className.empty()) {
            args.append(" device_class=");
            args.append(audioModuleInfo.className);
        }

        if (!audioModuleInfo.fileName.empty()) {
            args.append(" file_path=");
            args.append(audioModuleInfo.fileName);
        }
    } else if (audioModuleInfo.lib == HDI_SOURCE) {
        UpdateCommonArgs(audioModuleInfo, args);
        if (!audioModuleInfo.name.empty()) {
            args.append(" source_name=");
            args.append(audioModuleInfo.name);
        }
       
        if (!audioModuleInfo.adapterName.empty()) {
            args.append(" adapter_name=");
            args.append(audioModuleInfo.adapterName);
        }

        if (!audioModuleInfo.className.empty()) {
            args.append(" device_class=");
            args.append(audioModuleInfo.className);
        }

        if (!audioModuleInfo.fileName.empty()) {
            args.append(" file_path=");
            args.append(audioModuleInfo.fileName);
        }
    } else if (audioModuleInfo.lib == PIPE_SINK) {
        if (!audioModuleInfo.fileName.empty()) {
            args = "file=";
            args.append(audioModuleInfo.fileName);
        }
    } else if (audioModuleInfo.lib == PIPE_SOURCE) {
        if (!audioModuleInfo.fileName.empty()) {
            args = "file=";
            args.append(audioModuleInfo.fileName);
        }
    }
    return args;
}

std::string AudioAdapterManager::GetStreamNameByStreamType(AudioStreamType streamType)
{
    switch (streamType) {
        case STREAM_MUSIC:
            return "music";
        case STREAM_RING:
            return "ring";
        case STREAM_SYSTEM:
            return "system";
        case STREAM_NOTIFICATION:
            return "notification";
        case STREAM_ALARM:
            return "alarm";
        case STREAM_DTMF:
            return "dtmf";
        case STREAM_VOICE_CALL:
            return "voice_call";
        case STREAM_VOICE_ASSISTANT:
            return "voice_assistant";
        default:
            return "";
    }
}

AudioStreamType AudioAdapterManager::GetStreamIDByType(std::string streamType)
{
    AudioStreamType stream = STREAM_MUSIC;

    if (!streamType.compare(std::string("music")))
        stream = STREAM_MUSIC;
    else if (!streamType.compare(std::string("ring")))
        stream = STREAM_RING;
    else if (!streamType.compare(std::string("voice_call")))
        stream = STREAM_VOICE_CALL;
    else if (!streamType.compare(std::string("system")))
        stream = STREAM_SYSTEM;
    else if (!streamType.compare(std::string("notification")))
        stream = STREAM_NOTIFICATION;
    else if (!streamType.compare(std::string("alarm")))
        stream = STREAM_ALARM;
    else if (!streamType.compare(std::string("voice_assistant")))
        stream = STREAM_VOICE_ASSISTANT;

    return stream;
}

AudioStreamType AudioAdapterManager::GetStreamForVolumeMap(AudioStreamType streamType)
{
    switch (streamType) {
        case STREAM_MUSIC:
            return STREAM_MUSIC;
        case STREAM_NOTIFICATION:
        case STREAM_DTMF:
        case STREAM_SYSTEM:
        case STREAM_RING:
            return STREAM_RING;
        case STREAM_ALARM:
            return STREAM_ALARM;
        case STREAM_VOICE_CALL:
            return STREAM_VOICE_CALL;
        case STREAM_VOICE_ASSISTANT:
            return STREAM_VOICE_ASSISTANT;
        default:
            return STREAM_MUSIC;
    }
}

bool AudioAdapterManager::InitAudioPolicyKvStore(bool& isFirstBoot)
{
    DistributedKvDataManager manager;
    Options options;

    options.createIfMissing = false;
    options.encrypt = false;
    options.autoSync = true;
    options.kvStoreType = KvStoreType::SINGLE_VERSION;

    AppId appId;
    appId.appId = "policymanager";
    StoreId storeId;
    storeId.storeId = "audiopolicy";
    Status status = Status::SUCCESS;

    // open and initialize kvstore instance.
    if (mAudioPolicyKvStore == nullptr) {
        uint32_t retries = 0;

        do {
            status = manager.GetSingleKvStore(options, appId, storeId, mAudioPolicyKvStore);
            if (status == Status::STORE_NOT_FOUND) {
                AUDIO_ERR_LOG("[AudioAdapterManager] InitAudioPolicyKvStore: STORE_NOT_FOUND!");
            }

            if ((status == Status::SUCCESS) || (status == Status::STORE_NOT_FOUND)) {
                break;
            } else {
                AUDIO_ERR_LOG("[AudioAdapterManager] InitAudioPolicyKvStore: Kvstore Connect failed! Retrying.");
                retries++;
                usleep(KVSTORE_CONNECT_RETRY_DELAY_TIME);
            }
        } while (retries <= KVSTORE_CONNECT_RETRY_COUNT);
    }

    if (mAudioPolicyKvStore == nullptr) {
        if (status == Status::STORE_NOT_FOUND) {
            AUDIO_INFO_LOG("[AudioAdapterManager] First Boot: Create AudioPolicyKvStore");
            options.createIfMissing = true;
            // [create and] open and initialize kvstore instance.
            status = manager.GetSingleKvStore(options, appId, storeId, mAudioPolicyKvStore);
            if (status == Status::SUCCESS) {
                isFirstBoot = true;
            } else {
                AUDIO_ERR_LOG("[AudioAdapterManager] Create AudioPolicyKvStore Failed!");
            }
        }
    }

    if (mAudioPolicyKvStore == nullptr) {
        AUDIO_ERR_LOG("[AudioAdapterManager] InitAudioPolicyKvStore: Failed!");
        return false;
    }

    return true;
}

void AudioAdapterManager::InitVolumeMap(bool isFirstBoot)
{
    if (isFirstBoot == true) {
        WriteVolumeToKvStore(STREAM_MUSIC, MAX_VOLUME);
        WriteVolumeToKvStore(STREAM_RING, MAX_VOLUME);
        WriteVolumeToKvStore(STREAM_VOICE_CALL, MAX_VOLUME);
        WriteVolumeToKvStore(STREAM_VOICE_ASSISTANT, MAX_VOLUME);
        AUDIO_INFO_LOG("[AudioAdapterManager] Wrote default stream volumes to KvStore");
    } else {
        LoadVolumeMap();
    }
}

void AudioAdapterManager::InitRingerMode(bool isFirstBoot)
{
    if (mAudioPolicyKvStore == nullptr) {
        AUDIO_ERR_LOG("[AudioAdapterManager] mAudioPolicyKvStore is null!");
        return;
    }

    if (isFirstBoot == true) {
        mRingerMode = RINGER_MODE_NORMAL;
        WriteRingerModeToKvStore(RINGER_MODE_NORMAL);
        AUDIO_INFO_LOG("[AudioAdapterManager] Wrote default ringer mode to KvStore");
    } else {
        LoadRingerMode();
    }
}

bool AudioAdapterManager::LoadVolumeFromKvStore(AudioStreamType streamType)
{
    Key key;
    Value value;

    switch (streamType) {
        case STREAM_MUSIC:
            key = "music";
            break;
        case STREAM_RING:
            key = "ring";
            break;
        case STREAM_VOICE_CALL:
            key = "voice_call";
            break;
        case STREAM_VOICE_ASSISTANT:
            key = "voice_assistant";
            break;
        default:
            return false;
    }

    Status status = mAudioPolicyKvStore->Get(key, value);
    if (status == Status::SUCCESS) {
        float volume = TransferByteArrayToType<float>(value.Data());
        mVolumeMap[streamType] = volume;
        AUDIO_DEBUG_LOG("[AudioAdapterManager] volume from kvStore %{public}f for streamType:%{public}d",
                        volume, streamType);
        return true;
    }

    return false;
}

bool AudioAdapterManager::LoadVolumeMap(void)
{
    if (mAudioPolicyKvStore == nullptr) {
        AUDIO_ERR_LOG("[AudioAdapterManager] LoadVolumeMap: mAudioPolicyKvStore is null!");
        return false;
    }

    if (!LoadVolumeFromKvStore(STREAM_MUSIC))
        AUDIO_ERR_LOG("[AudioAdapterManager] LoadVolumeMap: Couldnot load volume for Music from kvStore!");

    if (!LoadVolumeFromKvStore(STREAM_RING))
        AUDIO_ERR_LOG("[AudioAdapterManager] LoadVolumeMap: Couldnot load volume for Ring from kvStore!");

    if (!LoadVolumeFromKvStore(STREAM_VOICE_CALL))
        AUDIO_ERR_LOG("[AudioAdapterManager] LoadVolumeMap: Couldnot load volume for voice_call from kvStore!");

    if (!LoadVolumeFromKvStore(STREAM_VOICE_ASSISTANT))
        AUDIO_ERR_LOG("[AudioAdapterManager] LoadVolumeMap: Couldnot load volume for voice_assistant from kvStore!");

    return true;
}

bool AudioAdapterManager::LoadRingerMode(void)
{
    if (mAudioPolicyKvStore == nullptr) {
        AUDIO_ERR_LOG("[AudioAdapterManager] LoadRingerMap: mAudioPolicyKvStore is null!");
        return false;
    }

    // get ringer mode value from kvstore.
    Key key = "ringermode";
    Value value;
    Status status = mAudioPolicyKvStore->Get(key, value);
    if (status == Status::SUCCESS) {
        mRingerMode = static_cast<AudioRingerMode>(TransferByteArrayToType<int>(value.Data()));
        AUDIO_DEBUG_LOG("[AudioAdapterManager] Ringer Mode from kvStore %{public}d", mRingerMode);
    }

    return true;
}

void AudioAdapterManager::WriteVolumeToKvStore(AudioStreamType streamType, float volume)
{
    if (mAudioPolicyKvStore == nullptr)
        return;

    Key key = GetStreamNameByStreamType(streamType);
    Value value = Value(TransferTypeToByteArray<float>(volume));

    Status status = mAudioPolicyKvStore->Put(key, value);
    if (status == Status::SUCCESS) {
        AUDIO_INFO_LOG("[AudioAdapterManager] volume %{public}f for %{public}s updated in kvStore", volume,
            GetStreamNameByStreamType(streamType).c_str());
    } else {
        AUDIO_ERR_LOG("[AudioAdapterManager] volume %{public}f for %{public}s failed to update in kvStore!", volume,
            GetStreamNameByStreamType(streamType).c_str());
    }

    return;
}

void AudioAdapterManager::WriteRingerModeToKvStore(AudioRingerMode ringerMode)
{
    if (mAudioPolicyKvStore == nullptr)
        return;

    Key key = "ringermode";
    Value value = Value(TransferTypeToByteArray<int>(ringerMode));

    Status status = mAudioPolicyKvStore->Put(key, value);
    if (status == Status::SUCCESS) {
        AUDIO_INFO_LOG("[AudioAdapterManager] Wrote RingerMode:%d to kvStore", ringerMode);
    } else {
        AUDIO_ERR_LOG("[AudioAdapterManager] Writing RingerMode:%d to kvStore failed!", ringerMode);
    }

    return;
}

void AudioAdapterManager::InitMuteStatusMap(bool isFirstBoot)
{
    if (isFirstBoot == true) {
        WriteMuteStatusToKvStore(STREAM_MUSIC, 0);
        WriteMuteStatusToKvStore(STREAM_RING, 0);
        WriteMuteStatusToKvStore(STREAM_VOICE_CALL, 0);
        WriteMuteStatusToKvStore(STREAM_VOICE_ASSISTANT, 0);
        AUDIO_INFO_LOG("[AudioAdapterManager] Wrote default mute status to KvStore");
    } else {
        LoadMuteStatusMap();
    }
}


bool AudioAdapterManager::LoadMuteStatusMap(void)
{
    if (mAudioPolicyKvStore == nullptr) {
        AUDIO_ERR_LOG("[AudioAdapterManager] LoadMuteStatusMap: mAudioPolicyKvStore is null!");
        return false;
    }

    if (!LoadMuteStatusFromKvStore(STREAM_MUSIC))
        AUDIO_ERR_LOG("[AudioAdapterManager] Could not load mute status for MUSIC from kvStore!");

    if (!LoadMuteStatusFromKvStore(STREAM_RING))
        AUDIO_ERR_LOG("[AudioAdapterManager] Could not load mute status for RING from kvStore!");

    if (!LoadMuteStatusFromKvStore(STREAM_VOICE_CALL))
        AUDIO_ERR_LOG("[AudioAdapterManager] Could not load mute status for VOICE_CALL from kvStore!");

    if (!LoadMuteStatusFromKvStore(STREAM_VOICE_ASSISTANT))
        AUDIO_ERR_LOG("[AudioAdapterManager] Could not load mute status for VOICE_ASSISTANT from kvStore!");

    return true;
}

bool AudioAdapterManager::LoadMuteStatusFromKvStore(AudioStreamType streamType)
{
    Value value;

    switch (streamType) {
        case STREAM_MUSIC:
            break;
        case STREAM_RING:
            break;
        case STREAM_VOICE_CALL:
            break;
        case STREAM_VOICE_ASSISTANT:
            break;
        default:
            return false;
    }
    Key key = GetStreamTypeKeyForMute(streamType);
    Status status = mAudioPolicyKvStore->Get(key, value);
    if (status == Status::SUCCESS) {
        int volumeStatus = TransferByteArrayToType<int>(value.Data());
        mMuteStatusMap[streamType] = volumeStatus;
        return true;
    }

    return false;
}

std::string AudioAdapterManager::GetStreamTypeKeyForMute(AudioStreamType streamType)
{
    switch (streamType) {
        case STREAM_MUSIC:
            return "music_mute_status";
        case STREAM_RING:
            return "ring_mute_status";
        case STREAM_SYSTEM:
            return "system_mute_status";
        case STREAM_NOTIFICATION:
            return "notification_mute_status";
        case STREAM_ALARM:
            return "alarm_mute_status";
        case STREAM_DTMF:
            return "dtmf_mute_status";
        case STREAM_VOICE_CALL:
            return "voice_call_mute_status";
        case STREAM_VOICE_ASSISTANT:
            return "voice_assistant_mute_status";
        default:
            return "";
    }
}

void AudioAdapterManager::WriteMuteStatusToKvStore(AudioStreamType streamType, bool muteStatus)
{
    if (mAudioPolicyKvStore == nullptr) {
        return;
    }

    Key key = GetStreamTypeKeyForMute(streamType);
    Value value = Value(TransferTypeToByteArray<int>(muteStatus));

    Status status = mAudioPolicyKvStore->Put(key, value);
    if (status == Status::SUCCESS) {
        AUDIO_INFO_LOG("[AudioAdapterManager] muteStatus %{public}d for %{public}s updated to kvStore", muteStatus,
            GetStreamNameByStreamType(streamType).c_str());
    } else {
        AUDIO_INFO_LOG("[AudioAdapterManager] muteStatus %{public}d for %{public}s update to kvStore failed",
            muteStatus, GetStreamNameByStreamType(streamType).c_str());
    }

    return;
}
} // namespace AudioStandard
} // namespace OHOS
