/*
 * sj_search_index.c defines a set of functions used to find
 * the PES offset of a frame given an Index file and a time reference
 * 
 */
#include <ffmpeg/avformat.h>
#include <stdlib.h>
#include <string.h>
#include "indexer.h"
#include "sj_search_index.h"

#define INDEX_SIZE 29
#define HEADER_SIZE 9
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
    ByteIOContext pb;
    register_protocol(&file_protocol);
    
    if (url_fopen(&pb, filename, URL_RDONLY) < 0) {
        // file could not be open
        return -1;
    }
    sj_ic->size = url_fsize(&pb) - HEADER_SIZE;
    sj_ic->index_num = (sj_ic->size / INDEX_SIZE);
    sj_ic->indexes = av_malloc(sj_ic->index_num * sizeof(Index));

    int64_t magic = get_le64(&pb);
    if (magic != 0x534A2D494E444558LL) {
        // not an index file
        url_fclose(&pb);
        return -2;
    }
    sj_ic->version = get_byte(&pb);
    if (!sj_ic->index_num) {
        // empty index
        url_fclose(&pb);
        return -4;
    }

    for(int i = 0; i < sj_ic->index_num; i++) {
        read_index(&sj_ic->indexes[i], &pb);
    } 
    url_fclose(&pb);
    return 0;
}

int sj_index_unload(SJ_IndexContext *sj_ic)
{
    free(sj_ic->indexes);
    memset(sj_ic, 0, sizeof(*sj_ic));
    return 0;
}

static av_always_inline uint64_t get_search_value(Index idx, int mode)
{
    if (mode == SJ_INDEX_TIMECODE_SEARCH) {
        return idx.timecode.hours * 1000000 + idx.timecode.minutes * 10000 + idx.timecode.seconds * 100 + idx.timecode.frames;
    }
    else if (mode == SJ_INDEX_PTS_SEARCH){
        return idx.pts;
    }
    else if (mode == SJ_INDEX_DTS_SEARCH){
        return idx.dts;
    }
    return -1; // invalid search mode
}

static int find_I_frame(Index *key_frame, SJ_IndexContext sj_ic, int index_pos)
{
    // if the next I_frame has a dts inferior to the searched dts then this I_frame is the related key_frame 
    for (int i = index_pos; i < sj_ic.index_num; i++){
        if (sj_ic.indexes[i].pic_type == FF_I_TYPE && sj_ic.indexes[i].dts < sj_ic.indexes[index_pos].dts) {
            *key_frame = sj_ic.indexes[i];
            return 0;
        }
    }
    // otherwise, look before the searched frame
    for (int i = index_pos; i >= 0; i--) {
        if (sj_ic.indexes[i].pic_type == FF_I_TYPE) {
            *key_frame = sj_ic.indexes[i];
            return 0;
        }
    }
    return 0;
}

static int search_frame(SJ_IndexContext *sj_ic, Index *read_idx, uint64_t search_time, int mode)
{
    int high = sj_ic->index_num;
    int low = 0;
    int mid;

    uint64_t read_time = 0; // used to store the timecode members in a single 64 bits integer to facilitate comparison

    while (low <= high) {
        mid = (high + low) / 2;
        read_time = get_search_value(sj_ic->indexes[mid], mode);

        if (read_time == search_time) {
            *read_idx = sj_ic->indexes[mid];
            return mid;
        } else if (read_time > search_time) {
            high = mid - 1;
        } else if (read_time < search_time) {
            low = mid + 1;
        }
    }
    return -2;
}

// if there are no P or I frame before pos, function returns -1 
static av_always_inline int find_previous_key_frame(SJ_IndexContext sj_ic, int pos)
{
    int i;
    for (i = pos; sj_ic.indexes[i].pic_type == FF_B_TYPE && i > 0; i--);
    return i ? i : -1;
}

static int search_frame_dts(SJ_IndexContext *sj_ic, Index *read_idx, uint64_t search_time)
{
    int high = sj_ic->index_num;
    int low = 0;
    int mid;
    int pos = -2; // pos initialized with a value that won't be returned by find_previous_key_frame

    while (1) {
        mid = (high + low) / 2;
        int kf_pos = find_previous_key_frame(*sj_ic, mid);
        // if the same position is returned twice, then no matching index can be found 
        if (pos == kf_pos) {
            return -2;
        }
        pos = kf_pos;
        int i;
        for (i = pos + 1; i < sj_ic->index_num && sj_ic->indexes[i].pic_type == FF_B_TYPE; i++) {
            if (sj_ic->indexes[i].dts == search_time) {
                loop:
                *read_idx = sj_ic->indexes[i];
                return i;
            }
        }
        // the dts wasn't found in the set of B frames -> testing if it is in the key_frame that follows
        if (sj_ic->indexes[i].dts == search_time) {
            goto loop;
        }

        if (search_time > sj_ic->indexes[i].dts) {
            low = pos + 1;
        } else {
            high = pos - 1;
        }
    }
    return -2;
}

char sj_index_get_frame_type(Index idx)
{
    char frame_types[3] = {'I','P','B'};
    if (idx.pic_type > 0 && idx.pic_type < 4) {
        return frame_types[idx.pic_type - 1];
    }
    return 'U';
}

int sj_index_search(SJ_IndexContext *sj_ic, uint64_t search_time, Index *idx, Index *key_frame, uint64_t mode)
{
    if (mode != SJ_INDEX_TIMECODE_SEARCH && mode != SJ_INDEX_PTS_SEARCH && mode != SJ_INDEX_DTS_SEARCH) {
        return -4;  // invalid flag value
    }
    int pos;
    if (mode != SJ_INDEX_DTS_SEARCH) {
        pos = search_frame(sj_ic, idx, search_time, mode);
    } else {
        pos = search_frame_dts(sj_ic, idx, search_time);
    }
    if (idx->pic_type != FF_I_TYPE && pos >= 0) {
        find_I_frame(key_frame, *sj_ic, pos);
    }
    return pos; // pos = -2 if frame wasn't found
}

