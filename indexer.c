#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ffmpeg/avformat.h>

#define GOP_START_CODE            0x000001b8
#define PICTURE_START_CODE        0x00000100

//#define DEBUG

static const int fps_list[8] = {24, 24, 25, 30, 30, 50, 60, 60};
typedef struct MpegDemuxContext {
    int32_t header_state;
    unsigned char psm_es_type[256];
} MpegDemuxContext;

typedef struct {
    int8_t hours;
    int8_t minutes;
    int8_t seconds;
    int8_t frames;
} Timecode;

typedef struct {
    uint8_t pic_type;
    int64_t pts;
    int64_t dts;
    offset_t pes_offset;
    Timecode timecode;
} Index;

typedef struct {
    Timecode gop_time;
    int fps;
    uint8_t drop_mode;
} TimeContext;

typedef struct {
    AVFormatContext *fc;
    AVStream *video;
    ByteIOContext pb;
    ByteIOContext opb;
    int need_gop;
    int need_pic;
    uint8_t *idx;
    unsigned int ind_size;
    uint64_t mpeg_size;
    MpegDemuxContext mpg_demux_ctx;
    int frame_num;
    Index *index;
    int64_t current_pts[5];
    int64_t current_dts[5];
    int frame_duration;
} StreamContext;

static int idx_sort_by_pts(const void *idx1, const void *idx2)
{
    return ((Index *)idx1)->pts - ((Index *)idx2)->pts;
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

    qsort(stcontext->index, stcontext->frame_num, sizeof(Index), idx_sort_by_pts);
    put_le64(&indexpb, 0x534A2D494E444558LL);       // Magic number : SJ-INDEX in hex
    put_byte(&indexpb, 0x00000000);                 // Version
    for (i = 0; i < stcontext->frame_num; i++) {
        Index *idx = &stcontext->index[i];
//      printf("\ntimecode :\t%02d:%02d:%02d:%02d\n", idx->timecode.hours, idx->timecode.minutes, idx->timecode.seconds, idx->timecode.frames);
        put_le64(&indexpb, idx->pts);               // PTS
        put_le64(&indexpb, idx->dts);               // DTS
        put_le64(&indexpb, idx->pes_offset);        // PES offset
        put_byte(&indexpb, idx->pic_type);          // Picture Type
        put_byte(&indexpb, idx->timecode.frames);   // Frame number in timecode
        put_byte(&indexpb, idx->timecode.seconds);  // Seconds number in timecode
        put_byte(&indexpb, idx->timecode.minutes);  // Minutes number in timecode
        put_byte(&indexpb, idx->timecode.hours);    // Hours number in timecode
        put_flush_packet(&indexpb);
    }
    index_size = url_close_dyn_buf(&indexpb, &index_buf);
    put_flush_packet(&indexpb);
    printf("index size %d\n", index_size);
//  url_fopen(&indexpb, "index", URL_WRONLY);
    put_buffer(&stcontext->opb, index_buf, index_size);
    put_flush_packet(&stcontext->opb);
//  url_fclose(&stcontext->opb);
    return 0;
}


static av_always_inline int idx_set(StreamContext *stc, Index *idx, AVPacket *pkt, AVStream *st)
{
    Index *oldidx = stc->frame_num ? &stc->index[stc->frame_num - 1] : NULL;
    idx->dts = stc->current_dts[st->index];
    idx->pts = stc->current_pts[st->index];
    if (oldidx && idx->dts <= oldidx->dts) {
        idx->dts = oldidx->dts + stc->frame_duration;
        idx->pts = oldidx->pts + stc->frame_duration;
//      printf("adjusting dts %lld -> %lld\n", stc->current_dts[st->index], idx->dts);
    }
    return 0;
}
#define BUFFER_SIZE 262144

static int parse_gop_timecode(Index *idx, TimeContext *tc, uint8_t *buf)
{
    tc->drop_mode = !!(buf[0] & 0x80);
    tc->gop_time.hours   = idx->timecode.hours   = (buf[0] >> 2) & 0x1f;
    tc->gop_time.minutes = idx->timecode.minutes = (buf[0] & 0x03) << 4 | (buf[1] >> 4);
    tc->gop_time.seconds = idx->timecode.seconds = (buf[1] & 0x07) << 3 | (buf[2] >> 5);
    tc->gop_time.frames  = idx->timecode.frames  = ((buf[2] & 0x1f) << 1 | (buf[3] >> 7));
//  printf("\nGOP timecode :\t%02d:%02d:%02d:%02d\tdrop : %d\n", idx->timecode.hours, idx->timecode.minutes, idx->timecode.seconds, idx->timecode.frames, tc->drop_mode);
    return 0;
}

static int parse_pic_timecode(Index *idx, TimeContext *tc, uint8_t *buf)
{
    int temp_ref = ((buf[0] << 8) + buf[1]) >> 6;
    idx->pic_type = (buf[1] >> 3) & 0x07;
//  printf("frame type : %x\ttemp_ref %d\n",idx->pic_type, temp_ref);
//  calculation of timecode for current frame

    idx->timecode = tc->gop_time;
    idx->timecode.frames = tc->gop_time.frames + temp_ref;

    while (idx->timecode.frames >= tc->fps) {
        idx->timecode.seconds++;
        idx->timecode.frames -= tc->fps;
    }

    while (idx->timecode.seconds >= 60) {
        idx->timecode.minutes++;
        idx->timecode.seconds -= 60;
    }

    while (idx->timecode.minutes >= 60) {
        idx->timecode.hours++;
        idx->timecode.minutes -= 60;
    }

    while (idx->timecode.hours >= 24)
        idx->timecode.hours = 0; // what to do ?

    if (tc->drop_mode && idx->timecode.minutes % 10 && idx->timecode.minutes != tc->gop_time.minutes) {
        printf ("dropping numbers 0 and 1 from timecode count\n");
        idx->timecode.frames += 2;
        //FIXME readjust if frames > fps
    }
//  printf("PIC timecode :\t%02d:%02d:%02d:%02d\n", idx->timecode.hours, idx->timecode.minutes, idx->timecode.seconds, idx->timecode.frames);
    return 0;
}

int main(int argc, char *argv[])
{
    AVFormatContext *ic = NULL;
    AVStream *st = NULL;
    AVPacket pkt;
    StreamContext stcontext;
    TimeContext tc;
    uint8_t *buffer;
    int i, ret;
    (void)buffer;
    uint32_t state = -1;
    offset_t last_offset = -1;

    memset(&stcontext, 0, sizeof(stcontext));
    memset(&tc, 0, sizeof(tc));

    uint8_t data_buf[8]; // used to store bits when data is divided in two packets

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
            stcontext.video = st;
        }
    }

    if (!stcontext.video) {
        printf("no video streams in input file\n");
        return 1;
    }

    tc.fps = (float)stcontext.video->codec->time_base.den
        / stcontext.video->codec->time_base.num + 0.5;
    stcontext.frame_duration = av_rescale(1, 90000, tc.fps);
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
        if (st->codec->codec_type == CODEC_TYPE_VIDEO) {
            if (stcontext.need_pic) {
                memcpy(data_buf + 2 - stcontext.need_pic, pkt.data, stcontext.need_pic);
                parse_pic_timecode(&stcontext.index[stcontext.frame_num-1], &tc, data_buf);
                stcontext.need_pic = 0;
                assert(stcontext.index[stcontext.frame_num-1].pic_type > 0 &&
                       stcontext.index[stcontext.frame_num-1].pic_type < 4);
            }
            if (stcontext.need_gop) {
                memcpy(data_buf + 4 - stcontext.need_gop, pkt.data, stcontext.need_gop);
                parse_gop_timecode(&stcontext.index[stcontext.frame_num-1], &tc, data_buf);
                stcontext.need_gop = 0;
            }
            for (i = 0; i < pkt.size; i++) {
                Index *idx = &stcontext.index[stcontext.frame_num];
                i = ff_find_start_code(pkt.data + i, pkt.data + pkt.size, &state) - pkt.data - 1;
                if (state == GOP_START_CODE) {
                    int bytes = FFMIN(pkt.size - i - 1, 4);
                    memcpy(data_buf, pkt.data + i + 1, bytes);
                    stcontext.need_gop = 4 - bytes;

                    if (!stcontext.need_gop)
                        parse_gop_timecode(idx, &tc, data_buf);
                } else if (state == PICTURE_START_CODE) {
                    if (i < 3) {
                        printf("Picture start code begins in previous packet : %lld\n", last_offset);
                        idx->pes_offset = last_offset;
                    } else {
                        offset_t off = url_ftell(&stcontext.fc->pb) - pkt.size;
                        idx->pes_offset = pes_find_packet_start(&stcontext.fc->pb, off, st->id);
                    }

                    int bytes = FFMIN(pkt.size - i - 1, 2);
                    memcpy(data_buf, pkt.data + i + 1, bytes);
                    stcontext.need_pic = 2 - bytes;
                    idx_set(&stcontext, idx, &pkt, st);

                    if (!stcontext.need_pic)
                        parse_pic_timecode(idx, &tc, data_buf);

                    if (!idx->pts)
                        idx->pts=idx->dts;
                    stcontext.frame_num++;
                    if (!(stcontext.frame_num % 1000))
                        stcontext.index = av_realloc(stcontext.index, (stcontext.frame_num + 1000) * sizeof(Index));
                }
            }
//          records the offset of the packet in case the next picture start code begins in it and finishes in the next packet
            offset_t pkt_start = url_ftell(&stcontext.fc->pb) - pkt.size;
            last_offset = pes_find_packet_start(&stcontext.fc->pb, pkt_start, st->id);
//          printf("last : %lld\n", last_offset);
        }
        av_free_packet(&pkt);
    }
    write_index(&stcontext);
    av_close_input_file(ic);
    url_fclose(&stcontext.opb);
    av_free(stcontext.index);
    printf("frame num %d\n", stcontext.frame_num);
    return 0;
}
