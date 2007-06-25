#include <ffmpeg/avformat.h>
#include <stdlib.h>

#include "indexer.h"

#define INDEX_SIZE 29
#define HEADER_SIZE 9 

typedef struct{
    uint64_t size;
    ByteIOContext *pb;
    Timecode timecode;
} SearchContext;

int search_frame(SearchContext search, Index *read_idx)
{
    int low = 0;
    int mid = search.size / 2;
    ByteIOContext *seek_pb = NULL;
    uint32_t read_time, search_time; // used to store the timecode members in a single 32 bits integer to facilitate comparison 
    int nb_index = (int)(search.size / INDEX_SIZE);
    search_time = search.timecode.hours * 1000000 + search.timecode.minutes * 10000 + search.timecode.seconds * 100 + search.timecode.frames;

    printf("%d indexes\n", nb_index);

    seek_pb = search.pb;
    while (low <= search.size) {
        mid = (int)((search.size + low) / 2);
        mid -= (mid % INDEX_SIZE) - HEADER_SIZE ;

        url_fseek(seek_pb, mid, SEEK_SET);
        read_idx->pts = get_le64(seek_pb);
        read_idx->dts = get_le64(seek_pb);
        read_idx->pes_offset = get_le64(seek_pb);
        read_idx->pic_type = get_byte(seek_pb);
        read_idx->timecode.frames = get_byte(seek_pb);
        read_idx->timecode.seconds = get_byte(seek_pb);
        read_idx->timecode.minutes = get_byte(seek_pb);
        read_idx->timecode.hours = get_byte(seek_pb);

        read_time = read_idx->timecode.hours * 1000000 + read_idx->timecode.minutes * 10000 + read_idx->timecode.seconds * 100 + read_idx->timecode.frames;
        printf("");
        if (read_time == search_time){
            return 1;
        } else if (read_time > search_time) {
            search.size = mid - INDEX_SIZE;
        } else if (read_time < search_time) {
            low = mid + INDEX_SIZE;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    SearchContext search;
    ByteIOContext pb1;
    ByteIOContext mpeg1, *mpeg = NULL;
    Index read_idx;


    search.pb = &pb1;
    if (argc < 6) {
        printf("usage: search_idx <index file> <hours> <minutes> <seconds> <frames>\n");
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
    search.timecode.hours = atoi(argv[2]);
    search.timecode.minutes = atoi(argv[3]);
    search.timecode.seconds = atoi(argv[4]);
    search.timecode.frames = atoi(argv[5]);
    printf("Index size : %lld\n", search.size);
    printf("magic %llx\n", get_le64(search.pb));
    printf("Version : %d\n", get_byte(search.pb));
//    printf("Looking for frame with timecode : %02d:%02d:%02d:%02d\n",time.hours ,time.minutes ,time.seconds ,time.frames );


    int res = search_frame(search, &read_idx); 
    if (!res)
        printf("Frame could not be found, check input data\n");
    else
        printf("Offset : %lld\n", read_idx.pes_offset);
    url_fclose(search.pb);
    return 0;
}
