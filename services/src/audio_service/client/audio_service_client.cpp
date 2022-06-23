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

#include "audio_service_client.h"

#include <fstream>

#include "iservice_registry.h"
#include "audio_log.h"
#include "hisysevent.h"
#include "securec.h"
#include "system_ability_definition.h"
#include "unistd.h"

using namespace std;

namespace OHOS {
namespace AudioStandard {
AudioRendererCallbacks::~AudioRendererCallbacks() = default;
AudioCapturerCallbacks::~AudioCapturerCallbacks() = default;
const uint32_t CHECK_UTIL_SUCCESS = 0;
const uint32_t READ_TIMEOUT_IN_SEC = 5;
const uint32_t DOUBLE_VALUE = 2;
const uint32_t MAX_LENGTH_FACTOR = 5;
const uint32_t T_LENGTH_FACTOR = 4;
const uint64_t MIN_BUF_DURATION_IN_USEC = 92880;

const string PATH_SEPARATOR = "/";
const string COOKIE_FILE_NAME = "cookie";

static int32_t CheckReturnIfinvalid(bool expr, const int32_t retVal)
{
    do {
        if (!(expr)) {
            return retVal;
        }
    } while (false);
    return CHECK_UTIL_SUCCESS;
}

static int32_t CheckPaStatusIfinvalid(pa_threaded_mainloop *mainLoop, pa_context *context,
    pa_stream *paStream, const int32_t retVal)
{
    return CheckReturnIfinvalid(mainLoop && context &&
        paStream && PA_CONTEXT_IS_GOOD(pa_context_get_state(context)) &&
        PA_STREAM_IS_GOOD(pa_stream_get_state(paStream)), retVal);
}

static int32_t CheckPaStatusIfinvalid(pa_threaded_mainloop *mainLoop, pa_context *context,
    pa_stream *paStream, const int32_t retVal, int32_t &pError)
{
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, -1) < 0) {
        pError = pa_context_errno(context);
        return -1;
    }
    return retVal;
}

AudioStreamParams AudioServiceClient::ConvertFromPAAudioParams(pa_sample_spec paSampleSpec)
{
    AudioStreamParams audioParams;

    audioParams.channels = paSampleSpec.channels;
    audioParams.samplingRate = paSampleSpec.rate;
    audioParams.encoding = ENCODING_PCM;

    switch (paSampleSpec.format) {
        case PA_SAMPLE_U8:
            audioParams.format = SAMPLE_U8;
            break;
        case PA_SAMPLE_S16LE:
            audioParams.format = SAMPLE_S16LE;
            break;
        case PA_SAMPLE_S24LE:
            audioParams.format = SAMPLE_S24LE;
            break;
        case PA_SAMPLE_S32LE:
            audioParams.format = SAMPLE_S32LE;
            break;
        default:
            audioParams.format = INVALID_WIDTH;
            break;
    }

    return audioParams;
}

pa_sample_spec AudioServiceClient::ConvertToPAAudioParams(AudioStreamParams audioParams)
{
    pa_sample_spec paSampleSpec;

    paSampleSpec.channels = audioParams.channels;
    paSampleSpec.rate     = audioParams.samplingRate;

    switch ((AudioSampleFormat)audioParams.format) {
        case SAMPLE_U8:
            paSampleSpec.format = (pa_sample_format_t)PA_SAMPLE_U8;
            break;
        case SAMPLE_S16LE:
            paSampleSpec.format = (pa_sample_format_t)PA_SAMPLE_S16LE;
            break;
        case SAMPLE_S24LE:
            paSampleSpec.format = (pa_sample_format_t)PA_SAMPLE_S24LE;
            break;
        case SAMPLE_S32LE:
            paSampleSpec.format = (pa_sample_format_t)PA_SAMPLE_S32LE;
            break;
        default:
            paSampleSpec.format = (pa_sample_format_t)PA_SAMPLE_INVALID;
            break;
    }

    return paSampleSpec;
}

static size_t AlignToAudioFrameSize(size_t l, const pa_sample_spec &ss)
{
    size_t fs = pa_frame_size(&ss);
    if (fs == 0) {
        AUDIO_ERR_LOG(" Error: pa_frame_size returned  0");
        return 0;
    }

    return (l / fs) * fs;
}

void AudioServiceClient::PAStreamStartSuccessCb(pa_stream *stream, int32_t success, void *userdata)
{
    if (!userdata) {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamStartSuccessCb: userdata is null");
        return;
    }

    AudioServiceClient *asClient = static_cast<AudioServiceClient *>(userdata);
    pa_threaded_mainloop *mainLoop = static_cast<pa_threaded_mainloop *>(asClient->mainLoop);

    asClient->state_ = RUNNING;
    asClient->WriteStateChangedSysEvents();
    std::shared_ptr<AudioStreamCallback> streamCb = asClient->streamCallback_.lock();
    if (streamCb != nullptr) {
        streamCb->OnStateChange(asClient->state_);
    }
    asClient->streamCmdStatus = success;
    pa_threaded_mainloop_signal(mainLoop, 0);
}

void AudioServiceClient::PAStreamStopSuccessCb(pa_stream *stream, int32_t success, void *userdata)
{
    if (!userdata) {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamStopSuccessCb: userdata is null");
        return;
    }

    AudioServiceClient *asClient = static_cast<AudioServiceClient *>(userdata);
    pa_threaded_mainloop *mainLoop = static_cast<pa_threaded_mainloop *>(asClient->mainLoop);

    asClient->state_ = STOPPED;
    asClient->WriteStateChangedSysEvents();
    std::shared_ptr<AudioStreamCallback> streamCb = asClient->streamCallback_.lock();
    if (streamCb != nullptr) {
        streamCb->OnStateChange(asClient->state_);
    }
    asClient->streamCmdStatus = success;
    pa_threaded_mainloop_signal(mainLoop, 0);
}

void AudioServiceClient::PAStreamPauseSuccessCb(pa_stream *stream, int32_t success, void *userdata)
{
    if (!userdata) {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamPauseSuccessCb: userdata is null");
        return;
    }

    AudioServiceClient *asClient = static_cast<AudioServiceClient *>(userdata);
    pa_threaded_mainloop *mainLoop = static_cast<pa_threaded_mainloop *>(asClient->mainLoop);

    asClient->state_ = PAUSED;
    asClient->WriteStateChangedSysEvents();
    std::shared_ptr<AudioStreamCallback> streamCb = asClient->streamCallback_.lock();
    if (streamCb != nullptr) {
        streamCb->OnStateChange(asClient->state_);
    }
    asClient->streamCmdStatus = success;
    pa_threaded_mainloop_signal(mainLoop, 0);
}

void AudioServiceClient::PAStreamDrainSuccessCb(pa_stream *stream, int32_t success, void *userdata)
{
    if (!userdata) {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamDrainSuccessCb: userdata is null");
        return;
    }

    AudioServiceClient *asClient = (AudioServiceClient *)userdata;
    pa_threaded_mainloop *mainLoop = (pa_threaded_mainloop *)asClient->mainLoop;

    asClient->streamDrainStatus = success;
    pa_threaded_mainloop_signal(mainLoop, 0);
}

void AudioServiceClient::PAStreamFlushSuccessCb(pa_stream *stream, int32_t success, void *userdata)
{
    if (!userdata) {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamFlushSuccessCb: userdata is null");
        return;
    }
    AudioServiceClient *asClient = (AudioServiceClient *)userdata;
    pa_threaded_mainloop *mainLoop = (pa_threaded_mainloop *)asClient->mainLoop;

    asClient->streamFlushStatus = success;
    pa_threaded_mainloop_signal(mainLoop, 0);
}

void AudioServiceClient::PAStreamSetBufAttrSuccessCb(pa_stream *stream, int32_t success, void *userdata)
{
    if (!userdata) {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamSetBufAttrSuccessCb: userdata is null");
        return;
    }
    AudioServiceClient *asClient = (AudioServiceClient *)userdata;
    pa_threaded_mainloop *mainLoop = (pa_threaded_mainloop *)asClient->mainLoop;

    AUDIO_DEBUG_LOG("AAudioServiceClient::PAStreamSetBufAttrSuccessCb is called");
    if (!success) {
        AUDIO_ERR_LOG("AAudioServiceClient::PAStreamSetBufAttrSuccessCb SetBufAttr failed");
    } else {
        AUDIO_ERR_LOG("AAudioServiceClient::PAStreamSetBufAttrSuccessCb SetBufAttr success");
    }
    pa_threaded_mainloop_signal(mainLoop, 0);
}

int32_t AudioServiceClient::SetAudioRenderMode(AudioRenderMode renderMode)
{
    AUDIO_DEBUG_LOG("AudioServiceClient::SetAudioRenderMode begin");
    renderMode_ = renderMode;

    if (renderMode_ != RENDER_MODE_CALLBACK) {
        return AUDIO_CLIENT_SUCCESS;
    }

    if (CheckReturnIfinvalid(mainLoop && context && paStream, AUDIO_CLIENT_ERR) < 0) {
        return AUDIO_CLIENT_ERR;
    }

    pa_threaded_mainloop_lock(mainLoop);
    pa_buffer_attr bufferAttr;
    bufferAttr.fragsize = static_cast<uint32_t>(-1);
    bufferAttr.prebuf = AlignToAudioFrameSize(pa_usec_to_bytes(MIN_BUF_DURATION_IN_USEC, &sampleSpec), sampleSpec);
    bufferAttr.maxlength = static_cast<uint32_t>(-1);
    bufferAttr.tlength = static_cast<uint32_t>(-1);
    bufferAttr.minreq = bufferAttr.prebuf;
    pa_operation *operation = pa_stream_set_buffer_attr(paStream, &bufferAttr,
        PAStreamSetBufAttrSuccessCb, (void *)this);
        while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainLoop);
    }
    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mainLoop);

    AUDIO_DEBUG_LOG("AudioServiceClient::SetAudioRenderMode end");

    return AUDIO_CLIENT_SUCCESS;
}

AudioRenderMode AudioServiceClient::GetAudioRenderMode()
{
    return renderMode_;
}

int32_t AudioServiceClient::SetAudioCaptureMode(AudioCaptureMode captureMode)
{
    AUDIO_DEBUG_LOG("AudioServiceClient::SetAudioCaptureMode.");
    captureMode_ = captureMode;

    return AUDIO_CLIENT_SUCCESS;
}

AudioCaptureMode AudioServiceClient::GetAudioCaptureMode()
{
    return captureMode_;
}

int32_t AudioServiceClient::SaveWriteCallback(const std::weak_ptr<AudioRendererWriteCallback> &callback)
{
    if (callback.lock() == nullptr) {
        AUDIO_ERR_LOG("AudioServiceClient::SaveWriteCallback callback == nullptr");
        return AUDIO_CLIENT_INIT_ERR;
    }
    writeCallback_ = callback;

    return AUDIO_CLIENT_SUCCESS;
}

int32_t AudioServiceClient::SaveReadCallback(const std::weak_ptr<AudioCapturerReadCallback> &callback)
{
    if (callback.lock() == nullptr) {
        AUDIO_ERR_LOG("AudioServiceClient::SaveReadCallback callback == nullptr");
        return AUDIO_CLIENT_INIT_ERR;
    }
    readCallback_ = callback;

    return AUDIO_CLIENT_SUCCESS;
}

void AudioServiceClient::PAStreamWriteCb(pa_stream *stream, size_t length, void *userdata)
{
    AUDIO_DEBUG_LOG("AudioServiceClient::Inside PA write callback");
    if (!userdata) {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamWriteCb: userdata is null");
        return;
    }

    auto asClient = static_cast<AudioServiceClient *>(userdata);
    auto mainLoop = static_cast<pa_threaded_mainloop *>(asClient->mainLoop);
    pa_threaded_mainloop_signal(mainLoop, 0);

    if (asClient->renderMode_ != RENDER_MODE_CALLBACK) {
        return;
    }

    std::shared_ptr<AudioRendererWriteCallback> cb = asClient->writeCallback_.lock();
    if (cb != nullptr) {
        size_t requestSize;
        asClient->GetMinimumBufferSize(requestSize);
        AUDIO_INFO_LOG("AudioServiceClient::PAStreamWriteCb: cb != nullptr firing OnWriteData");
        AUDIO_INFO_LOG("AudioServiceClient::OnWriteData requestSize : %{public}zu", requestSize);
        cb->OnWriteData(requestSize);
    } else {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamWriteCb: cb == nullptr not firing OnWriteData");
    }
}

void AudioServiceClient::PAStreamReadCb(pa_stream *stream, size_t length, void *userdata)
{
    AUDIO_DEBUG_LOG("AudioServiceClient::PAStreamReadCb Inside PA read callback");
    if (!userdata) {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamReadCb: userdata is null");
        return;
    }
    auto asClient = static_cast<AudioServiceClient *>(userdata);
    auto mainLoop = static_cast<pa_threaded_mainloop *>(asClient->mainLoop);
    pa_threaded_mainloop_signal(mainLoop, 0);

    if (asClient->captureMode_ != CAPTURE_MODE_CALLBACK) {
        return;
    }

    std::shared_ptr<AudioCapturerReadCallback> cb = asClient->readCallback_.lock();
    if (cb != nullptr) {
        size_t requestSize;
        asClient->GetMinimumBufferSize(requestSize);
        AUDIO_INFO_LOG("AudioServiceClient::PAStreamReadCb: cb != nullptr firing OnReadData");
        AUDIO_INFO_LOG("AudioServiceClient::OnReadData requestSize : %{public}zu", requestSize);
        cb->OnReadData(requestSize);
    } else {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamReadCb: cb == nullptr not firing OnReadData");
    }
}

void AudioServiceClient::PAStreamUnderFlowCb(pa_stream *stream, void *userdata)
{
    if (!userdata) {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamUnderFlowCb: userdata is null");
        return;
    }

    AudioServiceClient *asClient = (AudioServiceClient *)userdata;
    asClient->underFlowCount++;
}

void AudioServiceClient::PAStreamLatencyUpdateCb(pa_stream *stream, void *userdata)
{
    pa_threaded_mainloop *mainLoop = (pa_threaded_mainloop *)userdata;
    pa_threaded_mainloop_signal(mainLoop, 0);
}

void AudioServiceClient::PAStreamStateCb(pa_stream *stream, void *userdata)
{
    if (!userdata) {
        AUDIO_ERR_LOG("AudioServiceClient::PAStreamStateCb: userdata is null");
        return;
    }

    AudioServiceClient *asClient = (AudioServiceClient *)userdata;
    pa_threaded_mainloop *mainLoop = (pa_threaded_mainloop *)asClient->mainLoop;

    if (asClient->mAudioRendererCallbacks)
        asClient->mAudioRendererCallbacks->OnStreamStateChangeCb();

    AUDIO_INFO_LOG("Current Stream State: %{private}d", pa_stream_get_state(stream));

    switch (pa_stream_get_state(stream)) {
        case PA_STREAM_READY:
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            pa_threaded_mainloop_signal(mainLoop, 0);
            break;

        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_CREATING:
        default:
            break;
    }
}

void AudioServiceClient::PAContextStateCb(pa_context *context, void *userdata)
{
    pa_threaded_mainloop *mainLoop = (pa_threaded_mainloop *)userdata;
    AUDIO_INFO_LOG("Current Context State: %{private}d", pa_context_get_state(context));

    switch (pa_context_get_state(context)) {
        case PA_CONTEXT_READY:
        case PA_CONTEXT_TERMINATED:
        case PA_CONTEXT_FAILED:
            pa_threaded_mainloop_signal(mainLoop, 0);
            break;

        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
        default:
            break;
    }
}

AudioServiceClient::AudioServiceClient()
{
    isMainLoopStarted = false;
    isContextConnected = false;
    isStreamConnected = false;

    sinkDevices.clear();
    sourceDevices.clear();
    sinkInputs.clear();
    sourceOutputs.clear();
    clientInfo.clear();

    mVolumeFactor = 1.0f;
    mUnMute_ = false;
    mStreamType = STREAM_MUSIC;
    mAudioSystemMgr = nullptr;

    streamIndex = 0;
    sessionID = 0;
    volumeChannels = STEREO;
    streamInfoUpdated = false;

    renderRate = RENDER_RATE_NORMAL;
    renderMode_ = RENDER_MODE_NORMAL;

    captureMode_ = CAPTURE_MODE_NORMAL;

    eAudioClientType = AUDIO_SERVICE_CLIENT_PLAYBACK;

    mFrameSize = 0;
    mFrameMarkPosition = 0;
    mMarkReached = false;
    mFramePeriodNumber = 0;

    mTotalBytesWritten = 0;
    mFramePeriodWritten = 0;
    mTotalBytesRead = 0;
    mFramePeriodRead = 0;
    mRenderPositionCb = nullptr;
    mRenderPeriodPositionCb = nullptr;

    mAudioRendererCallbacks = nullptr;
    mAudioCapturerCallbacks = nullptr;
    internalReadBuffer = nullptr;
    mainLoop = nullptr;
    paStream = nullptr;
    context  = nullptr;
    api = nullptr;

    internalRdBufIndex = 0;
    internalRdBufLen = 0;
    streamCmdStatus = 0;
    streamDrainStatus = 0;
    streamFlushStatus = 0;
    underFlowCount = 0;

    acache.readIndex = 0;
    acache.writeIndex = 0;
    acache.isFull = false;
    acache.totalCacheSize = 0;
    acache.buffer = nullptr;

    setBufferSize = 0;
    PAStreamCorkSuccessCb = PAStreamStopSuccessCb;
}

void AudioServiceClient::ResetPAAudioClient()
{
    lock_guard<mutex> lock(ctrlMutex);
    if (mainLoop && (isMainLoopStarted == true))
        pa_threaded_mainloop_stop(mainLoop);

    if (paStream) {
        pa_stream_set_state_callback(paStream, nullptr, nullptr);
        pa_stream_set_write_callback(paStream, nullptr, nullptr);
        pa_stream_set_read_callback(paStream, nullptr, nullptr);
        pa_stream_set_latency_update_callback(paStream, nullptr, nullptr);
        pa_stream_set_underflow_callback(paStream, nullptr, nullptr);

        if (isStreamConnected == true)
            pa_stream_disconnect(paStream);
        pa_stream_unref(paStream);
    }

    if (context) {
        pa_context_set_state_callback(context, nullptr, nullptr);
        if (isContextConnected == true)
            pa_context_disconnect(context);
        pa_context_unref(context);
    }

    if (mainLoop)
        pa_threaded_mainloop_free(mainLoop);

    for (auto &thread : mPositionCBThreads) {
        if (thread && thread->joinable()) {
            thread->join();
        }
    }

    for (auto &thread : mPeriodPositionCBThreads) {
        if (thread && thread->joinable()) {
            thread->join();
        }
    }

    if (appCookiePath.compare("")) {
        remove(appCookiePath.c_str());
        appCookiePath = "";
    }

    isMainLoopStarted  = false;
    isContextConnected = false;
    isStreamConnected  = false;

    sinkDevices.clear();
    sourceDevices.clear();
    sinkInputs.clear();
    sourceOutputs.clear();
    clientInfo.clear();

    mAudioRendererCallbacks = nullptr;
    mAudioCapturerCallbacks = nullptr;
    internalReadBuffer      = nullptr;

    mainLoop = nullptr;
    paStream = nullptr;
    context  = nullptr;
    api      = nullptr;

    internalRdBufIndex = 0;
    internalRdBufLen   = 0;
    underFlowCount     = 0;

    acache.buffer = nullptr;
    acache.readIndex = 0;
    acache.writeIndex = 0;
    acache.isFull = false;
    acache.totalCacheSize = 0;

    setBufferSize = 0;
    PAStreamCorkSuccessCb = nullptr;
}

AudioServiceClient::~AudioServiceClient()
{
    ResetPAAudioClient();
}

void AudioServiceClient::SetEnv()
{
    AUDIO_INFO_LOG("SetEnv called");
    int ret = 0;
    const char *env_home_pa = getenv("HOME");
    if (!env_home_pa) {
        ret = setenv("HOME", PA_HOME_DIR, 1);
        AUDIO_INFO_LOG("set env HOME: %{public}d", ret);
    }

    const char *env_runtime_pa = getenv("PULSE_RUNTIME_PATH");
    if (!env_runtime_pa) {
        ret = setenv("PULSE_RUNTIME_PATH", PA_RUNTIME_DIR, 1);
        AUDIO_INFO_LOG("set env PULSE_RUNTIME_DIR: %{public}d", ret);
    }

    const char *env_state_pa = getenv("PULSE_STATE_PATH");
    if (!env_state_pa) {
        ret = setenv("PULSE_STATE_PATH", PA_STATE_DIR, 1);
        AUDIO_INFO_LOG("set env PULSE_STATE_PATH: %{public}d", ret);
    }
}

void AudioServiceClient::SetApplicationCachePath(const std::string cachePath)
{
    AUDIO_DEBUG_LOG("SetApplicationCachePath in");

    char realPath[PATH_MAX] = {0x00};

    if ((strlen(cachePath.c_str()) >= PATH_MAX) || (realpath(cachePath.c_str(), realPath) == nullptr)) {
        AUDIO_ERR_LOG("Invalid cache path. err = %{public}d", errno);
        return;
    }

    cachePath_ = realPath;
}

bool AudioServiceClient::VerifyClientPermission(const std::string &permissionName, uint32_t appTokenId)
{   // for capturer check for MICROPHONE PERMISSION
    if (!AudioPolicyManager::GetInstance().VerifyClientPermission(permissionName, appTokenId)) {
        AUDIO_DEBUG_LOG("Client doesn't have MICROPHONE permission");
        return false;
    }

    return true;
}

int32_t AudioServiceClient::Initialize(ASClientType eClientType)
{
    int error = PA_ERR_INTERNAL;
    eAudioClientType = eClientType;

    mMarkReached = false;
    mTotalBytesWritten = 0;
    mFramePeriodWritten = 0;
    mTotalBytesRead = 0;
    mFramePeriodRead = 0;

    SetEnv();

    mAudioSystemMgr = AudioSystemManager::GetInstance();

    mainLoop = pa_threaded_mainloop_new();
    if (mainLoop == nullptr)
        return AUDIO_CLIENT_INIT_ERR;

    api = pa_threaded_mainloop_get_api(mainLoop);
    if (api == nullptr) {
        ResetPAAudioClient();
        return AUDIO_CLIENT_INIT_ERR;
    }

    context = pa_context_new(api, "AudioServiceClient");
    if (context == nullptr) {
        ResetPAAudioClient();
        return AUDIO_CLIENT_INIT_ERR;
    }

    pa_context_set_state_callback(context, PAContextStateCb, mainLoop);

    if (!cachePath_.empty()) {
        AUDIO_DEBUG_LOG("abilityContext not null");
        int32_t size = 0;

        const char *cookieData = mAudioSystemMgr->RetrieveCookie(size);
        if (size <= 0) {
            AUDIO_ERR_LOG("Error retrieving cookie");
            return AUDIO_CLIENT_INIT_ERR;
        }

        appCookiePath = cachePath_ + PATH_SEPARATOR + COOKIE_FILE_NAME;

        ofstream cookieCache(appCookiePath.c_str(), std::ofstream::binary);
        cookieCache.write(cookieData, size);
        cookieCache.flush();
        cookieCache.close();

        pa_context_load_cookie_from_file(context, appCookiePath.c_str());
    }

    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFAIL, nullptr) < 0) {
        error = pa_context_errno(context);
        AUDIO_ERR_LOG("context connect error: %{public}s", pa_strerror(error));
        ResetPAAudioClient();
        return AUDIO_CLIENT_INIT_ERR;
    }

    isContextConnected = true;
    pa_threaded_mainloop_lock(mainLoop);

    if (pa_threaded_mainloop_start(mainLoop) < 0) {
        pa_threaded_mainloop_unlock(mainLoop);
        ResetPAAudioClient();
        return AUDIO_CLIENT_INIT_ERR;
    }

    isMainLoopStarted = true;
    while (true) {
        pa_context_state_t state = pa_context_get_state(context);
        if (state == PA_CONTEXT_READY)
            break;

        if (!PA_CONTEXT_IS_GOOD(state)) {
            error = pa_context_errno(context);
            AUDIO_ERR_LOG("context bad state error: %{public}s", pa_strerror(error));
            pa_threaded_mainloop_unlock(mainLoop);
            ResetPAAudioClient();
            return AUDIO_CLIENT_INIT_ERR;
        }

        pa_threaded_mainloop_wait(mainLoop);
    }

    if (appCookiePath.compare("")) {
        remove(appCookiePath.c_str());
        appCookiePath = "";
    }

    pa_threaded_mainloop_unlock(mainLoop);
    return AUDIO_CLIENT_SUCCESS;
}

const std::string AudioServiceClient::GetStreamName(AudioStreamType audioType)
{
    std::string name;
    switch (audioType) {
        case STREAM_VOICE_ASSISTANT:
            name = "voice_assistant";
            break;
        case STREAM_VOICE_CALL:
            name = "voice_call";
            break;
        case STREAM_SYSTEM:
            name = "system";
            break;
        case STREAM_RING:
            name = "ring";
            break;
        case STREAM_MUSIC:
            name = "music";
            break;
        case STREAM_ALARM:
            name = "alarm";
            break;
        case STREAM_NOTIFICATION:
            name = "notification";
            break;
        case STREAM_BLUETOOTH_SCO:
            name = "bluetooth_sco";
            break;
        case STREAM_DTMF:
            name = "dtmf";
            break;
        case STREAM_TTS:
            name = "tts";
            break;
        case STREAM_ACCESSIBILITY:
            name = "accessibility";
            break;
        default:
            name = "unknown";
    }

    const std::string streamName = name;
    return streamName;
}

int32_t AudioServiceClient::ConnectStreamToPA()
{
    int error, result;

    if (CheckReturnIfinvalid(mainLoop && context && paStream, AUDIO_CLIENT_ERR) < 0) {
        return AUDIO_CLIENT_ERR;
    }
    uint64_t latency_in_msec = AudioSystemManager::GetInstance()->GetAudioLatencyFromXml();
    pa_threaded_mainloop_lock(mainLoop);

    pa_buffer_attr bufferAttr;
    bufferAttr.fragsize = static_cast<uint32_t>(-1);

    bufferAttr.prebuf = pa_usec_to_bytes(latency_in_msec * PA_USEC_PER_MSEC, &sampleSpec);
    bufferAttr.maxlength = pa_usec_to_bytes(latency_in_msec * PA_USEC_PER_MSEC * MAX_LENGTH_FACTOR, &sampleSpec);
    bufferAttr.tlength = pa_usec_to_bytes(latency_in_msec * PA_USEC_PER_MSEC * T_LENGTH_FACTOR, &sampleSpec);
    bufferAttr.minreq = pa_usec_to_bytes(latency_in_msec * PA_USEC_PER_MSEC, &sampleSpec);

    if (eAudioClientType == AUDIO_SERVICE_CLIENT_PLAYBACK)
        result = pa_stream_connect_playback(paStream, nullptr, &bufferAttr,
                                            (pa_stream_flags_t)(PA_STREAM_ADJUST_LATENCY
                                            | PA_STREAM_INTERPOLATE_TIMING
                                            | PA_STREAM_START_CORKED
                                            | PA_STREAM_VARIABLE_RATE), nullptr, nullptr);
    else
        result = pa_stream_connect_record(paStream, nullptr, nullptr,
                                          (pa_stream_flags_t)(PA_STREAM_INTERPOLATE_TIMING
                                          | PA_STREAM_ADJUST_LATENCY
                                          | PA_STREAM_START_CORKED
                                          | PA_STREAM_AUTO_TIMING_UPDATE));

    if (result < 0) {
        error = pa_context_errno(context);
        AUDIO_ERR_LOG("connection to stream error: %{public}d", error);
        pa_threaded_mainloop_unlock(mainLoop);
        ResetPAAudioClient();
        return AUDIO_CLIENT_CREATE_STREAM_ERR;
    }

    while (true) {
        pa_stream_state_t state = pa_stream_get_state(paStream);
        if (state == PA_STREAM_READY)
            break;

        if (!PA_STREAM_IS_GOOD(state)) {
            error = pa_context_errno(context);
            pa_threaded_mainloop_unlock(mainLoop);
            AUDIO_ERR_LOG("connection to stream error: %{public}d", error);
            ResetPAAudioClient();
            return AUDIO_CLIENT_CREATE_STREAM_ERR;
        }

        pa_threaded_mainloop_wait(mainLoop);
    }

    isStreamConnected = true;
    pa_threaded_mainloop_unlock(mainLoop);
    return AUDIO_CLIENT_SUCCESS;
}

int32_t AudioServiceClient::InitializeAudioCache()
{
    AUDIO_INFO_LOG("Initializing internal audio cache");

    if (CheckReturnIfinvalid(mainLoop && context && paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    const pa_buffer_attr *bufferAttr = pa_stream_get_buffer_attr(paStream);
    if (bufferAttr == nullptr) {
        AUDIO_ERR_LOG("pa stream get buffer attribute returned null");
        return AUDIO_CLIENT_INIT_ERR;
    }

    acache.buffer = make_unique<uint8_t[]>(bufferAttr->minreq);
    if (acache.buffer == nullptr) {
        AUDIO_ERR_LOG("Allocate memory for buffer failed");
        return AUDIO_CLIENT_INIT_ERR;
    }

    acache.readIndex = 0;
    acache.writeIndex = 0;
    acache.totalCacheSize = bufferAttr->minreq;
    acache.isFull = false;
    return AUDIO_CLIENT_SUCCESS;
}

int32_t AudioServiceClient::CreateStream(AudioStreamParams audioParams, AudioStreamType audioType)
{
    int error;
    if (CheckReturnIfinvalid(mainLoop && context, AUDIO_CLIENT_ERR) < 0) {
        return AUDIO_CLIENT_ERR;
    }

    if (eAudioClientType == AUDIO_SERVICE_CLIENT_CONTROLLER) {
        return AUDIO_CLIENT_INVALID_PARAMS_ERR;
    }

    pa_threaded_mainloop_lock(mainLoop);
    mStreamType = audioType;
    const std::string streamName = GetStreamName(audioType);

    auto timenow = chrono::system_clock::to_time_t(chrono::system_clock::now());
    const std::string streamStartTime = ctime(&timenow);

    sampleSpec = ConvertToPAAudioParams(audioParams);
    mFrameSize = pa_frame_size(&sampleSpec);

    pa_proplist *propList = pa_proplist_new();
    if (propList == nullptr) {
        AUDIO_ERR_LOG("pa_proplist_new failed");
        ResetPAAudioClient();
        return AUDIO_CLIENT_CREATE_STREAM_ERR;
    }

    pa_proplist_sets(propList, "stream.type", streamName.c_str());
    pa_proplist_sets(propList, "stream.volumeFactor", std::to_string(mVolumeFactor).c_str());
    pa_proplist_sets(propList, "stream.sessionID", std::to_string(pa_context_get_index(context)).c_str());
    pa_proplist_sets(propList, "stream.startTime", streamStartTime.c_str());

    AUDIO_ERR_LOG("Creating stream fo channels %{public}d", audioParams.channels);
    pa_channel_map map;
    if (audioParams.channels > CHANNEL_6) {
        pa_channel_map_init(&map);
        map.channels = audioParams.channels;
        switch (audioParams.channels) {
            case CHANNEL_8:
                map.map[CHANNEL8_IDX] = PA_CHANNEL_POSITION_AUX1;
            case CHANNEL_7:
                map.map[CHANNEL1_IDX] = PA_CHANNEL_POSITION_FRONT_LEFT;
                map.map[CHANNEL2_IDX] = PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
                map.map[CHANNEL3_IDX] = PA_CHANNEL_POSITION_FRONT_CENTER;
                map.map[CHANNEL4_IDX] = PA_CHANNEL_POSITION_FRONT_RIGHT;
                map.map[CHANNEL5_IDX] = PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
                map.map[CHANNEL6_IDX] = PA_CHANNEL_POSITION_REAR_CENTER;
                map.map[CHANNEL7_IDX] = PA_CHANNEL_POSITION_AUX0;
                break;
            default:
                AUDIO_ERR_LOG("Invalid channel count");
                return AUDIO_CLIENT_CREATE_STREAM_ERR;
        }

        paStream = pa_stream_new_with_proplist(context, streamName.c_str(), &sampleSpec, &map, propList);
    } else {
        paStream = pa_stream_new_with_proplist(context, streamName.c_str(), &sampleSpec, nullptr, propList);
    }

    if (!paStream) {
        error = pa_context_errno(context);
        AUDIO_ERR_LOG("create stream Failed, error: %{public}d", error);
        pa_proplist_free(propList);
        pa_threaded_mainloop_unlock(mainLoop);
        ResetPAAudioClient();
        AUDIO_ERR_LOG("pa_stream_new_with_proplist failed, error: %{public}d", error);
        return AUDIO_CLIENT_CREATE_STREAM_ERR;
    }

    pa_proplist_free(propList);
    pa_stream_set_state_callback(paStream, PAStreamStateCb, (void *)this);
    pa_stream_set_write_callback(paStream, PAStreamWriteCb, (void *)this);
    pa_stream_set_read_callback(paStream, PAStreamReadCb, (void *)this);
    pa_stream_set_latency_update_callback(paStream, PAStreamLatencyUpdateCb, mainLoop);
    pa_stream_set_underflow_callback(paStream, PAStreamUnderFlowCb, (void *)this);

    pa_threaded_mainloop_unlock(mainLoop);

    error = ConnectStreamToPA();
    if (error < 0) {
        AUDIO_ERR_LOG("Create Stream Failed");
        ResetPAAudioClient();
        return AUDIO_CLIENT_CREATE_STREAM_ERR;
    }

    if (eAudioClientType == AUDIO_SERVICE_CLIENT_PLAYBACK) {
        error = InitializeAudioCache();
        if (error < 0) {
            AUDIO_ERR_LOG("Initialize audio cache failed");
            ResetPAAudioClient();
            return AUDIO_CLIENT_CREATE_STREAM_ERR;
        }

        if (SetStreamRenderRate(renderRate) != AUDIO_CLIENT_SUCCESS) {
            AUDIO_ERR_LOG("Set render rate failed");
        }
    }

    state_ = PREPARED;
    WriteStateChangedSysEvents();
    std::shared_ptr<AudioStreamCallback> streamCb = streamCallback_.lock();
    if (streamCb != nullptr) {
        streamCb->OnStateChange(state_);
    }

    AUDIO_INFO_LOG("Created Stream");
    return AUDIO_CLIENT_SUCCESS;
}

int32_t AudioServiceClient::GetSessionID(uint32_t &sessionID) const
{
    AUDIO_DEBUG_LOG("AudioServiceClient: GetSessionID");
    uint32_t client_index = pa_context_get_index(context);
    if (client_index == PA_INVALID_INDEX) {
        return AUDIO_CLIENT_ERR;
    }

    sessionID = client_index;

    return AUDIO_CLIENT_SUCCESS;
}

int32_t AudioServiceClient::StartStream()
{
    int error;

    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    pa_operation *operation = nullptr;

    pa_threaded_mainloop_lock(mainLoop);

    pa_stream_state_t state = pa_stream_get_state(paStream);
    if (state != PA_STREAM_READY) {
        error = pa_context_errno(context);
        pa_threaded_mainloop_unlock(mainLoop);
        AUDIO_ERR_LOG("Stream Start Failed, error: %{public}d", error);
        ResetPAAudioClient();
        return AUDIO_CLIENT_START_STREAM_ERR;
    }

    streamCmdStatus = 0;
    operation = pa_stream_cork(paStream, 0, PAStreamStartSuccessCb, (void *)this);

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainLoop);
    }
    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mainLoop);

    if (!streamCmdStatus) {
        AUDIO_ERR_LOG("Stream Start Failed");
        ResetPAAudioClient();
        return AUDIO_CLIENT_START_STREAM_ERR;
    } else {
        AUDIO_INFO_LOG("Stream Started Successfully");
        return AUDIO_CLIENT_SUCCESS;
    }
}

int32_t AudioServiceClient::PauseStream()
{
    lock_guard<mutex> lock(ctrlMutex);
    PAStreamCorkSuccessCb = PAStreamPauseSuccessCb;
    int32_t ret = CorkStream();
    if (ret) {
        return ret;
    }

    if (!streamCmdStatus) {
        AUDIO_ERR_LOG("Stream Pause Failed");
        return AUDIO_CLIENT_ERR;
    } else {
        AUDIO_INFO_LOG("Stream Paused Successfully");
        return AUDIO_CLIENT_SUCCESS;
    }
}

int32_t AudioServiceClient::StopStream()
{
    lock_guard<mutex> lock(ctrlMutex);
    PAStreamCorkSuccessCb = PAStreamStopSuccessCb;
    int32_t ret = CorkStream();
    if (ret) {
        return ret;
    }

    if (!streamCmdStatus) {
        AUDIO_ERR_LOG("Stream Stop Failed");
        return AUDIO_CLIENT_ERR;
    } else {
        AUDIO_INFO_LOG("Stream Stopped Successfully");
        if (internalRdBufLen) {
            (void)pa_stream_drop(paStream);
            internalReadBuffer = nullptr;
            internalRdBufLen = 0;
            internalRdBufIndex = 0;
        }
        return AUDIO_CLIENT_SUCCESS;
    }
}

int32_t AudioServiceClient::CorkStream()
{
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    pa_operation *operation = nullptr;

    pa_threaded_mainloop_lock(mainLoop);
    pa_stream_state_t state = pa_stream_get_state(paStream);
    if (state != PA_STREAM_READY) {
        int32_t error = pa_context_errno(context);
        pa_threaded_mainloop_unlock(mainLoop);
        AUDIO_ERR_LOG("Stream Stop Failed : %{public}d", error);
        return AUDIO_CLIENT_ERR;
    }

    streamCmdStatus = 0;
    operation = pa_stream_cork(paStream, 1, PAStreamCorkSuccessCb, (void *)this);

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainLoop);
    }
    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mainLoop);

    return AUDIO_CLIENT_SUCCESS;
}

int32_t AudioServiceClient::FlushStream()
{
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    pa_operation *operation = nullptr;

    lock_guard<mutex> lock(dataMutex);
    pa_threaded_mainloop_lock(mainLoop);

    pa_stream_state_t state = pa_stream_get_state(paStream);
    if (state != PA_STREAM_READY) {
        int error = pa_context_errno(context);
        pa_threaded_mainloop_unlock(mainLoop);
        AUDIO_ERR_LOG("Stream Flush Failed, error: %{public}d", error);
        return AUDIO_CLIENT_ERR;
    }

    streamFlushStatus = 0;
    operation = pa_stream_flush(paStream, PAStreamFlushSuccessCb, (void *)this);
    if (operation == nullptr) {
        AUDIO_ERR_LOG("Stream Flush Operation Failed");
        pa_threaded_mainloop_unlock(mainLoop);
        return AUDIO_CLIENT_ERR;
    }

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainLoop);
    }
    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mainLoop);

    if (!streamFlushStatus) {
        AUDIO_ERR_LOG("Stream Flush Failed");
        return AUDIO_CLIENT_ERR;
    } else {
        AUDIO_INFO_LOG("Stream Flushed Successfully");
        acache.readIndex = 0;
        acache.writeIndex = 0;
        acache.isFull = false;
        return AUDIO_CLIENT_SUCCESS;
    }
}

int32_t AudioServiceClient::DrainStream()
{
    int32_t error;

    if (eAudioClientType != AUDIO_SERVICE_CLIENT_PLAYBACK) {
        AUDIO_ERR_LOG("Drain is not supported");
        return AUDIO_CLIENT_ERR;
    }

    lock_guard<mutex> lock(dataMutex);

    error = DrainAudioCache();
    if (error != AUDIO_CLIENT_SUCCESS) {
        AUDIO_ERR_LOG("Audio cache drain failed");
        return AUDIO_CLIENT_ERR;
    }
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }
    pa_operation *operation = nullptr;

    pa_threaded_mainloop_lock(mainLoop);

    pa_stream_state_t state = pa_stream_get_state(paStream);
    if (state != PA_STREAM_READY) {
        error = pa_context_errno(context);
        pa_threaded_mainloop_unlock(mainLoop);
        AUDIO_ERR_LOG("Stream Drain Failed");
        return AUDIO_CLIENT_ERR;
    }

    streamDrainStatus = 0;
    operation = pa_stream_drain(paStream, PAStreamDrainSuccessCb, (void *)this);

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainLoop);
    }
    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mainLoop);

    if (!streamDrainStatus) {
        AUDIO_ERR_LOG("Stream Drain Failed");
        return AUDIO_CLIENT_ERR;
    } else {
        AUDIO_INFO_LOG("Stream Drained Successfully");
        return AUDIO_CLIENT_SUCCESS;
    }
}

int32_t AudioServiceClient::SetStreamVolume(uint32_t sessionID, uint32_t volume)
{
    return AUDIO_CLIENT_SUCCESS;
}

int32_t AudioServiceClient::PaWriteStream(const uint8_t *buffer, size_t &length)
{
    int error = 0;

    while (length > 0) {
        size_t writableSize;

        while (!(writableSize = pa_stream_writable_size(paStream))) {
            pa_threaded_mainloop_wait(mainLoop);
        }

        AUDIO_DEBUG_LOG("Write stream: writable size = %{public}zu, length = %{public}zu", writableSize, length);
        if (writableSize > length) {
            writableSize = length;
        }

        writableSize = AlignToAudioFrameSize(writableSize, sampleSpec);
        if (writableSize == 0) {
            AUDIO_ERR_LOG("Align to frame size failed");
            error = AUDIO_CLIENT_WRITE_STREAM_ERR;
            break;
        }

        error = pa_stream_write(paStream, (void *)buffer, writableSize, nullptr, 0LL,
                                PA_SEEK_RELATIVE);
        if (error < 0) {
            AUDIO_ERR_LOG("Write stream failed");
            error = AUDIO_CLIENT_WRITE_STREAM_ERR;
            break;
        }

        AUDIO_DEBUG_LOG("Writable size: %{public}zu, bytes to write: %{public}zu, return val: %{public}d",
                        writableSize, length, error);
        buffer = buffer + writableSize;
        length -= writableSize;

        HandleRenderPositionCallbacks(writableSize);
    }

    return error;
}

void AudioServiceClient::HandleRenderPositionCallbacks(size_t bytesWritten)
{
    mTotalBytesWritten += bytesWritten;
    if (mFrameSize == 0) {
        AUDIO_ERR_LOG("HandleRenderPositionCallbacks: capturePeriodPositionCb not set");
        return;
    }

    uint64_t writtenFrameNumber = mTotalBytesWritten / mFrameSize;
    AUDIO_DEBUG_LOG("frame size: %{public}d", mFrameSize);

    {
        std::lock_guard<std::mutex> lock(rendererMarkReachedMutex_);
        if (!mMarkReached && mRenderPositionCb) {
            AUDIO_DEBUG_LOG("frame mark position: %{public}" PRIu64 ", Total frames written: %{public}" PRIu64,
                static_cast<uint64_t>(mFrameMarkPosition), static_cast<uint64_t>(writtenFrameNumber));
            if (writtenFrameNumber >= mFrameMarkPosition) {
                AUDIO_DEBUG_LOG("audio service client OnMarkReached");
                mPositionCBThreads.emplace_back(std::make_unique<std::thread>(&RendererPositionCallback::OnMarkReached,
                                                mRenderPositionCb, mFrameMarkPosition));
                mMarkReached = true;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(rendererPeriodReachedMutex_);
        if (mRenderPeriodPositionCb) {
            mFramePeriodWritten += (bytesWritten / mFrameSize);
            AUDIO_DEBUG_LOG("frame period number: %{public}" PRIu64 ", Total frames written: %{public}" PRIu64,
                static_cast<uint64_t>(mFramePeriodNumber), static_cast<uint64_t>(writtenFrameNumber));
            if (mFramePeriodWritten >= mFramePeriodNumber) {
                mFramePeriodWritten %= mFramePeriodNumber;
                AUDIO_DEBUG_LOG("OnPeriodReached, remaining frames: %{public}" PRIu64,
                    static_cast<uint64_t>(mFramePeriodWritten));
                mPeriodPositionCBThreads.emplace_back(std::make_unique<std::thread>(
                    &RendererPeriodPositionCallback::OnPeriodReached, mRenderPeriodPositionCb, mFramePeriodNumber));
            }
        }
    }
}

int32_t AudioServiceClient::DrainAudioCache()
{
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    pa_threaded_mainloop_lock(mainLoop);

    int32_t error = 0;
    if (acache.buffer == nullptr) {
        AUDIO_ERR_LOG("Drain cache failed");
        return AUDIO_CLIENT_ERR;
    }

    size_t length = acache.writeIndex - acache.readIndex;
    const uint8_t *buffer = acache.buffer.get();

    error = PaWriteStream(buffer, length);

    acache.readIndex = 0;
    acache.writeIndex = 0;

    pa_threaded_mainloop_unlock(mainLoop);
    return error;
}

size_t AudioServiceClient::WriteToAudioCache(const StreamBuffer &stream)
{
    if (stream.buffer == nullptr) {
        return 0;
    }

    const uint8_t *inputBuffer = stream.buffer;
    uint8_t *cacheBuffer = acache.buffer.get() + acache.writeIndex;

    size_t inputLen = stream.bufferLen;

    while (inputLen > 0) {
        size_t writableSize = acache.totalCacheSize - acache.writeIndex;

        if (writableSize > inputLen) {
            writableSize = inputLen;
        }

        if (writableSize == 0) {
            break;
        }

        if (memcpy_s(cacheBuffer, acache.totalCacheSize, inputBuffer, writableSize)) {
            break;
        }

        inputBuffer = inputBuffer + writableSize;
        cacheBuffer = cacheBuffer + writableSize;
        inputLen -= writableSize;
        acache.writeIndex += writableSize;
    }

    if ((acache.writeIndex - acache.readIndex) == acache.totalCacheSize) {
        acache.isFull = true;
    }

    return (stream.bufferLen - inputLen);
}

size_t AudioServiceClient::WriteStreamInCb(const StreamBuffer &stream, int32_t &pError)
{
    lock_guard<mutex> lock(dataMutex);
    int error = 0;
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, 0, pError) < 0) {
        return 0;
    }

    pa_threaded_mainloop_lock(mainLoop);

    const uint8_t *buffer = stream.buffer;
    size_t length = stream.bufferLen;
    error = PaWriteStream(buffer, length);
    pa_threaded_mainloop_unlock(mainLoop);
    pError = error;
    return (stream.bufferLen - length);
}

size_t AudioServiceClient::WriteStream(const StreamBuffer &stream, int32_t &pError)
{
    lock_guard<mutex> lock(dataMutex);
    int error = 0;
    size_t cachedLen = WriteToAudioCache(stream);

    if (!acache.isFull) {
        pError = error;
        return cachedLen;
    }
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, 0, pError) < 0) {
        return 0;
    }
    pa_threaded_mainloop_lock(mainLoop);

    if (acache.buffer == nullptr) {
        AUDIO_ERR_LOG("Buffer is null");
        pError = AUDIO_CLIENT_WRITE_STREAM_ERR;
        return cachedLen;
    }

    const uint8_t *buffer = acache.buffer.get();
    size_t length = acache.totalCacheSize;

    error = PaWriteStream(buffer, length);
    acache.readIndex += acache.totalCacheSize;
    acache.isFull = false;

    if (!error && (length >= 0) && !acache.isFull) {
        uint8_t *cacheBuffer = acache.buffer.get();
        uint32_t offset = acache.readIndex;
        uint32_t size = (acache.writeIndex - acache.readIndex);
        if (size > 0) {
            if (memcpy_s(cacheBuffer, acache.totalCacheSize, cacheBuffer + offset, size)) {
                AUDIO_ERR_LOG("Update cache failed");
                pError = AUDIO_CLIENT_WRITE_STREAM_ERR;
                return cachedLen;
            }
            AUDIO_INFO_LOG("rearranging the audio cache");
        }
        acache.readIndex = 0;
        acache.writeIndex = 0;

        if (cachedLen < stream.bufferLen) {
            StreamBuffer str;
            str.buffer = stream.buffer + cachedLen;
            str.bufferLen = stream.bufferLen - cachedLen;
            AUDIO_DEBUG_LOG("writing pending data to audio cache: %{public}d", str.bufferLen);
            cachedLen += WriteToAudioCache(str);
        }
    }

    pa_threaded_mainloop_unlock(mainLoop);
    pError = error;
    return cachedLen;
}

int32_t AudioServiceClient::UpdateReadBuffer(uint8_t *buffer, size_t &length, size_t &readSize)
{
    size_t l = (internalRdBufLen < length) ? internalRdBufLen : length;
    if (memcpy_s(buffer, length, (const uint8_t*)internalReadBuffer + internalRdBufIndex, l)) {
        AUDIO_ERR_LOG("Update read buffer failed");
        return AUDIO_CLIENT_READ_STREAM_ERR;
    }

    length -= l;
    internalRdBufIndex += l;
    internalRdBufLen -= l;
    readSize += l;

    if (!internalRdBufLen) {
        int retVal = pa_stream_drop(paStream);
        internalReadBuffer = nullptr;
        internalRdBufLen = 0;
        internalRdBufIndex = 0;
        if (retVal < 0) {
            AUDIO_ERR_LOG("pa_stream_drop failed, retVal: %{public}d", retVal);
            return AUDIO_CLIENT_READ_STREAM_ERR;
        }
    }

    return 0;
}

void AudioServiceClient::OnTimeOut()
{
    AUDIO_ERR_LOG("Inside read timeout callback");
    pa_threaded_mainloop_lock(mainLoop);
    pa_threaded_mainloop_signal(mainLoop, 0);
    pa_threaded_mainloop_unlock(mainLoop);
}

void AudioServiceClient::HandleCapturePositionCallbacks(size_t bytesRead)
{
    mTotalBytesRead += bytesRead;
    if (mFrameSize == 0) {
        AUDIO_ERR_LOG("HandleCapturePositionCallbacks: capturePeriodPositionCb not set");
        return;
    }

    uint64_t readFrameNumber = mTotalBytesRead / mFrameSize;
    AUDIO_DEBUG_LOG("frame size: %{public}d", mFrameSize);
    {
        std::lock_guard<std::mutex> lock(capturerMarkReachedMutex_);
        if (!mMarkReached && mCapturePositionCb) {
            AUDIO_DEBUG_LOG("frame mark position: %{public}" PRIu64 ", Total frames read: %{public}" PRIu64,
                static_cast<uint64_t>(mFrameMarkPosition), static_cast<uint64_t>(readFrameNumber));
            if (readFrameNumber >= mFrameMarkPosition) {
                AUDIO_DEBUG_LOG("audio service client capturer OnMarkReached");
                mPositionCBThreads.emplace_back(std::make_unique<std::thread>(&CapturerPositionCallback::OnMarkReached,
                                                mCapturePositionCb, mFrameMarkPosition));
                mMarkReached = true;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(capturerPeriodReachedMutex_);
        if (mCapturePeriodPositionCb) {
            mFramePeriodRead += (bytesRead / mFrameSize);
            AUDIO_DEBUG_LOG("frame period number: %{public}" PRIu64 ", Total frames read: %{public}" PRIu64,
                static_cast<uint64_t>(mFramePeriodNumber), static_cast<uint64_t>(readFrameNumber));
            if (mFramePeriodRead >= mFramePeriodNumber) {
                mFramePeriodRead %= mFramePeriodNumber;
                AUDIO_DEBUG_LOG("audio service client OnPeriodReached, remaining frames: %{public}" PRIu64,
                    static_cast<uint64_t>(mFramePeriodRead));
                mPeriodPositionCBThreads.emplace_back(std::make_unique<std::thread>(
                    &CapturerPeriodPositionCallback::OnPeriodReached, mCapturePeriodPositionCb, mFramePeriodNumber));
            }
        }
    }
}

int32_t AudioServiceClient::ReadStream(StreamBuffer &stream, bool isBlocking)
{
    uint8_t *buffer = stream.buffer;
    size_t length = stream.bufferLen;
    size_t readSize = 0;
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    lock_guard<mutex> lock(dataMutex);
    pa_threaded_mainloop_lock(mainLoop);
    while (length > 0) {
        while (!internalReadBuffer) {
            int retVal = pa_stream_peek(paStream, &internalReadBuffer, &internalRdBufLen);
            if (retVal < 0) {
                AUDIO_ERR_LOG("pa_stream_peek failed, retVal: %{public}d", retVal);
                pa_threaded_mainloop_unlock(mainLoop);
                return AUDIO_CLIENT_READ_STREAM_ERR;
            }

            if (internalRdBufLen <= 0) {
                if (isBlocking) {
                    StartTimer(READ_TIMEOUT_IN_SEC);
                    pa_threaded_mainloop_wait(mainLoop);
                    StopTimer();
                    if (IsTimeOut()) {
                        AUDIO_ERR_LOG("Read timeout");
                        pa_threaded_mainloop_unlock(mainLoop);
                        return AUDIO_CLIENT_READ_STREAM_ERR;
                    }
                } else {
                    pa_threaded_mainloop_unlock(mainLoop);
                    HandleCapturePositionCallbacks(readSize);
                    return readSize;
                }
            } else if (!internalReadBuffer) {
                retVal = pa_stream_drop(paStream);
                if (retVal < 0) {
                    AUDIO_ERR_LOG("pa_stream_drop failed, retVal: %{public}d", retVal);
                    pa_threaded_mainloop_unlock(mainLoop);
                    return AUDIO_CLIENT_READ_STREAM_ERR;
                }
            } else {
                internalRdBufIndex = 0;
                AUDIO_DEBUG_LOG("buffer size from PA: %{public}zu", internalRdBufLen);
            }
        }

        if (UpdateReadBuffer(buffer, length, readSize) != 0) {
            pa_threaded_mainloop_unlock(mainLoop);
            return AUDIO_CLIENT_READ_STREAM_ERR;
        }
        buffer = stream.buffer + readSize;
    }
    pa_threaded_mainloop_unlock(mainLoop);
    HandleCapturePositionCallbacks(readSize);

    return readSize;
}

int32_t AudioServiceClient::ReleaseStream()
{
    state_ = RELEASED;
    WriteStateChangedSysEvents();
    ResetPAAudioClient();

    std::shared_ptr<AudioStreamCallback> streamCb = streamCallback_.lock();
    if (streamCb != nullptr) {
        streamCb->OnStateChange(state_);
    }

    return AUDIO_CLIENT_SUCCESS;
}

int32_t AudioServiceClient::SetBufferSizeInMsec(int32_t bufferSizeInMsec)
{
    size_t bufferSize =  pa_usec_to_bytes(bufferSizeInMsec * PA_USEC_PER_MSEC, &sampleSpec);
    setBufferSize = bufferSize;
    return AUDIO_CLIENT_SUCCESS;
}

int32_t AudioServiceClient::GetMinimumBufferSize(size_t &minBufferSize) const
{
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    const pa_buffer_attr *bufferAttr = pa_stream_get_buffer_attr(paStream);

    if (bufferAttr == nullptr) {
        AUDIO_ERR_LOG("pa_stream_get_buffer_attr returned nullptr");
        return AUDIO_CLIENT_ERR;
    }

    if (eAudioClientType == AUDIO_SERVICE_CLIENT_PLAYBACK) {
        if (renderMode_ == RENDER_MODE_CALLBACK) {
            minBufferSize = (size_t)bufferAttr->minreq;
        } else {
            if (setBufferSize) {
                minBufferSize = setBufferSize;
            } else {
                minBufferSize = (size_t)bufferAttr->minreq;
            }
        }
    }

    if (eAudioClientType == AUDIO_SERVICE_CLIENT_RECORD) {
        minBufferSize = (size_t)bufferAttr->fragsize;
    }

    return AUDIO_CLIENT_SUCCESS;
}

int32_t AudioServiceClient::GetMinimumFrameCount(uint32_t &frameCount) const
{
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    size_t minBufferSize = 0;

    const pa_buffer_attr *bufferAttr = pa_stream_get_buffer_attr(paStream);

    if (bufferAttr == nullptr) {
        AUDIO_ERR_LOG("pa_stream_get_buffer_attr returned nullptr");
        return AUDIO_CLIENT_ERR;
    }

    if (eAudioClientType == AUDIO_SERVICE_CLIENT_PLAYBACK) {
        if (renderMode_ == RENDER_MODE_CALLBACK) {
            minBufferSize = (size_t)bufferAttr->minreq;
        } else {
            if (setBufferSize) {
                minBufferSize = setBufferSize;
            } else {
                minBufferSize = (size_t)bufferAttr->minreq;
            }
        }
    }

    if (eAudioClientType == AUDIO_SERVICE_CLIENT_RECORD) {
        minBufferSize = (size_t)bufferAttr->fragsize;
    }

    uint32_t bytesPerSample = pa_frame_size(&sampleSpec);
    if (bytesPerSample == 0) {
        AUDIO_ERR_LOG("GetMinimumFrameCount Failed");
        return AUDIO_CLIENT_ERR;
    }

    frameCount = minBufferSize / bytesPerSample;
    AUDIO_INFO_LOG("frame count: %{private}d", frameCount);
    return AUDIO_CLIENT_SUCCESS;
}

uint32_t AudioServiceClient::GetSamplingRate() const
{
    return DEFAULT_SAMPLING_RATE;
}

uint8_t AudioServiceClient::GetChannelCount() const
{
    return DEFAULT_CHANNEL_COUNT;
}

uint8_t AudioServiceClient::GetSampleSize() const
{
    return DEFAULT_SAMPLE_SIZE;
}

int32_t AudioServiceClient::GetAudioStreamParams(AudioStreamParams& audioParams) const
{
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    const pa_sample_spec *paSampleSpec = pa_stream_get_sample_spec(paStream);

    if (!paSampleSpec) {
        AUDIO_ERR_LOG("GetAudioStreamParams Failed");
        return AUDIO_CLIENT_ERR;
    }

    audioParams = ConvertFromPAAudioParams(*paSampleSpec);
    return AUDIO_CLIENT_SUCCESS;
}

uint32_t AudioServiceClient::GetStreamVolume(uint32_t sessionID)
{
    return DEFAULT_STREAM_VOLUME;
}

int32_t AudioServiceClient::GetCurrentTimeStamp(uint64_t &timeStamp) const
{
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    int32_t retVal = AUDIO_CLIENT_SUCCESS;

    pa_threaded_mainloop_lock(mainLoop);
    const pa_timing_info *info = pa_stream_get_timing_info(paStream);
    if (info == nullptr) {
        retVal = AUDIO_CLIENT_ERR;
    } else {
        if (eAudioClientType == AUDIO_SERVICE_CLIENT_PLAYBACK) {
            timeStamp = pa_bytes_to_usec(info->write_index, &sampleSpec);
        } else if (eAudioClientType == AUDIO_SERVICE_CLIENT_RECORD) {
            if (pa_stream_get_time(paStream, &timeStamp)) {
                AUDIO_ERR_LOG("AudioServiceClient::GetCurrentTimeStamp failed for AUDIO_SERVICE_CLIENT_RECORD");
            }
        }
    }
    pa_threaded_mainloop_unlock(mainLoop);

    return retVal;
}

int32_t AudioServiceClient::GetAudioLatency(uint64_t &latency) const
{
    if (CheckPaStatusIfinvalid(mainLoop, context, paStream, AUDIO_CLIENT_PA_ERR) < 0) {
        return AUDIO_CLIENT_PA_ERR;
    }

    pa_usec_t paLatency;
    pa_usec_t cacheLatency;
    int32_t retVal = AUDIO_CLIENT_SUCCESS;
    int negative = 0;

    // Get PA latency
    pa_threaded_mainloop_lock(mainLoop);

    pa_operation *operation = pa_stream_update_timing_info(paStream, NULL, NULL);
    if (operation != nullptr) {
        pa_operation_unref(operation);
    } else {
        AUDIO_ERR_LOG("pa_stream_update_timing_info failed");
    }

    while (true) {
        if (pa_stream_get_latency(paStream, &paLatency, &negative) >= 0) {
            if (negative) {
                latency = 0;
                retVal = AUDIO_CLIENT_ERR;
                return retVal;
            }
            break;
        }
        AUDIO_INFO_LOG("waiting for audio latency information");
        pa_threaded_mainloop_wait(mainLoop);
    }
    pa_threaded_mainloop_unlock(mainLoop);

    if (eAudioClientType == AUDIO_SERVICE_CLIENT_PLAYBACK) {
        // Get audio write cache latency
        cacheLatency = pa_bytes_to_usec((acache.totalCacheSize - acache.writeIndex), &sampleSpec);

        // Total latency will be sum of audio write cache latency + PA latency
        latency = paLatency + cacheLatency;
        AUDIO_INFO_LOG("total latency: %{public}" PRIu64 ", pa latency: %{public}"
            PRIu64 ", cache latency: %{public}" PRIu64, latency, paLatency, cacheLatency);
    } else if (eAudioClientType == AUDIO_SERVICE_CLIENT_RECORD) {
        // Get audio read cache latency
        cacheLatency = pa_bytes_to_usec(internalRdBufLen, &sampleSpec);

        // Total latency will be sum of audio read cache latency + PA latency
        latency = paLatency + cacheLatency;
        AUDIO_INFO_LOG("total latency: %{public}" PRIu64 ", pa latency: %{public}" PRIu64, latency, paLatency);
    }

    return retVal;
}

void AudioServiceClient::RegisterAudioRendererCallbacks(const AudioRendererCallbacks &cb)
{
    AUDIO_INFO_LOG("Registering audio render callbacks");
    mAudioRendererCallbacks = (AudioRendererCallbacks *)&cb;
}

void AudioServiceClient::RegisterAudioCapturerCallbacks(const AudioCapturerCallbacks &cb)
{
    AUDIO_INFO_LOG("Registering audio record callbacks");
    mAudioCapturerCallbacks = (AudioCapturerCallbacks *)&cb;
}

void AudioServiceClient::SetRendererPositionCallback(int64_t markPosition,
    const std::shared_ptr<RendererPositionCallback> &callback)
{
    AUDIO_INFO_LOG("Registering render frame position callback");
    AUDIO_INFO_LOG("mark position: %{public}" PRIu64, markPosition);
    mFrameMarkPosition = markPosition;
    mRenderPositionCb = callback;
}

void AudioServiceClient::UnsetRendererPositionCallback()
{
    AUDIO_INFO_LOG("Unregistering render frame position callback");
    std::lock_guard<std::mutex> lock(rendererMarkReachedMutex_);
    mRenderPositionCb = nullptr;
    mMarkReached = false;
    mFrameMarkPosition = 0;
}

void AudioServiceClient::SetRendererPeriodPositionCallback(int64_t periodPosition,
    const std::shared_ptr<RendererPeriodPositionCallback> &callback)
{
    AUDIO_INFO_LOG("Registering render period position callback");
    mFramePeriodNumber = periodPosition;
    if ((mFrameSize != 0) && (mFramePeriodNumber != 0)) {
        mFramePeriodWritten = (mTotalBytesWritten / mFrameSize) % mFramePeriodNumber;
    } else {
        AUDIO_ERR_LOG("AudioServiceClient::SetRendererPeriodPositionCallback failed");
        return;
    }
    mRenderPeriodPositionCb = callback;
}

void AudioServiceClient::UnsetRendererPeriodPositionCallback()
{
    AUDIO_INFO_LOG("Unregistering render period position callback");
    std::lock_guard<std::mutex> lock(rendererPeriodReachedMutex_);
    mRenderPeriodPositionCb = nullptr;
    mFramePeriodWritten = 0;
    mFramePeriodNumber = 0;
}

void AudioServiceClient::SetCapturerPositionCallback(int64_t markPosition,
    const std::shared_ptr<CapturerPositionCallback> &callback)
{
    AUDIO_INFO_LOG("Registering capture frame position callback");
    AUDIO_INFO_LOG("mark position: %{public}" PRIu64, markPosition);
    mFrameMarkPosition = markPosition;
    mCapturePositionCb = callback;
}

void AudioServiceClient::UnsetCapturerPositionCallback()
{
    AUDIO_INFO_LOG("Unregistering capture frame position callback");
    std::lock_guard<std::mutex> lock(capturerMarkReachedMutex_);
    mCapturePositionCb = nullptr;
    mMarkReached = false;
    mFrameMarkPosition = 0;
}

void AudioServiceClient::SetCapturerPeriodPositionCallback(int64_t periodPosition,
    const std::shared_ptr<CapturerPeriodPositionCallback> &callback)
{
    AUDIO_INFO_LOG("Registering period position callback");
    mFramePeriodNumber = periodPosition;
    if ((mFrameSize != 0) && (mFramePeriodNumber) != 0) {
        mFramePeriodRead = (mTotalBytesRead / mFrameSize) % mFramePeriodNumber;
    } else {
        AUDIO_INFO_LOG("AudioServiceClient::SetCapturerPeriodPositionCallback failed");
        return;
    }
    mCapturePeriodPositionCb = callback;
}

void AudioServiceClient::UnsetCapturerPeriodPositionCallback()
{
    AUDIO_INFO_LOG("Unregistering period position callback");
    std::lock_guard<std::mutex> lock(capturerPeriodReachedMutex_);
    mCapturePeriodPositionCb = nullptr;
    mFramePeriodRead = 0;
    mFramePeriodNumber = 0;
}

int32_t AudioServiceClient::SetStreamType(AudioStreamType audioStreamType)
{
    AUDIO_INFO_LOG("SetStreamType: %{public}d", audioStreamType);

    if (context == nullptr) {
        AUDIO_ERR_LOG("context is null");
        return AUDIO_CLIENT_ERR;
    }

    pa_threaded_mainloop_lock(mainLoop);

    mStreamType = audioStreamType;
    const std::string streamName = GetStreamName(audioStreamType);

    pa_proplist *propList = pa_proplist_new();
    if (propList == nullptr) {
        AUDIO_ERR_LOG("pa_proplist_new failed");
        pa_threaded_mainloop_unlock(mainLoop);
        return AUDIO_CLIENT_ERR;
    }

    pa_proplist_sets(propList, "stream.type", streamName.c_str());
    pa_proplist_sets(propList, "media.name", streamName.c_str());
    pa_operation *updatePropOperation = pa_stream_proplist_update(paStream, PA_UPDATE_REPLACE, propList,
        nullptr, nullptr);
    pa_proplist_free(propList);
    pa_operation_unref(updatePropOperation);

    pa_threaded_mainloop_unlock(mainLoop);

    return AUDIO_CLIENT_SUCCESS;
}

int32_t AudioServiceClient::SetStreamVolume(float volume)
{
    lock_guard<mutex> lock(ctrlMutex);
    AUDIO_INFO_LOG("SetVolume volume: %{public}f", volume);

    if (context == nullptr) {
        AUDIO_ERR_LOG("context is null");
        return AUDIO_CLIENT_ERR;
    }

    /* Validate and return INVALID_PARAMS error */
    if ((volume < MIN_STREAM_VOLUME_LEVEL) || (volume > MAX_STREAM_VOLUME_LEVEL)) {
        AUDIO_ERR_LOG("Invalid Volume Input!");
        return AUDIO_CLIENT_INVALID_PARAMS_ERR;
    }

    int32_t volumeFactor = AudioSystemManager::MapVolumeFromHDI(mVolumeFactor);
    int32_t newVolumeFactor = AudioSystemManager::MapVolumeFromHDI(volume);
    if (newVolumeFactor > volumeFactor) {
        mUnMute_ = true;
    }
    AUDIO_INFO_LOG("mUnMute_ %{public}d", mUnMute_);
    pa_threaded_mainloop_lock(mainLoop);

    mVolumeFactor = volume;
    pa_proplist *propList = pa_proplist_new();
    if (propList == nullptr) {
        AUDIO_ERR_LOG("pa_proplist_new failed");
        pa_threaded_mainloop_unlock(mainLoop);
        return AUDIO_CLIENT_ERR;
    }

    pa_proplist_sets(propList, "stream.volumeFactor", std::to_string(mVolumeFactor).c_str());
    pa_operation *updatePropOperation = pa_stream_proplist_update(paStream, PA_UPDATE_REPLACE, propList,
        nullptr, nullptr);
    pa_proplist_free(propList);
    pa_operation_unref(updatePropOperation);

    if (mAudioSystemMgr == nullptr) {
        AUDIO_ERR_LOG("System manager instance is null");
        pa_threaded_mainloop_unlock(mainLoop);
        return AUDIO_CLIENT_ERR;
    }

    if (!streamInfoUpdated) {
        uint32_t idx = pa_stream_get_index(paStream);
        pa_operation *operation = pa_context_get_sink_input_info(context, idx, AudioServiceClient::GetSinkInputInfoCb,
            reinterpret_cast<void *>(this));
        if (operation == nullptr) {
            AUDIO_ERR_LOG("pa_context_get_sink_input_info_list returned null");
            pa_threaded_mainloop_unlock(mainLoop);
            return AUDIO_CLIENT_ERR;
        }

        pa_threaded_mainloop_accept(mainLoop);

        pa_operation_unref(operation);
    } else {
        SetPaVolume(*this);
    }

    pa_threaded_mainloop_unlock(mainLoop);

    return AUDIO_CLIENT_SUCCESS;
}

float AudioServiceClient::GetStreamVolume()
{
    return mVolumeFactor;
}

void AudioServiceClient::GetSinkInputInfoCb(pa_context *context, const pa_sink_input_info *info, int eol,
    void *userdata)
{
    AUDIO_INFO_LOG("GetSinkInputInfoVolumeCb in");
    AudioServiceClient *thiz = reinterpret_cast<AudioServiceClient *>(userdata);

    if (eol < 0) {
        AUDIO_ERR_LOG("Failed to get sink input information: %{public}s", pa_strerror(pa_context_errno(context)));
        return;
    }

    if (eol) {
        pa_threaded_mainloop_signal(thiz->mainLoop, 1);
        return;
    }

    if (info->proplist == nullptr) {
        AUDIO_ERR_LOG("Invalid prop list for sink input (%{public}d).", info->index);
        return;
    }

    uint32_t sessionID = 0;
    const char *sessionCStr = pa_proplist_gets(info->proplist, "stream.sessionID");
    if (sessionCStr != nullptr) {
        std::stringstream sessionStr;
        sessionStr << sessionCStr;
        sessionStr >> sessionID;
    }

    thiz->cvolume = info->volume;
    thiz->streamIndex = info->index;
    thiz->sessionID = sessionID;
    thiz->volumeChannels = info->channel_map.channels;
    thiz->streamInfoUpdated = true;

    SetPaVolume(*thiz);

    return;
}

void AudioServiceClient::SetPaVolume(const AudioServiceClient &client)
{
    pa_cvolume cv = client.cvolume;
    int32_t systemVolumeInt
        = client.mAudioSystemMgr->GetVolume(static_cast<AudioSystemManager::AudioVolumeType>(client.mStreamType));
    float systemVolume = AudioSystemManager::MapVolumeToHDI(systemVolumeInt);
    float vol = systemVolume * client.mVolumeFactor;

    AudioRingerMode ringerMode = client.mAudioSystemMgr->GetRingerMode();
    if ((client.mStreamType == STREAM_RING) && (ringerMode != RINGER_MODE_NORMAL)) {
        vol = MIN_STREAM_VOLUME_LEVEL;
    }

    if (client.mAudioSystemMgr->IsStreamMute(static_cast<AudioSystemManager::AudioVolumeType>(client.mStreamType))) {
        if (client.mUnMute_) {
            client.mAudioSystemMgr->SetMute(static_cast<AudioSystemManager::AudioVolumeType>(client.mStreamType),
                false);
        } else {
            vol = MIN_STREAM_VOLUME_LEVEL;
        }
    }
    uint32_t volume = pa_sw_volume_from_linear(vol);
    pa_cvolume_set(&cv, client.volumeChannels, volume);
    pa_operation_unref(pa_context_set_sink_input_volume(client.context, client.streamIndex, &cv, nullptr, nullptr));

    AUDIO_INFO_LOG("Applied volume : %{public}f, pa volume: %{public}d", vol, volume);
    HiviewDFX::HiSysEvent::Write("AUDIO", "AUDIO_VOLUME_CHANGE", HiviewDFX::HiSysEvent::EventType::BEHAVIOR,
        "ISOUTPUT", 1,
        "STREAMID", client.sessionID,
        "STREAMTYPE", client.mStreamType,
        "VOLUME", vol);
}

int32_t AudioServiceClient::SetStreamRenderRate(AudioRendererRate audioRendererRate)
{
    AUDIO_INFO_LOG("SetStreamRenderRate in");
    renderRate = audioRendererRate;
    if (!paStream) {
        return AUDIO_CLIENT_SUCCESS;
    }

    uint32_t rate = sampleSpec.rate;
    switch (audioRendererRate) {
        case RENDER_RATE_NORMAL:
            break;
        case RENDER_RATE_DOUBLE:
            rate *= DOUBLE_VALUE;
            break;
        case RENDER_RATE_HALF:
            rate /= DOUBLE_VALUE;
            break;
        default:
            return AUDIO_CLIENT_INVALID_PARAMS_ERR;
    }

    pa_threaded_mainloop_lock(mainLoop);
    pa_operation *operation = pa_stream_update_sample_rate(paStream, rate, nullptr, nullptr);
    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mainLoop);

    return AUDIO_CLIENT_SUCCESS;
}

AudioRendererRate AudioServiceClient::GetStreamRenderRate()
{
    return renderRate;
}

void AudioServiceClient::SaveStreamCallback(const std::weak_ptr<AudioStreamCallback> &callback)
{
    streamCallback_ = callback;

    if (state_ != PREPARED) {
        return;
    }

    std::shared_ptr<AudioStreamCallback> streamCb = streamCallback_.lock();
    if (streamCb != nullptr) {
        streamCb->OnStateChange(state_);
    }
}

void AudioServiceClient::WriteStateChangedSysEvents()
{
    uint32_t sessionID = 0;
    uint64_t transactionId = 0;
    int32_t callingPid = getpid();
    uint32_t callingUid = getuid();

    bool isOutput = true;
    if (eAudioClientType == AUDIO_SERVICE_CLIENT_PLAYBACK) {
        transactionId = mAudioSystemMgr->GetTransactionId(DEVICE_TYPE_SPEAKER, OUTPUT_DEVICE);
    } else {
        transactionId = mAudioSystemMgr->GetTransactionId(DEVICE_TYPE_MIC, INPUT_DEVICE);
        isOutput = false;
    }

    GetSessionID(sessionID);

    HiviewDFX::HiSysEvent::Write("AUDIO", "AUDIO_STREAM_CHANGE",
        HiviewDFX::HiSysEvent::EventType::BEHAVIOR,
        "ISOUTPUT", isOutput ? 1 : 0,
        "STREAMID", sessionID,
        "UID", callingUid,
        "PID", callingPid,
        "TRANSACTIONID", transactionId,
        "STREAMTYPE", mStreamType,
        "STATE", state_,
        "DEVICETYPE", DEVICE_TYPE_INVALID);
}
} // namespace AudioStandard
} // namespace OHOS
