/*
 * audio_playback.c - 用 NuttX 标准 audio buffer 管理播放 PCM
 * 替代 aw-alsa-lib（Allwinner 专用，P4 上编译不过）
 * 设备：/dev/audio/pcm0（pcm_decode -> ES8311）
 *
 * 播放链路（同步阻塞）：
 *   open  -> GETCAPS -> RESERVE -> CONFIGURE -> GETBUFFERINFO
 *         -> REGISTERMQ -> ALLOCBUFFER xN
 *   write : ENQUEUEBUFFER（首个 buffer 入队后 START）
 *   stop  : STOP（驱动会把在途 buffer 全部 dequeue 回来）
 *   close : 等播完 -> STOP -> FREEBUFFER -> UNREGISTERMQ -> RELEASE -> close
 */

#include <nuttx/config.h>
#include <nuttx/audio/audio.h>

#include "voice/audio_playback.h"
#include "agent_config.h"

#include <sys/ioctl.h>
#include <mqueue.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>

static const char *TAG = "audio_pb";

/* GETBUFFERINFO 失败时的兜底（CONFIG_AUDIO_DRIVER_SPECIFIC_BUFFERS 未开启时
 * ES8311 返回 -ENOTTY）。4096B @16kHz/16bit = 128ms 音频/块。 */
#define PB_FALLBACK_BUFFER_SIZE 4096
#define PB_FALLBACK_NBUFFERS    4

struct audio_playback
{
    int fd;
    mqd_t mq;
    char mqname[32];

    struct ap_buffer_s **bufs;       /* 全部 buffer 指针（ALLOCBUFFER 分配） */
    struct ap_buffer_s **free_stack; /* 空闲 buffer 栈（dequeue 回收） */
    int nbuffers;
    int buffer_size;
    int free_count;

    int started;
    volatile int stopped;
    size_t total_written;
};

/* 阻塞收 mq 消息，把 dequeue 回来的 buffer 压入 free_stack。
 * 返回 0：拿到一个空闲 buffer；<0：出错或已停止。 */
static int pb_wait_dequeue(audio_playback_t *pb)
{
    struct audio_msg_s msg;
    unsigned int prio;

    while (1)
    {
        if (pb->stopped)
            return -ECANCELED;

        ssize_t ret = mq_receive(pb->mq, (char *)&msg, sizeof(msg), &prio);
        if (ret != sizeof(msg))
        {
            if (errno == EINTR)
                continue;
            return -EIO;
        }

        switch (msg.msg_id)
        {
        case AUDIO_MSG_DEQUEUE:
            if (pb->free_count < pb->nbuffers)
                pb->free_stack[pb->free_count++] =
                    (struct ap_buffer_s *)msg.u.ptr;
            return 0;

        case AUDIO_MSG_STOP:
            pb->stopped = 1;
            return -ECANCELED;

        case AUDIO_MSG_IOERR:
            return -EIO;

        case AUDIO_MSG_COMPLETE:
        default:
            /* 其他消息继续等 */
            break;
        }
    }
}

audio_playback_t *audio_playback_open(const char *dev_path,
                                      unsigned int sample_rate,
                                      unsigned int channels,
                                      unsigned int bits_per_sample)
{
    struct audio_caps_desc_s cap_desc;
    struct audio_caps_s caps;
    struct ap_buffer_info_s buf_info;
    struct mq_attr attr;
    int ret;
    int i;

    if (bits_per_sample != 16)
    {
        syslog(LOG_ERR, "[%s] unsupported bits %u\n", TAG, bits_per_sample);
        return NULL;
    }

    audio_playback_t *pb = calloc(1, sizeof(*pb));
    if (!pb)
        return NULL;
    pb->fd = -1;
    pb->mq = (mqd_t)-1;

    pb->fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (pb->fd < 0)
    {
        syslog(LOG_ERR, "[%s] open %s failed: %d\n", TAG, dev_path, errno);
        goto fail;
    }

    /* GETCAPS：验证支持 OUTPUT + PCM */
    memset(&caps, 0, sizeof(caps));
    caps.ac_len = sizeof(caps);
    caps.ac_type = AUDIO_TYPE_QUERY;
    caps.ac_subtype = AUDIO_TYPE_QUERY;
    ret = ioctl(pb->fd, AUDIOIOC_GETCAPS, (unsigned long)&caps);
    if (ret != caps.ac_len)
    {
        syslog(LOG_ERR, "[%s] GETCAPS failed: %d\n", TAG, ret);
        goto fail;
    }
    if ((caps.ac_controls.b[0] & AUDIO_TYPE_OUTPUT) == 0 ||
        (caps.ac_format.hw & (1 << (AUDIO_FMT_PCM - 1))) == 0)
    {
        syslog(LOG_ERR, "[%s] device doesn't support PCM output\n", TAG);
        goto fail;
    }

    /* RESERVE */
    ret = ioctl(pb->fd, AUDIOIOC_RESERVE, 0);
    if (ret < 0)
    {
        syslog(LOG_ERR, "[%s] RESERVE failed: %d\n", TAG, ret);
        goto fail;
    }

    /* CONFIGURE：16bit PCM / 采样率 / 通道 */
    memset(&cap_desc, 0, sizeof(cap_desc));
    cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
    cap_desc.caps.ac_type = AUDIO_TYPE_OUTPUT;
    cap_desc.caps.ac_channels = channels;
    cap_desc.caps.ac_chmap = 0;
    cap_desc.caps.ac_controls.hw[0] = sample_rate;
    cap_desc.caps.ac_controls.b[3] = sample_rate >> 16;
    cap_desc.caps.ac_controls.b[2] = bits_per_sample;
    cap_desc.caps.ac_subtype = AUDIO_FMT_PCM;
    ret = ioctl(pb->fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc);
    if (ret < 0)
    {
        syslog(LOG_ERR,
               "[%s] CONFIGURE failed: ret=%d errno=%d (rate=%u ch=%u bits=%u)\n",
               TAG, ret, errno, sample_rate, channels, bits_per_sample);
        goto fail;
    }

    /* 设置音量（ES8311 默认静音，DAC_REG32=0，需显式设置音量才出声） */
    memset(&cap_desc, 0, sizeof(cap_desc));
    cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
    cap_desc.caps.ac_type = AUDIO_TYPE_FEATURE;
    cap_desc.caps.ac_format.hw = AUDIO_FU_VOLUME;
    cap_desc.caps.ac_controls.hw[0] = 800;
    ret = ioctl(pb->fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc);
    if (ret < 0)
    {
        syslog(LOG_WARNING, "[%s] set volume failed: %d\n", TAG, ret);
    }

    /* GETBUFFERINFO */
    ret = ioctl(pb->fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&buf_info);
    if (ret < 0 || buf_info.nbuffers < 2 || buf_info.buffer_size < 256)
    {
        buf_info.nbuffers = PB_FALLBACK_NBUFFERS;
        buf_info.buffer_size = PB_FALLBACK_BUFFER_SIZE;
    }
    pb->nbuffers = buf_info.nbuffers;
    pb->buffer_size = buf_info.buffer_size;

    /* 建 mq */
    attr.mq_maxmsg = pb->nbuffers + 8;
    attr.mq_msgsize = sizeof(struct audio_msg_s);
    attr.mq_curmsgs = 0;
    attr.mq_flags = 0;
    snprintf(pb->mqname, sizeof(pb->mqname), "/tmp/pb%p", (void *)pb);
    pb->mq = mq_open(pb->mqname, O_RDWR | O_CREAT, 0644, &attr);
    if (pb->mq == (mqd_t)-1)
    {
        syslog(LOG_ERR, "[%s] mq_open failed: %d\n", TAG, errno);
        goto fail;
    }

    /* REGISTERMQ */
    ret = ioctl(pb->fd, AUDIOIOC_REGISTERMQ, (unsigned long)pb->mq);
    if (ret < 0)
    {
        syslog(LOG_ERR, "[%s] REGISTERMQ failed: %d\n", TAG, ret);
        goto fail;
    }

    /* ALLOCBUFFER xN */
    pb->bufs = calloc(pb->nbuffers, sizeof(struct ap_buffer_s *));
    pb->free_stack = calloc(pb->nbuffers, sizeof(struct ap_buffer_s *));
    if (!pb->bufs || !pb->free_stack)
    {
        syslog(LOG_ERR, "[%s] calloc buffers failed\n", TAG);
        goto fail;
    }

    for (i = 0; i < pb->nbuffers; i++)
    {
        struct audio_buf_desc_s desc;
        memset(&desc, 0, sizeof(desc));
        desc.numbytes = pb->buffer_size;
        desc.u.pbuffer = &pb->bufs[i];
        ret = ioctl(pb->fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&desc);
        if (ret < 0)
        {
            syslog(LOG_ERR, "[%s] ALLOCBUFFER[%d] failed: %d\n",
                   TAG, i, ret);
            goto fail;
        }
        pb->free_stack[pb->free_count++] = pb->bufs[i];
    }

    syslog(LOG_INFO,
           "[%s] opened %s (%uHz %uch %ubit, %dx%d buffers)\n",
           TAG, dev_path, sample_rate, channels, bits_per_sample,
           pb->nbuffers, pb->buffer_size);
    return pb;

fail:
    audio_playback_close(pb);
    return NULL;
}

int audio_playback_write(audio_playback_t *pb, const void *buf, size_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    size_t remaining = len;
    size_t written = 0;

    if (!pb || pb->fd < 0 || !buf || len == 0)
        return -EINVAL;
    if (pb->stopped)
        return -ECANCELED;

    while (remaining > 0)
    {
        struct ap_buffer_s *apb;
        struct audio_buf_desc_s desc;
        size_t n;
        int ret;

        if (pb->free_count == 0)
        {
            ret = pb_wait_dequeue(pb);
            if (ret < 0)
                return written > 0 ? (int)written : ret;
        }

        apb = pb->free_stack[--pb->free_count];

        n = remaining < (size_t)apb->nmaxbytes
                ? remaining : (size_t)apb->nmaxbytes;
        memcpy(apb->samp, src, n);
        apb->nbytes = n;
        apb->curbyte = 0;
        apb->flags = 0;

        memset(&desc, 0, sizeof(desc));
        desc.numbytes = n;
        desc.u.buffer = apb;
        ret = ioctl(pb->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc);
        if (ret < 0)
        {
            pb->free_stack[pb->free_count++] = apb;
            syslog(LOG_ERR, "[%s] ENQUEUEBUFFER failed: %d\n", TAG, ret);
            return written > 0 ? (int)written : -EIO;
        }

        if (!pb->started)
        {
            ret = ioctl(pb->fd, AUDIOIOC_START, 0);
            if (ret < 0)
            {
                syslog(LOG_ERR, "[%s] START failed: %d\n", TAG, ret);
                return written > 0 ? (int)written : -EIO;
            }
            pb->started = 1;
        }

        src += n;
        remaining -= n;
        written += n;
    }

    pb->total_written += written;
    return (int)written;
}

void audio_playback_stop(audio_playback_t *pb)
{
    if (!pb)
        return;

    pb->stopped = 1;
    if (pb->fd >= 0 && pb->started)
    {
        ioctl(pb->fd, AUDIOIOC_STOP, 0);
    }
}

void audio_playback_close(audio_playback_t *pb)
{
    int i;

    if (!pb)
        return;

    /* 正常播完：等所有在途 buffer dequeue 回来（带超时保护） */
    if (pb->started && !pb->stopped && pb->mq != (mqd_t)-1)
    {
        int timeout_ms = 5000;
        while (pb->free_count < pb->nbuffers && timeout_ms > 0)
        {
            struct audio_msg_s msg;
            struct timespec ts;
            unsigned int prio;
            ssize_t ret;

            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100 * 1000 * 1000;
            if (ts.tv_nsec >= 1000 * 1000 * 1000)
            {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000 * 1000 * 1000;
            }

            ret = mq_timedreceive(pb->mq, (char *)&msg, sizeof(msg),
                                  &prio, &ts);
            if (ret == sizeof(msg) && msg.msg_id == AUDIO_MSG_DEQUEUE)
            {
                if (pb->free_count < pb->nbuffers)
                    pb->free_stack[pb->free_count++] =
                        (struct ap_buffer_s *)msg.u.ptr;
            }
            timeout_ms -= 100;
        }
    }

    /* STOP 兜底（确保驱动停、在途 buffer 回收） */
    if (pb->fd >= 0 && pb->started && !pb->stopped)
    {
        ioctl(pb->fd, AUDIOIOC_STOP, 0);
    }

    /* FREEBUFFER：用原始指针数组（不依赖 free_stack） */
    if (pb->fd >= 0 && pb->bufs)
    {
        for (i = 0; i < pb->nbuffers; i++)
        {
            if (pb->bufs[i])
            {
                struct audio_buf_desc_s desc;
                memset(&desc, 0, sizeof(desc));
                desc.u.buffer = pb->bufs[i];
                ioctl(pb->fd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
            }
        }
    }

    /* UNREGISTERMQ + RELEASE + close */
    if (pb->fd >= 0)
    {
        if (pb->mq != (mqd_t)-1)
            ioctl(pb->fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)pb->mq);
        ioctl(pb->fd, AUDIOIOC_RELEASE, 0);
        syslog(LOG_INFO, "[%s] closing (%zu bytes written)\n",
               TAG, pb->total_written);
        close(pb->fd);
    }

    if (pb->mq != (mqd_t)-1)
    {
        mq_close(pb->mq);
        mq_unlink(pb->mqname);
    }

    free(pb->free_stack);
    free(pb->bufs);
    free(pb);
}
