#include <ffmpeg/avformat.h>
#include <stdlib.h>
#include <string.h>

#include "libsjindex/indexer.h"
#include "libsjindex/sj_search_index.h"

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

    // Index file loading and checks 
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
    printf("Index size : %lld\n", sj_ic.size);
    search_val = atoll(argv[3]);
    uint64_t flags = atoll(argv[1]);

    // Search
    int res = sj_index_search(&sj_ic, search_val, &read_idx, &key_frame, flags);
    
    // Check search result
    if (res == -1){
        printf("Frame could not be found, check input data\n");
        return -2;
    } 

    if (res == -4) {
        printf("Invalid search mode\n");
        return -4;
    }

    printf("Frame %c : \t\ntimecode\t%02d:%02d:%02d:%02d\nPTS\t\t%lld\nDTS\t\t%lld\nPES-OFFSET\t\t%lld\n", sj_index_get_frame_type(read_idx) ,read_idx.timecode.hours, read_idx.timecode.minutes, read_idx.timecode.seconds, read_idx.timecode.frames, read_idx.pts, read_idx.dts, read_idx.pes_offset);
    printf("Related key-frame : \t\ntimecode\t%02d:%02d:%02d:%02d\nPTS\t\t%lld\nDTS\t\t%lld\nPES-OFFSET\t\t%lld\n", key_frame.timecode.hours,key_frame.timecode.minutes, key_frame.timecode.seconds, key_frame.timecode.frames, key_frame.pts, key_frame.dts, key_frame.pes_offset);
    sj_index_unload(&sj_ic);
    
    return 0;
}
