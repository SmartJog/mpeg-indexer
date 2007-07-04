#include <ffmpeg/avformat.h>
#include <stdlib.h>

#include "indexer.h"

#define INDEX_SIZE 29
#define HEADER_SIZE 9 

typedef struct{
    uint64_t size;
    ByteIOContext pb;
    uint64_t search_time;
    int index_binary_offset;
    uint8_t start_at;
    int key_frame_num;
    char mode;
} SearchContext;

static av_always_inline int compute_idx(Index *read_idx, ByteIOContext *seek_pb)
{
    read_idx->pts = get_le64(seek_pb);
    read_idx->dts = get_le64(seek_pb);
    read_idx->pes_offset = get_le64(seek_pb);
    read_idx->pic_type = get_byte(seek_pb);
    read_idx->timecode.frames = get_byte(seek_pb);
    read_idx->timecode.seconds = get_byte(seek_pb);
    read_idx->timecode.minutes = get_byte(seek_pb);
    read_idx->timecode.hours = get_byte(seek_pb);
    return read_idx->timecode.hours * 1000000 + read_idx->timecode.minutes * 10000 + read_idx->timecode.seconds * 100 + read_idx->timecode.frames;
}

int search_frame(SearchContext *search, Index *read_idx)
{
    int low = 0;
    int mid = search->size / 2;
    ByteIOContext *seek_pb = NULL;
    uint32_t read_time; // used to store the timecode members in a single 32 bits integer to facilitate comparison 
    int nb_index = (int)(search->size / INDEX_SIZE);

    printf("%d indexes\n", nb_index);
    seek_pb = &search->pb;

    // Checks if the value we want is inferior or equal to the first value in the file
    url_fseek(seek_pb, HEADER_SIZE, SEEK_SET); // reads the first index in the file
    read_time = compute_idx(read_idx, seek_pb);
    if (search->mode == 'p') {                   // if the program was given a pts
        read_time = read_idx->pts;
    }

    if (read_time == search->search_time){
        return 1;
    } else if (read_time > search->search_time) {
        return -1;
    }
    while (low <= search->size) {
        mid = (int)((search->size + low) / 2);
        mid -= (mid % INDEX_SIZE) - HEADER_SIZE ;

        url_fseek(seek_pb, mid, SEEK_SET);
        read_time = compute_idx(read_idx, seek_pb);
        if (search->mode == 'p') {
            read_time = read_idx->pts;
        }
        if (read_time == search->search_time){
            search->index_binary_offset = mid;
            return 1;
        } else if (read_time > search->search_time) {
            search->size = mid - INDEX_SIZE;
        } else if (read_time < search->search_time) {
            low = mid + INDEX_SIZE;
        }
    }
    return 0;
}

int find_previous_key_frame(Index *key_frame, Index read_idx, SearchContext search)
{
    int i = search.index_binary_offset - INDEX_SIZE;
    int key_frame_nb = search.start_at;
    ByteIOContext *seek_pb = &search.pb;

    while (i >= 0){
        Index tmp = key_frame[key_frame_nb];
        url_fseek(seek_pb, i, SEEK_SET);
        compute_idx(&tmp, seek_pb);
        if (tmp.pic_type == 2){
            key_frame[key_frame_nb] = tmp;
            key_frame_nb++;
            if (!(key_frame_nb % 10)){
                key_frame = av_realloc(key_frame,(key_frame_nb + 10) *  sizeof(Index));
            }
        }
        else if (tmp.pic_type == 1){
            key_frame[key_frame_nb] = tmp;
            return key_frame_nb; 
        }
        i -= INDEX_SIZE;
    }
    return 0;
}

char get_frame_type(Index idx)
{
    char frame = 'U';
    switch(idx.pic_type){
        case 1 : 
            frame = 'I';
            break;
        case 2 : 
            frame = 'P';
            break;
        case 3 :
            frame = 'B';
            break;
        default :
            printf("type of frame unknown\n");
    }
    return frame;
}

Index * get_needed_frame(Index read_idx, SearchContext *search){
    char frame = get_frame_type(read_idx);
    Index *key_frame = NULL;
    printf("\n------ %c-Frame -------\nTimecode : %02d:%02d:%02d:%02d\nDTS : %lld\nPTS : %lld\nOffset : %lld\n------------------\n",  get_frame_type(read_idx), read_idx.timecode.hours, read_idx.timecode.minutes, read_idx.timecode.seconds, read_idx.timecode.frames, read_idx.dts, read_idx.pts, read_idx.pes_offset);
    if (frame != 'I'){
        key_frame = av_malloc(10 * sizeof(Index));
        search->start_at = 0;
        search->key_frame_num = -1;
        if (frame == 'B'){
            // B-frames need the P or I frame that follows to be decoded
            int pos = search->index_binary_offset;
            do{
                url_fseek(&search->pb, pos, SEEK_SET);
                compute_idx(&key_frame[0], &search->pb);
                pos += INDEX_SIZE; 
            } while (key_frame[0].pic_type == 3);
            search->start_at = 1;
        }
        printf("\nList of frames needed to decode the seeked frame: \n");
        search->key_frame_num = find_previous_key_frame(key_frame, read_idx, *search);
    }
    return key_frame;
}

int main(int argc, char **argv)
{
    SearchContext search;
    Index read_idx;
    Index *key_frame = NULL;
//    ByteIOContext pb1;
    //search.pb = &pb1;

    if (argc < 4) {
        printf("usage: search_idx <parameter type> <index file> <hhmmssff>\n");
        printf("parameters types are :\n\t-t\ttimecode\n\t-p\tpts\n\t-d\tdts\n");
        return 1;
    }

    register_protocol(&file_protocol);

    if (url_fopen(&search.pb, argv[2], URL_RDONLY) < 0) {
        printf("error opening file %s\n", argv[2]);
        return 1;
    }
    search.size = url_fsize(&search.pb) - HEADER_SIZE;

    search.search_time = atoll(argv[3]);

    printf("Index size : %lld\n", search.size);
    int64_t magic = get_le64(&search.pb);
    if (magic != 0x534A2D494E444558LL){
        printf("%s is not an index file.\n", argv[2]);
        return 1;
    }
//    printf("Version : %d\n", get_byte(&search.pb));
    search.key_frame_num =0;
    int res = 0;
    search.mode = argv[1][1];
    switch(search.mode){
        case 't':
            if (argv[3][8] != '\0'){
                printf("invalid time value\n\ttime_code must be input as follow : hhmmssff\n");
                return 0;
            }
            printf("Looking for frame with timecode : %c%c:%c%c:%c%c:%c%c\n",argv[3][0], argv[3][1], argv[3][2], argv[3][3], argv[3][4], argv[3][5], argv[3][6], argv[3][7]);
        case 'p':
            res = search_frame(&search, &read_idx); 
            break;
        case 'd':
          //  res = search_frame_by_dts(&search, &read_idx); 
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

    key_frame = get_needed_frame(read_idx, &search);
    int i;
    for (i = search.key_frame_num; i >= 0; i--){
        printf("\n------ %c-Frame -------\nTimecode : %02d:%02d:%02d:%02d\nDTS : %lld\nPTS : %lld\nOffset : %lld\n------------------\n",  get_frame_type(key_frame[i]), key_frame[i].timecode.hours, key_frame[i].timecode.minutes, key_frame[i].timecode.seconds, key_frame[i].timecode.frames, key_frame[i].dts, key_frame[i].pts, key_frame[i].pes_offset);
    }
    av_free(key_frame);
    url_fclose(&search.pb);
    return 0;
}
