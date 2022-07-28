/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
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

#include <common.h>

using namespace std;
using namespace OHOS;
using namespace OHOS::AudioStandard;

namespace OHOS {
namespace AudioStandard {
AudioCapturerAdapter::AudioCapturerAdapter() { }

AudioCapturerAdapter::~AudioCapturerAdapter() { }

AudioCapturerAdapter* AudioCapturerAdapter::GetInstance()
{
    static AudioCapturerAdapter audioCapturerAdapter_;
    return &audioCapturerAdapter_;
}

AudioCapturer *AudioCapturerAdapter::GetAudioCapturerById(SLuint32 id)
{
    AUDIO_INFO_LOG("AudioCapturerAdapter::GetAudioCapturerById: %{public}lu", id);
    return captureMap_.find(id)->second;
}

void AudioCapturerAdapter::EraseAudioCapturerById(SLuint32 id)
{
    AUDIO_INFO_LOG("AudioCapturerAdapter::EraseAudioCapturerById: %{public}lu", id);
    captureMap_.erase(id);
    callbackMap_.erase(id);
}

SLresult AudioCapturerAdapter::CreateAudioCapturerAdapter(SLuint32 id, SLDataSource *dataSource,
    SLDataSink *dataSink, AudioStreamType streamType)
{
    AUDIO_INFO_LOG("AudioCapturerAdapter::CreateAudioCapturerAdapter .");
    SLDataFormat_PCM *pcmFormat = (SLDataFormat_PCM *)dataSink->pFormat;
    AudioCapturerParams capturerParams;
    ConvertPcmFormat(pcmFormat, &capturerParams);
    streamType = AudioStreamType::STREAM_MUSIC;
    AudioCapturerOptions capturerOptions;
    capturerOptions.streamInfo.samplingRate = capturerParams.samplingRate;
    capturerOptions.streamInfo.encoding = AudioEncodingType::ENCODING_PCM;
    capturerOptions.streamInfo.format = capturerParams.audioSampleFormat;
    capturerOptions.streamInfo.channels = capturerParams.audioChannel;
    capturerOptions.capturerInfo.sourceType = SourceType::SOURCE_TYPE_MIC;
    capturerOptions.capturerInfo.capturerFlags = 0;
    string cachePath = "/data/storage/el2/base/haps/entry/files";
    unique_ptr<AudioCapturer> capturerHolder = AudioCapturer::Create(capturerOptions, cachePath.c_str());
    capturerHolder->SetParams(capturerParams);
    AudioCapturer *capturer = capturerHolder.release();
    AUDIO_INFO_LOG("AudioCapturerAdapter::CreateAudioCapturerAdapter ID: %{public}lu", id);
    capturer->SetCaptureMode(CAPTURE_MODE_CALLBACK);
    captureMap_.insert(make_pair(id, capturer));
    return SL_RESULT_SUCCESS;
}

SLresult AudioCapturerAdapter::SetCaptureStateAdapter(SLuint32 id, SLuint32 state)
{
    AUDIO_INFO_LOG("AudioCapturerAdapter::SetCaptureStateAdapter state: %{public}lu.", state);
    AudioCapturer *audioCapturer = GetAudioCapturerById(id);
    SLresult slResult = SL_RESULT_SUCCESS;
    bool reseult = false;
    switch (state) {
        case SL_RECORDSTATE_RECORDING:
            reseult = audioCapturer->Start();
            break;
        case SL_RECORDSTATE_PAUSED:
            reseult = audioCapturer->Pause();
            break;
        case SL_RECORDSTATE_STOPPED: {
            reseult = audioCapturer->Clear();
            reseult = reseult && audioCapturer->Stop();
            reseult = reseult && audioCapturer->Release();
            break;
        }
        default:
            AUDIO_ERR_LOG("AudioPlayerAdapter::play state not supported ");
            break;
    }
    slResult = reseult ? SL_RESULT_SUCCESS : SL_RESULT_RESOURCE_ERROR;
    return slResult;
}

SLresult AudioCapturerAdapter::GetCaptureStateAdapter(SLuint32 id, SLuint32 *state)
{
    AudioCapturer *audioCapturer = GetAudioCapturerById(id);
    CapturerState capturerState = audioCapturer->GetStatus();
    switch (capturerState) {
        case CAPTURER_RUNNING:
            *state = SL_RECORDSTATE_RECORDING;
            break;
        case CAPTURER_PAUSED:
            *state = SL_RECORDSTATE_PAUSED;
            break;
        case CAPTURER_STOPPED:
            *state = SL_RECORDSTATE_STOPPED;
            break;
        default:
            *state = -1;
            break;
    }
    AUDIO_INFO_LOG("AudioCapturerAdapter::GetCaptureStateAdapter state: %{public}lu.", *state);
    return SL_RESULT_SUCCESS;
}

SLresult AudioCapturerAdapter::EnqueueAdapter(SLuint32 id, const void *buffer, SLuint32 size)
{
    AudioCapturer *audioCapturer = GetAudioCapturerById(id);
    BufferDesc bufDesc = {};
    bufDesc.buffer = (uint8_t*) buffer;
    bufDesc.bufLength = size;
    AUDIO_INFO_LOG("AudioCapturerAdapter::EnqueueAdapter bufferlength: %{public}zu", bufDesc.bufLength);
    audioCapturer->Enqueue(bufDesc);
    return SL_RESULT_SUCCESS;
}

SLresult AudioCapturerAdapter::ClearAdapter(SLuint32 id)
{
    AudioCapturer *audioCapturer = GetAudioCapturerById(id);
    audioCapturer->Clear();
    return SL_RESULT_SUCCESS;
}

SLresult AudioCapturerAdapter::GetStateAdapter(SLuint32 id, SLOHBufferQueueState *state)
{
    AudioCapturer *audioCapturer = GetAudioCapturerById(id);
    BufferQueueState queueState = {0, 0};
    audioCapturer->GetBufQueueState(queueState);
    state->count = queueState.numBuffers;
    state->index = queueState.currentIndex;
    return SL_RESULT_SUCCESS;
}

SLresult AudioCapturerAdapter::GetBufferAdapter(SLuint32 id, SLuint8 **buffer, SLuint32 *size)
{
    AudioCapturer *audioCapturer = GetAudioCapturerById(id);
    BufferDesc bufferDesc = {};
    audioCapturer->GetBufferDesc(bufferDesc);
    *buffer = bufferDesc.buffer;
    *size = bufferDesc.bufLength;
    return SL_RESULT_SUCCESS;
}

SLresult AudioCapturerAdapter::RegisterCallbackAdapter(SLOHBufferQueueItf itf,
    SlOHBufferQueueCallback callback, void *pContext)
{
    IOHBufferQueue *thiz = (IOHBufferQueue *)itf;
    AudioCapturer *audioCapturer = GetAudioCapturerById(thiz->mId);
    callbackPtr_ = make_shared<ReadOrWriteCallbackAdapter>(callback, itf, pContext);
    audioCapturer->SetCapturerReadCallback(static_pointer_cast<AudioCapturerReadCallback>(callbackPtr_));
    callbackMap_.insert(make_pair(thiz->mId, callbackPtr_));
    return SL_RESULT_SUCCESS;
}

void AudioCapturerAdapter::ConvertPcmFormat(SLDataFormat_PCM *slFormat, AudioCapturerParams *capturerParams)
{
    AudioSampleFormat sampleFormat = SlToOhosSampelFormat(slFormat);
    AudioSamplingRate sampleRate = SlToOhosSamplingRate(slFormat);
    AudioChannel channelCount = SlToOhosChannel(slFormat);
    capturerParams->audioSampleFormat = sampleFormat;
    capturerParams->samplingRate = sampleRate;
    capturerParams->audioChannel = channelCount;
    capturerParams->audioEncoding = ENCODING_PCM;
}

AudioSampleFormat AudioCapturerAdapter::SlToOhosSampelFormat(SLDataFormat_PCM *pcmFormat)
{
    AudioSampleFormat sampleFormat;
    switch (pcmFormat->bitsPerSample) {
        case SL_PCMSAMPLEFORMAT_FIXED_8:
            sampleFormat = SAMPLE_U8;
            break;
        case SL_PCMSAMPLEFORMAT_FIXED_16:
            sampleFormat = SAMPLE_S16LE;
            break;
        case SL_PCMSAMPLEFORMAT_FIXED_20:
            sampleFormat = INVALID_WIDTH;
            break;
        case SL_PCMSAMPLEFORMAT_FIXED_24:
            sampleFormat = SAMPLE_S24LE;
            break;
        case SL_PCMSAMPLEFORMAT_FIXED_28:
            sampleFormat = INVALID_WIDTH;
            break;
        case SL_PCMSAMPLEFORMAT_FIXED_32:
            sampleFormat = SAMPLE_S32LE;
            break;
        default:
            sampleFormat = INVALID_WIDTH;
    }
    return sampleFormat;
}

AudioSamplingRate AudioCapturerAdapter::SlToOhosSamplingRate(SLDataFormat_PCM *pcmFormat)
{
    AudioSamplingRate sampleRate;
    switch (pcmFormat->samplesPerSec) {
        case SL_SAMPLINGRATE_8:
            sampleRate = SAMPLE_RATE_8000;
            break;
        case SL_SAMPLINGRATE_11_025:
            sampleRate = SAMPLE_RATE_11025;
            break;
        case SL_SAMPLINGRATE_12:
            sampleRate = SAMPLE_RATE_12000;
            break;
        case SL_SAMPLINGRATE_16:
            sampleRate = SAMPLE_RATE_16000;
            break;
        case SL_SAMPLINGRATE_22_05:
            sampleRate = SAMPLE_RATE_22050;
            break;
        case SL_SAMPLINGRATE_24:
            sampleRate = SAMPLE_RATE_24000;
            break;
        case SL_SAMPLINGRATE_32:
            sampleRate = SAMPLE_RATE_32000;
            break;
        case SL_SAMPLINGRATE_44_1:
            sampleRate = SAMPLE_RATE_44100;
            break;
        case SL_SAMPLINGRATE_48:
            sampleRate = SAMPLE_RATE_48000;
            break;
        case SL_SAMPLINGRATE_64:
            sampleRate = SAMPLE_RATE_64000;
            break;
        case SL_SAMPLINGRATE_88_2:
            sampleRate = SAMPLE_RATE_44100;
            break;
        case SL_SAMPLINGRATE_96:
            sampleRate = SAMPLE_RATE_96000;
            break;
        case SL_SAMPLINGRATE_192:
            sampleRate = SAMPLE_RATE_44100;
            break;
        default: {
            AUDIO_ERR_LOG("AudioCapturerAdapter::SlToOhosSamplingRate mismatch, use default.");
            sampleRate = SAMPLE_RATE_44100;
        }
    }
    return sampleRate;
}

AudioChannel AudioCapturerAdapter::SlToOhosChannel(SLDataFormat_PCM *pcmFormat)
{
    AudioChannel channelCount;
    switch (pcmFormat->numChannels) {
        case MONO:
            channelCount = MONO;
            break;
        case STEREO:
            channelCount = STEREO;
            break;
        default:
            channelCount = MONO;
            AUDIO_ERR_LOG("AudioPlayerAdapter::channel count not supported ");
    }
    return channelCount;
}
}
}
