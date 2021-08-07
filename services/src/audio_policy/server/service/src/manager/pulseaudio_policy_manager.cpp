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

#include <unistd.h>

#include "audio_errors.h"
#include "media_log.h"
#include "pulseaudio_policy_manager.h"

namespace OHOS {
namespace AudioStandard {
bool PulseAudioPolicyManager::Init()
{
    mMainLoop = pa_threaded_mainloop_new();
    if (!mMainLoop) {
        MEDIA_ERR_LOG("[PolicyManager] MainLoop creation failed");
        return false;
    }

    if (pa_threaded_mainloop_start(mMainLoop) < 0) {
        MEDIA_ERR_LOG("[PolicyManager] Failed to start mainloop");
        pa_threaded_mainloop_free (mMainLoop);
        return false;
    }

    pa_threaded_mainloop_lock(mMainLoop);

    while (true) {
        pa_context_state_t state;

        if (mContext != NULL) {
            state = pa_context_get_state(mContext);
            if (state == PA_CONTEXT_READY)
                break;
            // if pulseaudio is ready, retry connect to pulseaudio. before retry wait for sometime. reduce sleep later
            usleep(PA_CONNECT_RETRY_SLEEP_IN_MICRO_SECONDS);
        }

        bool result = ConnectToPulseAudio();
        if (!result || !PA_CONTEXT_IS_GOOD(pa_context_get_state(mContext))) {
            continue;
        }

        MEDIA_DEBUG_LOG("[PolicyManager] pa context not ready... wait");

        // Wait for the context to be ready
        pa_threaded_mainloop_wait(mMainLoop);
    }

    pa_threaded_mainloop_unlock(mMainLoop);

    bool isFirstBoot = false;
    InitAudioPolicyKvStore(isFirstBoot);
    InitVolumeMap(isFirstBoot);
    InitRingerMode(isFirstBoot);
    return true;
}

void PulseAudioPolicyManager::Deinit(void)
{
    if (mContext != NULL) {
        pa_context_disconnect (mContext);

        /* Make sure we don't get any further callbacks */
        pa_context_set_state_callback (mContext, NULL, NULL);
        pa_context_set_subscribe_callback (mContext, NULL, NULL);

        pa_context_unref (mContext);
    }

    if (mMainLoop != NULL) {
        pa_threaded_mainloop_stop (mMainLoop);
        pa_threaded_mainloop_free (mMainLoop);
    }

    return;
}

int32_t PulseAudioPolicyManager::SetStreamVolume(AudioStreamType streamType, float volume)
{
    std::unique_ptr<UserData> userData = std::make_unique<UserData>();
    userData->thiz = this;
    userData->volume = volume;
    userData->streamType = streamType;

    if (mContext == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] mContext is nullptr");
        return ERROR;
    }

    // Incase if KvStore didnot connect during  bootup
    if (mAudioPolicyKvStore == nullptr) {
        bool isFirstBoot = false;
        InitAudioPolicyKvStore(isFirstBoot);
    }

    mVolumeMap[streamType] = volume;
    WriteVolumeToKvStore(streamType, volume);

    pa_threaded_mainloop_lock(mMainLoop);

    pa_operation *operation = pa_context_get_sink_input_info_list(mContext,
        PulseAudioPolicyManager::GetSinkInputInfoVolumeCb, reinterpret_cast<void*>(userData.get()));
    if (operation == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] pa_context_get_sink_input_info_list returned nullptr");
        pa_threaded_mainloop_unlock(mMainLoop);
        return ERROR;
    }
    userData.release();

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mMainLoop);
    }

    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mMainLoop);
    return SUCCESS;
}

float PulseAudioPolicyManager::GetStreamVolume(AudioStreamType streamType)
{
    return mVolumeMap[streamType];
}

int32_t PulseAudioPolicyManager::SetStreamMute(AudioStreamType streamType, bool mute)
{
    std::unique_ptr<UserData> userData = std::make_unique<UserData>();
    userData->thiz = this;
    userData->mute = mute;
    userData->streamType = streamType;

    if (mContext == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] mContext is nullptr");
        return ERROR;
    }

    pa_threaded_mainloop_lock(mMainLoop);

    pa_operation* operation = pa_context_get_sink_input_info_list(mContext,
        PulseAudioPolicyManager::GetSinkInputInfoMuteCb, reinterpret_cast<void*>(userData.get()));
    if (operation == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] pa_context_get_sink_input_info_list returned nullptr");
        pa_threaded_mainloop_unlock(mMainLoop);
        return ERROR;
    }

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mMainLoop);
    }

    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mMainLoop);

    return SUCCESS;
}

bool PulseAudioPolicyManager::GetStreamMute(AudioStreamType streamType)
{
    std::unique_ptr<UserData> userData = std::make_unique<UserData>();
    userData->thiz = this;
    userData->streamType = streamType;
    userData->mute = false;

    if (mContext == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] mContext is nullptr");
        return false;
    }

    pa_threaded_mainloop_lock(mMainLoop);

    pa_operation *operation = pa_context_get_sink_input_info_list(mContext,
        PulseAudioPolicyManager::GetSinkInputInfoMuteStatusCb, reinterpret_cast<void*>(userData.get()));
    if (operation == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] pa_context_get_sink_input_info_list returned nullptr");
        pa_threaded_mainloop_unlock(mMainLoop);
        return false;
    }

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mMainLoop);
    }

    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mMainLoop);

    return (userData->mute) ? true : false;
}

bool PulseAudioPolicyManager::IsStreamActive(AudioStreamType streamType)
{
    std::unique_ptr<UserData> userData = std::make_unique<UserData>();
    userData->thiz = this;
    userData->streamType = streamType;
    userData->isCorked = true;

    if (mContext == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] mContext is nullptr");
        return false;
    }

    pa_threaded_mainloop_lock(mMainLoop);

    pa_operation *operation = pa_context_get_sink_input_info_list(mContext,
        PulseAudioPolicyManager::GetSinkInputInfoCorkStatusCb, reinterpret_cast<void*>(userData.get()));
    if (operation == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] pa_context_get_sink_input_info_list returned nullptr");
        pa_threaded_mainloop_unlock(mMainLoop);
        return false;
    }

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mMainLoop);
    }

    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mMainLoop);

    MEDIA_INFO_LOG("[PolicyManager] cork for stream %s : %d",
        GetStreamNameByStreamType(streamType).c_str(), userData->isCorked);

    return (userData->isCorked) ? false : true;
}


int32_t PulseAudioPolicyManager::SetDeviceActive(AudioIOHandle ioHandle, DeviceType deviceType,
                                                 std::string name, bool active)
{
    pa_threaded_mainloop_lock(mMainLoop);

    switch (deviceType) {
        case SPEAKER:
        case BLUETOOTH_A2DP: {
            pa_operation* operation = pa_context_set_default_sink(mContext, name.c_str(), NULL, NULL);
            if (operation == NULL) {
                MEDIA_ERR_LOG("[PolicyManager] set default sink failed!");
                pa_threaded_mainloop_unlock(mMainLoop);
                return ERR_OPERATION_FAILED;
            }
            pa_operation_unref(operation);
            break;
        }
        case MIC:
        case BLUETOOTH_SCO: {
            pa_operation* operation = pa_context_set_default_source(mContext, name.c_str(), NULL, NULL);
            if (operation == NULL) {
                MEDIA_ERR_LOG("[PolicyManager] set default source failed!");
                pa_threaded_mainloop_unlock(mMainLoop);
                return ERR_OPERATION_FAILED;
            }

            pa_operation_unref(operation);
            break;
        }
        default:
            break;
    }

    pa_threaded_mainloop_unlock(mMainLoop);
    return SUCCESS;
}

int32_t PulseAudioPolicyManager::SetRingerMode(AudioRingerMode ringerMode)
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

AudioRingerMode PulseAudioPolicyManager::GetRingerMode()
{
    return mRingerMode;
}

AudioIOHandle PulseAudioPolicyManager::OpenAudioPort(std::shared_ptr<AudioPortInfo> audioPortInfo)
{
    std::string moduleArgs = GetModuleArgs(audioPortInfo);
    MEDIA_INFO_LOG("[PolicyManager] load-module %{public}s %{public}s", audioPortInfo->name, moduleArgs.c_str());

    if (!strcmp(audioPortInfo->name, PIPE_SOURCE) || !strcmp(audioPortInfo->name, PIPE_SINK)) {
        if (audioPortInfo->fileName != nullptr) {
            if (access(audioPortInfo->fileName, F_OK) == 0) {
                int32_t ret = std::remove(audioPortInfo->fileName);
                if(ret) {
                    MEDIA_ERR_LOG("[PolicyManager] Error Removing file: %{public}s Failed! ret val: %{public}d",
                                  audioPortInfo->fileName, ret);
                }
            } else {
                MEDIA_DEBUG_LOG("[PolicyManager] File: %{public}s does not exist!", audioPortInfo->fileName);
           }
        } else {
            MEDIA_ERR_LOG("[PolicyManager] Error audioPortInfo->fileName is null!");
        }
    }

    pa_threaded_mainloop_lock(mMainLoop);

    std::unique_ptr<UserData> userData = std::make_unique<UserData>();
    userData->thiz = this;

    pa_operation* operation = pa_context_load_module(mContext, audioPortInfo->name,  moduleArgs.c_str(), ModuleLoadCb,
        reinterpret_cast<void*>(userData.get()));
    if (operation == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] pa_context_load_module returned nullptr");
        pa_threaded_mainloop_unlock(mMainLoop);
        return reinterpret_cast<AudioIOHandle>(ERR_INVALID_HANDLE);
    }

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mMainLoop);
    }

    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mMainLoop);

    AudioIOHandle ioHandle = reinterpret_cast<void*>(userData->idx);
    return ioHandle;
}

int32_t PulseAudioPolicyManager::CloseAudioPort(AudioIOHandle ioHandle)
{
    pa_threaded_mainloop_lock(mMainLoop);

    pa_operation* operation = pa_context_unload_module(mContext, reinterpret_cast<uint32_t>(ioHandle), NULL, NULL);
    if (operation == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] pa_context_unload_module returned nullptr!");
        pa_threaded_mainloop_unlock(mMainLoop);
        return ERROR;
    }

    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mMainLoop);
    return SUCCESS;
}

// Private Members
bool PulseAudioPolicyManager::ConnectToPulseAudio(void)
{
    if (mContext != NULL) {
        pa_context_disconnect (mContext);
        pa_context_set_state_callback (mContext, NULL, NULL);
        pa_context_set_subscribe_callback (mContext, NULL, NULL);

        pa_context_unref (mContext);
    }

    pa_proplist *proplist = pa_proplist_new();
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, "PulseAudio Service");
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_ID, "com.ohos.pulseaudio.service");
    mContext = pa_context_new_with_proplist(pa_threaded_mainloop_get_api(mMainLoop), NULL, proplist);
    pa_proplist_free(proplist);

    if (mContext == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] creating pa context failed");
        return false;
    }

    pa_context_set_state_callback(mContext, PulseAudioPolicyManager::ContextStateCb, this);

    if (pa_context_connect(mContext, NULL, PA_CONTEXT_NOFAIL, NULL) < 0) {
        if (pa_context_errno(mContext) == PA_ERR_INVALID) {
            MEDIA_ERR_LOG("[PolicyManager] pa context connect failed: %{public}s",
                pa_strerror(pa_context_errno(mContext)));
            goto Fail;
        }
    }

    return true;

Fail:
    /* Make sure we don't get any further callbacks */
    pa_context_set_state_callback (mContext, NULL, NULL);
    pa_context_set_subscribe_callback (mContext, NULL, NULL);
    pa_context_unref (mContext);
    return false;
}

std::string PulseAudioPolicyManager::GetModuleArgs(std::shared_ptr<AudioPortInfo> audioPortInfo)
{
    std::string args;

    if (!strcmp(audioPortInfo->name, HDI_SINK)) {
        if (audioPortInfo->rate != nullptr) {
            args = "rate=";
            args.append(audioPortInfo->rate);
        }

        if (audioPortInfo->channels != nullptr) {
            args.append(" channels=");
            args.append(audioPortInfo->channels);
        }

        if (audioPortInfo->buffer_size != nullptr) {
            args.append(" buffer_size=");
            args.append(audioPortInfo->buffer_size);
        }
    } else if (!strcmp(audioPortInfo->name, HDI_SOURCE)) {
        if (audioPortInfo->rate != nullptr) {
            args = "rate=";
            args.append(audioPortInfo->rate);
        }

        if (audioPortInfo->channels != nullptr) {
            args.append(" channels=");
            args.append(audioPortInfo->channels);
        }

        if (audioPortInfo->buffer_size != nullptr) {
            args.append(" buffer_size=");
            args.append(audioPortInfo->buffer_size);
        }
    } else if (!strcmp(audioPortInfo->name, PIPE_SINK)) {
        if (audioPortInfo->fileName != nullptr) {
            args = "file=";
            args.append(audioPortInfo->fileName);
        }
    } else if (!strcmp(audioPortInfo->name, PIPE_SOURCE)) {
        if (audioPortInfo->fileName != nullptr) {
            args = "file=";
            args.append(audioPortInfo->fileName);
        }
    }

    return args;
}

std::string PulseAudioPolicyManager::GetStreamNameByStreamType(AudioStreamType streamType)
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
        default:
            return "";
    }
}

AudioStreamType PulseAudioPolicyManager::GetStreamIDByType(std::string streamType)
{
    AudioStreamType stream = STREAM_MUSIC;

    if (!streamType.compare(std::string("music")))
        stream = STREAM_MUSIC;
    else if (!streamType.compare(std::string("ring")))
        stream = STREAM_RING;
    else if (!streamType.compare(std::string("system")))
        stream = STREAM_SYSTEM;
    else if (!streamType.compare(std::string("notification")))
        stream = STREAM_NOTIFICATION;
    else if (!streamType.compare(std::string("alarm")))
        stream = STREAM_ALARM;

    return stream;
}

bool PulseAudioPolicyManager::InitAudioPolicyKvStore(bool& isFirstBoot)
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

    // open and initialize kvstore instance.
    if (mAudioPolicyKvStore == nullptr) {
        manager.GetSingleKvStore(
        options, appId, storeId, [&](Status status, std::unique_ptr<SingleKvStore> singleKvStore) {
            if (status == Status::STORE_NOT_FOUND) {
                MEDIA_ERR_LOG("[PolicyManager] InitAudioPolicyKvStore: STORE_NOT_FOUND!");
                return;
            } else {
                mAudioPolicyKvStore = std::move(singleKvStore);
            }
        });
    }

    if (mAudioPolicyKvStore == nullptr) {
        MEDIA_INFO_LOG("[PolicyManager] First Boot: Create AudioPolicyKvStore");
        options.createIfMissing = true;
        // [create and] open and initialize kvstore instance.
        manager.GetSingleKvStore(
         options, appId, storeId,[&](Status status, std::unique_ptr<SingleKvStore> singleKvStore) {
            if (status != Status::SUCCESS) {
                MEDIA_ERR_LOG("[PolicyManager] Create AudioPolicyKvStore Failed!");
                return;
            }

            mAudioPolicyKvStore = std::move(singleKvStore);
            isFirstBoot = true;
        });
    }

    if (mAudioPolicyKvStore == nullptr) {
        MEDIA_ERR_LOG("[PolicyManager] InitAudioPolicyKvStore: Failed!");
        return false;
    }

    return true;
}

void PulseAudioPolicyManager::InitVolumeMap(bool isFirstBoot)
{
    if (isFirstBoot == true) {
        WriteVolumeToKvStore(STREAM_MUSIC, MAX_VOLUME);
        WriteVolumeToKvStore(STREAM_RING, MAX_VOLUME);
        MEDIA_INFO_LOG("[PolicyManager] Wrote default stream volumes to KvStore");
    } else {
        LoadVolumeMap();
    }
    return;
}

void PulseAudioPolicyManager::InitRingerMode(bool isFirstBoot)
{
    if (mAudioPolicyKvStore == nullptr) {
        MEDIA_ERR_LOG("[PolicyManager] mAudioPolicyKvStore is null!");
        return;
    }

    if (isFirstBoot == true) {
        mRingerMode = RINGER_MODE_NORMAL;
        WriteRingerModeToKvStore(RINGER_MODE_NORMAL);
        MEDIA_INFO_LOG("[PolicyManager] Wrote default ringer mode to KvStore");
    } else {
        LoadRingerMode();
    }
}

bool PulseAudioPolicyManager::LoadVolumeFromKvStore(AudioStreamType streamType)
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
        default:
            return false;
    }

    Status status = mAudioPolicyKvStore->Get(key, value);
    if (status == Status::SUCCESS) {
        float volume = TransferByteArrayToType<float>(value.Data());
        mVolumeMap[streamType] = volume;
        MEDIA_DEBUG_LOG("[PolicyManager] volume from kvStore %{public}f for streamType:%{public}d",
                        volume, streamType);
        return true;
    }

    return false;
}

bool PulseAudioPolicyManager::LoadVolumeMap(void)
{
    if (mAudioPolicyKvStore == nullptr) {
        MEDIA_ERR_LOG("[PolicyManager] LoadVolumeMap: mAudioPolicyKvStore is null!");
        return false;
    }

    if (!LoadVolumeFromKvStore(STREAM_MUSIC))
        MEDIA_ERR_LOG("[PolicyManager] LoadVolumeMap: Couldnot load volume for Music from kvStore!");

    if (!LoadVolumeFromKvStore(STREAM_RING))
        MEDIA_ERR_LOG("[PolicyManager] LoadVolumeMap: Couldnot load volume for Ring from kvStore!");

    return true;
}

bool PulseAudioPolicyManager::LoadRingerMode(void)
{
    if (mAudioPolicyKvStore == nullptr) {
        MEDIA_ERR_LOG("[PolicyManager] LoadRingerMap: mAudioPolicyKvStore is null!");
        return false;
    }

    // get ringer mode value from kvstore.
    Key key = "ringermode";
    Value value;
    Status status = mAudioPolicyKvStore->Get(key, value);
    if (status == Status::SUCCESS) {
        mRingerMode = static_cast<AudioRingerMode>(TransferByteArrayToType<int>(value.Data()));
        MEDIA_DEBUG_LOG("[PolicyManager] Ringer Mode from kvStore %{public}d", mRingerMode);
    }

    return true;
}

void PulseAudioPolicyManager::WriteVolumeToKvStore(AudioStreamType streamType, float volume)
{
    if (mAudioPolicyKvStore == nullptr)
        return;

    Key key = GetStreamNameByStreamType(streamType);
    Value value = Value(TransferTypeToByteArray<float>(volume));

    Status status = mAudioPolicyKvStore->Put(key, value);
    if (status == Status::SUCCESS) {
        MEDIA_INFO_LOG("[PolicyManager] volume %{public}f for %{public}s updated in kvStore", volume,
                    GetStreamNameByStreamType(streamType).c_str());
    } else {
        MEDIA_ERR_LOG("[PolicyManager] volume %{public}f for %{public}s failed to update in kvStore!", volume,
                    GetStreamNameByStreamType(streamType).c_str());
    }

    return;
}

void PulseAudioPolicyManager::WriteRingerModeToKvStore(AudioRingerMode ringerMode)
{
    if (mAudioPolicyKvStore == nullptr)
        return;

    Key key = "ringermode";
    Value value = Value(TransferTypeToByteArray<int>(ringerMode));

    Status status = mAudioPolicyKvStore->Put(key, value);
    if (status == Status::SUCCESS) {
        MEDIA_INFO_LOG("[PolicyManager] Wrote RingerMode:%d to kvStore", ringerMode);
    } else {
        MEDIA_ERR_LOG("[PolicyManager] Writing RingerMode:%d to kvStore failed!", ringerMode);
    }

    return;
}

void PulseAudioPolicyManager::HandleSinkInputEvent(pa_context *c, pa_subscription_event_type_t t, uint32_t idx,
    void *userdata)
{
    std::unique_ptr<UserData> userData = std::make_unique<UserData>();
    userData->thiz = this;

    if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
        pa_operation* operation = pa_context_get_sink_input_info(c, idx,
            PulseAudioPolicyManager::GetSinkInputInfoVolumeCb, reinterpret_cast<void*>(userData.get()));
        if (operation == NULL) {
            MEDIA_ERR_LOG("[PolicyManager] pa_context_get_sink_input_info_list returned nullptr");
            return;
        }
        userData.release();

        pa_operation_unref(operation);
    }

    return;
}

void PulseAudioPolicyManager::ContextStateCb(pa_context *c, void *userdata)
{
    PulseAudioPolicyManager* thiz = reinterpret_cast<PulseAudioPolicyManager*>(userdata);

    MEDIA_DEBUG_LOG("[PolicyManager] ContextStateCb %d", pa_context_get_state(c));

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY: {
            pa_context_set_subscribe_callback(c, PulseAudioPolicyManager::SubscribeCb, thiz);

            pa_operation* operation = pa_context_subscribe(c, (pa_subscription_mask_t)
                (PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE |
                PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT |
                PA_SUBSCRIPTION_MASK_CARD), NULL, NULL);
            if (operation == NULL) {
                pa_threaded_mainloop_signal(thiz->mMainLoop, 0);
                return;
            }
            pa_operation_unref(operation);
            pa_threaded_mainloop_signal(thiz->mMainLoop, 0);
            break;
        }

        case PA_CONTEXT_FAILED:
            pa_threaded_mainloop_signal(thiz->mMainLoop, 0);
            return;

        case PA_CONTEXT_TERMINATED:
        default:
            return;
    }
}

void PulseAudioPolicyManager::SubscribeCb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata)
{
    PulseAudioPolicyManager* thiz = reinterpret_cast<PulseAudioPolicyManager*>(userdata);

    switch (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
        case PA_SUBSCRIPTION_EVENT_SINK:
            break;

        case PA_SUBSCRIPTION_EVENT_SOURCE:
            break;

        case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
            thiz->HandleSinkInputEvent(c, t, idx, userdata);
            break;

        case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
            break;

        default:
            break;
    }

    return;
}

void PulseAudioPolicyManager::ModuleLoadCb(pa_context *c, uint32_t idx, void *userdata)
{
    UserData *userData = reinterpret_cast<UserData*>(userdata);

    if (idx == PA_INVALID_INDEX) {
        MEDIA_ERR_LOG("[PolicyManager] Failure: %s", pa_strerror(pa_context_errno(c)));
        userData->idx = PA_INVALID_INDEX;
    } else {
        userData->idx = idx;
    }
    pa_threaded_mainloop_signal(userData->thiz->mMainLoop, 0);

    return;
}

void PulseAudioPolicyManager::GetSinkInputInfoVolumeCb(pa_context *c, const pa_sink_input_info *i, int eol,
    void *userdata)
{
    UserData* userData = reinterpret_cast<UserData*>(userdata);
    PulseAudioPolicyManager* thiz = userData->thiz;

    MEDIA_ERR_LOG("[PolicyManager] GetSinkInputInfoVolumeCb");
    if (eol < 0) {
        delete userData;
        MEDIA_ERR_LOG("[PolicyManager] Failed to get sink input information: %s", pa_strerror(pa_context_errno(c)));
        return;
    }

    if (eol) {
        pa_threaded_mainloop_signal(thiz->mMainLoop, 0);
        delete userData;
        return;
    }

    if (i->proplist == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] Invalid Proplist for sink input (%{public}d).", i->index);
        return;
    }

    const char *streamtype = pa_proplist_gets(i->proplist, "stream.type");
    if (streamtype == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] Invalid StreamType.");
        return;
    }

    std::string streamType(streamtype);

    AudioStreamType streamID = thiz->GetStreamIDByType(streamType);
    float vol = thiz->mVolumeMap[streamID];

    if (thiz->mRingerMode != RINGER_MODE_NORMAL) {
        if (!streamType.compare("ring")) {
            vol = MIN_VOLUME;
        }
    }

    pa_cvolume cv = i->volume;
    int32_t volume = pa_sw_volume_from_linear(vol);
    pa_cvolume_set(&cv, i->channel_map.channels, volume);
    pa_operation_unref(pa_context_set_sink_input_volume(c, i->index, &cv, NULL, NULL));

    if (streamID == userData->streamType) {
        if (i->mute) {
            pa_operation_unref(pa_context_set_sink_input_mute(c, i->index, 0, NULL, NULL));
        }
    }

    MEDIA_INFO_LOG("[PolicyManager] Applied volume : %{public}f for stream : %{public}s, volumeInt%{public}d",
        vol, i->name, volume);

    return;
}

void PulseAudioPolicyManager::GetSinkInputInfoCb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata)
{
    UserData* userData = reinterpret_cast<UserData*>(userdata);

    if (eol < 0) {
        MEDIA_ERR_LOG("[PolicyManager] Failed to get sink input information: %{public}s",
            pa_strerror(pa_context_errno(c)));
        return;
    }

    pa_operation* operation = pa_context_move_sink_input_by_index(c, i->index, userData->idx, NULL, NULL);
    if (operation == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] Moving Sink Input %{public}d to Sink %{public}d Failed",
            i->index, userData->idx);
    }
    MEDIA_ERR_LOG("[PolicyManager] Moving Sink Input %{public}d to Sink %{public}d", i->index, userData->idx);

    if (eol) {
        MEDIA_DEBUG_LOG("[PolicyManager] All inputs moved");
        pa_threaded_mainloop_signal(userData->thiz->mMainLoop, 0);
        return;
    }

    return;
}

void PulseAudioPolicyManager::GetSinkInputInfoMuteCb(pa_context *c, const pa_sink_input_info *i,
                                                     int eol, void *userdata)
{
    UserData* userData = reinterpret_cast<UserData*>(userdata);
    PulseAudioPolicyManager* thiz = userData->thiz;

    if (eol < 0) {
        MEDIA_ERR_LOG("[PolicyManager] Failed to get sink input information: %s", pa_strerror(pa_context_errno(c)));
        return;
    }

    if (eol) {
        pa_threaded_mainloop_signal(thiz->mMainLoop, 0);
        return;
    }

    if (i->proplist == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] Invalid Proplist for sink input (%{public}d).", i->index);
        return;
    }

    const char *streamtype = pa_proplist_gets(i->proplist, "stream.type");
    if (streamtype == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] Invalid StreamType.");
        return;
    }

    std::string streamType(streamtype);
    if (!streamType.compare(thiz->GetStreamNameByStreamType(userData->streamType))) {
        pa_operation_unref(pa_context_set_sink_input_mute(c, i->index, (userData->mute) ? 1 : 0, NULL, NULL));

        MEDIA_INFO_LOG("[PolicyManager] Applied Mute : %{public}d for stream : %{public}s", userData->mute, i->name);
    }

    return;
}

void PulseAudioPolicyManager::GetSinkInputInfoMuteStatusCb(pa_context *c, const pa_sink_input_info *i, int eol,
                                                           void *userdata)
{
    UserData* userData = reinterpret_cast<UserData*>(userdata);
    PulseAudioPolicyManager* thiz = userData->thiz;

    if (eol < 0) {
        MEDIA_ERR_LOG("[PolicyManager] Failed to get sink input information: %s", pa_strerror(pa_context_errno(c)));
        return;
    }

    if (eol) {
        pa_threaded_mainloop_signal(thiz->mMainLoop, 0);
        return;
    }

    if (i->proplist == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] Invalid Proplist for sink input (%{public}d).", i->index);
        return;
    }

    const char *streamtype = pa_proplist_gets(i->proplist, "stream.type");
    if (streamtype == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] Invalid StreamType.");
        return;
    }

    std::string streamType(streamtype);
    if (!streamType.compare(thiz->GetStreamNameByStreamType(userData->streamType))) {
        userData->mute = i->mute;
        MEDIA_INFO_LOG("[PolicyManager] Mute : %{public}d for stream : %{public}s", userData->mute, i->name);
    }

    return;
}

void PulseAudioPolicyManager::GetSinkInputInfoCorkStatusCb(pa_context *c, const pa_sink_input_info *i, int eol,
                                                          void *userdata)
{
    UserData* userData = reinterpret_cast<UserData*>(userdata);
    PulseAudioPolicyManager* thiz = userData->thiz;

    if (eol < 0) {
        MEDIA_ERR_LOG("[PolicyManager] Failed to get sink input information: %s", pa_strerror(pa_context_errno(c)));
        return;
    }

    if (eol) {
        pa_threaded_mainloop_signal(thiz->mMainLoop, 0);
        return;
    }

    if (i->proplist == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] Invalid Proplist for sink input (%{public}d).", i->index);
        return;
    }

    const char *streamtype = pa_proplist_gets(i->proplist, "stream.type");
    if (streamtype == NULL) {
        MEDIA_ERR_LOG("[PolicyManager] Invalid StreamType.");
        return;
    }

    std::string streamType(streamtype);
    if (!streamType.compare(thiz->GetStreamNameByStreamType(userData->streamType))) {
        userData->isCorked = i->corked;
        MEDIA_INFO_LOG("[PolicyManager] corked : %{public}d for stream : %{public}s", userData->isCorked, i->name);
    }

    return;
}
} // namespace AudioStandard
} // namespace OHOS
