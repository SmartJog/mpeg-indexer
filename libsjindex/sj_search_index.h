#ifndef SJ_SEARCH_H
#define SJ_SEARCH_H

#define LIBSJINDEX_MAJOR 0
#define LIBSJINDEX_MINOR 0
#define LIBSJINDEX_PATCH 1

#define LIBSJINDEX_VERSION ((LIBSJINDEX_MAJOR << 16) | (LIBSJINDEX_MINOR << 8) | LIBSJINDEX_PATCH)
#define SJ_INDEX_TIMECODE_SEARCH 1
#define SJ_INDEX_PTS_SEARCH 2
#define SJ_INDEX_DTS_SEARCH 4

/**
 * Index context, initialized with sj_index_load
 * Used in sj_index_search to find a frame
 */
typedef struct {
    uint64_t size; /// size of the input index file
    uint8_t version; /// version number of the index file
    int index_num; /// number of indexes in the file
    int64_t start_dts; /// dts of the first frame to be decoded
    int64_t start_pts; /// pts of the first frame to be displayed
    Timecode start_timecode; /// timecode of the first frame to be displayed
    Index *indexes; /// list of indexes read from the file
    char *filename; /// index file name
} SJ_IndexContext;

/**
 * Reads the content of an index file and initialises the SJ_IndexContext
 * with the file's content.
 */
int sj_index_load(char *filename, SJ_IndexContext *sj_ic);

/**
 * Resets the SJ_IndexContext (empties the list, set all other variables to 0.
 */
int sj_index_unload(SJ_IndexContext *sj_ic);

/**
 * Returns a char corresponding to the frame type (I, P, B, or U if the type could not be determined)
 * Used for display, no real use in the search.
 */
char sj_index_get_frame_type(Index idx);

/**
 * Searches the list of indexes in the SJ_IndexContext for the one that has search_time as a timecode, dts or pts
 * idx is the corresponding index if it was found.
 * key_frame is the related key_frame's index needed to decode the frame referenced by idx.
 *
 * The type of the value the function should look for is determined by mode :
 *      if mode = SJ_INDEX_TIMECODE_SEARCH then the function will look for a index with a timecode equal to search_time
 *      if mode = SJ_INDEX_PTS_SEARCH then the function will look for a index with a pts equal to search_time
 *      if mode = SJ_INDEX_DTS_SEARCH then the function will look for a index with a dts equal to search_time
 */
int sj_index_search(SJ_IndexContext *sj_ic, uint64_t search_time, Index *idx, Index *key_frame, uint64_t mode);

#endif /* SJ_SEARCH_H */

