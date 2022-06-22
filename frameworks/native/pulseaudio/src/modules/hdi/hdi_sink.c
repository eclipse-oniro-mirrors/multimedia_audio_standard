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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/modargs.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>

#include <audio_manager.h>
#include <renderer_sink_adapter.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/thread.h>

#include "audio_log.h"

#define DEFAULT_SINK_NAME "hdi_output"
#define DEFAULT_AUDIO_DEVICE_NAME "Speaker"
#define DEFAULT_DEVICE_CLASS "primary"
#define DEFAULT_BUFFER_SIZE 8192
#define MAX_SINK_VOLUME_LEVEL 1.0

const char *DEVICE_CLASS_A2DP = "a2dp";

enum {
    HDI_INIT,
    HDI_DEINIT,
    HDI_START,
    HDI_STOP,
    HDI_RENDER,
    QUIT
};

struct Userdata {
    const char *adapterName;
    uint32_t buffer_size;
    uint32_t fixed_latency;
    uint32_t render_in_idle_state;
    uint32_t open_mic_speaker;
    size_t bytes_dropped;
    pa_thread_mq thread_mq;
    pa_memchunk memchunk;
    pa_usec_t block_usec;
    pa_usec_t timestamp;
    pa_thread *thread;
    pa_thread *thread_hdi;
    pa_rtpoll *rtpoll;
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
    pa_sample_spec ss;
    pa_channel_map map;
    bool isHDISinkStarted;
    struct RendererSinkAdapter *sinkAdapter;
    pa_asyncmsgq *dq;
    pa_atomic_t dflag;
};

static void UserdataFree(struct Userdata *u);
static int32_t PrepareDevice(struct Userdata *u, const char* filePath);

static ssize_t RenderWrite(struct Userdata *u, pa_memchunk *pchunk)
{
    size_t index, length;
    ssize_t count = 0;
    void *p = NULL;

    pa_assert(pchunk);

    index = pchunk->index;
    length = pchunk->length;
    p = pa_memblock_acquire(pchunk->memblock);
    pa_assert(p);

    while (true) {
        uint64_t writeLen = 0;

        int32_t ret = u->sinkAdapter->RendererRenderFrame((char *)p + index, (uint64_t)length, &writeLen);
        if (writeLen > length) {
            AUDIO_ERR_LOG("Error writeLen > actual bytes. Length: %zu, Written: %" PRIu64 " bytes, %d ret",
                         length, writeLen, ret);
            count = -1 - count;
            break;
        }
        if (writeLen == 0) {
            AUDIO_ERR_LOG("Failed to render Length: %zu, Written: %" PRIu64 " bytes, %d ret",
                         length, writeLen, ret);
            count = -1 - count;
            break;
        } else {
            count += writeLen;
            index += writeLen;
            length -= writeLen;
            if (length <= 0) {
                break;
            }
        }
    }
    pa_memblock_release(pchunk->memblock);
    pa_memblock_unref(pchunk->memblock);

    return count;
}

static void ProcessRenderUseTiming(struct Userdata *u, pa_usec_t now)
{
    pa_assert(u);

    // Fill the buffer up the latency size
    pa_memchunk chunk;

    // Change from pa_sink_render to pa_sink_render_full for alignment issue in 3516
    pa_sink_render_full(u->sink, u->sink->thread_info.max_request, &chunk);
    pa_assert(chunk.length > 0);

    pa_asyncmsgq_post(u->dq, NULL, HDI_RENDER, NULL, 0, &chunk, NULL);
    u->timestamp += pa_bytes_to_usec(chunk.length, &u->sink->sample_spec);
}

static void ThreadFuncUseTiming(void *userdata)
{
    struct Userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread (use timing) starting up");
    pa_thread_mq_install(&u->thread_mq);

    u->timestamp = pa_rtclock_now();

    while (true) {
        pa_usec_t now = 0;
        int ret;

        if (u->render_in_idle_state) {
            if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
                now = pa_rtclock_now();
            }
        } else {
            if (PA_SINK_IS_RUNNING(u->sink->thread_info.state)) {
                now = pa_rtclock_now();
            }
        }

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested)) {
            pa_sink_process_rewind(u->sink, 0);
        }

        // Render some data and drop it immediately
        if (u->render_in_idle_state && PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            if (u->timestamp <= now && pa_atomic_load(&u->dflag) == 0) {
                pa_atomic_add(&u->dflag, 1);
                ProcessRenderUseTiming(u, now);
            }

            pa_usec_t sleep_for_usec = pa_bytes_to_usec(u->sink->thread_info.max_request, &u->sink->sample_spec);
            pa_rtpoll_set_timer_relative(u->rtpoll, sleep_for_usec);
        } else if (!u->render_in_idle_state && PA_SINK_IS_RUNNING(u->sink->thread_info.state)) {
            if (u->timestamp <= now && pa_atomic_load(&u->dflag) == 0) {
                pa_atomic_add(&u->dflag, 1);
                ProcessRenderUseTiming(u, now);
            }

            pa_usec_t sleep_for_usec = pa_bytes_to_usec(u->sink->thread_info.max_request, &u->sink->sample_spec);
            pa_rtpoll_set_timer_relative(u->rtpoll, sleep_for_usec);
        } else {
            pa_rtpoll_set_timer_disabled(u->rtpoll);
        }
        // Hmm, nothing to do. Let's sleep
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0) {
            goto fail;
        }

        if (ret == 0) {
            goto finish;
        }
    }

fail:
    // If this was no regular exit from the loop we have to continue
    // processing messages until we received PA_MESSAGE_SHUTDOWN
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE,
                      u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    AUDIO_INFO_LOG("Thread (use timing) shutting down");
}

static void ThreadFuncWriteHDI(void *userdata)
{
    struct Userdata *u = userdata;
    pa_assert(u);

    int quit = 0;

    do {
        int code = 0;
        pa_memchunk chunk;

        pa_assert_se(pa_asyncmsgq_get(u->dq, NULL, &code, NULL, NULL, &chunk, 1) == 0);

        switch (code) {
            case HDI_RENDER:
                if (RenderWrite(u, &chunk) < 0) {
                    u->bytes_dropped += chunk.length;
                    AUDIO_ERR_LOG("RenderWrite failed");
                }
                if (pa_atomic_load(&u->dflag) == 1) {
                    pa_atomic_sub(&u->dflag, 1);
                }
                break;
            case QUIT:
                quit = 1;
                break;
            default:
                break;
        }
        pa_asyncmsgq_done(u->dq, 0);
    } while (!quit);
}
static void SinkUpdateRequestedLatencyCb(pa_sink *s)
{
    struct Userdata *u = NULL;
    size_t nbytes;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    u->block_usec = pa_sink_get_requested_latency_within_thread(s);

    if (u->block_usec == (pa_usec_t) - 1)
        u->block_usec = s->thread_info.max_latency;

    nbytes = pa_usec_to_bytes(u->block_usec, &s->sample_spec);
    pa_sink_set_max_request_within_thread(s, nbytes);
}

static int SinkProcessMsg(pa_msgobject *o, int code, void *data, int64_t offset,
                          pa_memchunk *chunk)
{
    struct Userdata *u = PA_SINK(o)->userdata;
    pa_assert(u);

    AUDIO_INFO_LOG("SinkProcessMsg: code: %{public}d", code);
    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY: {
            uint64_t latency;
            uint32_t hdiLatency;

            // Tries to fetch latency from HDI else will make an estimate based
            // on samples to be rendered based on the timestamp and current time
            if (u->sinkAdapter->RendererSinkGetLatency(&hdiLatency) == 0) {
                latency = (PA_USEC_PER_MSEC * hdiLatency);
            } else {
                pa_usec_t now = pa_rtclock_now();
                latency = (now - u->timestamp);
            }

            *((uint64_t *)data) = latency;
            return 0;
        }
        default:
            break;
    }
    return pa_sink_process_msg(o, code, data, offset, chunk);
}

// Called from the IO thread.
static int SinkSetStateInIoThreadCb(pa_sink *s, pa_sink_state_t newState,
                                    pa_suspend_cause_t newSuspendCause)
{
    struct Userdata *u = NULL;

    pa_assert(s);
    pa_assert_se(u = s->userdata);

    AUDIO_INFO_LOG("Sink old state: %{public}d, new state: %{public}d", s->thread_info.state, newState);
    if (!strcmp(GetDeviceClass(), DEVICE_CLASS_A2DP)) {
        if (s->thread_info.state == PA_SINK_IDLE && newState == PA_SINK_RUNNING) {
            u->sinkAdapter->RendererSinkResume();
        } else if (s->thread_info.state == PA_SINK_RUNNING && newState == PA_SINK_IDLE) {
            u->sinkAdapter->RendererSinkPause();
        }
    }

    if (s->thread_info.state == PA_SINK_SUSPENDED || s->thread_info.state == PA_SINK_INIT) {
        if (!PA_SINK_IS_OPENED(newState)) {
            return 0;
        }

        u->timestamp = pa_rtclock_now();
        if (u->isHDISinkStarted) {
            return 0;
        }

        AUDIO_INFO_LOG("Restarting HDI rendering device with rate:%d,channels:%d", u->ss.rate, u->ss.channels);
        if (u->sinkAdapter->RendererSinkStart()) {
            AUDIO_ERR_LOG("audiorenderer control start failed!");
            u->sinkAdapter->RendererSinkDeInit();
            pa_core_exit(u->core, true, 0);
        } else {
            u->isHDISinkStarted = true;
            AUDIO_INFO_LOG("Successfully restarted HDI renderer");
        }
    } else if (PA_SINK_IS_OPENED(s->thread_info.state)) {
        if (newState != PA_SINK_SUSPENDED) {
            return 0;
        }
        // Continuously dropping data (clear counter on entering suspended state.
        if (u->bytes_dropped != 0) {
            AUDIO_INFO_LOG("HDI-sink continuously dropping data - clear statistics (%zu -> 0 bytes dropped)",
                           u->bytes_dropped);
            u->bytes_dropped = 0;
        }

        if (u->isHDISinkStarted) {
            u->sinkAdapter->RendererSinkStop();
            AUDIO_DEBUG_LOG("Stopped HDI renderer");
            u->isHDISinkStarted = false;
        }
    }

    return 0;
}

static enum AudioFormat ConvertToHDIAudioFormat(pa_sample_format_t format)
{
    enum AudioFormat hdiAudioFormat;
    switch (format) {
        case PA_SAMPLE_U8:
            hdiAudioFormat = AUDIO_FORMAT_PCM_8_BIT;
            break;
        case PA_SAMPLE_S16LE:
            hdiAudioFormat = AUDIO_FORMAT_PCM_16_BIT;
            break;
        case PA_SAMPLE_S24LE:
            hdiAudioFormat = AUDIO_FORMAT_PCM_24_BIT;
            break;
        case PA_SAMPLE_S32LE:
            hdiAudioFormat = AUDIO_FORMAT_PCM_32_BIT;
            break;
        default:
            hdiAudioFormat = AUDIO_FORMAT_PCM_16_BIT;
            break;
    }

    return hdiAudioFormat;
}

static int32_t PrepareDevice(struct Userdata *u, const char* filePath)
{
    SinkAttr sample_attrs;
    int32_t ret;

    enum AudioFormat format = ConvertToHDIAudioFormat(u->ss.format);
    sample_attrs.format = format;
    sample_attrs.sampleFmt = format;
    AUDIO_DEBUG_LOG("audiorenderer format: %d", sample_attrs.format);
    sample_attrs.adapterName = u->adapterName;
    sample_attrs.open_mic_speaker = u->open_mic_speaker;
    sample_attrs.sampleRate = u->ss.rate;
    sample_attrs.channel = u->ss.channels;
    sample_attrs.volume = MAX_SINK_VOLUME_LEVEL;
    sample_attrs.filePath = filePath;

    ret = u->sinkAdapter->RendererSinkInit(&sample_attrs);
    if (ret != 0) {
        AUDIO_ERR_LOG("audiorenderer Init failed!");
        return -1;
    }

    ret = u->sinkAdapter->RendererSinkStart();
    if (ret != 0) {
        AUDIO_ERR_LOG("audiorenderer control start failed!");
        u->sinkAdapter->RendererSinkDeInit();
        return -1;
    }

    return 0;
}

static pa_sink* PaHdiSinkInit(struct Userdata *u, pa_modargs *ma, const char *driver)
{
    pa_sink_new_data data;
    pa_module *m;
    pa_sink *sink = NULL;

    m = u->module;
    u->ss = m->core->default_sample_spec;
    u->map = m->core->default_channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &u->ss, &u->map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        AUDIO_ERR_LOG("Failed to parse sample specification and channel map");
        goto fail;
    }

    AUDIO_INFO_LOG("Initializing HDI rendering device with rate: %d, channels: %d", u->ss.rate, u->ss.channels);
    if (PrepareDevice(u, pa_modargs_get_value(ma, "file_path", "")) < 0)
        goto fail;

    u->isHDISinkStarted = true;
    AUDIO_DEBUG_LOG("Initialization of HDI rendering device completed");
    pa_sink_new_data_init(&data);
    data.driver = driver;
    data.module = m;

    pa_sink_new_data_set_name(&data, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
    pa_sink_new_data_set_sample_spec(&data, &u->ss);
    pa_sink_new_data_set_channel_map(&data, &u->map);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, DEFAULT_AUDIO_DEVICE_NAME);
    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "HDI sink is %s",
                     DEFAULT_AUDIO_DEVICE_NAME);

    if (pa_modargs_get_proplist(ma, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        AUDIO_ERR_LOG("Invalid properties");
        pa_sink_new_data_done(&data);
        goto fail;
    }

    sink = pa_sink_new(m->core, &data,
                       PA_SINK_HARDWARE | PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY);
    pa_sink_new_data_done(&data);

    return sink;

fail:
    return NULL;
}

pa_sink *PaHdiSinkNew(pa_module *m, pa_modargs *ma, const char *driver)
{
    struct Userdata *u = NULL;
    char *paThreadName = NULL;
    char *hdiThreadName = NULL;

    pa_assert(m);
    pa_assert(ma);

    u = pa_xnew0(struct Userdata, 1);
    pa_assert(u);
    u->core = m->core;
    u->module = m;

    pa_memchunk_reset(&u->memchunk);
    u->rtpoll = pa_rtpoll_new();

    if (pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll) < 0) {
        AUDIO_ERR_LOG("pa_thread_mq_init() failed.");
        goto fail;
    }

    AUDIO_DEBUG_LOG("Load sink adapter");
    int32_t ret = LoadSinkAdapter(pa_modargs_get_value(ma, "device_class", DEFAULT_DEVICE_CLASS), &u->sinkAdapter);
    if (ret) {
        AUDIO_ERR_LOG("Load adapter failed");
        goto fail;
    }
    if (pa_modargs_get_value_u32(ma, "fixed_latency", &u->fixed_latency) < 0) {
        AUDIO_ERR_LOG("Failed to parse fixed latency argument.");
        goto fail;
    }

    u->adapterName = pa_modargs_get_value(ma, "adapter_name", DEFAULT_DEVICE_CLASS);

    if (pa_modargs_get_value_u32(ma, "render_in_idle_state", &u->render_in_idle_state) < 0) {
        AUDIO_ERR_LOG("Failed to parse render_in_idle_state  argument.");
        goto fail;
    }

    if (pa_modargs_get_value_u32(ma, "open_mic_speaker", &u->open_mic_speaker) < 0) {
        AUDIO_ERR_LOG("Failed to parse open_mic_speaker argument.");
        goto fail;
    }

    pa_atomic_store(&u->dflag, 0);
    u->dq = pa_asyncmsgq_new(0);

    u->sink = PaHdiSinkInit(u, ma, driver);
    if (!u->sink) {
        AUDIO_ERR_LOG("Failed to create sink object");
        goto fail;
    }

    u->sink->parent.process_msg = SinkProcessMsg;
    u->sink->set_state_in_io_thread = SinkSetStateInIoThreadCb;
    if (!u->fixed_latency) {
        u->sink->update_requested_latency = SinkUpdateRequestedLatencyCb;
    }
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    u->bytes_dropped = 0;
    u->buffer_size = DEFAULT_BUFFER_SIZE;
    if (pa_modargs_get_value_u32(ma, "buffer_size", &u->buffer_size) < 0) {
        AUDIO_ERR_LOG("Failed to parse buffer_size argument.");
        goto fail;
    }

    u->block_usec = pa_bytes_to_usec(u->buffer_size, &u->sink->sample_spec);

    if (u->fixed_latency) {
        pa_sink_set_fixed_latency(u->sink, u->block_usec);
    } else {
        pa_sink_set_latency_range(u->sink, 0, u->block_usec);
    }

    pa_sink_set_max_request(u->sink, u->buffer_size);

    paThreadName = "write-pa";
    if (!(u->thread = pa_thread_new(paThreadName, ThreadFuncUseTiming, u))) {
        AUDIO_ERR_LOG("Failed to write-pa thread.");
        goto fail;
    }

    hdiThreadName = "write-hdi";
    if (!(u->thread_hdi = pa_thread_new(hdiThreadName, ThreadFuncWriteHDI, u))) {
        AUDIO_ERR_LOG("Failed to write-hdi thread.");
        goto fail;
    }

    pa_sink_put(u->sink);

    return u->sink;
fail:
    UserdataFree(u);

    return NULL;
}

static void UserdataFree(struct Userdata *u)
{
    pa_assert(u);

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    if (u->thread_hdi) {
        pa_asyncmsgq_post(u->dq, NULL, QUIT, NULL, 0, NULL, NULL);
        pa_thread_free(u->thread_hdi);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->sinkAdapter) {
        u->sinkAdapter->RendererSinkStop();
        u->sinkAdapter->RendererSinkDeInit();
        UnLoadSinkAdapter(u->sinkAdapter);
    }

    pa_xfree(u);
}

void PaHdiSinkFree(pa_sink *s)
{
    struct Userdata *u = NULL;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    UserdataFree(u);
}
