#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ffmpeg/avformat.h>

#define SEQ_START_CODE            0x000001b3
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
    uint32_t temp_ref;
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
    Timecode gop_time;
    uint8_t *buf;
    int fps;
    uint8_t drop_mode;
} TimeContext;

typedef struct {
    AVFormatContext *fc;
    AVStream *video;
    ByteIOContext pb;
    ByteIOContext opb;
    int need_gop;
    int need_pic_type;
    uint8_t *idx;
    unsigned int ind_size;
    uint64_t mpeg_size;
    MpegDemuxContext mpg_demux_ctx;
    int frame_num;
    Index *index;
    int64_t current_pts[5];
    int64_t current_dts[5];
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
        //printf("\ntimecode :\t%02d:%02d:%02d:%02d\n", idx->timecode.hours, idx->timecode.minutes, idx->timecode.seconds, idx->timecode.frames);
        put_le64(&indexpb, idx->pts);               // PTS
        put_le64(&indexpb, idx->dts);               // DTS
        put_le64(&indexpb, idx->pes_offset);        // PES offset
        put_byte(&indexpb, idx->timecode.frames);   // Frame number in timecode
        put_byte(&indexpb, idx->timecode.seconds);  // Seconds number in timecode
        put_byte(&indexpb, idx->timecode.minutes);  // Minutes number in timecode
        put_byte(&indexpb, idx->timecode.hours);    // Hours number in timecode
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


static av_always_inline int idx_set(StreamContext *stc, Index *idx, AVPacket *pkt, AVStream *st, int i)
{
    Index *oldidx = stc->frame_num ? &stc->index[stc->frame_num - 1] : NULL;
    offset_t pkt_start = url_ftell(&stc->fc->pb) - pkt->size;
    idx->pes_offset = pes_find_packet_start(&stc->fc->pb, pkt_start, st->id);
    idx->dts = stc->current_dts[st->index];
    idx->pts = stc->current_pts[st->index];
    idx->pes_offset = pkt->pos;
    if (oldidx && idx->dts <= oldidx->dts) {
        idx->dts = oldidx->dts + 3600;
        idx->pts = oldidx->pts + 3600;
        printf("adjusting dts %lld -> %lld\n", stc->current_dts[st->index], idx->dts);
    }
    return 0;
}
#define BUFFER_SIZE 262144

static int get_frame_rate(AVStream *st, AVPacket *pkt)
{
    if (st){
        if (st->codec->codec_type == CODEC_TYPE_VIDEO) {
            uint8_t *buf = pkt->data;
            int fps = -1;
            int tmp = buf[7] & 0xF;
            if (tmp < 1 || tmp > 8){
                printf("Unknown frame rate\n");
                return 0;
            }else{
                fps = fps_list[tmp-1];
            }
            return fps;
        }
    }
    return 0;
}

static int parse_gop_timecode(Index *idx, TimeContext *tc)
{
    //uint8_t *buf = pkt->data;

    tc->drop_mode = !!(tc->buf[0] & 0x80);
    tc->gop_time.hours   = idx->timecode.hours   = (tc->buf[0] >> 2) & 0x1f;
    tc->gop_time.minutes = idx->timecode.minutes = (tc->buf[0] & 0x03) << 4 | (tc->buf[1] >> 4);
    tc->gop_time.seconds = idx->timecode.seconds = (tc->buf[1] & 0x07) << 3 | (tc->buf[2] >> 5);
    tc->gop_time.frames  = idx->timecode.frames  = ((tc->buf[2] & 0x1f) << 1 | (tc->buf[3] >> 7));
    printf("\nGOP timecode :\t%02d:%02d:%02d:%02d\tdrop : %d\n", idx->timecode.hours, idx->timecode.minutes, idx->timecode.seconds, idx->timecode.frames, tc->drop_mode);
    return 0;
}

static int parse_pic_timecode(Index *idx, TimeContext *tc)
{
    //uint8_t *buf = pkt->data;
    // if idx->temp_ref is not empty then the first octet was in the previous packet and its value was stored in idx->tem_ref which means
    // only the first two bits of the second octet are needed
    if (idx->temp_ref)
        idx->temp_ref += tc->buf[1] >> 6;
    else
        idx->temp_ref = ((tc->buf[0] << 8) + tc->buf[1]) >> 6;
    printf("temp_ref %d\n", idx->temp_ref);
    // calculation of timecode for current frame

    idx->timecode = tc->gop_time;
    idx->timecode.frames = tc->gop_time.frames + idx->temp_ref;

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
    }
    printf("PIC timecode :\t%02d:%02d:%02d:%02d\n", idx->timecode.hours, idx->timecode.minutes, idx->timecode.seconds, idx->timecode.frames);
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
    int closed_gop = 0;

    memset(&stcontext, 0, sizeof(stcontext));
    memset(&tc, 0, sizeof(tc));

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
    stcontext.need_pic_type = -1;
    stcontext.need_gop = -1;
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
//    int count = 0;
    while (1) {
//        printf("------------------------PACKET n°%d-------------------\n",count++);
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
            if (stcontext.need_pic_type != -1) {
                printf("debug need pic idx->tempref : %d\n",stcontext.index[stcontext.frame_num-1].temp_ref );
                stcontext.index[stcontext.frame_num-1].pic_type = (pkt.data[stcontext.need_pic_type] >> 3) & 7;

                tc.buf = pkt.data + stcontext.need_pic_type-1;
                parse_pic_timecode(&stcontext.index[stcontext.frame_num-1], &tc);
                stcontext.need_pic_type = -1;
                assert(stcontext.index[stcontext.frame_num-1].pic_type > 0 &&
                        stcontext.index[stcontext.frame_num-1].pic_type < 4);
            }
            if (stcontext.need_gop != -1) {
                printf("debug need gop\n");
                closed_gop = !!(pkt.data[stcontext.need_gop] & 0x40);
                tc.buf = pkt.data + stcontext.need_gop + 1;
                parse_gop_timecode(&stcontext.index[stcontext.frame_num-1], &tc); 
                printf("gop %d\n", closed_gop);
                stcontext.need_gop = -1;
            }
            for (i = 0; i < pkt.size; i++) {
                Index *idx = &stcontext.index[stcontext.frame_num];
                idx->temp_ref = 0;
                i = ff_find_start_code(pkt.data + i, pkt.data + pkt.size, &state) - pkt.data - 1;
                if (state == SEQ_START_CODE){
                    if (!tc.fps){
                        tc.fps = get_frame_rate(st, &pkt);
                        printf("fps %d\n", tc.fps);
                        if (!tc.fps){
                            printf("Frame rate could not be found\n");
                            return -1;
                        }
                    }
                    idx->seq = 1;
                    idx_set(&stcontext, idx, &pkt, st, i);
                } else if (state == GOP_START_CODE) {
                    idx->gop = 1;
                    if (!idx->seq)
                        idx_set(&stcontext, idx, &pkt, st, i);
                    if (i + 5 > pkt.size) {
                        stcontext.need_gop = 4 - pkt.size + i;
                        printf("could not get GOP type, need %d\n", stcontext.need_gop);
                    } else {
                        closed_gop = !!(pkt.data[i + 4] & 0x40);
//                        printf("pkt[i] : %x",pkt.data[i]);
                        tc.buf = pkt.data + i + 1;
                        parse_gop_timecode(idx, &tc); 
                    }
                } else if (state == PICTURE_START_CODE) {
                    if (!idx->seq && !idx->gop)
                        idx_set(&stcontext, idx, &pkt, st, i);
                    if (i + 3 > pkt.size) {
                        stcontext.need_pic_type = 2 - pkt.size + i;
                        idx->pic_type = 0;
                        // if the temporal reference's first octet is in this packet, it needs to be stored
                        if (i + 1 <= pkt.size){
                            idx->temp_ref = pkt.data[i+1] << 2;
                        }
                        printf("could not get picture type, need %d\n", stcontext.need_pic_type);
                    } else {
                        idx->pic_type = (pkt.data[i + 2] >> 3) & 7;
                        assert(idx->pic_type > 0 && idx->pic_type < 4);
                        tc.buf = pkt.data + i + 1;
                        parse_pic_timecode(idx, &tc);
                    }
                    if (!idx->pts)
                        idx->pts=idx->dts;
                    idx->closed_gop = closed_gop;
                    stcontext.frame_num++;
                    if (!(stcontext.frame_num % 1000))
                        stcontext.index = av_realloc(stcontext.index, (stcontext.frame_num + 1000) * sizeof(Index));
                    stcontext.index[stcontext.frame_num].seq = 0;
                    stcontext.index[stcontext.frame_num].gop = 0;

                }
            }
        }
        av_free_packet(&pkt);
    }
    write_index(&stcontext);
    av_close_input_file(ic);
    url_fclose(&stcontext.opb);
    printf("frame num %d\n", stcontext.frame_num);
    return 0;
}
