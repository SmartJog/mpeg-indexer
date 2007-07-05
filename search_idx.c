#include <ffmpeg/avformat.h>
#include <stdlib.h>
#include <string.h>

#include "indexer.h"

#define INDEX_SIZE 29
#define HEADER_SIZE 9 

typedef struct{
    uint64_t size;
    ByteIOContext pb;
    uint64_t search_time;
    uint64_t index_pos;
    uint8_t start_at;
    int key_frame_num;
    char mode;
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

static int load_index(char *filename, SJ_IndexContext *sj_ic)
{
    register_protocol(&file_protocol);
    
    if (url_fopen(&sj_ic->pb, filename, URL_RDONLY) < 0) {
        return -1;
    }
    sj_ic->size = url_fsize(&sj_ic->pb) - HEADER_SIZE;
    sj_ic->index_num = (int)(sj_ic->size / INDEX_SIZE);
    sj_ic->indexes = av_malloc(sj_ic->index_num * sizeof(Index)); 

    printf("Index size : %lld\n", sj_ic->size);
    int64_t magic = get_le64(&sj_ic->pb);
    if (magic != 0x534A2D494E444558LL){
        return -2;
    }
    if (!sj_ic->index_num){
        return -4;
    }

    for(int i = 0; i < sj_ic->index_num; i++){
        read_index(&sj_ic->indexes[i], &sj_ic->pb);
    }
    return 0;
}

static uint64_t get_search_value(Index idx, SJ_IndexContext sj_ic)
{
    if (sj_ic.mode == 't') {
        return idx.timecode.hours << 24 & idx.timecode.minutes << 16 & idx.timecode.seconds << 8 & idx.timecode.frames;
    }
    else if (sj_ic.mode == 'p') {
        return idx.pts;
    }
    else if (sj_ic.mode == 'd') {
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
        return 1;
    } else if (read_time > sj_ic->search_time) {
        return -1;
    }
    while (low <= high) {
        mid = (int)((high + low) / 2);

        if (sj_ic->mode == 't') {
            read_time = sj_ic->indexes[mid]->timecode.hours << 24 & sj_ic->indexes[mid]->timecode.minutes << 16 & sj_ic->indexes[mid]->timecode.seconds << 8 & sj_ic->indexes[mid]->timecode.frames;
        }
        else {
            read_time = sj_ic->indexes[mid]->pts;
        }
        if (read_time == sj_ic->search_time){
            sj_ic->index_pos = mid;
            return 1;
        } else if (read_time > sj_ic->search_time) {
            high = mid - INDEX_SIZE;
        } else if (read_time < sj_ic->search_time) {
            low = mid + INDEX_SIZE;
        }
    }
    return 0;
}

int search_frame_dts(SJ_IndexContext *sj_ic, Index *read_idx)
{
    uint64_t i = sj_ic->index_binary_offset + INDEX_SIZE;
    uint64_t tmp_pts = read_idx->pts;
    ByteIOContext *seek_pb = &sj_ic->pb;
    while (i < sj_ic->size) {
        url_fseek(seek_pb, i, SEEK_SET);
        read_index(read_idx, seek_pb);
        // looks for the frame that has the dts we're looking for, it's located after the frame with that value as pts
        if (read_idx->dts == tmp_pts) {
            sj_ic->index_binary_offset = i; 
            return 1;
        }
        i += INDEX_SIZE;
    }
    return 0;
}

int find_previous_key_frame(Index *key_frame, Index read_idx, SJ_IndexContext sj_ic)
{
    int i = sj_ic.index_binary_offset - INDEX_SIZE;
    ByteIOContext *seek_pb = &sj_ic.pb;

    while (i >= 0){
        url_fseek(seek_pb, i, SEEK_SET);
        read_index(key_frame, seek_pb);
        if (key_frame->pic_type == 1){
            return 1; 
        }
        i -= INDEX_SIZE;
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

int main(int argc, char **argv)
{
    SJ_IndexContext sj_ic;
    Index read_idx;
    Index key_frame;

    if (argc < 4) {
        printf("usage: search_idx <parameter type> <index file> <hhmmssff>\n");
        printf("parameters types are :\n\t-t\ttimecode\n\t-p\tpts\n\t-d\tdts\n");
        return 1;
    }

    int len = strlen(argv[3]);
    for (int i = 0; i < len; i++){
        if (argv[3][i] < '0' || argv[3][i] > '9'){
            printf("search value must be integer\n");
            return 0;
        }
    }
    
    int load_res = load_index(argv[2], &sj_ic);
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
//    printf("Version : %d\n", get_byte(&sj_ic.pb));
    sj_ic.key_frame_num =0;
    int res = 0;
    sj_ic.mode = argv[1][1];
    switch(sj_ic.mode){
        case 't':
            if (strlen(argv[3]) != 8){
                printf("timecode is invalid\n\tmust be of the form : hhmmssff\n");
                return 0;
            }
            printf("Looking for frame with timecode : %c%c:%c%c:%c%c:%c%c\n",argv[3][0], argv[3][1], argv[3][2], argv[3][3], argv[3][4], argv[3][5], argv[3][6], argv[3][7]);
        case 'p':
            sj_ic.search_time = atoll(argv[3]);
            res = search_frame(&sj_ic, &read_idx); 
            break;
        case 'd':
            sj_ic.search_time = atoll(argv[3]);
            res = search_frame(&sj_ic, &read_idx);
            if (read_idx.pts != read_idx.dts && read_idx.dts != sj_ic.search_time) {
                res = search_frame_dts(&sj_ic, &read_idx); 
            }
            break;
    }
    if (!res){
        printf("Frame could not be found, check input data\n");
        return 1;
    } 

    if (res == -1) {
        printf("Video starts at\ntimecode\t%02d:%02d:%02d:%02d\nPTS\t\t%lld\nDTS\t\t%lld\n", read_idx.timecode.hours, read_idx.timecode.minutes, read_idx.timecode.seconds, read_idx.timecode.frames, read_idx.pts, read_idx.dts);
        return 1;
    }

    char frame = get_frame_type(read_idx);
    printf("\n------ %c-Frame -------\nTimecode : %02d:%02d:%02d:%02d\nDTS : %lld\nPTS : %lld\nOffset : %lld\n------------------\n",  get_frame_type(read_idx), read_idx.timecode.hours, read_idx.timecode.minutes, read_idx.timecode.seconds, read_idx.timecode.frames, read_idx.dts, read_idx.pts, read_idx.pes_offset);
    if (frame != 'I'){
        printf("\nClosest I-frame to the seeked frame: \n");
        find_previous_key_frame(&key_frame, read_idx, sj_ic);
        printf("\n------ %c-Frame -------\nTimecode : %02d:%02d:%02d:%02d\nDTS : %lld\nPTS : %lld\nOffset : %lld\n------------------\n",  get_frame_type(key_frame), key_frame.timecode.hours, key_frame.timecode.minutes, key_frame.timecode.seconds, key_frame.timecode.frames, key_frame.dts, key_frame.pts, key_frame.pes_offset);
    }

    url_fclose(&sj_ic.pb);
    return 0;
}
