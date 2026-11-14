/* SPDX-License-Identifier: MIT */
/* Isolated PCM export from stock libraries; media is inherited fd 4 only.
 * This program accepts no paths, URLs, output devices or codec names. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "audio_stock_wire.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>

#define CODECS "aac,aac_latm,alac,opus,vorbis,wmav1,wmav2,wmapro,wmalossless,ape,dsd_lsbf,dsd_msbf,dsd_lsbf_planar,dsd_msbf_planar,dst"
#define FORMATS "aac,mov,ogg,asf,ape,dsf,iff"
#ifndef STOCK_CONFIG_HASH
#define STOCK_CONFIG_HASH 0xff474fbfu
#endif
/* Results deliberately match audio_decode.h without linking the UI. */
enum { OK, END, FORMAT, IO, MEMORY, CANCELLED, TRUNCATED, ARGUMENT, DECODE };
typedef struct {
    int64_t cursor, bytes;
    AVFormatContext *format;
    AVIOContext *io;
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    unsigned rate, channels, bits, offset;
    int stream, draining, eof, vorbis_started, vorbis_single_page;
    uint64_t frames, decoded;
} decoder;

static uint32_t config_hash(const char *p){
    uint32_t h = 2166136261u;
    while(*p) h = (h ^ (unsigned char)*p++) * 16777619u;
    return h;
}
static int stock_matches(void){
    return avcodec_version() == AV_VERSION_INT(58,54,100) &&
        avformat_version() == AV_VERSION_INT(58,29,100) &&
        avutil_version() == AV_VERSION_INT(56,31,100) &&
        config_hash(avcodec_configuration()) == STOCK_CONFIG_HASH &&
        config_hash(avformat_configuration()) == STOCK_CONFIG_HASH &&
        config_hash(avutil_configuration()) == STOCK_CONFIG_HASH;
}
static int transfer(void *buf, size_t n, int writing){
    unsigned char *p = buf;
    while(n){
        ssize_t r = writing ? write(3,p,n) : read(3,p,n);
        if(r < 0 && errno == EINTR) continue;
        if(r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}
static int reply(const decoder *d, unsigned result, unsigned frames){
    unsigned char h[AS_HEADER] = {0};
    as_put32(h,AS_MAGIC); as_put32(h+4,result);
    as_put32(h+8,d->rate); as_put32(h+12,d->channels);
    as_put32(h+16,d->bits); as_put32(h+20,frames); as_put64(h+24,d->frames);
    return transfer(h,sizeof h,1);
}
static int read_media(void *ctx, uint8_t *buf, int size){
    decoder *d = ctx;
    if(size <= 0) return AVERROR(EINVAL);
    ssize_t n;
    do n = pread(4,buf,(size_t)size,(off_t)d->cursor); while(n < 0 && errno == EINTR);
    if(n < 0) return AVERROR(errno);
    d->cursor += n;
    return n ? (int)n : AVERROR_EOF;
}
static int64_t seek_media(void *ctx, int64_t offset, int whence){
    decoder *d = ctx;
    if(whence == AVSEEK_SIZE) return d->bytes;
    whence &= ~AVSEEK_FORCE;
    int64_t base;
    if(whence == SEEK_SET) base = 0;
    else if(whence == SEEK_CUR) base = d->cursor;
    else if(whence == SEEK_END) base = d->bytes;
    else return AVERROR(EINVAL);
    if(offset < -base || offset > d->bytes - base) return AVERROR(EINVAL);
    return d->cursor = base + offset;
}
static int deny_open(AVFormatContext *s, AVIOContext **pb, const char *url, int flags, AVDictionary **opts){
    (void)s; (void)pb; (void)url; (void)flags; (void)opts;
    return AVERROR(EACCES);
}
static int allowed(enum AVCodecID id){
    switch(id){
    case AV_CODEC_ID_AAC: case AV_CODEC_ID_AAC_LATM: case AV_CODEC_ID_ALAC:
    case AV_CODEC_ID_OPUS: case AV_CODEC_ID_VORBIS: case AV_CODEC_ID_WMAV1:
    case AV_CODEC_ID_WMAV2: case AV_CODEC_ID_WMAPRO: case AV_CODEC_ID_WMALOSSLESS:
    case AV_CODEC_ID_APE: case AV_CODEC_ID_DSD_LSBF: case AV_CODEC_ID_DSD_MSBF:
    case AV_CODEC_ID_DSD_LSBF_PLANAR: case AV_CODEC_ID_DSD_MSBF_PLANAR:
    case AV_CODEC_ID_DST: return 1;
    default: return 0;
    }
}
static void release(decoder *d){
    av_packet_free(&d->packet); av_frame_free(&d->frame);
    avcodec_free_context(&d->codec); avformat_close_input(&d->format);
    if(d->io){ av_freep(&d->io->buffer); avio_context_free(&d->io); }
    d->cursor = 0; d->offset = 0; d->decoded = 0; d->draining = d->eof = 0;
    d->vorbis_started = d->vorbis_single_page = 0;
}
static int next_frame(decoder *d);
static int prepare(decoder *d){
    AVDictionary *opts = NULL;
    d->format = avformat_alloc_context();
    unsigned char *buffer = av_malloc(32768);
    if(!d->format || !buffer){ av_free(buffer); return MEMORY; }
    d->io = avio_alloc_context(buffer,32768,0,d,read_media,NULL,seek_media);
    if(!d->io){ av_free(buffer); return MEMORY; }
    d->format->pb = d->io; d->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    d->format->io_open = deny_open;
    d->format->probesize = 1024*1024; d->format->max_analyze_duration = 5*AV_TIME_BASE;
    av_dict_set(&opts,"format_whitelist",FORMATS,0);
    av_dict_set(&opts,"codec_whitelist",CODECS,0);
    av_dict_set(&opts,"protocol_whitelist","",0);
    av_dict_set(&opts,"enable_drefs","0",0);
    int r = avformat_open_input(&d->format,NULL,NULL,&opts);
    av_dict_free(&opts);
    if(r < 0 || avformat_find_stream_info(d->format,NULL) < 0) return FORMAT;
    if(d->format->nb_streams > 16) return FORMAT;
    d->stream = -1;
    for(unsigned i=0;i<d->format->nb_streams;i++){
        AVCodecParameters *p = d->format->streams[i]->codecpar;
        if(p->codec_type == AVMEDIA_TYPE_AUDIO){ d->stream = (int)i; break; }
    }
    if(d->stream < 0) return FORMAT;
    AVCodecParameters *p = d->format->streams[d->stream]->codecpar;
    if(!allowed(p->codec_id)) return FORMAT;
    const AVCodec *c = avcodec_find_decoder(p->codec_id);
    if(!c) return FORMAT;
    d->codec = avcodec_alloc_context3(c);
    if(!d->codec) return MEMORY;
    if(avcodec_parameters_to_context(d->codec,p) < 0) return FORMAT;
    d->codec->thread_count = 1;
    d->codec->pkt_timebase = d->format->streams[d->stream]->time_base;
    d->codec->err_recognition = AV_EF_CRCCHECK | AV_EF_CAREFUL | AV_EF_EXPLODE;
    if(avcodec_open2(d->codec,c,NULL) < 0) return FORMAT;
    d->rate = (unsigned)d->codec->sample_rate; d->channels = (unsigned)d->codec->channels;
    d->bits = d->codec->bits_per_raw_sample > 0 ? (unsigned)d->codec->bits_per_raw_sample : 0;
    d->frames = UINT64_MAX;
    AVStream *stream = d->format->streams[d->stream];
    /* ASF duration is millisecond-rounded, not an exact lossless PCM count. */
    if((p->codec_id == AV_CODEC_ID_ALAC || p->codec_id == AV_CODEC_ID_APE) &&
       stream->duration > 0 && stream->duration != AV_NOPTS_VALUE){
        int64_t count = av_rescale_q(stream->duration,stream->time_base,(AVRational){1,(int)d->rate});
        if(count > 0) d->frames = (uint64_t)count;
    }
    if(d->rate < 8000 || d->rate > AS_RATE_MAX || d->channels < 1 || d->channels > 2 || d->bits > 32) return FORMAT;
    int dsd = p->codec_id == AV_CODEC_ID_DSD_LSBF || p->codec_id == AV_CODEC_ID_DSD_MSBF ||
        p->codec_id == AV_CODEC_ID_DSD_LSBF_PLANAR || p->codec_id == AV_CODEC_ID_DSD_MSBF_PLANAR || p->codec_id == AV_CODEC_ID_DST;
    if(!dsd && d->rate > 384000) return FORMAT;
    d->frame = av_frame_alloc(); d->packet = av_packet_alloc();
    if(!d->frame || !d->packet) return MEMORY;
    /* Establish the granule-bounded length before the initial IPC header.
     * Keep the first decoded frame for the first read (also after a seek). */
    return p->codec_id == AV_CODEC_ID_VORBIS ? next_frame(d) : OK;
}
static int next_frame(decoder *d){
    if(d->eof || (d->vorbis_started && d->decoded == d->frames)) return END;
    av_frame_unref(d->frame); d->offset = 0;
    for(;;){
        int r = avcodec_receive_frame(d->codec,d->frame);
        if(r == AVERROR_EOF){ d->eof = 1; return d->frames != UINT64_MAX && d->decoded < d->frames ? TRUNCATED : END; }
        if(r >= 0){
            AVFrame *f = d->frame;
            if(f->channels != (int)d->channels || f->sample_rate != (int)d->rate ||
               f->nb_samples < 1 || f->nb_samples > 1024*1024 ||
               f->flags & AV_FRAME_FLAG_CORRUPT || f->decode_error_flags) return DECODE;
            if(d->codec->codec_id == AV_CODEC_ID_VORBIS){
                AVStream *st = d->format->streams[d->stream];
                if(!d->vorbis_started){
                    unsigned char page[14];
                    if(f->pkt_pos < 0 || pread(4,page,sizeof page,f->pkt_pos) != (ssize_t)sizeof page ||
                       memcmp(page,"OggS\0",5)) return FORMAT;
                    d->vorbis_single_page = !!(page[5] & 4);
                    if(!d->vorbis_single_page && f->pts == AV_NOPTS_VALUE) return FORMAT;
                    /* On an EOS-only audio page stock assigns time to the
                     * priming packet, although it yields no PCM. Packet
                     * duration then cuts valid audio. Use the final granule
                     * and actual decoded origin instead (Vorbis I, A.2). */
                    int64_t origin = d->vorbis_single_page ? 0 : f->pts;
                    int64_t base = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;
                    if(base < 0 || st->duration == AV_NOPTS_VALUE || st->duration > INT64_MAX-base) return FORMAT;
                    /* The same priming timestamp can make stock report zero
                     * stream duration for a valid short EOS-only page. The
                     * demuxer-validated page carries its exact endpoint. */
                    int64_t end = d->vorbis_single_page ? (int64_t)AV_RL64(page+6) : st->duration + base;
                    if(origin > end) return FORMAT;
                    int64_t count = av_rescale_q(end-(origin > 0 ? origin : 0),st->time_base,(AVRational){1,(int)d->rate});
                    if(count <= 0) return FORMAT;
                    d->frames = (uint64_t)count; d->vorbis_started = 1;
                }
                /* An edited beginning may precede time zero. The priming
                 * packet itself is already suppressed by the stock decoder. */
                if(!d->vorbis_single_page && f->pts < 0){
                    if(f->pts == INT64_MIN) return FORMAT;
                    int64_t skip = av_rescale_q(-f->pts,st->time_base,(AVRational){1,(int)d->rate});
                    if(skip >= f->nb_samples){ av_frame_unref(f); continue; }
                    d->offset = (unsigned)skip;
                }
                uint64_t remaining = d->frames-d->decoded;
                if(remaining < (unsigned)f->nb_samples-d->offset) f->nb_samples = (int)(d->offset+remaining);
            }
            unsigned valid = (unsigned)f->nb_samples-d->offset;
            if(d->decoded > UINT64_MAX-valid) return DECODE;
            d->decoded += valid;
            return OK;
        }
        if(r != AVERROR(EAGAIN) || d->draining) return DECODE;
        do {
            r = av_read_frame(d->format,d->packet);
            if(r < 0) break;
            if(d->packet->stream_index == d->stream) break;
            av_packet_unref(d->packet);
        } while(1);
        if(r == AVERROR_EOF){
            d->draining = 1;
            if(avcodec_send_packet(d->codec,NULL) < 0) return DECODE;
        } else if(r < 0) return TRUNCATED;
        else {
            r = d->packet->flags & AV_PKT_FLAG_CORRUPT ? -1 : avcodec_send_packet(d->codec,d->packet);
            av_packet_unref(d->packet);
            if(r < 0) return DECODE;
        }
    }
}
static int samples(decoder *d, unsigned char *out, unsigned n){
    AVFrame *f = d->frame;
    enum AVSampleFormat fmt = av_get_packed_sample_fmt(f->format);
    int bytes = av_get_bytes_per_sample(fmt), planar = av_sample_fmt_is_planar(f->format);
    if(bytes < 1 || bytes > 8 || !f->extended_data) return DECODE;
    for(unsigned i=0;i<n;i++) for(unsigned ch=0;ch<d->channels;ch++){
        const unsigned char *p = f->extended_data[planar ? ch : 0];
        if(!p) return DECODE;
        p += ((size_t)(d->offset+i)*(planar ? 1 : d->channels)+(planar ? 0 : ch))*bytes;
        float v;
        switch(fmt){
        case AV_SAMPLE_FMT_U8: v = ((int)*p-128)*(1.0f/128); break;
        case AV_SAMPLE_FMT_S16: { int16_t s; memcpy(&s,p,2); v = s*(1.0f/32768); break; }
        case AV_SAMPLE_FMT_S32: { int32_t s; memcpy(&s,p,4); v = (float)(s*(1.0/2147483648.0)); break; }
        case AV_SAMPLE_FMT_FLT: memcpy(&v,p,4); break;
        case AV_SAMPLE_FMT_DBL: { double s; memcpy(&s,p,8); v = (float)s; break; }
        default: return DECODE;
        }
        if(!isfinite(v)) return DECODE;
        uint32_t word; memcpy(&word,&v,4); as_put32(out+4*(i*d->channels+ch),word);
    }
    d->offset += n;
    return OK;
}
int main(int argc, char **argv){
    (void)argv;
    decoder d = {0}; struct stat64 st;
    av_log_set_level(AV_LOG_QUIET);
    if(argc != 1 || !stock_matches()) return 1;
    unsigned char ready[AS_HEADER] = {0};
    as_put32(ready, AS_MAGIC);
    as_put32(ready+4, FORMAT);
    as_put32(ready+8, AS_READY);
    if(transfer(ready, sizeof ready, 1)) return 1;
    /* MIPS glibc 2.29 exposes the versioned stat ABI, not glibc 2.33 fstat64. */
    extern int __fxstat64(int, int, struct stat64 *);
    if(__fxstat64(3,4,&st) || !S_ISREG(st.st_mode) || st.st_size <= 0) return 1;
    d.bytes = st.st_size;
    struct rlimit mem = {96*1024*1024,96*1024*1024}, core = {0,0};
    if(setrlimit(RLIMIT_AS,&mem) || setrlimit(RLIMIT_CORE,&core)) return 1;
    av_max_alloc(16*1024*1024);
    int r = prepare(&d);
    if(reply(&d,(unsigned)r,0) || r) { release(&d); return 1; }
    unsigned char req[AS_REQUEST], pcm[AS_MAX_FRAMES*2*4];
    while(!transfer(req,sizeof req,0)){
        unsigned op = as_get32(req), capacity = as_get32(req+4), n = 0;
        if(op == AS_SEEK && !capacity){
            uint64_t left = as_get64(req+8);
            /* Reopen and discard from the beginning: sample-accurate for every
             * allowlisted codec, including codec priming and DSD filter state. */
            release(&d); r = prepare(&d);
            while(!r && left){
                if(d.offset == (unsigned)d.frame->nb_samples) r = next_frame(&d);
                if(r) break;
                unsigned take = (unsigned)d.frame->nb_samples-d.offset;
                if(left < take) take = (unsigned)left;
                d.offset += take; left -= take;
            }
            if(r == END) r = DECODE;
        } else if(op == AS_READ && capacity && capacity <= AS_MAX_FRAMES && !as_get64(req+8)){
            r = d.offset == (unsigned)d.frame->nb_samples ? next_frame(&d) : OK;
            if(!r){
                n = (unsigned)d.frame->nb_samples-d.offset;
                if(n > capacity) n = capacity;
                r = samples(&d,pcm,n);
                if(r) n = 0;
            }
        } else r = ARGUMENT;
        if(reply(&d,(unsigned)r,n) || (n && transfer(pcm,n*d.channels*4,1))) break;
        if(r && r != END && r != ARGUMENT) break;
    }
    release(&d); return 0;
}
