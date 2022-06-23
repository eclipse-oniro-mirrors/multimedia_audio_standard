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

#include "audio_service_dump.h"

using namespace std;

namespace OHOS {
namespace AudioStandard {
AudioServiceDump::AudioServiceDump() : mainLoop(nullptr),
                                       api(nullptr),
                                       context(nullptr),
                                       isMainLoopStarted(false),
                                       isContextConnected(false)
{
    AUDIO_DEBUG_LOG("AudioServiceDump ctor");
}

AudioServiceDump::~AudioServiceDump()
{
    ResetPAAudioDump();
}

void AudioServiceDump::ResetPAAudioDump()
{
    lock_guard<mutex> lock(ctrlMutex);
    if (mainLoop && (isMainLoopStarted == true)) {
        pa_threaded_mainloop_stop(mainLoop);
    }

    if (context) {
        pa_context_set_state_callback(context, nullptr, nullptr);
        if (isContextConnected == true) {
            pa_context_disconnect(context);
        }
        pa_context_unref(context);
    }

    if (mainLoop) {
        pa_threaded_mainloop_free(mainLoop);
    }

    isMainLoopStarted  = false;
    isContextConnected = false;
    mainLoop = nullptr;
    context  = nullptr;
    api      = nullptr;
}

int32_t AudioServiceDump::Initialize()
{
    int error = PA_ERR_INTERNAL;
    mainLoop = pa_threaded_mainloop_new();
    if (mainLoop == nullptr) {
        return AUDIO_DUMP_INIT_ERR;
    }

    api = pa_threaded_mainloop_get_api(mainLoop);
    if (api == nullptr) {
        ResetPAAudioDump();
        return AUDIO_DUMP_INIT_ERR;
    }

    context = pa_context_new(api, "AudioServiceDump");
    if (context == nullptr) {
        ResetPAAudioDump();
        return AUDIO_DUMP_INIT_ERR;
    }

    pa_context_set_state_callback(context, PAContextStateCb, mainLoop);

    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFAIL, nullptr) < 0) {
        error = pa_context_errno(context);
        AUDIO_ERR_LOG("context connect error: %{public}s", pa_strerror(error));
        ResetPAAudioDump();
        return AUDIO_DUMP_INIT_ERR;
    }

    isContextConnected = true;
    pa_threaded_mainloop_lock(mainLoop);

    if (pa_threaded_mainloop_start(mainLoop) < 0) {
        AUDIO_ERR_LOG("Audio Service not started");
        pa_threaded_mainloop_unlock(mainLoop);
        ResetPAAudioDump();
        return AUDIO_DUMP_INIT_ERR;
    }

    isMainLoopStarted = true;
    while (true) {
        pa_context_state_t state = pa_context_get_state(context);
        if (state == PA_CONTEXT_READY) {
            break;
        }

        if (!PA_CONTEXT_IS_GOOD(state)) {
            error = pa_context_errno(context);
            AUDIO_ERR_LOG("context bad state error: %{public}s", pa_strerror(error));
            pa_threaded_mainloop_unlock(mainLoop);
            ResetPAAudioDump();
            return AUDIO_DUMP_INIT_ERR;
        }

        pa_threaded_mainloop_wait(mainLoop);
    }

    pa_threaded_mainloop_unlock(mainLoop);
    return AUDIO_DUMP_SUCCESS;
}

void AudioServiceDump::OnTimeOut()
{
    pa_threaded_mainloop_lock(mainLoop);
    pa_threaded_mainloop_signal(mainLoop, 0);
    pa_threaded_mainloop_unlock(mainLoop);
}

bool AudioServiceDump::IsEndWith(const std::string &mainStr, const std::string &toMatch)
{
    if (mainStr.size() >= toMatch.size() &&
        mainStr.compare(mainStr.size() - toMatch.size(), toMatch.size(), toMatch) == 0) {
        return true;
    }
    return false;
}

bool AudioServiceDump::IsValidModule(const std::string moduleName)
{
    if (moduleName.rfind("fifo", 0) == SUCCESS) {
        return false; // Module starts with fifo, Not valid module
    }

    if (IsEndWith(moduleName, "monitor")) {
        return false; // Module ends with monitor, Not valid module
    }
    return true;
}

bool AudioServiceDump::IsStreamSupported(AudioStreamType streamType)
{
    switch (streamType) {
        case STREAM_MUSIC:
        case STREAM_RING:
        case STREAM_VOICE_CALL:
        case STREAM_VOICE_ASSISTANT:
            return true;
        default:
            return false;
    }
}

const std::string AudioServiceDump::GetStreamName(AudioStreamType audioType)
{
    string name;
    switch (audioType) {
        case STREAM_VOICE_ASSISTANT:
            name = "VOICE_ASSISTANT";
            break;
        case STREAM_VOICE_CALL:
            name = "VOICE_CALL";
            break;
        case STREAM_SYSTEM:
            name = "SYSTEM";
            break;
        case STREAM_RING:
            name = "RING";
            break;
        case STREAM_MUSIC:
            name = "MUSIC";
            break;
        case STREAM_ALARM:
            name = "ALARM";
            break;
        case STREAM_NOTIFICATION:
            name = "NOTIFICATION";
            break;
        case STREAM_BLUETOOTH_SCO:
            name = "BLUETOOTH_SCO";
            break;
        case STREAM_DTMF:
            name = "DTMF";
            break;
        case STREAM_TTS:
            name = "TTS";
            break;
        default:
            name = "UNKNOWN";
    }

    const string streamName = name;
    return streamName;
}

const std::string AudioServiceDump::GetStreamUsgaeName(StreamUsage streamUsage)
{
    string usage;
    switch (streamUsage) {
        case STREAM_USAGE_MEDIA:
            usage = "MEDIA";
            break;
        case STREAM_USAGE_VOICE_COMMUNICATION:
            usage = "VOICE_COMMUNICATION";
            break;
        case STREAM_USAGE_NOTIFICATION_RINGTONE:
            usage = "NOTIFICATION_RINGTONE";
            break;
        case STREAM_USAGE_VOICE_ASSISTANT:
            usage = "VOICE_ASSISTANT";
            break;
        default:
            usage = "STREAM_USAGE_UNKNOWN";
    }

    const string streamUsageName = usage;
    return streamUsageName;
}

const std::string AudioServiceDump::GetContentTypeName(ContentType contentType)
{
    string content;
    switch (contentType) {
        case CONTENT_TYPE_SPEECH:
            content = "SPEECH";
            break;
        case CONTENT_TYPE_MUSIC:
            content = "MUSIC";
            break;
        case CONTENT_TYPE_MOVIE:
            content = "MOVIE";
            break;
        case CONTENT_TYPE_SONIFICATION:
            content = "SONIFICATION";
            break;
        case CONTENT_TYPE_RINGTONE:
            content = "RINGTONE";
            break;
        default:
            content = "UNKNOWN";
    }

    const string contentTypeName = content;
    return contentTypeName;
}

const std::string AudioServiceDump::GetDeviceTypeName(DeviceType deviceType)
{
    string device;
    switch (deviceType) {
        case DEVICE_TYPE_SPEAKER:
            device = "SPEAKER";
            break;
        case DEVICE_TYPE_WIRED_HEADSET:
            device = "WIRED_HEADSET";
            break;
        case DEVICE_TYPE_BLUETOOTH_SCO:
             device = "BLUETOOTH_SCO";
            break;
        case DEVICE_TYPE_BLUETOOTH_A2DP:
            device = "BLUETOOTH_A2DP";
            break;
        case DEVICE_TYPE_MIC:
            device = "MIC";
            break;
        case DEVICE_TYPE_NONE:
            device = "NONE";
            break;
        case DEVICE_TYPE_INVALID:
            device = "INVALID";
            break;
        default:
            device = "UNKNOWN";
    }

    const string deviceTypeName = device;
    return deviceTypeName;
}

void AudioServiceDump::PlaybackStreamDump(std::string &dumpString)
{
    char s[PA_SAMPLE_SPEC_SNPRINT_MAX];

    dumpString += "Audio Data Dump:\n\n";
    dumpString += "Playback Streams\n";

    AppendFormat(dumpString, "%d  Playback stream (s) available:\n\n", audioData_.streamData.sinkInputs.size());

    for (auto it = audioData_.streamData.sinkInputs.begin(); it != audioData_.streamData.sinkInputs.end(); it++) {
        InputOutputInfo sinkInputInfo = *it;

        AppendFormat(dumpString, "Stream Id: %s\n", (sinkInputInfo.sessionId).c_str());
        AppendFormat(dumpString, "Application Name: %s\n", ((sinkInputInfo.applicationName).c_str()));
        AppendFormat(dumpString, "Process Id: %s\n", (sinkInputInfo.processId).c_str());
        AppendFormat(dumpString, "User Id: %u\n", sinkInputInfo.userId);

        char *inputSampleSpec = pa_sample_spec_snprint(s, sizeof(s), &(sinkInputInfo.sampleSpec));
        AppendFormat(dumpString, "Stream Configuration: %s\n", inputSampleSpec);
        dumpString += "Status:";
        dumpString += (sinkInputInfo.corked) ? "STOPPED/PAUSED" : "RUNNING";
        AppendFormat(dumpString, "\nStream Start Time: %s\n", (sinkInputInfo.sessionStartTime).c_str());
        dumpString += "\n";
    }
}

void AudioServiceDump::RecordStreamDump(std::string &dumpString)
{
    char s[PA_SAMPLE_SPEC_SNPRINT_MAX];
    dumpString += "Record Streams \n";
    AppendFormat(dumpString, "%d  Record stream (s) available:\n\n", audioData_.streamData.sourceOutputs.size());

    for (auto it = audioData_.streamData.sourceOutputs.begin(); it != audioData_.streamData.sourceOutputs.end(); it++) {
        InputOutputInfo sourceOutputInfo = *it;
        AppendFormat(dumpString, "Stream Id: %s\n", (sourceOutputInfo.sessionId).c_str());
        AppendFormat(dumpString, "Application Name: %s\n", (sourceOutputInfo.applicationName).c_str());
        AppendFormat(dumpString, "Process Id: %s\n", sourceOutputInfo.processId.c_str());
        AppendFormat(dumpString, "User Id: %d\n", sourceOutputInfo.userId);

        char *outputSampleSpec = pa_sample_spec_snprint(s, sizeof(s), &(sourceOutputInfo.sampleSpec));
        AppendFormat(dumpString, "Stream Configuration: %s\n", outputSampleSpec);
        dumpString += "Status:";
        dumpString += (sourceOutputInfo.corked) ? "STOPPED/PAUSED" : "RUNNING";
        AppendFormat(dumpString, "\nStream Start Time: %s\n", (sourceOutputInfo.sessionStartTime).c_str());
        dumpString += "\n";
    }
}

void AudioServiceDump::HDFModulesDump(std::string &dumpString)
{
    char s[PA_SAMPLE_SPEC_SNPRINT_MAX];

    dumpString += "\nHDF Input Modules\n";
    AppendFormat(dumpString, "%d  HDF Input Modules (s) available:\n\n", audioData_.streamData.sourceDevices.size());

    for (auto it = audioData_.streamData.sourceDevices.begin(); it != audioData_.streamData.sourceDevices.end(); it++) {
        SinkSourceInfo sourceInfo = *it;

        AppendFormat(dumpString, "Module Name: %s\n", (sourceInfo.name).c_str());
        char *hdfOutSampleSpec = pa_sample_spec_snprint(s, sizeof(s), &(sourceInfo.sampleSpec));
        AppendFormat(dumpString, "Module Configuration: %s\n\n", hdfOutSampleSpec);
    }

    dumpString += "HDF Output Modules\n";
    AppendFormat(dumpString, "%d  HDF Output Modules (s) available:\n\n", audioData_.streamData.sinkDevices.size());

    for (auto it = audioData_.streamData.sinkDevices.begin(); it != audioData_.streamData.sinkDevices.end(); it++) {
        SinkSourceInfo sinkInfo = *it;
        AppendFormat(dumpString, "Module Name: %s\n", (sinkInfo.name).c_str());
        char *hdfInSampleSpec = pa_sample_spec_snprint(s, sizeof(s), &(sinkInfo.sampleSpec));
        AppendFormat(dumpString, "Module Configuration: %s\n\n", hdfInSampleSpec);
    }
}

void AudioServiceDump::CallStatusDump(std::string &dumpString)
{
    dumpString += "\nAudio Scene:";
    switch (audioData_.policyData.callStatus) {
        case AUDIO_SCENE_DEFAULT:
            dumpString += "DEFAULT";
            break;
        case AUDIO_SCENE_RINGING:
            dumpString += "RINGING";
            break;
        case AUDIO_SCENE_PHONE_CALL:
            dumpString += "PHONE_CALL";
            break;
        case AUDIO_SCENE_PHONE_CHAT:
            dumpString += "PHONE_CHAT";
            break;
        default:
            dumpString += "UNKNOWN";
    }
    dumpString += "\n";
}

void AudioServiceDump::RingerModeDump(std::string &dumpString)
{
    dumpString += "Ringer Mode:";
    switch (audioData_.policyData.ringerMode) {
        case RINGER_MODE_NORMAL:
            dumpString += "NORMAL";
            break;
        case RINGER_MODE_SILENT:
            dumpString += "SILENT";
            break;
        case RINGER_MODE_VIBRATE:
            dumpString += "VIBRATE";
            break;
        default:
            dumpString += "UNKNOWN";
    }
    dumpString += "\n";
}

void AudioServiceDump::StreamVolumesDump (string &dumpString)
{
    dumpString += "\nStream: Volumes\n";
    for (auto it = audioData_.policyData.streamVolumes.cbegin(); it != audioData_.policyData.streamVolumes.cend();
        ++it) {
        AppendFormat(dumpString, "%s: %d\n", GetStreamName(it->first).c_str(), it->second);
    }
}

void AudioServiceDump::AudioFocusInfoDump(string &dumpString)
{
    dumpString += "\nAudio In Focus Info:\n";
    uint32_t invalidSessionID = static_cast<uint32_t>(-1);

    if (audioData_.policyData.audioFocusInfo.sessionID == invalidSessionID) {
        AUDIO_DEBUG_LOG("No streams in focus");
        dumpString += "Not available\n";
        return;
    }

    AppendFormat(dumpString, "Stream Id: %d\n", audioData_.policyData.audioFocusInfo.sessionID);
    AppendFormat(dumpString, "Stream Usage: %s\n",
        GetStreamUsgaeName(audioData_.policyData.audioFocusInfo.streamUsage).c_str());
    AppendFormat(dumpString, "Content Type: %s\n",
        GetContentTypeName(audioData_.policyData.audioFocusInfo.contentType).c_str());
    AppendFormat(dumpString, "Stream Name: %s\n",
        GetStreamName(audioData_.policyData.audioFocusInfo.streamType).c_str());

	return;
}

void AudioServiceDump::DevicesInfoDump(string &dumpString)
{
    dumpString += "\nInput Devices:\n";
    AppendFormat(dumpString, "%d  Input Devices (s) available :\n\n", audioData_.policyData.inputDevices.size());

    for (auto it = audioData_.policyData.inputDevices.begin(); it != audioData_.policyData.inputDevices.end(); it++) {
        DevicesInfo devicesInfo = *it;
        AppendFormat(dumpString, "%s\n", GetDeviceTypeName(devicesInfo.deviceType).c_str());
    }

    dumpString += "\nOutput Devices:\n";
    AppendFormat(dumpString, "%d  Output Devices (s) available :\n\n", audioData_.policyData.outputDevices.size());

    for (auto it = audioData_.policyData.outputDevices.begin(); it != audioData_.policyData.outputDevices.end(); it++) {
        DevicesInfo devicesInfo = *it;
        AppendFormat(dumpString, "%s\n", GetDeviceTypeName(devicesInfo.deviceType).c_str());
    }
}

void AudioServiceDump::DataDump(string &dumpString)
{
    PlaybackStreamDump(dumpString);
    RecordStreamDump(dumpString);
    HDFModulesDump(dumpString);
    DevicesInfoDump(dumpString);
    CallStatusDump(dumpString);
    RingerModeDump(dumpString);
    StreamVolumesDump(dumpString);
    AudioFocusInfoDump(dumpString);
}

void AudioServiceDump::AudioDataDump(PolicyData &policyData, string &dumpString)
{
    if (mainLoop == nullptr || context == nullptr) {
        AUDIO_ERR_LOG("Audio Service Not running");
        return;
    }

    pa_threaded_mainloop_lock(mainLoop);
    pa_operation *operation = nullptr;
    operation = pa_context_get_sink_info_list(context, AudioServiceDump::PASinkInfoCallback, (void *)(this));

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainLoop);
    }

    pa_operation_unref(operation);
    operation = pa_context_get_sink_input_info_list(context, AudioServiceDump::PASinkInputInfoCallback, (void *)this);

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainLoop);
    }

    pa_operation_unref(operation);
    operation = pa_context_get_source_info_list(context, AudioServiceDump::PASourceInfoCallback, (void *)this);

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainLoop);
    }

    pa_operation_unref(operation);
    operation = pa_context_get_source_output_info_list(context,
        AudioServiceDump::PASourceOutputInfoCallback, (void *)this);

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(mainLoop);
    }

    pa_operation_unref(operation);
    pa_threaded_mainloop_unlock(mainLoop);

    audioData_.policyData = policyData;
    DataDump(dumpString);

    return;
}

void AudioServiceDump::PAContextStateCb(pa_context *context, void *userdata)
{
    pa_threaded_mainloop *mainLoop = (pa_threaded_mainloop *)userdata;

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
    return;
}

void AudioServiceDump::PASinkInfoCallback(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
    AudioServiceDump *asDump = (AudioServiceDump *)userdata;
    if (asDump == nullptr) {
        AUDIO_ERR_LOG("Failed to get sink information");
        return;
    }

    pa_threaded_mainloop *mainLoop = (pa_threaded_mainloop *)asDump->mainLoop;

    if (eol < 0) {
        AUDIO_ERR_LOG("Failed to get sink information: %{public}s", pa_strerror(pa_context_errno(c)));
        return;
    }

    if (eol) {
        pa_threaded_mainloop_signal(mainLoop, 0);
        return;
    }

    SinkSourceInfo sinkInfo;

    if (i->name != nullptr) {
        string sinkName(i->name);
        if (IsValidModule(sinkName)) {
            (sinkInfo.name).assign(sinkName);
            sinkInfo.sampleSpec = i->sample_spec;
            asDump->audioData_.streamData.sinkDevices.push_back(sinkInfo);
        }
    }
}

void AudioServiceDump::PASinkInputInfoCallback(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata)
{
    AudioServiceDump *asDump = (AudioServiceDump *)userdata;
    if (asDump == nullptr) {
        AUDIO_ERR_LOG("Failed to get sink input information");
        return;
    }

    pa_threaded_mainloop *mainLoop = (pa_threaded_mainloop *)asDump->mainLoop;

    if (eol < 0) {
        AUDIO_ERR_LOG("Failed to get sink input information: %{public}s", pa_strerror(pa_context_errno(c)));
        return;
    }

    if (eol) {
        pa_threaded_mainloop_signal(mainLoop, 0);
        return;
    }

    InputOutputInfo sinkInputInfo;

    sinkInputInfo.sampleSpec = i->sample_spec;
    sinkInputInfo.corked = i->corked;

    if (i->proplist !=nullptr) {
        const char *applicationname = pa_proplist_gets(i->proplist, "application.name");
        const char *processid = pa_proplist_gets(i->proplist, "application.process.id");
        const char *user = pa_proplist_gets(i->proplist, "application.process.user");
        const char *sessionid = pa_proplist_gets(i->proplist, "stream.sessionID");
        const char *sessionstarttime = pa_proplist_gets(i->proplist, "stream.startTime");

        if (applicationname != nullptr) {
            string applicationName(applicationname);
            (sinkInputInfo.applicationName).assign(applicationName);
        }

        if (processid != nullptr) {
            string processId(processid);
            (sinkInputInfo.processId).assign(processId);
        }

        if (user != nullptr) {
            struct passwd *p;
            if ((p = getpwnam(user)) != nullptr) {
                sinkInputInfo.userId = uint32_t(p->pw_uid);
            }
        }

        if (sessionid != nullptr) {
            string sessionId(sessionid);
            (sinkInputInfo.sessionId).assign(sessionId);
        }

        if (sessionstarttime != nullptr) {
            string sessionStartTime(sessionstarttime);
            (sinkInputInfo.sessionStartTime).assign(sessionStartTime);
        }
    }
    asDump->audioData_.streamData.sinkInputs.push_back(sinkInputInfo);
}

void AudioServiceDump::PASourceInfoCallback(pa_context *c, const pa_source_info *i, int eol, void *userdata)
{
    AudioServiceDump *asDump = (AudioServiceDump *)userdata;
    if (asDump == nullptr) {
        AUDIO_ERR_LOG("Failed to get source information");
        return;
    }

    pa_threaded_mainloop *mainLoop = (pa_threaded_mainloop *)asDump->mainLoop;

    if (eol < 0) {
        AUDIO_ERR_LOG("Failed to get source information: %{public}s", pa_strerror(pa_context_errno(c)));
        return;
    }

    if (eol) {
        pa_threaded_mainloop_signal(mainLoop, 0);
        return;
    }

    SinkSourceInfo sourceInfo;

    if (i->name != nullptr) {
        string sourceName(i->name);
        if (IsValidModule(sourceName)) {
            (sourceInfo.name).assign(sourceName);
            sourceInfo.sampleSpec = i->sample_spec;
            asDump->audioData_.streamData.sourceDevices.push_back(sourceInfo);
        }
    }
}

void AudioServiceDump::PASourceOutputInfoCallback(pa_context *c, const pa_source_output_info *i, int eol,
    void *userdata)
{
    AudioServiceDump *asDump = (AudioServiceDump *)userdata;
    if (asDump == nullptr) {
        AUDIO_ERR_LOG("Failed to get source output information");
        return;
    }

    pa_threaded_mainloop *mainLoop = (pa_threaded_mainloop *)asDump->mainLoop;

    if (eol < 0) {
        AUDIO_ERR_LOG("Failed to get source output information: %{public}s", pa_strerror(pa_context_errno(c)));
        return;
    }

    if (eol) {
        pa_threaded_mainloop_signal(mainLoop, 0);
        return;
    }

    InputOutputInfo sourceOutputInfo;
    sourceOutputInfo.sampleSpec = i->sample_spec;
    sourceOutputInfo.corked = i->corked;

    if (i->proplist !=nullptr) {
        const char *applicationname = pa_proplist_gets(i->proplist, "application.name");
        const char *processid = pa_proplist_gets(i->proplist, "application.process.id");
        const char *user = pa_proplist_gets(i->proplist, "application.process.user");
        const char *sessionid = pa_proplist_gets(i->proplist, "stream.sessionID");
        const char *sessionstarttime = pa_proplist_gets(i->proplist, "stream.startTime");

        if (applicationname != nullptr) {
            string applicationName(applicationname);
            (sourceOutputInfo.applicationName).assign(applicationName);
        }

        if (processid != nullptr) {
            string processId(processid);
            (sourceOutputInfo.processId).assign(processId);
        }

        if (user != nullptr) {
            struct passwd *p;
            if ((p = getpwnam(user)) != nullptr) {
                sourceOutputInfo.userId = uint32_t(p->pw_uid);
            }
        }

        if (sessionid != nullptr) {
            string sessionId(sessionid);
            (sourceOutputInfo.sessionId).assign(sessionId);
        }

        if (sessionstarttime != nullptr) {
            string sessionStartTime(sessionstarttime);
            (sourceOutputInfo.sessionStartTime).assign(sessionStartTime);
        }
    }
    asDump->audioData_.streamData.sourceOutputs.push_back(sourceOutputInfo);
}
} // namespace AudioStandard
} // namespace OHOS
