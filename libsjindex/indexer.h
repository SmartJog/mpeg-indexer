#ifndef INDEXER_H
#define INDEXER_H

/**
 * Timecode structure : defines a frame time-code
 * using four 8 bits integers
 */
typedef struct {
    int8_t hours;
    int8_t minutes;
    int8_t seconds;
    int8_t frames;
} Timecode;

/**
 * Index structure references a frame's :
 * type,
 * PTS and DTS,
 * first transport packet's offset
 * timecode
 */
typedef struct {
    uint8_t pic_type;
    int64_t pts;
    int64_t dts;
    offset_t pes_offset;
    Timecode timecode;
} Index;

#endif
