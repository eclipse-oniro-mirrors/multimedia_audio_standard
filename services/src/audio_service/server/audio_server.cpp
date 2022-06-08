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

#include <cinttypes>
#include <fstream>
#include <sstream>

#include "audio_capturer_source.h"
#include "audio_errors.h"
#include "audio_renderer_sink.h"
#include "iservice_registry.h"
#include "audio_log.h"
#include "system_ability_definition.h"

#include "audio_server.h"

#define PA
#ifdef PA
extern "C" {
    extern int ohos_pa_main(int argc, char *argv[]);
}
#endif

using namespace std;

namespace OHOS {
namespace AudioStandard {
std::map<std::string, std::string> AudioServer::audioParameters;
const string DEFAULT_COOKIE_PATH = "/data/data/.pulse_dir/state/cookie";

REGISTER_SYSTEM_ABILITY_BY_ID(AudioServer, AUDIO_DISTRIBUTED_SERVICE_ID, true)

#ifdef PA
constexpr int PA_ARG_COUNT = 1;

void *AudioServer::paDaemonThread(void *arg)
{
    /* Load the mandatory pulseaudio modules at start */
    char *argv[] = {
        (char*)"pulseaudio",
    };

    AUDIO_INFO_LOG("Calling ohos_pa_main\n");
    ohos_pa_main(PA_ARG_COUNT, argv);
    AUDIO_INFO_LOG("Exiting ohos_pa_main\n");
    exit(-1);
}
#endif

AudioServer::AudioServer(int32_t systemAbilityId, bool runOnCreate)
    : SystemAbility(systemAbilityId, runOnCreate)
{}

void AudioServer::OnDump()
{}

void AudioServer::OnStart()
{
    AUDIO_DEBUG_LOG("AudioService OnStart");
    bool res = Publish(this);
    if (res) {
        AUDIO_DEBUG_LOG("AudioService OnStart res=%{public}d", res);
    }

#ifdef PA
    int32_t ret = pthread_create(&m_paDaemonThread, nullptr, AudioServer::paDaemonThread, nullptr);
    if (ret != 0) {
        AUDIO_ERR_LOG("pthread_create failed %d", ret);
    }
    AUDIO_INFO_LOG("Created paDaemonThread\n");
#endif
}

void AudioServer::OnStop()
{
    AUDIO_DEBUG_LOG("AudioService OnStop");
}

void AudioServer::SetAudioParameter(const std::string &key, const std::string &value)
{
    AUDIO_DEBUG_LOG("server: set audio parameter");
    if (!VerifyClientPermission(MODIFY_AUDIO_SETTINGS_PERMISSION)) {
        AUDIO_ERR_LOG("SetAudioParameter: MODIFY_AUDIO_SETTINGS permission denied");
        return;
    }

    AudioServer::audioParameters[key] = value;
}

const std::string AudioServer::GetAudioParameter(const std::string &key)
{
    AUDIO_DEBUG_LOG("server: get audio parameter");

    if (AudioServer::audioParameters.count(key)) {
        return AudioServer::audioParameters[key];
    } else {
        return "";
    }
}

const char *AudioServer::RetrieveCookie(int32_t &size)
{
    char *cookieInfo = nullptr;
    size = 0;
    std::ifstream cookieFile(DEFAULT_COOKIE_PATH, std::ifstream::binary);
    if (!cookieFile) {
        return cookieInfo;
    }

    cookieFile.seekg (0, cookieFile.end);
    size = cookieFile.tellg();
    cookieFile.seekg (0, cookieFile.beg);

    if ((size > 0) && (size < PATH_MAX)) {
        cookieInfo = (char *)malloc(size * sizeof(char));
        if (cookieInfo == nullptr) {
            AUDIO_ERR_LOG("AudioServer::RetrieveCookie: No memory");
            cookieFile.close();
            return cookieInfo;
        }
        AUDIO_DEBUG_LOG("Reading: %{public}d characters...", size);
        cookieFile.read(cookieInfo, size);
    }
    cookieFile.close();
    return cookieInfo;
}

uint64_t AudioServer::GetTransactionId(DeviceType deviceType, DeviceRole deviceRole)
{
    uint64_t transactionId = 0;
    AUDIO_INFO_LOG("GetTransactionId in: device type: %{public}d, device role: %{public}d", deviceType, deviceRole);

    if (deviceRole == OUTPUT_DEVICE) {
        AudioRendererSink *audioRendererSinkInstance = AudioRendererSink::GetInstance();
        if (audioRendererSinkInstance) {
            transactionId = audioRendererSinkInstance->GetTransactionId();
        }
    } else if (deviceRole == INPUT_DEVICE) {
        AudioCapturerSource *audioCapturerSourceInstance = AudioCapturerSource::GetInstance();
        if (audioCapturerSourceInstance) {
            transactionId = audioCapturerSourceInstance->GetTransactionId();
        }
    }

    AUDIO_INFO_LOG("Transaction Id: %{public}" PRIu64, transactionId);
    return transactionId;
}

int32_t AudioServer::GetMaxVolume(AudioSystemManager::AudioVolumeType volumeType)
{
    AUDIO_DEBUG_LOG("GetMaxVolume server");
    return MAX_VOLUME;
}

int32_t AudioServer::GetMinVolume(AudioSystemManager::AudioVolumeType volumeType)
{
    AUDIO_DEBUG_LOG("GetMinVolume server");
    return MIN_VOLUME;
}

int32_t AudioServer::SetMicrophoneMute(bool isMute)
{
    if (!VerifyClientPermission(MICROPHONE_PERMISSION)) {
        AUDIO_ERR_LOG("SetMicrophoneMute: MICROPHONE permission denied");
        return ERR_PERMISSION_DENIED;
    }

    AudioCapturerSource *audioCapturerSourceInstance = AudioCapturerSource::GetInstance();

    if (!audioCapturerSourceInstance->capturerInited_) {
            AUDIO_INFO_LOG("Capturer is not initialized. Set the flag mute state flag");
            AudioCapturerSource::micMuteState_ = isMute;
            return 0;
    }

    return audioCapturerSourceInstance->SetMute(isMute);
}

bool AudioServer::IsMicrophoneMute()
{
    if (!VerifyClientPermission(MICROPHONE_PERMISSION)) {
        AUDIO_ERR_LOG("IsMicrophoneMute: MICROPHONE permission denied");
        return false;
    }

    AudioCapturerSource *audioCapturerSourceInstance = AudioCapturerSource::GetInstance();
    bool isMute = false;

    if (!audioCapturerSourceInstance->capturerInited_) {
        AUDIO_INFO_LOG("Capturer is not initialized. Get the mic mute state flag value!");
        return AudioCapturerSource::micMuteState_;
    }

    if (audioCapturerSourceInstance->GetMute(isMute)) {
        AUDIO_ERR_LOG("GetMute status in capturer returned Error !");
    }

    return isMute;
}

int32_t AudioServer::SetAudioScene(AudioScene audioScene)
{
    AudioCapturerSource *audioCapturerSourceInstance = AudioCapturerSource::GetInstance();
    AudioRendererSink *audioRendererSinkInstance = AudioRendererSink::GetInstance();

    if (!audioCapturerSourceInstance->capturerInited_) {
        AUDIO_WARNING_LOG("Capturer is not initialized.");
    } else {
        audioCapturerSourceInstance->SetAudioScene(audioScene);
    }

    if (!audioRendererSinkInstance->rendererInited_) {
        AUDIO_WARNING_LOG("Renderer is not initialized.");
    } else {
        audioRendererSinkInstance->SetAudioScene(audioScene);
    }

    return SUCCESS;
}

int32_t AudioServer::UpdateActiveDeviceRoute(DeviceType type, DeviceFlag flag)
{
    AUDIO_INFO_LOG("[%{public}s]", __func__);
    AudioCapturerSource *audioCapturerSourceInstance = AudioCapturerSource::GetInstance();
    AudioRendererSink *audioRendererSinkInstance = AudioRendererSink::GetInstance();

    switch (flag) {
        case DeviceFlag::INPUT_DEVICES_FLAG: {
            audioCapturerSourceInstance->OpenInput(type);
            break;
        }
        case DeviceFlag::OUTPUT_DEVICES_FLAG: {
            audioRendererSinkInstance->OpenOutput(type);
            break;
        }
        case DeviceFlag::ALL_DEVICES_FLAG: {
            audioCapturerSourceInstance->OpenInput(type);
            audioRendererSinkInstance->OpenOutput(type);
            break;
        }
        default:
            break;
    }

    return SUCCESS;
}

bool AudioServer::VerifyClientPermission(const std::string &permissionName)
{
    auto callerUid = IPCSkeleton::GetCallingUid();
    AUDIO_INFO_LOG("AudioServer: ==[%{public}s] [uid:%{public}d]==", permissionName.c_str(), callerUid);

    // Root users should be whitelisted
    if (callerUid == ROOT_UID) {
        AUDIO_INFO_LOG("Root user. Permission GRANTED!!!");
        return true;
    }

    Security::AccessToken::AccessTokenID clientTokenId = IPCSkeleton::GetCallingTokenID();
    int res = Security::AccessToken::AccessTokenKit::VerifyAccessToken(clientTokenId, permissionName);
    if (res != Security::AccessToken::PermissionState::PERMISSION_GRANTED) {
        AUDIO_ERR_LOG("Permission denied [tid:%{public}d]", clientTokenId);
        return false;
    }

    return true;
}

std::vector<sptr<AudioDeviceDescriptor>> AudioServer::GetDevices(DeviceFlag deviceFlag)
{
    std::vector<sptr<AudioDeviceDescriptor>> audioDeviceDescriptor = {};
    return audioDeviceDescriptor;
}
} // namespace AudioStandard
} // namespace OHOS
