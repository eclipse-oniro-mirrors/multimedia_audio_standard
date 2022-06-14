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

#ifndef ST_AUDIO_POLICY_INTERFACE_H
#define ST_AUDIO_POLICY_INTERFACE_H

#include "audio_module_info.h"
#include "audio_info.h"
#include "audio_policy_manager.h"
#include "audio_policy_types.h"
#include "audio_session_callback.h"

#include <memory>
#include <string>

namespace OHOS {
namespace AudioStandard {
class IAudioPolicyInterface {
public:
    virtual ~IAudioPolicyInterface() {}

    virtual bool Init() = 0;

    virtual void InitKVStore() = 0;

    virtual bool ConnectServiceAdapter() = 0;

    virtual int32_t SetStreamVolume(AudioStreamType streamType, float volume) = 0;

    virtual float GetStreamVolume(AudioStreamType streamType) = 0;

    virtual int32_t SetStreamMute(AudioStreamType streamType, bool mute) = 0;

    virtual bool GetStreamMute(AudioStreamType streamType) = 0;

    virtual bool IsStreamActive(AudioStreamType streamType) = 0;

    virtual std::vector<SinkInput> GetAllSinkInputs() = 0;

    virtual std::vector<SourceOutput> GetAllSourceOutputs() = 0;

    virtual AudioIOHandle OpenAudioPort(const AudioModuleInfo &audioPortInfo) = 0;

    virtual int32_t CloseAudioPort(AudioIOHandle ioHandle) = 0;

    virtual int32_t SetDeviceActive(AudioIOHandle ioHandle, InternalDeviceType deviceType,
                                    std::string name, bool active) = 0;

    virtual int32_t SetRingerMode(AudioRingerMode ringerMode) = 0;

    virtual AudioRingerMode GetRingerMode() const = 0;

    virtual int32_t SetAudioSessionCallback(AudioSessionCallback *callback) = 0;

    virtual int32_t SuspendAudioDevice(std::string &name, bool isSuspend) = 0;
};
} // namespace AudioStandard
} // namespace OHOS

#endif // ST_AUDIO_POLICY_INTERFACE_H
