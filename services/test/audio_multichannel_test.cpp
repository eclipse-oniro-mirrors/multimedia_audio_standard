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
#include <cstdio>
#include <iostream>

#include "audio_service_client.h"
#include "audio_log.h"
#include "audio_system_manager.h"
#include "pcm2wav.h"


using namespace OHOS::AudioStandard;
using namespace std;

namespace {
    constexpr uint8_t DEFAULT_FORMAT = SAMPLE_S16LE;
    constexpr uint8_t CHANNEL_INDEX = 2;
    constexpr uint8_t SLEEP_TIME = 2;
    constexpr uint8_t MAX_FRAME_COUNT = 10;
} // namespace

static bool InitClient(std::unique_ptr<AudioServiceClient> &client, uint32_t samplingRate, uint32_t channelCount)
{
    AudioStreamParams audioParams;
    audioParams.format = DEFAULT_FORMAT;
    audioParams.samplingRate = samplingRate;
    audioParams.channels = channelCount;

    if (client->Initialize(AUDIO_SERVICE_CLIENT_RECORD) < 0)
        return false;

    if (client->CreateStream(audioParams, STREAM_MUSIC) < 0) {
        client->ReleaseStream();
        return false;
    }

    if (client->StartStream() < 0)
        return false;

    return true;
}

int32_t StartRecording(char *recPath, int samplingRate, size_t frames, uint32_t channelCount)
{
    AUDIO_INFO_LOG("Recording::Path [%{public}s], rate [%{public}d], frames [%{public}zu], channel [%{public}d]",
        recPath, samplingRate, frames, channelCount);

    if (frames > MAX_FRAME_COUNT || frames <= 0) {
        std::cout << "ERROR!!<buffer count> supported value is from 1-10" << std::endl;
        return AUDIO_ERR;
    }

    std::unique_ptr client = std::make_unique<AudioServiceClient>();
    if (!InitClient(client, samplingRate, channelCount)) {
        AUDIO_ERR_LOG("Initialize client failed");
        return AUDIO_ERR;
    }

    size_t bufferLen;
    if (client->GetMinimumBufferSize(bufferLen) < 0) {
        return AUDIO_ERR;
    }

    uint8_t* buffer = nullptr;
    buffer = (uint8_t *) malloc(bufferLen);
    if (buffer == nullptr) {
        AUDIO_ERR_LOG("Failed to allocate buffer");
        return AUDIO_ERR;
    }

    char realPath[PATH_MAX + 1] = {0x00};
    std::string sourceFilePath(recPath);
    std::string rootPath;
    std::string fileName;

    auto pos = sourceFilePath.rfind("/");
    if (pos!= std::string::npos) {
        rootPath = sourceFilePath.substr(0, pos);
        fileName = sourceFilePath.substr(pos);
    }

    if ((strlen(sourceFilePath.c_str()) >= PATH_MAX) || (realpath(rootPath.c_str(), realPath) == nullptr)) {
        AUDIO_ERR_LOG("StartRecording:: Invalid path errno = %{public}d", errno);
        return AUDIO_ERR;
    }

    std::string verifiedPath(realPath);
    FILE *pFile = fopen(verifiedPath.append(fileName).c_str(), "wb");
    if (pFile == nullptr) {
        AUDIO_ERR_LOG("StartRecording:: Failed to open file errno = %{public}d", errno);
        return AUDIO_ERR;
    }

    size_t size = 1;
    size_t numBuffersToCapture = frames * 1024;
    StreamBuffer stream;
    stream.buffer = buffer;
    stream.bufferLen = bufferLen;
    int32_t bytesRead = 0;

    while (numBuffersToCapture) {
        bytesRead = client->ReadStream(stream, false);
        if (bytesRead < 0) {
            break;
        }

        if (bytesRead > 0) {
            fwrite(stream.buffer, size, bytesRead, pFile);
            numBuffersToCapture--;
        }
    }

    (void)fflush(pFile);
    client->FlushStream();
    client->StopStream();
    client->ReleaseStream();
    free(buffer);
    (void)fclose(pFile);
    sleep(SLEEP_TIME);
    AUDIO_INFO_LOG("================RECORDING OVER==================");
    return 0;
}

static int32_t InitPlayback(std::unique_ptr<AudioServiceClient> &client, AudioStreamParams &audioParams)
{
    if (client == nullptr) {
        AUDIO_ERR_LOG("Create AudioServiceClient instance failed");
        return AUDIO_ERR;
    }

    AUDIO_INFO_LOG("Initializing of AudioServiceClient");
    if (client->Initialize(AUDIO_SERVICE_CLIENT_PLAYBACK) < 0) {
        AUDIO_ERR_LOG("Initializing of AudioServiceClien failed");
        return AUDIO_ERR;
    }

    AUDIO_INFO_LOG("Creating Stream");
    if (client->CreateStream(audioParams, STREAM_MUSIC) < 0) {
        AUDIO_ERR_LOG("Creating Stream failed");
        return AUDIO_ERR;
    }

    AUDIO_INFO_LOG("Starting Stream");
    if (client->StartStream() < 0) {
        AUDIO_ERR_LOG("Starting Stream failed");
        return AUDIO_ERR;
    }

    return 0;
}

int32_t StartPlayback(std::unique_ptr<AudioServiceClient> &client, FILE *wavFile)
{
    uint8_t* buffer = nullptr;
    size_t n = 2;
    size_t bytesToWrite = 0;
    size_t bytesWritten = 0;
    size_t minBytes = 4;
    int32_t writeError;
    uint64_t timeStamp = 0;
    size_t bufferLen = 0;
    StreamBuffer stream;

    if (client->GetMinimumBufferSize(bufferLen) < 0) {
        AUDIO_ERR_LOG(" GetMinimumBufferSize failed");
        return AUDIO_ERR;
    }

    buffer = (uint8_t *) malloc(n * bufferLen);
    if (buffer == nullptr) {
        AUDIO_ERR_LOG("Failed to allocate buffer");
        return AUDIO_ERR;
    }

    while (!feof(wavFile)) {
        bytesToWrite = fread(buffer, 1, bufferLen, wavFile);
        bytesWritten = 0;

        while ((bytesWritten < bytesToWrite) && ((bytesToWrite - bytesWritten) > minBytes)) {
            stream.buffer = buffer + bytesWritten;
            stream.bufferLen = bytesToWrite - bytesWritten;
            bytesWritten += client->WriteStream(stream, writeError);
            if (client->GetCurrentTimeStamp(timeStamp) >= 0) {
                AUDIO_DEBUG_LOG("current timestamp: %{public}" PRIu64, timeStamp);
            }
        }
    }

    AUDIO_DEBUG_LOG("File writing over");

    free(buffer);

    return 0;
}

int32_t StartRendererPlayback(char *inputPath)
{
    AUDIO_INFO_LOG("================PLAYBACK STARTED==================");
    wav_hdr wavHeader;
    size_t headerSize = sizeof(wav_hdr);
    char path[PATH_MAX + 1] = { 0x00 };
    if ((strlen(inputPath) > PATH_MAX) || (realpath(inputPath, path) == nullptr)) {
        AUDIO_ERR_LOG("Invalid path");
        return AUDIO_ERR;
    }
    AUDIO_INFO_LOG("path = %{public}s", path);
    FILE* wavFile = fopen(path, "rb");
    if (wavFile == nullptr) {
        AUDIO_ERR_LOG("Unable to open wave file");
        return AUDIO_ERR;
    }

    float volume = 1.0f;
    (void)fread(&wavHeader, 1, headerSize, wavFile);
    AudioStreamParams audioParams;
    audioParams.format = wavHeader.bitsPerSample;
    audioParams.samplingRate = wavHeader.SamplesPerSec;
    audioParams.channels = wavHeader.NumOfChan;

    std::unique_ptr client = std::make_unique<AudioServiceClient>();
    if (InitPlayback(client, audioParams) < 0) {
        AUDIO_INFO_LOG("Initialize playback failed");
        (void)fclose(wavFile);
        return AUDIO_ERR;
    }

    AudioSystemManager *audioSystemMgr = AudioSystemManager::GetInstance();
    audioSystemMgr->SetVolume(AudioSystemManager::AudioVolumeType::STREAM_MUSIC, volume);

    if (StartPlayback(client, wavFile) < 0) {
        AUDIO_INFO_LOG("Start playback failed");
        (void)fclose(wavFile);
        return AUDIO_ERR;
    }

    client->FlushStream();
    client->StopStream();
    client->ReleaseStream();
    (void)fclose(wavFile);
    sleep(SLEEP_TIME);
    AUDIO_INFO_LOG("================PLAYBACK OVER==================");
    return 0;
}

int32_t ActivateFileIoDevice(bool isActive)
{
    AUDIO_INFO_LOG("==ActivateFileIoDevice== flag[%{public}d]", isActive);
    AudioSystemManager::GetInstance()->SetDeviceActive(ActiveDeviceType::FILE_SINK_DEVICE, isActive);
    return 0;
}

int32_t ReconfigureChannel(uint32_t count, DeviceType type)
{
    AUDIO_INFO_LOG("==ReconfigureChannel== count [%{public}d] type [%{public}d]", count, type);
    AudioSystemManager::GetInstance()->ReconfigureAudioChannel(count, type);
    return 0;
}

void PrintUsage()
{
    cout << "[Audio Multichannel Test App]" << endl << endl;
    cout << "Supported Functionalities:" << endl;
    cout << "  a) Activates/Deactivates file sink and source module" << endl;
    cout << "  b) Plays an input audio file"<< endl;
    cout << "  c) Records audio stream and writes to a file"<< endl;
    cout << "  d) Dynamically modify the sink and source module channel configuration"<< endl << endl;
    cout << "================================Usage=======================================" << endl << endl;
    cout << "-a\n\tActivates/Deactivates file sink and source module" << endl;
    cout << "\tUsage : ./audio_multichannel_test -a <boolean>" << endl;
    cout << "\tExample 1 : ./audio_multichannel_test -a true" << endl;
    cout << "\tExample 2 : ./audio_multichannel_test -a false" << endl << endl;
    cout << "-p\n\tPlays an input audio file" << endl;
    cout << "\tUsage : ./audio_multichannel_test -p <audio file path>" << endl;
    cout << "\tExample : ./audio_multichannel_test -p /data/test.wav" << endl << endl;
    cout << "-r\n\tRecords audio stream and writes to a file" << endl;
    cout << "\tUsage : ./audio_multichannel_test -r <file path> <sampling rate> <buffer count> <channel>" << endl;
    cout << "\tNOTE :Total num of buffer to record = <buffer count> * 1024. <buffer count> Max value is 10" << endl;
    cout << "\tExample 1 : ./audio_multichannel_test -r /data/record.pcm 48000 1 6" << endl;
    cout << "\tExample 2 : ./audio_multichannel_test -r /data/record.pcm 44100 2 4" << endl << endl;
    cout << "-c\n\tDynamically modify the sink and source module channel configuration" << endl;
    cout << "\tUsage : ./audio_multichannel_test -c <channel count> <DeviceType>" << endl;
    cout << "\tExample 1 : ./audio_multichannel_test -c 4 50" << endl;
    cout << "\tExample 2: ./audio_multichannel_test -c 2 51" << endl;
    cout << "\tNOTE :DeviceType 50=file_sink | 51=file_source" << endl;
}

int main(int argc, char* argv[])
{
    int c;
    bool flag;
    opterr = 0;
    while ((c = getopt(argc, argv, "a:c:p:r:h")) != AUDIO_INVALID_PARAM) {
        switch (c) {
            case 'a':
                flag = (strcmp(optarg, "true") == 0 ? true : false);
                ActivateFileIoDevice(flag);
                break;
            case 'p':
                StartRendererPlayback(optarg);
                break;
            case 'r':
                StartRecording(optarg, atoi(argv[optind]), atoi(argv[optind+1]), atoi(argv[optind+CHANNEL_INDEX]));
                break;
            case 'c':
                ReconfigureChannel(atoi(optarg), static_cast<DeviceType>(atoi(argv[optind])));
                break;
            case 'h':
                PrintUsage();
                break;
            default:
                cout << "Unsuported option. Exiting!!!" << endl;
                exit(0);
                break;
        }
    }

    return 0;
}
