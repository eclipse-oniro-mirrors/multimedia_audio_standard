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

#ifndef AUDIO_SERVICE_DUMP_H
#define AUDIO_SERVICE_DUMP_H

#include <pulse/pulseaudio.h>
#include <audio_info.h>
#include <audio_timer.h>
#include <audio_errors.h>
#include <vector>
#include <pwd.h>
#include <map>
#include "securec.h"
#include "audio_log.h"
#include "nocopyable.h"

namespace OHOS {
namespace AudioStandard {
constexpr int STRING_BUFFER_SIZE = 4096;
template <typename...Args>

void AppendFormat(std::string& out, const char* fmt, Args&& ... args)
{
    char buf[STRING_BUFFER_SIZE] = {0};
    int len = ::sprintf_s(buf, sizeof(buf), fmt, args...);
    if (len <= 0) {
        AUDIO_ERR_LOG("snprintf_s error : buffer allocation fails");
        return;
    }
    out += buf;
}

static const int32_t AUDIO_DUMP_SUCCESS = 0;
static const int32_t AUDIO_DUMP_INIT_ERR = -1;

typedef struct {
    DeviceType deviceType;
    DeviceRole deviceRole;
} DevicesInfo;

typedef struct {
    std::string name;
    pa_sample_spec sampleSpec;
} SinkSourceInfo;

typedef struct {
    uint32_t userId;
    uint32_t corked;                   // status
    std::string sessionId;
    std::string sessionStartTime;
    std::string applicationName;
    std::string processId;
    pa_sample_spec sampleSpec;
}InputOutputInfo;

typedef struct {
    std::vector<SinkSourceInfo> sinkDevices;
    std::vector<SinkSourceInfo> sourceDevices;
    std::vector<InputOutputInfo> sinkInputs;
    std::vector<InputOutputInfo> sourceOutputs;
} StreamData;

typedef struct {
    std::vector<DevicesInfo> inputDevices;
    std::vector<DevicesInfo> outputDevices;
    std::map<AudioStreamType, int32_t> streamVolumes;
    AudioRingerMode ringerMode;
    AudioScene callStatus;
    AudioInterrupt audioFocusInfo;
} PolicyData;

typedef struct {
    StreamData streamData;
    PolicyData policyData;
} AudioData;

class AudioServiceDump : public AudioTimer {
public:
    DISALLOW_COPY_AND_MOVE(AudioServiceDump);

    AudioServiceDump();
    ~AudioServiceDump();
    int32_t Initialize();
    void AudioDataDump(PolicyData &policyData, std::string &dumpString);
    static bool IsStreamSupported(AudioStreamType streamType);
    virtual void OnTimeOut();

private:
    pa_threaded_mainloop *mainLoop;
    pa_mainloop_api *api;
    pa_context *context;
    std::mutex ctrlMutex;

    bool isMainLoopStarted;
    bool isContextConnected;
    AudioData audioData_ = {};

    int32_t ConnectStreamToPA();
    void ResetPAAudioDump();

    void PlaybackStreamDump(std::string &dumpString);
    void RecordStreamDump(std::string &dumpString);
    void HDFModulesDump(std::string &dumpString);
    void CallStatusDump(std::string &dumpString);
    void RingerModeDump(std::string &dumpString);
    void StreamVolumesDump(std::string &dumpString);
    void DevicesInfoDump(std::string &dumpString);
    void AudioFocusInfoDump(std::string &dumpString);
    void DataDump(std::string &dumpString);
    static const std::string GetStreamName(AudioStreamType audioType);
    static const std::string GetStreamUsgaeName(StreamUsage streamUsage);
    static const std::string GetContentTypeName(ContentType contentType);
    static const std::string GetDeviceTypeName(DeviceType deviceType);
    static bool IsEndWith(const std::string &mainStr, const std::string &toMatch);
    static bool IsValidModule (const std::string moduleName);

    // Callbacks
    static void PAContextStateCb(pa_context *context, void *userdata);
    static void PASinkInfoCallback(pa_context *c, const pa_sink_info *i, int eol, void *userdata);
    static void PASinkInputInfoCallback(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata);
    static void PASourceInfoCallback(pa_context *c, const pa_source_info *i, int eol, void *userdata);
    static void PASourceOutputInfoCallback(pa_context *c, const pa_source_output_info *i, int eol, void *userdata);
};
} // namespace AudioStandard
} // namespace OHOS
#endif // AUDIO_SERVICE_DUMP_H
