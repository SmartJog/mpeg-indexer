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
        if (tmp.pic_type == 1){
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

int main(int argc, char **argv)
{
    SearchContext search;
    Index read_idx;
    Index key_frame;
//    ByteIOContext pb1;
    //search.pb = &pb1;

    if (argc < 4) {
        printf("usage: search_idx <parameter type> <index file> <hhmmssff>\n");
        printf("parameters types are :\n\t-t\ttimecode\n\t-p\tpts\n\t-d\tdts\n");
        return 1;
    }

    register_protocol(&file_protocol);
    int i = 0;
    for (i = 0; i < strlen(argv[3]); i++){
        if (argv[3][i] < '0' || argv[3][i] > '9'){
            printf("search value must be integer\n");
            return 0;
        }
    }

    if (url_fopen(&search.pb, argv[2], URL_RDONLY) < 0) {
        printf("error opening file %s\n", argv[2]);
        return 1;
    }
    search.size = url_fsize(&search.pb) - HEADER_SIZE;

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
            if (strlen(argv[3]) != 8){
                printf("timecode is invalid\n\tmust be of the form : hhmmssff\n");
                return 0;
            }
            printf("Looking for frame with timecode : %c%c:%c%c:%c%c:%c%c\n",argv[3][0], argv[3][1], argv[3][2], argv[3][3], argv[3][4], argv[3][5], argv[3][6], argv[3][7]);
        case 'p':
            search.search_time = atoll(argv[3]);
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

    char frame = get_frame_type(read_idx);
    printf("\n------ %c-Frame -------\nTimecode : %02d:%02d:%02d:%02d\nDTS : %lld\nPTS : %lld\nOffset : %lld\n------------------\n",  get_frame_type(read_idx), read_idx.timecode.hours, read_idx.timecode.minutes, read_idx.timecode.seconds, read_idx.timecode.frames, read_idx.dts, read_idx.pts, read_idx.pes_offset);
    if (frame != 'I'){
        printf("\nClosest I-frame to the seeked frame: \n");
        search.key_frame_num = find_previous_key_frame(&key_frame, read_idx, search);
        printf("\n------ %c-Frame -------\nTimecode : %02d:%02d:%02d:%02d\nDTS : %lld\nPTS : %lld\nOffset : %lld\n------------------\n",  get_frame_type(key_frame), key_frame.timecode.hours, key_frame.timecode.minutes, key_frame.timecode.seconds, key_frame.timecode.frames, key_frame.dts, key_frame.pts, key_frame.pes_offset);
    }

    url_fclose(&search.pb);
    return 0;
}
