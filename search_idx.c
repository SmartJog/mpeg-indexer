#include <ffmpeg/avformat.h>
#include <stdlib.h>
#include <string.h>

#include "indexer.h"

#define INDEX_SIZE 29
#define HEADER_SIZE 9

#define SJ_INDEX_TIMECODE 1
#define SJ_INDEX_PTS 2
#define SJ_INDEX_DTS 4
typedef struct{
    uint64_t size;
    uint8_t version;
    ByteIOContext pb;
    uint64_t search_time;
    uint64_t index_pos;
    uint64_t mode;
    uint64_t index_num;
    Index *indexes;
} SJ_IndexContext;

static av_always_inline int read_index(Index *read_idx, ByteIOContext *seek_pb)
{
    read_idx->pts = get_le64(seek_pb);
    read_idx->dts = get_le64(seek_pb);
    read_idx->pes_offset = get_le64(seek_pb);
    read_idx->pic_type = get_byte(seek_pb);
    read_idx->timecode.frames = get_byte(seek_pb);
    read_idx->timecode.seconds = get_byte(seek_pb);
    read_idx->timecode.minutes = get_byte(seek_pb);
    read_idx->timecode.hours = get_byte(seek_pb);
    return 0;
}

int sj_index_load(char *filename, SJ_IndexContext *sj_ic)
{
    register_protocol(&file_protocol);

    if (url_fopen(&sj_ic->pb, filename, URL_RDONLY) < 0) {
        // file could not be open
        url_fclose(&sj_ic->pb);
        return -1;
    }
    sj_ic->size = url_fsize(&sj_ic->pb) - HEADER_SIZE;
    sj_ic->index_num = (int)(sj_ic->size / INDEX_SIZE);
    sj_ic->indexes = av_malloc(sj_ic->index_num * sizeof(Index));

    printf("Index size : %lld\n", sj_ic->size);
    int64_t magic = get_le64(&sj_ic->pb);
    if (magic != 0x534A2D494E444558LL){
        // not an index file
        url_fclose(&sj_ic->pb);
        return -2;
    }
    sj_ic->version = get_byte(&sj_ic->pb);
    if (!sj_ic->index_num){
        // empty index
        url_fclose(&sj_ic->pb);
        return -4;
    }

    for(int i = 0; i < sj_ic->index_num; i++){
        read_index(&sj_ic->indexes[i], &sj_ic->pb);
    }
    url_fclose(&sj_ic->pb);
    return 0;
}

static uint64_t get_search_value(Index idx, SJ_IndexContext sj_ic)
{
    if (sj_ic.mode == SJ_INDEX_TIMECODE) {
        return idx.timecode.hours * 1000000 + idx.timecode.minutes * 10000 + idx.timecode.seconds * 100 + idx.timecode.frames;
        //return (idx.timecode.hours << 24) | (idx.timecode.minutes << 16) | (idx.timecode.seconds << 8) | idx.timecode.frames;
    }
    else if (sj_ic.mode == SJ_INDEX_PTS) {
        return idx.pts;
    }
    else if (sj_ic.mode == SJ_INDEX_DTS) {
        return idx.dts;
    }
    return 0;
}

static int search_frame(SJ_IndexContext *sj_ic, Index *read_idx)
{
    uint64_t high = sj_ic->index_num;
    uint64_t low = 0;
    uint64_t mid = high / 2;

    uint64_t read_time = 0; // used to store the timecode members in a single 64 bits integer to facilitate comparison

    // Checks if the value we want is inferior or equal to the first value in the file
    read_time = get_search_value(sj_ic->indexes[0], *sj_ic);

    if (read_time == sj_ic->search_time){
        *read_idx = sj_ic->indexes[0];
        return 1;
    } else if (read_time > sj_ic->search_time) {
        return -1;
    }
    while (low <= high) {
        mid = (int)((high + low) / 2);
        if (sj_ic->mode == SJ_INDEX_TIMECODE) {
            read_time = sj_ic->indexes[mid].timecode.hours * 10000000 + sj_ic->indexes[mid].timecode.minutes * 10000 +  sj_ic->indexes[mid].timecode.seconds * 100 + sj_ic->indexes[mid].timecode.frames;
//            read_time = sj_ic->indexes[mid].timecode.hours << 24 & sj_ic->indexes[mid].timecode.minutes << 16 & sj_ic->indexes[mid].timecode.seconds << 8 & sj_ic->indexes[mid].timecode.frames;
        }
        else {
            read_time = sj_ic->indexes[mid].pts;
        }
        if (read_time == sj_ic->search_time){
            sj_ic->index_pos = mid;
            *read_idx = sj_ic->indexes[mid];
            return 1;
        } else if (read_time > sj_ic->search_time) {
            high = mid - 1;
        } else if (read_time < sj_ic->search_time) {
            low = mid + 1;
        }
    }
    return 0;
}

static int search_frame_dts(SJ_IndexContext *sj_ic, Index *read_idx)
{
    uint64_t i = sj_ic->index_pos;

    while (i < sj_ic->index_num) {
        // looks for the frame that has the dts we're looking for, it's located after the frame with that value as pts
        if (sj_ic->indexes[i].dts == sj_ic->search_time) {
            *read_idx = sj_ic->indexes[i];
            sj_ic->index_pos = i;
            return 1;
        }
        i++;
    }
    return 0;
}

static int find_previous_key_frame(Index *key_frame, SJ_IndexContext sj_ic)
{
    int i = sj_ic.index_pos;

    while (i >= 0){
        if (sj_ic.indexes[i].pic_type == 1) {
            *key_frame = sj_ic.indexes[i];
            sj_ic.index_pos = i;
            return 1;
        }
        i--;
    }
    return 0;
}

char get_frame_type(Index idx)
{
    char frame = 'U';
    char frame_types[3] = {'I','P','B'};
    if (idx.pic_type > 0 && idx.pic_type < 4) {
        frame = frame_types[idx.pic_type - 1];
    }
    return frame;
}

int sj_index_search(SJ_IndexContext *sj_ic, uint64_t search_val, Index *idx, Index *key_frame, uint64_t flags)
{
    int res = 0;
    sj_ic->mode = flags;
    sj_ic->search_time = search_val;
    res = search_frame(sj_ic, idx);
    if (flags == SJ_INDEX_DTS){
        if (idx->pts != idx->dts && idx->dts != sj_ic->search_time) {
            res = search_frame_dts(sj_ic, idx);
        }
    }
    char frame = get_frame_type(*idx);
    if (frame != 'I'){
        find_previous_key_frame(key_frame, *sj_ic);
    }

    return res; // res = 0 if frame wasn't found, -1 if the first value in the index is greater than the one we're looking for
}

int main(int argc, char **argv)
{
    SJ_IndexContext sj_ic;
    Index read_idx;
    Index key_frame;
    uint64_t search_val;

    memset(&key_frame, 0, sizeof(key_frame));
    memset(&read_idx, 0, sizeof(read_idx));
    if (argc < 4) {
        printf("usage: search_idx <parameter type> <index file> <hhmmssff>\n");
        printf("parameters types are :\n\t1\ttimecode\n\t2\tpts\n\t4\tdts\n");
        return 1;
    }

    int len = strlen(argv[3]);
    for (int i = 0; i < len; i++){
        if (argv[3][i] < '0' || argv[3][i] > '9'){
            printf("search value must be integer\n");
            return 0;
        }
    }

    int load_res = sj_index_load(argv[2], &sj_ic);
    if (load_res == -1) {
        printf("File could not be open\n");
        return 0;
    }

    if (load_res == -2) {
        printf("File is not a index file\n");
        return 0;
    }

    if (load_res == -4) {
        printf("Index is empty\n");
        return 0;
    }
    search_val = atoll(argv[3]);
    uint64_t flags = atoll(argv[1]);
    if ((flags != 1) && (flags != 2) && (flags != 4)){
        printf("Wrong flag\n");
        return 0;
    }
    int res = sj_index_search(&sj_ic, search_val, &read_idx, &key_frame, flags);

    if (!res){
        printf("Frame could not be found, check input data\n");
        return 1;
    }

    if (res == -1) {
        printf("Video starts at\ntimecode\t%02d:%02d:%02d:%02d\nPTS\t\t%lld\nDTS\t\t%lld\n", sj_ic.indexes[0].timecode.hours, sj_ic.indexes[0].timecode.minutes, sj_ic.indexes[0].timecode.seconds, sj_ic.indexes[0].timecode.frames, sj_ic.indexes[0].pts, sj_ic.indexes[0].dts);
        return 1;
    }

    printf("Frame %c : \t\ntimecode\t%02d:%02d:%02d:%02d\nPTS\t\t%lld\nDTS\t\t%lld\nPES-OFFSET\t\t%lld\n", get_frame_type(read_idx) ,read_idx.timecode.hours, read_idx.timecode.minutes, read_idx.timecode.seconds, read_idx.timecode.frames, read_idx.pts, read_idx.dts, read_idx.pes_offset);
    printf("Related key-frame : \t\ntimecode\t%02d:%02d:%02d:%02d\nPTS\t\t%lld\nDTS\t\t%lld\nPES-OFFSET\t\t%lld\n", key_frame.timecode.hours,key_frame.timecode.minutes, key_frame.timecode.seconds, key_frame.timecode.frames, key_frame.pts, key_frame.dts, key_frame.pes_offset);

    return 0;
}
