/*
 * Indexer takes a Mpeg file as first argument and the name of an output file as second argument
 * it then creates an Index file of the initial mpeg stream containing for each frame
 * its timecode, pts, dts, pes offset and type of encoding (I, P, B)
 *
 */
#include <ffmpeg/avformat.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "libsjindex/indexer.h"

#define GOP_START_CODE            0x000001b8
#define PICTURE_START_CODE        0x00000100

//#define DEBUG

typedef struct {
    Timecode gop_time;
    int gop_num;
    int fps;
    uint8_t drop_mode;
    int timecode_generate;
} TimeContext;

typedef struct {
    AVFormatContext *fc;
    AVStream *video;
    ByteIOContext opb;
    int need_gop;
    int need_pic;
    int frame_num;
    Index *index;
    int64_t current_pts;
    int64_t current_dts;
    int frame_duration;
    int64_t start_dts;
    int64_t start_pts;
    Timecode start_timecode;
} StreamContext;

static int idx_sort_by_pts(const void *idx1, const void *idx2)
{
    return ((Index *)idx1)->pts - ((Index *)idx2)->pts;
}

/*static int idx_sort_by_time(const void *idx1, const void *idx2)
{
    return (((Index *)idx1)->timecode.hours * 1000000 + ((Index *)idx1)->timecode.minutes * 10000 + ((Index *)idx1)->timecode.seconds * 100 + ((Index *)idx1)->timecode.frames) - (((Index *)idx2)->timecode.hours * 1000000 + ((Index *)idx2)->timecode.minutes * 10000 + ((Index *)idx2)->timecode.seconds * 100 + ((Index *)idx2)->timecode.frames);
}*/

static Timecode timecode_min(Timecode t1, Timecode t2)
{
    return t1.hours * 1000000 + t1.minutes * 10000 + t1.seconds * 100 + t1.frames < t2.hours * 1000000 + t2.minutes * 10000 + t2.seconds * 100 + t2.frames ? t1 : t2 ;
}

extern AVInputFormat mpegps_demuxer;
extern AVCodec       mpegvideo_decoder;

extern const uint8_t *ff_find_start_code(const uint8_t *p, const uint8_t *end, uint32_t *state);

#define PES_SEARCH_LEN 48

static av_always_inline offset_t pes_find_packet_start(ByteIOContext *pb, offset_t from, uint32_t id)
{
    offset_t pos = url_ftell(pb);
    uint8_t buffer[PES_SEARCH_LEN];
    uint32_t state = -1;
    offset_t res;
    int i;

    url_fseek(pb, from - PES_SEARCH_LEN, SEEK_SET);
    get_buffer(pb, buffer, PES_SEARCH_LEN);
    for (i = 0; i < PES_SEARCH_LEN; i++) {
        i = ff_find_start_code(buffer + i, buffer + PES_SEARCH_LEN, &state) - buffer - 1;
        if (state == id) {
            res = url_ftell(pb) - PES_SEARCH_LEN + i - 3;
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
    put_le64(&indexpb, stcontext->start_pts);                 // PTS of the first frame to be displayed
    put_le64(&indexpb, stcontext->start_dts);                 // DTS of the first frame to be decoded
    put_byte(&indexpb, stcontext->start_timecode.frames);     // Frame component of first diplayed frame's timecode
    put_byte(&indexpb, stcontext->start_timecode.seconds);    // Seconds component of first diplayed frame's timecode
    put_byte(&indexpb, stcontext->start_timecode.minutes);    // Minutes component of first diplayed frame's timecode
    put_byte(&indexpb, stcontext->start_timecode.hours);      // Hours component of first diplayed frame's timecode
    for (i = 0; i < stcontext->frame_num; i++) {
        Index *idx = &stcontext->index[i];
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
    put_buffer(&stcontext->opb, index_buf, index_size);
    put_flush_packet(&stcontext->opb);
    return 0;
}


static av_always_inline int idx_set_timestamps(StreamContext *stc, Index *idx, AVPacket *pkt, AVStream *st)
{
    Index *oldidx = stc->frame_num ? &stc->index[stc->frame_num - 1] : NULL;
    idx->dts = stc->current_dts;
    idx->pts = stc->current_pts;
    if (oldidx){
        if (idx->dts <= oldidx->dts) {
            idx->dts = oldidx->dts + stc->frame_duration;
            idx->pts = oldidx->dts + stc->frame_duration;
//            printf("adjusting dts %lld -> %lld\n", stc->current_dts, idx->dts);
        }
    }
    return 0;
}
#define BUFFER_SIZE 262144
static int check_timecode_presence(TimeContext *tc)
{
    if (!tc->gop_time.hours && !tc->gop_time.minutes && !tc->gop_time.seconds && !tc->gop_time.frames) {
        tc->timecode_generate = 1;
        printf("No timecode present in stream, generating timecode from 00:00:00:00\n");
    }
    return 0;
}
static int parse_gop_timecode(Index *idx, TimeContext *tc, uint8_t *buf)
{
    tc->drop_mode = !!(buf[0] & 0x80);
    tc->gop_time.hours   = idx->timecode.hours   = (buf[0] >> 2) & 0x1f;
    tc->gop_time.minutes = idx->timecode.minutes = (buf[0] & 0x03) << 4 | (buf[1] >> 4);
    tc->gop_time.seconds = idx->timecode.seconds = (buf[1] & 0x07) << 3 | (buf[2] >> 5);
    tc->gop_time.frames  = idx->timecode.frames  = (buf[2] & 0x1f) << 1 | (buf[3] >> 7);
    tc->gop_num++;
//  printf("\nGOP timecode :\t%02d:%02d:%02d:%02d\tdrop : %d\n", idx->timecode.hours, idx->timecode.minutes, idx->timecode.seconds, idx->timecode.frames, tc->drop_mode);
    return 0;
}

static av_always_inline int adjust_timecode(Index *idx, TimeContext *tc)
{
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
    return 0;
}

static int parse_pic_timecode(Index *idx, TimeContext *tc, Index *last_in_gop, uint8_t *buf)
{
    int temp_ref = (buf[0] << 2) | (buf[1] >> 6);
    idx->pic_type = (buf[1] >> 3) & 0x07;

//  calculation of timecode for current frame
    if (tc->timecode_generate) {
        idx->timecode = last_in_gop->timecode;
        idx->timecode.frames += temp_ref + 1;
    } else {
        idx->timecode = tc->gop_time;
        idx->timecode.frames = tc->gop_time.frames + temp_ref;
    }

    adjust_timecode(idx, tc);

    if (tc->drop_mode && idx->timecode.minutes % 10 && idx->timecode.minutes != tc->gop_time.minutes) {
        printf ("dropping numbers 0 and 1 from timecode count\n");
        idx->timecode.frames += 2;
        adjust_timecode(idx, tc);
    }
    //printf("PIC timecode :\t\t%02d:%02d:%02d:%02d\n", idx->timecode.hours, idx->timecode.minutes, idx->timecode.seconds, idx->timecode.frames);
    return 0;
}

static int calculate_pts_from_dts(StreamContext *stc)
{
    int i = 0, j = 1;
    while (i < stc->frame_num && j < stc->frame_num) {
        if (stc->index[i].pic_type != 3 && stc->index[j].pic_type != 3) {
            stc->index[i].pts = stc->index[j].dts;
            stc->start_pts = FFMIN(stc->start_pts, stc->index[i].pts);
            stc->start_timecode = timecode_min(stc->start_timecode, stc->index[i].timecode);
            i++;
            j++;
        }
        while (i < stc->frame_num && stc->index[i].pic_type == 3) {
            stc->start_pts = FFMIN(stc->start_pts, stc->index[i].pts);
            stc->start_timecode = timecode_min(stc->start_timecode, stc->index[i].timecode);
            i++;
        }
        while (j < stc->frame_num && stc->index[j].pic_type == 3) {
            j++;
        }
    }
    // the last I frame will not get a pts from another frame's dts unless its pts was the transport's package pts
    // in other words if the last I frame's pts is equal to the one just before then it needs to be incremented
    if (stc->index[i].pts == stc->index[i - 1].pts) {
        stc->index[i].pts += stc->frame_duration;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    AVFormatContext *ic = NULL;
    AVStream *st = NULL;
    AVPacket pkt;
    StreamContext stcontext;
    TimeContext tc;
    int i, ret;
    uint32_t state = -1;
    offset_t last_pkt_offset = 0;

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

    stcontext.start_pts = 1000000000;
    stcontext.start_timecode.hours = 23;
    stcontext.start_timecode.minutes = 59;
    stcontext.start_timecode.seconds = 59;
    stcontext.start_timecode.frames = tc.fps - 1;

    if (url_fopen(&stcontext.opb, argv[2], URL_WRONLY) < 0) {
        printf("error opening outfile: %s\n", argv[2]);
        return 1;
    }

    stcontext.index = av_malloc(1000 * sizeof(Index));
    printf("creating index\n");
    int count_gop = 0;
    Index *last_in_gop = NULL;
    while (1) {
        ret = av_read_packet(ic, &pkt);
        if (ret < 0)
            break;

        st = ic->streams[pkt.stream_index];
        if (st->codec->codec_type == CODEC_TYPE_VIDEO) {
//          records the offset of the packet in case the next picture start code begins in it and finishes in the next packet
            offset_t pkt_offset = pes_find_packet_start(&ic->pb, url_ftell(&ic->pb) - pkt.size, st->id);

            if (pkt.dts != AV_NOPTS_VALUE) {
                stcontext.current_dts = pkt.dts;
                stcontext.current_pts = pkt.pts;
            }
            if (!stcontext.start_dts) {
                stcontext.start_dts = stcontext.current_dts;
            }
            if (stcontext.need_pic) {
                memcpy(data_buf + 2 - stcontext.need_pic, pkt.data, stcontext.need_pic);
                parse_pic_timecode(&stcontext.index[stcontext.frame_num-1], &tc, last_in_gop, data_buf);
                stcontext.need_pic = 0;
                assert(stcontext.index[stcontext.frame_num-1].pic_type > 0 &&
                       stcontext.index[stcontext.frame_num-1].pic_type < 4);
            }
            if (stcontext.need_gop) {
                memcpy(data_buf + 4 - stcontext.need_gop, pkt.data, stcontext.need_gop);
                if (!tc.timecode_generate)
                    parse_gop_timecode(&stcontext.index[stcontext.frame_num-1], &tc, data_buf);
                if (count_gop == 2)
                    check_timecode_presence(&tc);
               stcontext.need_gop = 0;
            }
            for (i = 0; i < pkt.size; i++) {
                Index *idx = &stcontext.index[stcontext.frame_num];
                i = ff_find_start_code(pkt.data + i, pkt.data + pkt.size, &state) - pkt.data - 1;
                if (state == GOP_START_CODE) {
                    last_in_gop = &stcontext.index[stcontext.frame_num - 1];
                    int bytes = FFMIN(pkt.size - i - 1, 4);
                    memcpy(data_buf, pkt.data + i + 1, bytes);
                    stcontext.need_gop = 4 - bytes;
                    count_gop++;
                    if (!stcontext.need_gop && !tc.timecode_generate) {
                        parse_gop_timecode(idx, &tc, data_buf);
                        if (count_gop == 2)
                            check_timecode_presence(&tc);
                    }
                } else if (state == PICTURE_START_CODE) {
                    int bytes = FFMIN(pkt.size - i - 1, 2);
                    memcpy(data_buf, pkt.data + i + 1, bytes);
                    stcontext.need_pic = 2 - bytes;

                    // check if startcode begins in last packet
                    idx->pes_offset = i < 3 ? last_pkt_offset : pkt_offset;

                    if (!stcontext.need_pic) {
                        parse_pic_timecode(idx, &tc, last_in_gop, data_buf);
                    }
                    idx_set_timestamps(&stcontext, idx, &pkt, st);
                    stcontext.frame_num++;
                    if (!(stcontext.frame_num % 1000)){
                        stcontext.index = av_realloc(stcontext.index, (stcontext.frame_num + 1000) * sizeof(Index));
                    }
                }
            }
            last_pkt_offset = pkt_offset;
        }
        av_free_packet(&pkt);
    }
    calculate_pts_from_dts(&stcontext);
    write_index(&stcontext);
    av_close_input_file(ic);
    url_fclose(&stcontext.opb);
    av_free(stcontext.index);
    printf("%d frames\n", stcontext.frame_num);
    return 0;
}
