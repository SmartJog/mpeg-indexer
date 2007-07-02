#include <ffmpeg/avformat.h>
#include <stdlib.h>

#include "indexer.h"

#define INDEX_SIZE 29
#define HEADER_SIZE 9 

typedef struct{
    uint64_t size;
    ByteIOContext *pb;
    uint32_t search_time;
    int index_binary_offset;
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
    seek_pb = search->pb;

    // Checks if the timecode we want is inferior to the first timecode in the file
    read_time = compute_idx(read_idx, seek_pb);
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

int find_previous_I_frame(Index *I_frame, Index read_idx, SearchContext search)
{
    int i = search.index_binary_offset - INDEX_SIZE;
    ByteIOContext *seek_pb = search.pb;

    while (i >= 0 && I_frame->pic_type != 1){
        url_fseek(seek_pb, i, SEEK_SET);
        compute_idx(I_frame, seek_pb);
        i -= INDEX_SIZE;
    }
    return 0;
}

int main(int argc, char **argv)
{
    SearchContext search;
    ByteIOContext pb1;
    ByteIOContext mpeg1, *mpeg = NULL;
    Index read_idx, I_frame;

    search.pb = &pb1;
    if (argc < 3) {
        printf("usage: search_idx <index file> <hhmmssff>\n");
        return 1;
    }

    register_protocol(&file_protocol);
    if (url_fopen(&mpeg1, argv[1], URL_RDONLY) < 0) {
        printf("error opening file %s\n", argv[1]);
        return 1;
    }
    mpeg = &mpeg1;

    if (url_fopen(search.pb, argv[1], URL_RDONLY) < 0) {
        printf("error opening file %s\n", argv[1]);
        return 1;
    }
    search.size = url_fsize(search.pb) - HEADER_SIZE;
    if (argv[2][8] != '\0'){
        printf("invalid time value\n\ttime must be input as follow : hhmmssff\n");
        return 0;
    }
    search.search_time = atoi(argv[2]);

    printf("Index size : %lld\n", search.size);
    int64_t magic = get_le64(search.pb);
    if (magic != 0x534A2D494E444558LL){
        printf("%s is not an index file.\n", argv[1]);
        return 1;
    }
//    printf("Version : %d\n", get_byte(search.pb));
    printf("Looking for frame with timecode : %c%c:%c%c:%c%c:%c%c\n",argv[2][0], argv[2][1], argv[2][2], argv[2][3], argv[2][4], argv[2][5], argv[2][6], argv[2][7]);

    int res = search_frame(&search, &read_idx); 
    if (!res){
        printf("Frame could not be found, check input data\n");
    } else if (res == -1) {
        printf("Video starts at %02d:%02d:%02d:%02d\n", read_idx.timecode.hours, read_idx.timecode.minutes, read_idx.timecode.seconds, read_idx.timecode.frames);
    } else {
        char frame = 'U';
        switch(read_idx.pic_type){
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
        if (frame != 'I'){
            find_previous_I_frame(&I_frame, read_idx, search);
            printf("------- Closest previous I-Frame : -------\n");
            printf("Offset : %lld\n", I_frame.pes_offset);
        }
        printf("\n------Frame-------\nDTS : %lld\nPTS : %lld\nType of frame : %c\n", read_idx.dts, read_idx.pts, frame);
        printf("Offset : %lld\n", read_idx.pes_offset);
    }
    url_fclose(search.pb);
    return 0;
}
