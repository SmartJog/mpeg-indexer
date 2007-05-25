#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ffmpeg/avformat.h>

#define SEQ_START_CODE            0x000001b3
#define GOP_START_CODE            0x000001b8
#define PICTURE_START_CODE        0x00000100

#define HEADER_TYPE_PRELUDE 0x00
#define HEADER_TYPE_PRIVATE 0x01
#define HEADER_TYPE_INDEX   0x02
#define HEADER_TYPE_END     0x04

//#define DEBUG

#define BE_32(x)  ((((uint8_t*)(x))[0] << 24) | \
                   (((uint8_t*)(x))[1] << 16) | \
                   (((uint8_t*)(x))[2] << 8) | \
                    ((uint8_t*)(x))[3])

typedef struct MpegDemuxContext {
    int32_t header_state;
    unsigned char psm_es_type[256];
} MpegDemuxContext;

typedef struct {
    int32_t hours;
    int32_t minutes;
    int32_t seconds;
    int32_t frames;
} Timecode;
typedef struct {
    uint8_t pic_type;
    int seq:1;
    int gop:1;
    int8_t closed_gop;
    int64_t pts;
    int64_t dts;
    offset_t pes_offset;
    Timecode timecode;
} Index;

typedef struct {
    AVFormatContext *fc;
    AVStream *video;
    ByteIOContext pb;
    ByteIOContext opb;
    int need_gop;
    int need_pic_type;
    uint8_t *ind;
    unsigned int ind_size;
    uint64_t mpeg_size;
    MpegDemuxContext mpg_demux_ctx;
    int frame_num;
    Index *index;
    int64_t current_pts[5];
    int64_t current_dts[5];
} StreamContext;

static int ind_sort_by_pts(const void *ind1, const void *ind2)
{
    return ((Index *)ind1)->pts - ((Index *)ind2)->pts;
}

static int write_eof_marker(StreamContext *s)
{
    ByteIOContext *pb = &s->opb;
    put_le64(pb, 0xADDEDEFAADDEADDELL);
    put_le32(pb, 0); /* reserved */
    put_le32(pb, 0xADDE);  /* crc not implemented */
    put_flush_packet(pb);
    return 0;
}

extern AVInputFormat mpegps_demuxer;

extern const uint8_t *ff_find_start_code(const uint8_t *p, const uint8_t *end, uint32_t *state);

static av_always_inline offset_t pes_find_packet_start(ByteIOContext *pb, offset_t from, uint32_t id)
{
    offset_t pos = url_ftell(pb);
    uint8_t buffer[32];
    uint32_t state = -1;
    offset_t res;
    int i;

    url_fseek(pb, from - 32, SEEK_SET);
    get_buffer(pb, buffer, 32);
    for (i = 0; i < 32; i++) {
        i = ff_find_start_code(buffer + i, buffer + 32, &state) - buffer - 1;
        if (state == id) {
            res = url_ftell(pb) - 32 + i - 3;
            url_fseek(pb, pos, SEEK_SET);
            return res;
        }
    }
    abort();
}
static int write_index(StreamContext *stcontext)
{
    ByteIOContext indexpb;
    unsigned int index_size;
    uint8_t *index_buf;

    url_open_dyn_buf(&indexpb);
    int i;

    qsort(stcontext->index, stcontext->frame_num, sizeof(Index), ind_sort_by_pts);
    put_le64(&indexpb, 0x534A2D494E444548LL);
    put_le16(&indexpb, 0x0000);
    for (i = 0; i < stcontext->frame_num; i++) {
        Index *ind = &stcontext->index[i];
        put_le64(&indexpb, ind->pts);
        put_le64(&indexpb, ind->dts);
        put_le64(&indexpb, ind->pes_offset);
        put_le32(&indexpb, ind->timecode.frames);
        put_le32(&indexpb, ind->timecode.seconds);
        put_le32(&indexpb, ind->timecode.minutes);
        put_le32(&indexpb, ind->timecode.hours);
        put_flush_packet(&indexpb);
    }
    index_size = url_close_dyn_buf(&indexpb, &index_buf);
    put_flush_packet(&indexpb);
    printf("index size %d\n", index_size);
    //url_fopen(&indexpb, "index", URL_WRONLY);
    put_buffer(&stcontext->opb, index_buf, index_size);
    put_flush_packet(&stcontext->opb);
    //url_fclose(&stcontext->opb);
    return 0;
}
static int write_trailer(StreamContext *s)
{
    if (write_index(s) < 0)
        return -1;
    write_eof_marker(s);
    return 0;
}

static av_always_inline int ind_set(StreamContext *stc, Index *ind, AVPacket *pkt, AVStream *st, int i)
{
    Index *oldind = stc->frame_num ? &stc->index[stc->frame_num - 1] : NULL;
    offset_t pkt_start = url_ftell(&stc->fc->pb) - pkt->size;
    ind->pes_offset = pes_find_packet_start(&stc->fc->pb, pkt_start, st->id);
    ind->dts = stc->current_dts[st->index];
    ind->pts = stc->current_pts[st->index];
    ind->pes_offset = pkt->pos;
    if (oldind && ind->dts <= oldind->dts) {
        ind->dts = oldind->dts + 3600;
        ind->pts = oldind->pts + 3600;
        printf("adjusting dts %lld -> %lld\n", stc->current_dts[st->index], ind->dts);
    }
    return 0;
}
#define BUFFER_SIZE 262144

static int get_frame_rate(AVStream *st, AVPacket *pkt)
{
    if (st){
        if (st->codec->codec_type == CODEC_TYPE_VIDEO) {
            uint8_t *buf = pkt->data;
            float fps = -1;
            //printf("buf 7 : %x\n", buf[7]);
            switch (buf[7] & 0xF){
                case 0x1:
                    fps = 24000/1001;
                    break;
                case 0x2:
                    fps = 24;
                    break;
                case 0x3:
                    fps = 25;
                    break;
                case 0x4:
                    fps = 30000/1001;
                    break;
                case 0x5:
                    fps = 30;
                    break;
                case 0x6:
                    fps = 50;
                    break;
                case 0x7:
                    fps = 60000/1001;
                    break;
                case 0x8:
                    fps = 60;
                    break;
             default :
                    printf("error fps could not be retrieved\n");
                    fps = -1;
                    break;
            }
            return fps;
        }
    }
    return -1;
}

static int find_timecode(Index *ind, AVStream *st, AVPacket *pkt, int *kf, Timecode *last_key_frame, float fps)
{
    int drop = 0;
    if (st) {
        if (st->codec->codec_type == CODEC_TYPE_VIDEO) {
            uint8_t *buf = pkt->data;
            uint32_t c = -1, d = -1;
            int i, j;
            for(i = 0; i < pkt->size - 4 && d != PICTURE_START_CODE; i++) {
                if (c != GOP_START_CODE){
                    c = (c<<8) + buf[i]; 
                    j = i+1;
                }
                d = (d<<8) + buf[i];
            } 
            if (c == GOP_START_CODE) { // found GOP header => I-frame follows 
                drop = !!(buf[j] & 0x80);
                last_key_frame->hours   = ind->timecode.hours   = (buf[j] >> 2) & 0x1f;
                last_key_frame->minutes = ind->timecode.minutes = (buf[j] & 0x03) << 4 | (buf[j+1] >> 4);
                last_key_frame->seconds = ind->timecode.seconds = (buf[j+1] & 0x07) << 3 | (buf[j+2] >> 5);
                last_key_frame->frames  = ind->timecode.frames  = (buf[j+2] & 0x1f) << 1 | (buf[j+3] >> 7);
                (*kf)++;
            }
            if (d == PICTURE_START_CODE) { // found picture start code
                uint32_t temp_ref = 0;
                uint32_t frame_type;
                int round_fps = (int)fps;
                temp_ref = (temp_ref<<8) + buf[i];
                temp_ref = (temp_ref<<8) + buf[i+1];
                temp_ref = (temp_ref>>6);
                frame_type = buf[i+1];
                frame_type = (frame_type >> 3) & 0x7;
                printf("\nframe type : %x, temp_ref : %d\n", frame_type, temp_ref);

                // calculation of timecode for current frame
                ind->timecode.frames  = (last_key_frame->frames  + temp_ref) % round_fps;
                ind->timecode.seconds = ((int)(last_key_frame->seconds + ((last_key_frame->frames + temp_ref) / fps))) % 60;
                ind->timecode.minutes = ((int)(last_key_frame->minutes + ((last_key_frame->frames + temp_ref) / (fps * 60)))) % 60;
                ind->timecode.hours   = ((int)(last_key_frame->hours   + ((last_key_frame->frames + temp_ref) / (fps * 3600)))) % 24;
                if ((last_key_frame->seconds + ((last_key_frame->frames + temp_ref) / fps)) == 60){
                    if ((ind->timecode.minutes + 1) == 60){
                        ind->timecode.hours   = (ind->timecode.hours + 1) % 24;
                    }
                    ind->timecode.minutes = (ind->timecode.minutes + 1) % 60;
                }
                printf("PIC timecode :\t%02d:%02d:%02d:%02d\n", ind->timecode.hours, ind->timecode.minutes, ind->timecode.seconds, ind->timecode.frames);
            } 
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    AVFormatContext *ic = NULL;
    AVStream *st = NULL;
    AVPacket pkt;
    StreamContext stcontext;
    uint8_t *buffer;
    int i, ret;
    (void)buffer;
    memset(&stcontext, 0, sizeof(stcontext));
    uint32_t state = -1;
    int kf = 0;
    //int gop:1;
    int closed_gop = 0;
    float fps = 0;
    Timecode last_key;

    memset(&last_key, 0, sizeof (last_key));

    if (argc < 3) {
        printf("indexing infile outfile\n");
        printf("create index file from the input program stream file\n");
        return 1;
    }

    register_protocol(&file_protocol);
    register_avcodec(&mpegvideo_decoder);
    if (av_open_input_file(&ic, argv[1], &mpegps_demuxer, BUFFER_SIZE, NULL) < 0) {
        printf("error opening infile: %s\n", argv[1]);
        return 1;
    }

    av_log_set_level(AV_LOG_QUIET);
    if (av_find_stream_info(ic) < 0) {
        printf("error getting infos from MPEG file\n");
        return 1;
    }
    av_log_set_level(AV_LOG_VERBOSE);

    if (ic->nb_streams > 5) {
        printf("too many streams in input file\n");
        return 1;
    }

    for (i = 0; i < ic->nb_streams; i++) {
        st = ic->streams[i];
        if (st->codec->codec_type == CODEC_TYPE_VIDEO) {
            if (stcontext.video != NULL) {
                printf("too many video streams in input file\n");
                return 1;
            }
            if (st->codec->width != 720) {
                printf("only PAL or NTSC MPEG PS files are supported\n");
                return 1;
            }
            stcontext.video = st;
        }
    }

    if (!stcontext.video) {
        printf("no video streams in input file\n");
        return 1;
    }

    stcontext.frame_num = 0;
    stcontext.fc = ic;
#ifdef DEBUG
    stcontext.mpeg_size = 400 * BUFFER_SIZE;
#else
    stcontext.mpeg_size = url_fsize(&ic->pb);
#endif
    printf("MPEG size %lld\n", stcontext.mpeg_size);

    if (url_fopen(&stcontext.opb, argv[2], URL_WRONLY) < 0) {
        printf("error opening outfile: %s\n", argv[2]);
        return 1;
    }

    stcontext.index = av_malloc(1000 * sizeof(Index));
    printf("creating index\n");
    while (1) {
        ret = av_read_packet(ic, &pkt);
        if (ret < 0)
            break;
#ifdef DEBUG
        if (url_ftell(&ic->pb) > 400 * ic->pb.buffer_size)
            break;
#endif
        st = ic->streams[pkt.stream_index];
        if (pkt.dts != AV_NOPTS_VALUE) {
            stcontext.current_dts[st->index] = pkt.dts;
            stcontext.current_pts[st->index] = pkt.pts;
        }
        for (i = 0; i < pkt.size; i++) {
            Index *ind = &stcontext.index[stcontext.frame_num];
            i = ff_find_start_code(pkt.data + i, pkt.data + pkt.size, &state) - pkt.data - 1;
//            printf("state : %x\n", state);
            if (state == SEQ_START_CODE){
                if (!fps){
                    fps = get_frame_rate(st, &pkt);
                    printf("fps %f\n", fps);
                }
                ind->seq = 1;
                ind_set(&stcontext, ind, &pkt, st, i);
            } else if (state == GOP_START_CODE) {
               ind->gop = 1;
                if (!ind->seq)
                    ind_set(&stcontext, ind, &pkt, st, i);
                if (i + 5 > pkt.size) {
                    stcontext.need_gop = 4 - pkt.size + i;
                    printf("could not get GOP type %d\n", stcontext.need_gop);
                } else {
                    closed_gop = !!(pkt.data[i + 4] & 0x40);
//                    printf("closed gop %d\n", ind->gop);
                }
            } else if (state == PICTURE_START_CODE) {
                if (!ind->seq && !ind->gop)
                    ind_set(&stcontext, ind, &pkt, st, i);
                if (i + 3 > pkt.size) {
                    stcontext.need_pic_type = 2 - pkt.size + i;
                    ind->pic_type = 0;
                    printf("could not get picture type, need %d\n", stcontext.need_pic_type);
                } else {
                    ind->pic_type = (pkt.data[i + 2] >> 3) & 7;
                    assert(ind->pic_type > 0 && ind->pic_type < 4);
                    //printf("frame num %d, data %llx, pic type %d, dts %lld\n", saf.frame_num, vix->pic_data, vix->pic_type, vix->dts);
                }
                ind->closed_gop = closed_gop;
                stcontext.frame_num++;
                find_timecode(ind, st, &pkt, &kf, &last_key, fps);
                if (!(stcontext.frame_num % 1000))
                    stcontext.index = av_realloc(stcontext.index, (stcontext.frame_num + 1000) * sizeof(Index));
                stcontext.index[stcontext.frame_num].seq = 0;
                stcontext.index[stcontext.frame_num].gop = 0;
            } 
        }
        av_free_packet(&pkt);
    }
    write_trailer(&stcontext);
    av_close_input_file(ic);
    url_fclose(&stcontext.opb);
    printf("frame num %d\n", stcontext.frame_num);
    printf("keyframes %d\n", kf);
    return 0;
}
