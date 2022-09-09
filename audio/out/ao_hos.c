/*
 * audio output driver for Horizon OS using audren
 * Copyright (C) 2021 averne <averne381@gmail.com>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <switch.h>

#include "config.h"
#include "common/common.h"
#include "common/msg.h"
#include "options/m_option.h"
#include "audio/format.h"
#include "ao.h"
#include "internal.h"

#define MAX_CHANS 6 // 5.1
#define MAX_BUF 16
#define MAX_SAMPLES 32768

struct priv {
    AudioDriver driver;
    int num_buffers;
    int num_samples;

    void *pool;
    AudioDriverWaveBuf *buffers;

    int cur_buf_idx;
    uint32_t cur_queued_samples, total_queued_samples;
};

static const AudioRendererConfig ar_config = {
    .output_rate     = AudioRendererOutputRate_48kHz,
    .num_voices      = MAX_CHANS,
    .num_effects     = 0,
    .num_sinks       = 1,
    .num_mix_objs    = 1,
    .num_mix_buffers = MAX_CHANS,
};

static const uint8_t sink_channel_ids[] = { 0, 1, 2, 3, 4, 5 };

static const struct mp_chmap possible_channel_layouts[] = {
    {0},
    MP_CHMAP_INIT_MONO,                 // mono
    MP_CHMAP_INIT_STEREO,               // stereo
    MP_CHMAP3(FL, FR, LFE),             // 2.1
    MP_CHMAP4(FL, FR, BL, BR),          // 4.0
    MP_CHMAP5(FL, FR, FC, BL, BR),      // 5.0
    MP_CHMAP6(FL, FR, FC, LFE, BL, BR), // 5.1
};

static int init(struct ao *ao) {
    struct priv *priv = ao->priv;

    Result rc;

    MP_VERBOSE(ao, "Initializing hos audio\n");

    ao->format   = AF_FORMAT_S16; // Only format supported by audrv with Adpcm which mpv can't output
    ao->channels = possible_channel_layouts[MPMIN(ao->channels.num, MAX_CHANS)];

    rc = audrenInitialize(&ar_config);
    if (R_FAILED(rc))
        return -rc;

    rc = audrvCreate(&priv->driver, &ar_config, MAX_CHANS);
    if (R_FAILED(rc))
        return -rc;

    size_t mempool_size = MP_ALIGN_UP(priv->num_samples * ao->channels.num *
        priv->num_buffers * sizeof(int16_t), AUDREN_MEMPOOL_ALIGNMENT);

    priv->pool = aligned_alloc(AUDREN_MEMPOOL_ALIGNMENT, mempool_size);
    if (!priv->pool)
        return -1;

    priv->buffers = malloc(sizeof(AudioDriverWaveBuf) * priv->num_buffers);
    for (int i = 0; i < priv->num_buffers; ++i) {
        priv->buffers[i] = (AudioDriverWaveBuf){
            .data_raw            = priv->pool,
            .size                = mempool_size,
            .start_sample_offset = priv->num_samples * i,
            .end_sample_offset   = priv->num_samples * (i + 1),
        };
    }

    int mpid = audrvMemPoolAdd(&priv->driver, priv->pool, mempool_size);
    audrvMemPoolAttach(&priv->driver, mpid);

    ao->device_buffer = priv->num_buffers * priv->num_samples;

    audrvDeviceSinkAdd(&priv->driver, AUDREN_DEFAULT_DEVICE_NAME, MAX_CHANS, sink_channel_ids);

    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc))
        return -rc;

    audrvVoiceInit(&priv->driver, 0, ao->channels.num, PcmFormat_Int16, ao->samplerate);
    audrvVoiceSetDestinationMix(&priv->driver, 0, AUDREN_FINAL_MIX_ID);

    for (int i = 0; i < ao->channels.num; ++i)
        audrvVoiceSetMixFactor(&priv->driver, 0, 1.0f, ao->channels.speaker[i], ao->channels.speaker[i]);

    return 0;
}

static void uninit(struct ao *ao) {
    struct priv *priv = ao->priv;

    MP_VERBOSE(ao, "Deinitializing hos audio\n");

    audrvVoiceStop(&priv->driver, 0);
    audrvUpdate(&priv->driver);

    audrvClose(&priv->driver);
    audrenExit();
    free(priv->buffers);
    free(priv->pool);
}

static void reset(struct ao *ao) {
    struct priv *priv = ao->priv;

    priv->cur_buf_idx = -1;
    priv->cur_queued_samples = priv->total_queued_samples = 0;
    audrvVoiceStop(&priv->driver, 0);
    audrvUpdate(&priv->driver);
}

static bool set_pause(struct ao *ao, bool paused) {
    struct priv *priv = ao->priv;

    audrvVoiceSetPaused(&priv->driver, 0, paused);
    return R_SUCCEEDED(audrvUpdate(&priv->driver));
}

static void start(struct ao *ao) {
    struct priv *priv = ao->priv;

    audrvVoiceStart(&priv->driver, 0);
    audrvUpdate(&priv->driver);
}

static int find_free_wavebuf(struct priv *priv) {
    for (int i = 0; i < priv->num_buffers; ++i) {
        AudioDriverWaveBuf *buf = &priv->buffers[i];
        if (buf->state == AudioDriverWaveBufState_Done ||
                buf->state == AudioDriverWaveBufState_Free)
            return i;
    }
    return -1;
}

static bool audio_write(struct ao *ao, void **data, int samples) {
    struct priv *priv = ao->priv;

    int idx = (priv->cur_buf_idx != -1) ? priv->cur_buf_idx : find_free_wavebuf(priv);
    if (idx == -1)
        return false;
    priv->cur_buf_idx = idx;

    AudioDriverWaveBuf *buf = &priv->buffers[idx];
    uint8_t *buf_offset = (uint8_t *)buf->data_raw + (idx * priv->num_samples * ao->sstride);

    size_t num_samples = MPMIN(samples, priv->num_samples - priv->cur_queued_samples);
    size_t size        = num_samples * ao->sstride;

    // We requested a linear PCM format so there is only one buffer
    memcpy(buf_offset + priv->cur_queued_samples * ao->sstride, data[0], size);
    priv->cur_queued_samples   += num_samples;
    priv->total_queued_samples += num_samples;

    if (priv->cur_queued_samples >= priv->num_samples) {
        // Append buffer once it's full
        armDCacheFlush(buf_offset, priv->num_samples * ao->sstride);
        audrvVoiceAddWaveBuf(&priv->driver, 0, buf);
        audrvUpdate(&priv->driver);

        priv->cur_buf_idx = -1, priv->cur_queued_samples = 0;

        // Write the rest of the data
        int remaining = samples - num_samples;
        if (remaining) {
            void *dat = (uint8_t *)(data[0]) + size;
            return audio_write(ao, &dat, remaining);
        }
    }

    return true;
}

static void get_state(struct ao *ao, struct mp_pcm_state *state) {
    struct priv *priv = ao->priv;

    Result rc = audrvUpdate(&priv->driver);
    if (R_FAILED(rc))
        return;

    state->free_samples = state->queued_samples = 0;
    for (int i = 0; i < priv->num_buffers; ++i) {
        AudioDriverWaveBuf *buf = &priv->buffers[i];
        if (buf->state == AudioDriverWaveBufState_Free
                || buf->state == AudioDriverWaveBufState_Done)
            state->free_samples += priv->num_samples;
    }

    if (priv->cur_buf_idx != -1)
        state->free_samples -= priv->num_samples - priv->cur_queued_samples;

    state->queued_samples = priv->total_queued_samples -
        audrvVoiceGetPlayedSampleCount(&priv->driver, 0);

    state->delay = (double)state->queued_samples / ao->samplerate;

    state->playing = audrvVoiceIsPlaying(&priv->driver, 0);
}

static int control(struct ao *ao, enum aocontrol cmd, void *arg) {
    struct priv *priv = ao->priv;

    int rc;

    switch (cmd) {
        case AOCONTROL_SET_MUTE:
        case AOCONTROL_SET_VOLUME: {
                float vol;
                if (cmd == AOCONTROL_SET_MUTE) {
                    bool in = *(bool *)arg;
                    vol = !in;
                } else {
                    ao_control_vol_t *in = (ao_control_vol_t *)arg;
                    vol = (in->left + in->right) / 200.0f;
                }

                audrvMixSetVolume(&priv->driver, 0, vol);
                rc = audrvUpdate(&priv->driver);
            }
            break;
        case AOCONTROL_GET_MUTE:
        case AOCONTROL_GET_VOLUME: {
                rc = audrvUpdate(&priv->driver);
                float vol = priv->driver.in_mixes[0].volume;
                if (cmd == AOCONTROL_GET_MUTE) {
                    bool *out = (bool *)arg;
                    *out = !vol;
                } else {
                    ao_control_vol_t *out = (ao_control_vol_t *)arg;
                    out->left = out->right = vol * 100.0f;
                }
            }
            break;
        default:
            return CONTROL_UNKNOWN;
    }

    return R_SUCCEEDED(rc) ? CONTROL_OK : CONTROL_ERROR;
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_hos = {
    .description     = "HOS Audio",
    .name            = "hos",
    .init            = init,
    .uninit          = uninit,
    .reset           = reset,
    .control         = control,
    .set_pause       = set_pause,
    .start           = start,
    .write           = audio_write,
    .get_state       = get_state,
    .priv_size       = sizeof(struct priv),
    .priv_defaults   = &(const struct priv){
        .num_buffers = 4,
        .num_samples = 8192,
    },
    .options         = (const struct m_option[]){
        {"num-buffers", OPT_INT(num_buffers), M_RANGE(2,   MAX_BUF)},
        {"num-samples", OPT_INT(num_samples), M_RANGE(256, MAX_SAMPLES)},
        {0}
    },
    .options_prefix   = "ao-hos",
};
