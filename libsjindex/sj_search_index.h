#ifndef SJ_SEARCH_H
#define SJ_SEARCH_H

#define LIBSJINDEX_VERSION 0.0.1

#define SJ_INDEX_TIMECODE_SEARCH 1
#define SJ_INDEX_PTS_SEARCH 2
#define SJ_INDEX_DTS_SEARCH 4

typedef struct {
    uint64_t size;
    uint8_t version;
    uint64_t index_num;
    Index *indexes;
} SJ_IndexContext;

int sj_index_load(char *filename, SJ_IndexContext *sj_ic);
int sj_index_unload(SJ_IndexContext *sj_ic);
char sj_index_get_frame_type(Index idx);
int sj_index_search(SJ_IndexContext *sj_ic, uint64_t search_val, Index *idx, Index *key_frame, uint64_t flags);
#endif /* SJ_SEARCH_H */ 

