#include "pcm.h"
#include "audio.h"
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/vmalloc.h>

#define T2AUDIO_PLAYBACK_TICK_NS NSEC_PER_MSEC

static u64 t2audio_get_alsa_fmtbit(struct t2audio_apple_description *desc)
{
    if (desc->format_flags & T2AUDIO_FORMAT_FLAG_FLOAT) {
        if (desc->bits_per_channel == 32) {
            if (desc->format_flags & T2AUDIO_FORMAT_FLAG_BIG_ENDIAN)
                return SNDRV_PCM_FMTBIT_FLOAT_BE;
            else
                return SNDRV_PCM_FMTBIT_FLOAT_LE;
        } else if (desc->bits_per_channel == 64) {
            if (desc->format_flags & T2AUDIO_FORMAT_FLAG_BIG_ENDIAN)
                return SNDRV_PCM_FMTBIT_FLOAT64_BE;
            else
                return SNDRV_PCM_FMTBIT_FLOAT64_LE;
        } else {
            pr_err("t2bce_audio: unsupported bits per channel for float format: %u\n", desc->bits_per_channel);
            return 0;
        }
    }
#define DEFINE_BPC_OPTION(val, b) \
    case val: \
        if (desc->format_flags & T2AUDIO_FORMAT_FLAG_BIG_ENDIAN) { \
            if (desc->format_flags & T2AUDIO_FORMAT_FLAG_SIGNED) \
                return SNDRV_PCM_FMTBIT_S ## b ## BE; \
            else \
                return SNDRV_PCM_FMTBIT_U ## b ## BE; \
        } else { \
            if (desc->format_flags & T2AUDIO_FORMAT_FLAG_SIGNED) \
                return SNDRV_PCM_FMTBIT_S ## b ## LE; \
            else \
                return SNDRV_PCM_FMTBIT_U ## b ## LE; \
        }
    if (desc->format_flags & T2AUDIO_FORMAT_FLAG_PACKED) {
        switch (desc->bits_per_channel) {
            case 8:
            case 16:
            case 32:
                break;
            DEFINE_BPC_OPTION(24, 24_3)
            default:
                pr_err("t2bce_audio: unsupported bits per channel for packed format: %u\n", desc->bits_per_channel);
                return 0;
        }
    }
    if (desc->format_flags & T2AUDIO_FORMAT_FLAG_ALIGNED_HIGH) {
        switch (desc->bits_per_channel) {
            DEFINE_BPC_OPTION(24, 32_)
            default:
                pr_err("t2bce_audio: unsupported bits per channel for high-aligned format: %u\n", desc->bits_per_channel);
                return 0;
        }
    }
    switch (desc->bits_per_channel) {
        case 8:
            if (desc->format_flags & T2AUDIO_FORMAT_FLAG_SIGNED)
                return SNDRV_PCM_FMTBIT_S8;
            else
                return SNDRV_PCM_FMTBIT_U8;
        DEFINE_BPC_OPTION(16, 16_)
        DEFINE_BPC_OPTION(24, 24_)
        DEFINE_BPC_OPTION(32, 32_)
        default:
            pr_err("t2bce_audio: unsupported bits per channel: %u\n", desc->bits_per_channel);
            return 0;
    }
}
int t2audio_create_hw_info(struct t2audio_apple_description *desc, struct snd_pcm_hardware *alsa_hw,
        size_t buf_size)
{
    uint rate;
    alsa_hw->info = (SNDRV_PCM_INFO_MMAP |
                     SNDRV_PCM_INFO_BLOCK_TRANSFER |
                     SNDRV_PCM_INFO_MMAP_VALID |
                     SNDRV_PCM_INFO_NO_PERIOD_WAKEUP |
                     SNDRV_PCM_INFO_DOUBLE);
    if (desc->format_flags & T2AUDIO_FORMAT_FLAG_NON_MIXABLE)
        pr_warn("t2bce_audio: unsupported hw flag: NON_MIXABLE\n");
    if (!(desc->format_flags & T2AUDIO_FORMAT_FLAG_NON_INTERLEAVED))
        alsa_hw->info |= SNDRV_PCM_INFO_INTERLEAVED;
    alsa_hw->formats = t2audio_get_alsa_fmtbit(desc);
    if (!alsa_hw->formats)
        return -EINVAL;
    rate = (uint) t2audio_double_to_u64(desc->sample_rate_double);
    alsa_hw->rates = snd_pcm_rate_to_rate_bit(rate);
    alsa_hw->rate_min = rate;
    alsa_hw->rate_max = rate;
    alsa_hw->channels_min = desc->channels_per_frame;
    alsa_hw->channels_max = desc->channels_per_frame;
    alsa_hw->buffer_bytes_max = buf_size;
    alsa_hw->period_bytes_min = desc->bytes_per_packet;
    alsa_hw->period_bytes_max = desc->bytes_per_packet;
    alsa_hw->periods_min = (uint) (buf_size / desc->bytes_per_packet);
    alsa_hw->periods_max = (uint) (buf_size / desc->bytes_per_packet);
    pr_debug("t2audio_create_hw_info: format = %llu, rate = %u/%u. channels = %u, periods = %u, period size = %lu\n",
            alsa_hw->formats, alsa_hw->rate_min, alsa_hw->rates, alsa_hw->channels_min, alsa_hw->periods_min,
            alsa_hw->period_bytes_min);
    return 0;
}

static struct t2audio_stream *t2audio_pcm_stream(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
        return &sdev->out_streams[substream->number];
    else
        return &sdev->in_streams[substream->number];
}

static struct t2audio_dma_buf *t2audio_pcm_dma_buf(struct t2audio_stream *stream)
{
    if (!stream->buffer_cnt || !stream->buffers)
        return NULL;

    return &stream->buffers[0];
}

static void t2audio_dma_memset(struct t2audio_dma_buf *buf, size_t offset, int value, size_t size)
{
    if (!buf || offset >= buf->size)
        return;

    size = min(size, buf->size - offset);
    switch (buf->type) {
        case T2AUDIO_DMA_BUF_IOMEM:
            memset_io((u8 __iomem *) buf->ptr + offset, value, size);
            break;
        case T2AUDIO_DMA_BUF_COHERENT:
            memset((u8 *) buf->ptr + offset, value, size);
            break;
    }
}

static void t2audio_dma_copy_to(struct t2audio_dma_buf *buf, size_t offset, const void *src, size_t size)
{
    if (!buf || offset >= buf->size)
        return;

    size = min(size, buf->size - offset);
    switch (buf->type) {
        case T2AUDIO_DMA_BUF_IOMEM:
            memcpy_toio((u8 __iomem *) buf->ptr + offset, src, size);
            break;
        case T2AUDIO_DMA_BUF_COHERENT:
            memcpy((u8 *) buf->ptr + offset, src, size);
            break;
    }
}

static void t2audio_playback_copy(struct snd_pcm_substream *substream,
        snd_pcm_uframes_t frames)
{
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    struct t2audio_dma_buf *bridge = t2audio_pcm_dma_buf(stream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    snd_pcm_uframes_t host_pos;

    if (!bridge || !runtime->buffer_size || !frames)
        return;

    host_pos = stream->playback_frames % runtime->buffer_size;
    while (frames) {
        snd_pcm_uframes_t chunk = frames;
        size_t src_offset;
        size_t dst_offset;

        chunk = min(chunk, runtime->buffer_size - host_pos);
        chunk = min(chunk, runtime->buffer_size - stream->bridge_pos);
        src_offset = frames_to_bytes(runtime, host_pos);
        dst_offset = frames_to_bytes(runtime, stream->bridge_pos);
        t2audio_dma_copy_to(bridge, dst_offset,
                (u8 *)stream->playback_area + src_offset,
                frames_to_bytes(runtime, chunk));

        host_pos = (host_pos + chunk) % runtime->buffer_size;
        stream->bridge_pos = (stream->bridge_pos + chunk) % runtime->buffer_size;
        stream->playback_frames += chunk;
        stream->period_pos += chunk;
        frames -= chunk;
    }
}

static enum hrtimer_restart t2audio_playback_timer(struct hrtimer *timer)
{
    struct t2audio_stream *stream = container_of(timer, struct t2audio_stream,
            playback_timer);
    struct snd_pcm_substream *substream = stream->playback_substream;
    struct snd_pcm_runtime *runtime;
    unsigned long flags;
    ktime_t now;
    s64 elapsed_ns;
    u64 scaled;
    snd_pcm_uframes_t frames;
    unsigned int periods = 0;

    if (!substream || !smp_load_acquire(&stream->started))
        return HRTIMER_NORESTART;

    runtime = substream->runtime;
    now = ktime_get();

    spin_lock_irqsave(&stream->playback_lock, flags);
    elapsed_ns = ktime_to_ns(ktime_sub(now, stream->playback_last));
    if (elapsed_ns > 0 && runtime->rate && runtime->buffer_size) {
        scaled = (u64)elapsed_ns * runtime->rate + stream->playback_remainder;
        frames = div64_u64_rem(scaled, NSEC_PER_SEC,
                &stream->playback_remainder);
        stream->playback_last = now;

        if (frames) {
            t2audio_playback_copy(substream, frames);
            if (runtime->period_size) {
                periods = stream->period_pos / runtime->period_size;
                stream->period_pos %= runtime->period_size;
            }
        }
    }
    spin_unlock_irqrestore(&stream->playback_lock, flags);

    if (!runtime->no_period_wakeup)
        while (periods--)
            snd_pcm_period_elapsed(substream);

    if (!smp_load_acquire(&stream->started))
        return HRTIMER_NORESTART;
    hrtimer_forward_now(timer, ns_to_ktime(T2AUDIO_PLAYBACK_TICK_NS));
    return HRTIMER_RESTART;
}

void t2audio_pcm_quiesce_stream(struct t2audio_stream *stream)
{
    smp_store_release(&stream->started, 0);
    if (stream->playback_timer_initialized)
        hrtimer_cancel(&stream->playback_timer);
}

static int t2audio_pcm_open(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);

    pr_debug("t2bce_audio: pcm open dev=%s direction=%s substream=%u\n",
            sdev->uid,
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
            substream->number);
    substream->runtime->hw = *stream->alsa_hw_desc;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        spin_lock_init(&stream->playback_lock);
        hrtimer_setup(&stream->playback_timer, t2audio_playback_timer,
                CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
        stream->playback_timer_initialized = true;
        stream->playback_substream = substream;
    }

    return 0;
}

static int t2audio_pcm_close(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        t2audio_pcm_quiesce_stream(stream);
        stream->playback_substream = NULL;
    }

    pr_debug("t2bce_audio: pcm close dev=%s direction=%s substream=%u\n",
            sdev->uid,
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
            substream->number);
    return 0;
}

static int t2audio_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);

    stream->waiting_for_first_ts = true;
    stream->remote_timestamp = 0;
    stream->timestamp_accept_after = 0;
    stream->frame_min = stream->latency;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        struct t2audio_dma_buf *bridge = t2audio_pcm_dma_buf(stream);

        if (stream->playback_area && stream->playback_bytes)
            memset(stream->playback_area, 0, stream->playback_bytes);
        if (bridge)
            t2audio_dma_memset(bridge, 0, 0, bridge->size);
    }

    return 0;
}

static int t2audio_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *astream = t2audio_pcm_stream(substream);

    pr_debug("t2bce_audio: pcm hw_params dev=%s direction=%s substream=%u\n",
            sdev->uid,
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
            substream->number);

    if (!astream->buffer_cnt || !astream->buffers)
        return -EINVAL;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        size_t bytes = params_buffer_bytes(hw_params);
        void *area;

        vfree(astream->playback_area);
        area = vmalloc_user(bytes);
        if (!area) {
            astream->playback_area = NULL;
            astream->playback_bytes = 0;
            return -ENOMEM;
        }
        astream->playback_area = area;
        astream->playback_bytes = bytes;
        substream->runtime->dma_area = area;
        substream->runtime->dma_addr = 0;
        substream->runtime->dma_bytes = bytes;
    } else {
        substream->runtime->dma_area = astream->buffers[0].ptr;
        substream->runtime->dma_addr = astream->buffers[0].dma_addr;
        substream->runtime->dma_bytes = astream->buffers[0].size;
    }
    return 0;
}

static int t2audio_pcm_hw_free(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        t2audio_pcm_quiesce_stream(stream);
        vfree(stream->playback_area);
        stream->playback_area = NULL;
        stream->playback_bytes = 0;
        substream->runtime->dma_area = NULL;
        substream->runtime->dma_bytes = 0;
    }

    pr_debug("t2bce_audio: pcm hw_free dev=%s direction=%s substream=%u\n",
            sdev->uid,
            substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture",
            substream->number);
    return 0;
}

static int t2audio_pcm_start(struct snd_pcm_substream *substream)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    ktime_t time_start, time_end;
    int status;

    time_start = ktime_get();
    smp_store_release(&stream->started, 0);

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        struct t2audio_dma_buf *bridge = t2audio_pcm_dma_buf(stream);

        if (!stream->playback_timer_initialized || !stream->playback_area ||
                !substream->runtime->buffer_size || !bridge ||
                bridge->size < frames_to_bytes(substream->runtime,
                                               substream->runtime->buffer_size))
            return -EINVAL;
    }

    status = t2audio_cmd_start_io(sdev->a, sdev->dev_id);
    if (status)
        return status;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        unsigned long flags;
        snd_pcm_uframes_t lead_frames;

        spin_lock_irqsave(&stream->playback_lock, flags);
        stream->playback_frames = 0;
        stream->bridge_pos = 0;
        stream->period_pos = 0;
        stream->playback_remainder = 0;
        stream->playback_last = ktime_get();
        lead_frames = min_t(snd_pcm_uframes_t, stream->latency,
                            substream->runtime->buffer_size - 1);
        t2audio_playback_copy(substream, lead_frames);
        spin_unlock_irqrestore(&stream->playback_lock, flags);
        smp_store_release(&stream->started, 1);
        hrtimer_start(&stream->playback_timer,
                ns_to_ktime(T2AUDIO_PLAYBACK_TICK_NS),
                HRTIMER_MODE_REL_PINNED);
    } else {
        smp_store_release(&stream->started, 1);
    }

    stream->remote_timestamp = 0;
    stream->waiting_for_first_ts = true;
    stream->timestamp_accept_after = ktime_get_boottime();
    stream->frame_min = stream->latency;

    time_end = ktime_get();
    pr_debug("t2bce_audio: start_io %s %lld us\n",
            sdev->uid, ktime_to_us(ktime_sub(time_end, time_start)));
    return 0;
}

static int t2audio_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    int err;

    /* bridgeOS exposes one ALSA substream per remote stream. */
    if (substream->number != 0)
        return 0;
    switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
            pr_debug("t2bce_audio: TRIGGER START %s\n", sdev->uid);
            err = t2audio_pcm_start(substream);
            if (err)
                return err;
            break;
        case SNDRV_PCM_TRIGGER_STOP:
            pr_debug("t2bce_audio: TRIGGER STOP %s\n", sdev->uid);
            t2audio_pcm_quiesce_stream(stream);
            err = t2audio_cmd_stop_io(sdev->a, sdev->dev_id);
            stream->remote_timestamp = 0;
            stream->waiting_for_first_ts = true;
            stream->timestamp_accept_after = 0;
            if (err)
                return err;
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

static snd_pcm_uframes_t t2audio_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    ktime_t time_from_start;
    snd_pcm_sframes_t frames;
    snd_pcm_sframes_t buffer_time_length;

    if (!smp_load_acquire(&stream->started))
        return 0;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        unsigned long flags;

        spin_lock_irqsave(&stream->playback_lock, flags);
        frames = stream->playback_frames % substream->runtime->buffer_size;
        spin_unlock_irqrestore(&stream->playback_lock, flags);
        return frames;
    }

    if (stream->waiting_for_first_ts)
        return 0;

    /* Advance continuously in the host time domain from the first timestamp. */
    time_from_start = ktime_get_boottime() - stream->remote_timestamp;
    if (ktime_to_ns(time_from_start) < 0)
        return 0;
    buffer_time_length = NSEC_PER_SEC * substream->runtime->buffer_size / substream->runtime->rate;
    frames = (ktime_to_ns(time_from_start) % buffer_time_length) * substream->runtime->buffer_size / buffer_time_length;
    if (ktime_to_ns(time_from_start) < buffer_time_length) {
        if (frames < stream->frame_min)
            frames = stream->frame_min;
        else
            stream->frame_min = 0;
    } else {
        if (ktime_to_ns(time_from_start) < 2 * buffer_time_length)
            stream->frame_min = frames;
        else
            stream->frame_min = 0;
    }
    frames -= stream->latency;
    if (frames < 0)
        frames += ((-frames - 1) / substream->runtime->buffer_size + 1) * substream->runtime->buffer_size;
    frames %= substream->runtime->buffer_size;
    return (snd_pcm_uframes_t) frames;
}

static int t2audio_pcm_mmap(struct snd_pcm_substream *substream, struct vm_area_struct *area)
{
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);
    struct t2audio_stream *stream = t2audio_pcm_stream(substream);
    struct t2audio_dma_buf *buf;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        if (!stream->playback_area)
            return -EINVAL;
        return remap_vmalloc_range(area, stream->playback_area, 0);
    }

    if (!stream->buffer_cnt || !stream->buffers)
        return -EINVAL;

    buf = &stream->buffers[0];
    switch (buf->type) {
        case T2AUDIO_DMA_BUF_IOMEM:
            return snd_pcm_lib_mmap_iomem(substream, area);
        case T2AUDIO_DMA_BUF_COHERENT:
            return dma_mmap_coherent(sdev->a->dev, area, buf->ptr, buf->dma_addr, buf->size);
        default:
            return -EINVAL;
    }
}

static struct snd_pcm_ops t2audio_pcm_ops = {
        .open =        t2audio_pcm_open,
        .close =       t2audio_pcm_close,
        .ioctl =       snd_pcm_lib_ioctl,
        .hw_params =   t2audio_pcm_hw_params,
        .hw_free =     t2audio_pcm_hw_free,
        .prepare =     t2audio_pcm_prepare,
        .trigger =     t2audio_pcm_trigger,
        .pointer =     t2audio_pcm_pointer,
        .mmap    =     t2audio_pcm_mmap
};

int t2audio_create_pcm(struct t2audio_subdevice *sdev)
{
    struct snd_pcm *pcm;
    struct t2audio_alsa_pcm_id_mapping *id_mapping;
    int err;

    if (!sdev->is_pcm || (sdev->in_stream_cnt == 0 && sdev->out_stream_cnt == 0)) {
        return -EINVAL;
    }

    for (id_mapping = t2audio_alsa_id_mappings; id_mapping->name; id_mapping++) {
        if (!strcmp(sdev->uid, id_mapping->name)) {
            sdev->alsa_id = id_mapping->alsa_id;
            break;
        }
    }
    if (!id_mapping->name)
        sdev->alsa_id = sdev->a->next_alsa_id++;
    err = snd_pcm_new(sdev->a->card, sdev->uid, sdev->alsa_id,
            (int) sdev->out_stream_cnt, (int) sdev->in_stream_cnt, &pcm);
    if (err < 0)
        return err;
    pcm->private_data = sdev;
    pcm->nonatomic = 1;
    sdev->pcm = pcm;
    strcpy(pcm->name, sdev->uid);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &t2audio_pcm_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &t2audio_pcm_ops);
    return 0;
}

static void t2audio_handle_stream_timestamp(struct snd_pcm_substream *substream,
                                            ktime_t host_timestamp)
{
    unsigned long flags;
    struct t2audio_stream *stream;
    struct t2audio_subdevice *sdev = snd_pcm_substream_chip(substream);

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
        return;

    stream = t2audio_pcm_stream(substream);
    snd_pcm_stream_lock_irqsave(substream, flags);
    if (!smp_load_acquire(&stream->started)) {
        snd_pcm_stream_unlock_irqrestore(substream, flags);
        return;
    }
    if (ktime_before(host_timestamp, stream->timestamp_accept_after)) {
        pr_debug("t2bce_audio: ignoring pre-start timestamp dev=%s event=%lld accept_after=%lld\n",
                sdev->uid, ktime_to_ns(host_timestamp),
                ktime_to_ns(stream->timestamp_accept_after));
        snd_pcm_stream_unlock_irqrestore(substream, flags);
        return;
    }
    if (stream->waiting_for_first_ts) {
        stream->remote_timestamp = host_timestamp;
        stream->waiting_for_first_ts = false;
        snd_pcm_stream_unlock_irqrestore(substream, flags);
        return;
    }
    snd_pcm_stream_unlock_irqrestore(substream, flags);
    if (!substream->runtime->no_period_wakeup)
        snd_pcm_period_elapsed(substream);
}

void t2audio_handle_timestamp(struct t2audio_subdevice *sdev,
        ktime_t host_timestamp)
{
    struct snd_pcm_substream *substream;

    substream = sdev->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
    if (substream)
        t2audio_handle_stream_timestamp(substream, host_timestamp);
    substream = sdev->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream;
    if (substream)
        t2audio_handle_stream_timestamp(substream, host_timestamp);
}
