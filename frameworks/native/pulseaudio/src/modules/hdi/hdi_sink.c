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

struct Userdata {
    uint32_t buffer_size;
    uint32_t sinkStreamCount;
    size_t bytes_dropped;
    pa_thread_mq thread_mq;
    pa_memchunk memchunk;
    pa_usec_t block_usec;
    pa_usec_t timestamp;
    pa_thread *thread;
    pa_rtpoll *rtpoll;
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
    pa_sample_spec ss;
    pa_channel_map map;
    bool isHDISinkStarted;
    struct RendererSinkAdapter *sinkAdapter;
};

static void UserdataFree(struct Userdata *u);
static int32_t PrepareDevice(struct Userdata *u);

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

    return count;
}

static void ProcessRenderUseTiming(struct Userdata *u, pa_usec_t now)
{
    size_t dropped;
    size_t consumed = 0;

    pa_assert(u);

    // Fill the buffer up the latency size
    while (u->timestamp < now + u->block_usec) {
        ssize_t written = 0;
        pa_memchunk chunk;
        // Change from pa_sink_render to pa_sink_render_full for alignment issue in 3516
        pa_sink_render_full(u->sink, u->sink->thread_info.max_request, &chunk);

        pa_assert(chunk.length > 0);

        if ((written = RenderWrite(u, &chunk)) < 0) {
            pa_memblock_unref(chunk.memblock);
            break;
        }

        pa_memblock_unref(chunk.memblock);

        u->timestamp += pa_bytes_to_usec(written, &u->sink->sample_spec);

        dropped = chunk.length - written;

        if (u->bytes_dropped != 0 && dropped != chunk.length) {
            AUDIO_INFO_LOG("HDI-sink continuously dropped %zu bytes", u->bytes_dropped);
            u->bytes_dropped = 0;
        }

        if (u->bytes_dropped == 0 && dropped != 0) {
            AUDIO_INFO_LOG("HDI-sink just dropped %zu bytes", dropped);
        }

        u->bytes_dropped += dropped;

        consumed += written;

        if (consumed >= u->sink->thread_info.max_request)
            break;
    }
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

#ifdef PRODUCT_RK3568
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
#else
        if (PA_SINK_IS_RUNNING(u->sink->thread_info.state)) {
#endif
            now = pa_rtclock_now();
        }

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested)) {
            pa_sink_process_rewind(u->sink, 0);
        }

        // Render some data and drop it immediately
#ifdef PRODUCT_RK3568
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
#else
        if (PA_SINK_IS_RUNNING(u->sink->thread_info.state)) {
#endif
            if (u->timestamp <= now)
                ProcessRenderUseTiming(u, now);

            pa_rtpoll_set_timer_absolute(u->rtpoll, u->timestamp);
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

#ifdef PRODUCT_M40
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
#endif

// Called from IO context
static int SinkProcessMsg(pa_msgobject *o, int code, void *data, int64_t offset,
                          pa_memchunk *chunk)
{
    struct Userdata *u = PA_SINK(o)->userdata;
    pa_assert(u);
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
            AUDIO_INFO_LOG("Stopped HDI renderer");
            u->isHDISinkStarted = false;
        }
    }

    return 0;
}

static pa_hook_result_t SinkStreamDisconnectCb(pa_core *c, pa_sink_input *s, struct Userdata *u)
{
    pa_assert(u);
    pa_sink_input_assert_ref(s);

    if (!s->sink || !u->sink) {
        AUDIO_ERR_LOG("SinkStreamDisconnectCb error");
        return PA_HOOK_OK;
    }

    if (u->sink->index != s->sink->index) {
        return PA_HOOK_OK;
    }

    u->sinkStreamCount--;

    if (u->sinkStreamCount == 0) {
        pa_sink_suspend(s->sink, true, PA_SUSPEND_IDLE);
        pa_core_maybe_vacuum(s->core);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t SinkStreamConnectCb(pa_core *c, pa_sink_input *s, struct Userdata *u)
{
    pa_assert(u);
    pa_sink_input_assert_ref(s);

    if (!s->sink || !u->sink) {
        AUDIO_ERR_LOG("SinkStreamConnectCb error");
        return PA_HOOK_OK;
    }

    // Callback for non default sink can be ignored
    if (u->sink->index != s->sink->index) {
        return PA_HOOK_OK;
    }

    if (!PA_SINK_IS_OPENED(s->sink->state)) {
        u->sinkStreamCount = 0;
        pa_sink_suspend(s->sink, false, PA_SUSPEND_IDLE);
    }

    u->sinkStreamCount++;

    return PA_HOOK_OK;
}

static pa_hook_result_t SinkStreamMoveFinishCb(pa_core *c, pa_sink_input *s, struct Userdata *u)
{
    pa_assert(u);
    pa_sink_input_assert_ref(s);

    if (!s->sink || !u->sink) {
        AUDIO_ERR_LOG("SinkStreamMoveFinishCb error");
        return PA_HOOK_OK;
    }

    // Callback for non default sink can be ignored
    if (u->sink->index != s->sink->index) {
        return PA_HOOK_OK;
    }

    u->sinkStreamCount = pa_idxset_size(s->sink->inputs);
    if (!PA_SINK_IS_OPENED(s->sink->state) && u->sinkStreamCount > 0) {
        pa_sink_suspend(s->sink, false, PA_SUSPEND_IDLE);
    }

    return PA_HOOK_OK;
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

static int32_t PrepareDevice(struct Userdata *u)
{
    SinkAttr sample_attrs;
    int32_t ret;

    enum AudioFormat format = ConvertToHDIAudioFormat(u->ss.format);
    sample_attrs.format = format;
    sample_attrs.sampleFmt = format;
    AUDIO_INFO_LOG("audiorenderer format: %d", sample_attrs.format);

    sample_attrs.sampleRate = u->ss.rate;
    sample_attrs.channel = u->ss.channels;
    sample_attrs.volume = MAX_SINK_VOLUME_LEVEL;

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
    if (PrepareDevice(u) < 0)
        goto fail;

    u->isHDISinkStarted = true;
    AUDIO_INFO_LOG("Initialization of HDI rendering device completed");
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
    char *threadName = NULL;

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

    AUDIO_INFO_LOG("Load sink adapter");
    int32_t ret = LoadSinkAdapter(pa_modargs_get_value(ma, "device_class", DEFAULT_DEVICE_CLASS), &u->sinkAdapter);
    if (ret) {
        AUDIO_ERR_LOG("Load adapter failed");
        goto fail;
    }

    u->sink = PaHdiSinkInit(u, ma, driver);
    if (!u->sink) {
        AUDIO_ERR_LOG("Failed to create sink object");
        goto fail;
    }

    u->sink->parent.process_msg = SinkProcessMsg;
    u->sink->set_state_in_io_thread = SinkSetStateInIoThreadCb;
#ifdef PRODUCT_M40
    u->sink->update_requested_latency = SinkUpdateRequestedLatencyCb;
#endif
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
#ifdef PRODUCT_M40
    pa_sink_set_latency_range(u->sink, 0, u->block_usec);
#else
    pa_sink_set_fixed_latency(u->sink, u->block_usec);
#endif
    pa_sink_set_max_request(u->sink, u->buffer_size);

    threadName = "hdi-sink-playback";
    if (!(u->thread = pa_thread_new(threadName, ThreadFuncUseTiming, u))) {
        AUDIO_ERR_LOG("Failed to create thread.");
        goto fail;
    }
    pa_sink_put(u->sink);
    // Start modules in suspended state
    pa_sink_suspend(u->sink, true, PA_SUSPEND_IDLE);

    // register for sink callbacks
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_NORMAL,
        (pa_hook_cb_t) SinkStreamDisconnectCb, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_NORMAL,
        (pa_hook_cb_t) SinkStreamConnectCb, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_NORMAL,
        (pa_hook_cb_t) SinkStreamMoveFinishCb, u);

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
