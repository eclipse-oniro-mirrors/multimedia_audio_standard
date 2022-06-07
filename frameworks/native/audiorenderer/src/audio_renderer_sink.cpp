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

#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>

#include "audio_errors.h"
#include "audio_log.h"
#include "audio_renderer_sink.h"

using namespace std;

namespace OHOS {
namespace AudioStandard {
namespace {
const int32_t HALF_FACTOR = 2;
const int32_t MAX_AUDIO_ADAPTER_NUM = 5;
const float DEFAULT_VOLUME_LEVEL = 1.0f;
const uint32_t AUDIO_CHANNELCOUNT = 2;
const uint32_t AUDIO_SAMPLE_RATE_48K = 48000;
const uint32_t DEEP_BUFFER_RENDER_PERIOD_SIZE = 4096;
const uint32_t INT_32_MAX = 0x7fffffff;
const uint32_t PCM_8_BIT = 8;
const uint32_t PCM_16_BIT = 16;
const uint32_t PCM_24_BIT = 24;
const uint32_t PCM_32_BIT = 32;
const uint32_t INTERNAL_OUTPUT_STREAM_ID = 0;
}

#ifdef DUMPFILE
const char *g_audioOutTestFilePath = "/data/local/tmp/audioout_test.pcm";
#endif // DUMPFILE

AudioRendererSink::AudioRendererSink()
    : rendererInited_(false), started_(false), paused_(false), leftVolume_(DEFAULT_VOLUME_LEVEL),
      rightVolume_(DEFAULT_VOLUME_LEVEL), audioManager_(nullptr), audioAdapter_(nullptr), audioRender_(nullptr)
{
    attr_ = {};
#ifdef DUMPFILE
    pfd = nullptr;
#endif // DUMPFILE
}

AudioRendererSink::~AudioRendererSink()
{
    DeInit();
}

AudioRendererSink *AudioRendererSink::GetInstance()
{
    static AudioRendererSink audioRenderer_;

    return &audioRenderer_;
}

void AudioRendererSink::DeInit()
{
    started_ = false;
    rendererInited_ = false;
    if ((audioRender_ != nullptr) && (audioAdapter_ != nullptr)) {
        audioAdapter_->DestroyRender(audioAdapter_, audioRender_);
    }
    audioRender_ = nullptr;

    if ((audioManager_ != nullptr) && (audioAdapter_ != nullptr)) {
        if (routeHandle_ != -1) {
            audioAdapter_->ReleaseAudioRoute(audioAdapter_, routeHandle_);
        }
        audioManager_->UnloadAdapter(audioManager_, audioAdapter_);
    }
    audioAdapter_ = nullptr;
    audioManager_ = nullptr;
#ifdef DUMPFILE
    if (pfd) {
        fclose(pfd);
        pfd = nullptr;
    }
#endif // DUMPFILE
}

void InitAttrs(struct AudioSampleAttributes &attrs)
{
    /* Initialization of audio parameters for playback */
    attrs.channelCount = AUDIO_CHANNELCOUNT;
    attrs.sampleRate = AUDIO_SAMPLE_RATE_48K;
    attrs.interleaved = 0;
    attrs.streamId = INTERNAL_OUTPUT_STREAM_ID;
    attrs.type = AUDIO_IN_MEDIA;
    attrs.period = DEEP_BUFFER_RENDER_PERIOD_SIZE;
    attrs.isBigEndian = false;
    attrs.isSignedData = true;
    attrs.stopThreshold = INT_32_MAX;
    attrs.silenceThreshold = 0;
}

static int32_t SwitchAdapterRender(struct AudioAdapterDescriptor *descs, string adapterNameCase,
    enum AudioPortDirection portFlag, struct AudioPort &renderPort, int32_t size)
{
    if (descs == nullptr) {
        return ERROR;
    }

    for (int32_t index = 0; index < size; index++) {
        struct AudioAdapterDescriptor *desc = &descs[index];
        if (desc == nullptr) {
            continue;
        }
        if (!strcmp(desc->adapterName, adapterNameCase.c_str())) {
            for (uint32_t port = 0; port < desc->portNum; port++) {
                // Only find out the port of out in the sound card
                if (desc->ports[port].dir == portFlag) {
                    renderPort = desc->ports[port];
                    return index;
                }
            }
        }
    }
    AUDIO_ERR_LOG("SwitchAdapterRender Fail");

    return ERR_INVALID_INDEX;
}

int32_t AudioRendererSink::InitAudioManager()
{
    AUDIO_INFO_LOG("AudioRendererSink: Initialize audio proxy manager");

    audioManager_ = GetAudioManagerFuncs();
    if (audioManager_ == nullptr) {
        return ERR_INVALID_HANDLE;
    }

    return 0;
}

uint32_t PcmFormatToBits(enum AudioFormat format)
{
    switch (format) {
        case AUDIO_FORMAT_PCM_8_BIT:
            return PCM_8_BIT;
        case AUDIO_FORMAT_PCM_16_BIT:
            return PCM_16_BIT;
        case AUDIO_FORMAT_PCM_24_BIT:
            return PCM_24_BIT;
        case AUDIO_FORMAT_PCM_32_BIT:
            return PCM_32_BIT;
        default:
            return PCM_24_BIT;
    }
}

int32_t AudioRendererSink::CreateRender(struct AudioPort &renderPort)
{
    int32_t ret;
    struct AudioSampleAttributes param;
    InitAttrs(param);
    param.sampleRate = attr_.sampleRate;
    param.channelCount = attr_.channel;
    param.format = attr_.format;
    param.frameSize = PcmFormatToBits(param.format) * param.channelCount / PCM_8_BIT;
    param.startThreshold = DEEP_BUFFER_RENDER_PERIOD_SIZE / (param.frameSize);
    AUDIO_INFO_LOG("AudioRendererSink Create render format: %{public}d", param.format);
    struct AudioDeviceDescriptor deviceDesc;
    deviceDesc.portId = renderPort.portId;
    deviceDesc.pins = PIN_OUT_SPEAKER;
    deviceDesc.desc = nullptr;
    ret = audioAdapter_->CreateRender(audioAdapter_, &deviceDesc, &param, &audioRender_);
    if (ret != 0 || audioRender_ == nullptr) {
        AUDIO_ERR_LOG("AudioDeviceCreateRender failed");
        audioManager_->UnloadAdapter(audioManager_, audioAdapter_);
        return ERR_NOT_STARTED;
    }

    return 0;
}

int32_t AudioRendererSink::Init(AudioSinkAttr &attr)
{
    attr_ = attr;
    adapterNameCase_ = attr_.adapterName;  // Set sound card information
    openSpeaker_ = attr_.openMicSpeaker;
    enum AudioPortDirection port = PORT_OUT; // Set port information

    if (InitAudioManager() != 0) {
        AUDIO_ERR_LOG("Init audio manager Fail");
        return ERR_NOT_STARTED;
    }

    int32_t size = 0;
    int32_t ret;
    struct AudioAdapterDescriptor *descs = nullptr;
    ret = audioManager_->GetAllAdapters(audioManager_, &descs, &size);
    if (size > MAX_AUDIO_ADAPTER_NUM || size == 0 || descs == nullptr || ret != 0) {
        AUDIO_ERR_LOG("Get adapters Fail");
        return ERR_NOT_STARTED;
    }

    // Get qualified sound card and port
    int32_t index = SwitchAdapterRender(descs, adapterNameCase_, port, audioPort_, size);
    if (index < 0) {
        AUDIO_ERR_LOG("Switch Adapter Fail");
        return ERR_NOT_STARTED;
    }

    struct AudioAdapterDescriptor *desc = &descs[index];
    if (audioManager_->LoadAdapter(audioManager_, desc, &audioAdapter_) != 0) {
        AUDIO_ERR_LOG("Load Adapter Fail");
        return ERR_NOT_STARTED;
    }
    if (audioAdapter_ == nullptr) {
        AUDIO_ERR_LOG("Load audio device failed");
        return ERR_NOT_STARTED;
    }

    // Initialization port information, can fill through mode and other parameters
    ret = audioAdapter_->InitAllPorts(audioAdapter_);
    if (ret != 0) {
        AUDIO_ERR_LOG("InitAllPorts failed");
        return ERR_NOT_STARTED;
    }

    if (CreateRender(audioPort_) != 0) {
        AUDIO_ERR_LOG("Create render failed, Audio Port: %{public}d", audioPort_.portId);
        return ERR_NOT_STARTED;
    }
    if (openSpeaker_) {
        ret = OpenOutput(DEVICE_TYPE_SPEAKER);
        if (ret < 0) {
            AUDIO_ERR_LOG("AudioRendererSink: Update route FAILED: %{public}d", ret);
        }
    }
    rendererInited_ = true;

#ifdef DUMPFILE
    pfd = fopen(g_audioOutTestFilePath, "wb+");
    if (pfd == nullptr) {
        AUDIO_ERR_LOG("Error opening pcm test file!");
    }
#endif // DUMPFILE

    return SUCCESS;
}

int32_t AudioRendererSink::RenderFrame(char &data, uint64_t len, uint64_t &writeLen)
{
    int32_t ret;
    if (audioRender_ == nullptr) {
        AUDIO_ERR_LOG("Audio Render Handle is nullptr!");
        return ERR_INVALID_HANDLE;
    }

#ifdef DUMPFILE
    size_t writeResult = fwrite((void*)&data, 1, len, pfd);
    if (writeResult != len) {
        AUDIO_ERR_LOG("Failed to write the file.");
    }
#endif // DUMPFILE

    ret = audioRender_->RenderFrame(audioRender_, (void*)&data, len, &writeLen);
    if (ret != 0) {
        AUDIO_ERR_LOG("RenderFrame failed ret: %{public}x", ret);
        return ERR_WRITE_FAILED;
    }

    return SUCCESS;
}

int32_t AudioRendererSink::Start(void)
{
    int32_t ret;

    if (!started_) {
        ret = audioRender_->control.Start(reinterpret_cast<AudioHandle>(audioRender_));
        if (!ret) {
            started_ = true;
            return SUCCESS;
        } else {
            AUDIO_ERR_LOG("AudioRendererSink::Start failed!");
            return ERR_NOT_STARTED;
        }
    }

    return SUCCESS;
}

int32_t AudioRendererSink::SetVolume(float left, float right)
{
    int32_t ret;
    float volume;

    if (audioRender_ == nullptr) {
        AUDIO_ERR_LOG("AudioRendererSink::SetVolume failed audioRender_ null");
        return ERR_INVALID_HANDLE;
    }

    leftVolume_ = left;
    rightVolume_ = right;
    if ((leftVolume_ == 0) && (rightVolume_ != 0)) {
        volume = rightVolume_;
    } else if ((leftVolume_ != 0) && (rightVolume_ == 0)) {
        volume = leftVolume_;
    } else {
        volume = (leftVolume_ + rightVolume_) / HALF_FACTOR;
    }

    ret = audioRender_->volume.SetVolume(reinterpret_cast<AudioHandle>(audioRender_), volume);
    if (ret) {
        AUDIO_ERR_LOG("AudioRendererSink::Set volume failed!");
    }

    return ret;
}

int32_t AudioRendererSink::GetVolume(float &left, float &right)
{
    left = leftVolume_;
    right = rightVolume_;
    return SUCCESS;
}

int32_t AudioRendererSink::GetLatency(uint32_t *latency)
{
    if (audioRender_ == nullptr) {
        AUDIO_ERR_LOG("AudioRendererSink: GetLatency failed audio render null");
        return ERR_INVALID_HANDLE;
    }

    if (!latency) {
        AUDIO_ERR_LOG("AudioRendererSink: GetLatency failed latency null");
        return ERR_INVALID_PARAM;
    }

    uint32_t hdiLatency;
    if (audioRender_->GetLatency(audioRender_, &hdiLatency) == 0) {
        *latency = hdiLatency;
        return SUCCESS;
    } else {
        return ERR_OPERATION_FAILED;
    }
}

static AudioCategory GetAudioCategory(AudioScene audioScene)
{
    AudioCategory audioCategory;
    switch (audioScene) {
        case AUDIO_SCENE_DEFAULT:
            audioCategory = AUDIO_IN_MEDIA;
            break;
        case AUDIO_SCENE_RINGING:
            audioCategory = AUDIO_IN_RINGTONE;
            break;
        case AUDIO_SCENE_PHONE_CALL:
            audioCategory = AUDIO_IN_CALL;
            break;
        case AUDIO_SCENE_PHONE_CHAT:
            audioCategory = AUDIO_IN_COMMUNICATION;
            break;
        default:
            audioCategory = AUDIO_IN_MEDIA;
            break;
    }
    AUDIO_DEBUG_LOG("AudioRendererSink: Audio category returned is: %{public}d", audioCategory);

    return audioCategory;
}

static int32_t SetOutputPortPin(DeviceType outputDevice, AudioRouteNode &sink)
{
    int32_t ret = SUCCESS;

    switch (outputDevice) {
        case DEVICE_TYPE_SPEAKER:
            sink.ext.device.type = PIN_OUT_SPEAKER;
            sink.ext.device.desc = "pin_out_speaker";
            break;
        case DEVICE_TYPE_WIRED_HEADSET:
            sink.ext.device.type = PIN_OUT_HEADSET;
            sink.ext.device.desc = "pin_out_headset";
            break;
        case DEVICE_TYPE_USB_HEADSET:
            sink.ext.device.type = PIN_OUT_USB_EXT;
            sink.ext.device.desc = "pin_out_usb_ext";
            break;
        default:
            ret = ERR_NOT_SUPPORTED;
            break;
    }

    return ret;
}

int32_t AudioRendererSink::OpenOutput(DeviceType outputDevice)
{
    AudioRouteNode source = {};
    AudioRouteNode sink = {};

    int32_t ret = SetOutputPortPin(outputDevice, sink);
    if (ret != SUCCESS) {
        AUDIO_ERR_LOG("AudioRendererSink: OpenOutput FAILED: %{public}d", ret);
        return ret;
    }

    source.portId = 0;
    source.role = AUDIO_PORT_SOURCE_ROLE;
    source.type = AUDIO_PORT_MIX_TYPE;
    source.ext.mix.moduleId = 0;
    source.ext.mix.streamId = INTERNAL_OUTPUT_STREAM_ID;

    sink.portId = audioPort_.portId;
    sink.role = AUDIO_PORT_SINK_ROLE;
    sink.type = AUDIO_PORT_DEVICE_TYPE;
    sink.ext.device.moduleId = 0;

    AudioRoute route = {
        .sourcesNum = 1,
        .sources = &source,
        .sinksNum = 1,
        .sinks = &sink,
    };

    ret = audioAdapter_->UpdateAudioRoute(audioAdapter_, &route, &routeHandle_);
    AUDIO_DEBUG_LOG("AudioRendererSink: UpdateAudioRoute returns: %{public}d", ret);
    if (ret != 0) {
        AUDIO_ERR_LOG("AudioRendererSink: UpdateAudioRoute failed");
        return ERR_OPERATION_FAILED;
    }

    return SUCCESS;
}

int32_t AudioRendererSink::SetAudioScene(AudioScene audioScene)
{
    AUDIO_INFO_LOG("AudioRendererSink::SetAudioScene in");
    CHECK_AND_RETURN_RET_LOG(audioScene >= AUDIO_SCENE_DEFAULT && audioScene <= AUDIO_SCENE_PHONE_CHAT,
                             ERR_INVALID_PARAM, "invalid audioScene");
    if (audioRender_ == nullptr) {
        AUDIO_ERR_LOG("AudioRendererSink::SetAudioScene failed audio render handle is null!");
        return ERR_INVALID_HANDLE;
    }
    if (openSpeaker_) {
        int32_t ret = OpenOutput(DEVICE_TYPE_SPEAKER);
        if (ret < 0) {
            AUDIO_ERR_LOG("AudioRendererSink: Update route FAILED: %{public}d", ret);
        }
        struct AudioSceneDescriptor scene;
        scene.scene.id = GetAudioCategory(audioScene);
        scene.desc.pins = PIN_OUT_SPEAKER;
        if (audioRender_->scene.SelectScene == nullptr) {
            AUDIO_ERR_LOG("AudioRendererSink: Select scene nullptr");
            return ERR_OPERATION_FAILED;
        }

        AUDIO_INFO_LOG("AudioRendererSink::SelectScene start");
        ret = audioRender_->scene.SelectScene((AudioHandle)audioRender_, &scene);
        AUDIO_INFO_LOG("AudioRendererSink::SelectScene over");
        if (ret < 0) {
            AUDIO_ERR_LOG("AudioRendererSink: Select scene FAILED: %{public}d", ret);
            return ERR_OPERATION_FAILED;
        }
    }

    AUDIO_INFO_LOG("AudioRendererSink::Select audio scene SUCCESS: %{public}d", audioScene);
    return SUCCESS;
}

uint64_t AudioRendererSink::GetTransactionId()
{
    AUDIO_INFO_LOG("AudioRendererSink::GetTransactionId in");
    return reinterpret_cast<uint64_t>(audioRender_);
}

int32_t AudioRendererSink::Stop(void)
{
    int32_t ret;

    if (audioRender_ == nullptr) {
        AUDIO_ERR_LOG("AudioRendererSink::Stop failed audioRender_ null");
        return ERR_INVALID_HANDLE;
    }

    if (started_) {
        ret = audioRender_->control.Stop(reinterpret_cast<AudioHandle>(audioRender_));
        if (!ret) {
            started_ = false;
            return SUCCESS;
        } else {
            AUDIO_ERR_LOG("AudioRendererSink::Stop failed!");
            return ERR_OPERATION_FAILED;
        }
    }

    return SUCCESS;
}

int32_t AudioRendererSink::Pause(void)
{
    int32_t ret;

    if (audioRender_ == nullptr) {
        AUDIO_ERR_LOG("AudioRendererSink::Pause failed audioRender_ null");
        return ERR_INVALID_HANDLE;
    }

    if (!started_) {
        AUDIO_ERR_LOG("AudioRendererSink::Pause invalid state!");
        return ERR_OPERATION_FAILED;
    }

    if (!paused_) {
        ret = audioRender_->control.Pause(reinterpret_cast<AudioHandle>(audioRender_));
        if (!ret) {
            paused_ = true;
            return SUCCESS;
        } else {
            AUDIO_ERR_LOG("AudioRendererSink::Pause failed!");
            return ERR_OPERATION_FAILED;
        }
    }

    return SUCCESS;
}

int32_t AudioRendererSink::Resume(void)
{
    int32_t ret;

    if (audioRender_ == nullptr) {
        AUDIO_ERR_LOG("AudioRendererSink::Resume failed audioRender_ null");
        return ERR_INVALID_HANDLE;
    }

    if (!started_) {
        AUDIO_ERR_LOG("AudioRendererSink::Resume invalid state!");
        return ERR_OPERATION_FAILED;
    }

    if (paused_) {
        ret = audioRender_->control.Resume(reinterpret_cast<AudioHandle>(audioRender_));
        if (!ret) {
            paused_ = false;
            return SUCCESS;
        } else {
            AUDIO_ERR_LOG("AudioRendererSink::Resume failed!");
            return ERR_OPERATION_FAILED;
        }
    }

    return SUCCESS;
}

int32_t AudioRendererSink::Reset(void)
{
    int32_t ret;

    if (started_ && audioRender_ != nullptr) {
        ret = audioRender_->control.Flush(reinterpret_cast<AudioHandle>(audioRender_));
        if (!ret) {
            return SUCCESS;
        } else {
            AUDIO_ERR_LOG("AudioRendererSink::Reset failed!");
            return ERR_OPERATION_FAILED;
        }
    }

    return ERR_OPERATION_FAILED;
}

int32_t AudioRendererSink::Flush(void)
{
    int32_t ret;

    if (started_ && audioRender_ != nullptr) {
        ret = audioRender_->control.Flush(reinterpret_cast<AudioHandle>(audioRender_));
        if (!ret) {
            return SUCCESS;
        } else {
            AUDIO_ERR_LOG("AudioRendererSink::Flush failed!");
            return ERR_OPERATION_FAILED;
        }
    }

    return ERR_OPERATION_FAILED;
}
} // namespace AudioStandard
} // namespace OHOS

#ifdef __cplusplus
extern "C" {
#endif

using namespace OHOS::AudioStandard;

AudioRendererSink *g_audioRendrSinkInstance = AudioRendererSink::GetInstance();

int32_t AudioRendererSinkInit(AudioSinkAttr *attr)
{
    int32_t ret;
    if (g_audioRendrSinkInstance->rendererInited_)
        return SUCCESS;

    ret = g_audioRendrSinkInstance->Init(*attr);
    return ret;
}

void AudioRendererSinkDeInit()
{
    if (g_audioRendrSinkInstance->rendererInited_)
        g_audioRendrSinkInstance->DeInit();
}

int32_t AudioRendererSinkStop()
{
    int32_t ret;

    if (!g_audioRendrSinkInstance->rendererInited_)
        return SUCCESS;

    ret = g_audioRendrSinkInstance->Stop();
    return ret;
}

int32_t AudioRendererSinkStart()
{
    int32_t ret;

    if (!g_audioRendrSinkInstance->rendererInited_) {
        AUDIO_ERR_LOG("audioRenderer Not Inited! Init the renderer first\n");
        return ERR_NOT_STARTED;
    }

    ret = g_audioRendrSinkInstance->Start();
    return ret;
}

int32_t AudioRendererSinkPause()
{
    if (!g_audioRendrSinkInstance->rendererInited_) {
        AUDIO_ERR_LOG("Renderer pause failed");
        return ERR_NOT_STARTED;
    }

    return g_audioRendrSinkInstance->Pause();
}

int32_t AudioRendererSinkResume()
{
    if (!g_audioRendrSinkInstance->rendererInited_) {
        AUDIO_ERR_LOG("Renderer resume failed");
        return ERR_NOT_STARTED;
    }

    return g_audioRendrSinkInstance->Resume();
}

int32_t AudioRendererRenderFrame(char &data, uint64_t len, uint64_t &writeLen)
{
    int32_t ret;

    if (!g_audioRendrSinkInstance->rendererInited_) {
        AUDIO_ERR_LOG("audioRenderer Not Inited! Init the renderer first\n");
        return ERR_NOT_STARTED;
    }

    ret = g_audioRendrSinkInstance->RenderFrame(data, len, writeLen);
    return ret;
}

int32_t AudioRendererSinkSetVolume(float left, float right)
{
    int32_t ret;

    if (!g_audioRendrSinkInstance->rendererInited_) {
        AUDIO_ERR_LOG("audioRenderer Not Inited! Init the renderer first\n");
        return ERR_NOT_STARTED;
    }

    ret = g_audioRendrSinkInstance->SetVolume(left, right);
    return ret;
}

int32_t AudioRendererSinkGetLatency(uint32_t *latency)
{
    int32_t ret;

    if (!g_audioRendrSinkInstance->rendererInited_) {
        AUDIO_ERR_LOG("audioRenderer Not Inited! Init the renderer first\n");
        return ERR_NOT_STARTED;
    }

    if (!latency) {
        AUDIO_ERR_LOG("AudioRendererSinkGetLatency failed latency null");
        return ERR_INVALID_PARAM;
    }

    ret = g_audioRendrSinkInstance->GetLatency(latency);
    return ret;
}
#ifdef __cplusplus
}
#endif
