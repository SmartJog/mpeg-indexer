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
    sj_ic->index_num = (int)(sj_ic->size / INDEX_SIZE);
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
    return 0;
}

static uint64_t get_search_value(Index idx, SJ_IndexContext sj_ic)
{
    if (sj_ic.mode == SJ_INDEX_TIMECODE_SEARCH) {
        return idx.timecode.hours * 1000000 + idx.timecode.minutes * 10000 + idx.timecode.seconds * 100 + idx.timecode.frames;
    }
    else if (sj_ic.mode == SJ_INDEX_PTS_SEARCH) {
        return idx.pts;
    }
    else if (sj_ic.mode == SJ_INDEX_DTS_SEARCH) {
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

    read_time = get_search_value(sj_ic->indexes[0], *sj_ic);
   
    // Checks if the value we want is inferior or equal to the first value in the file
    if (read_time == sj_ic->search_time) {
        *read_idx = sj_ic->indexes[0];
        return 1;
    } else if (read_time > sj_ic->search_time) {
        return -1;
    }
    while (low <= high) {
        mid = (int)((high + low) / 2);
        if (sj_ic->mode == SJ_INDEX_TIMECODE_SEARCH) {
            read_time = sj_ic->indexes[mid].timecode.hours * 1000000 + sj_ic->indexes[mid].timecode.minutes * 10000 +  sj_ic->indexes[mid].timecode.seconds * 100 + sj_ic->indexes[mid].timecode.frames;
        }
        else {
            read_time = sj_ic->indexes[mid].pts;
        }
        if (read_time == sj_ic->search_time) {
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

static int find_relative_key_frame(Index *key_frame, SJ_IndexContext sj_ic)
{
    // if the next I_frame has a dts inferior to the searched dts then this I_frame is the related key_frame 
    for (int i = sj_ic.index_pos; i < sj_ic.index_num; i++){
        if (sj_ic.indexes[i].pic_type == 1 && sj_ic.indexes[i].dts < sj_ic.indexes[sj_ic.index_pos].dts) {
            key_frame = &sj_ic.indexes[i];
            return 0;
        }
    }
    // otherwise, look before the searched frame
    for (int i = sj_ic.index_pos; i >= 0; i--) {
        if (sj_ic.indexes[i].pic_type == 1) {
            key_frame = &sj_ic.indexes[i];
            return 0;
        }
    }
    return 0;
}

char sj_index_get_frame_type(Index idx)
{
    char frame_types[3] = {'I','P','B'};
    if (idx.pic_type > 0 && idx.pic_type < 4) {
        return frame_types[idx.pic_type - 1];
    }
    return 'U';
}

int sj_index_search(SJ_IndexContext *sj_ic, uint64_t search_val, Index *idx, Index *key_frame, uint64_t flags)
{
    int res = 0;
    if (flags != SJ_INDEX_TIMECODE_SEARCH && flags != SJ_INDEX_PTS_SEARCH && flags != SJ_INDEX_DTS_SEARCH) {
        return -2;  // invalid flag value
    }
    sj_ic->mode = flags;
    sj_ic->search_time = search_val;
    res = search_frame(sj_ic, idx);

    if (flags == SJ_INDEX_DTS_SEARCH) {
        if (idx->pts != idx->dts && idx->dts != sj_ic->search_time) {
            res = search_frame_dts(sj_ic, idx);
        }
    }
    char frame = sj_index_get_frame_type(*idx);
    if (frame != 'I') {
        find_relative_key_frame(key_frame, *sj_ic);
    }

    return res; // res = 0 if frame wasn't found, -1 if the first value in the index is greater than the one we're looking for
}

