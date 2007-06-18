#include <ffmpeg/avformat.h>

int main(int argc, char **argv)
{
    ByteIOContext pb1, *pb = &pb1;
    ByteIOContext mpeg1, *mpeg = NULL;
//    uint8_t buffer[16];
//    offset_t header_offset;
//    offset_t first_offset;
//    uint64_t magic;

    if (argc < 2) {
        printf("usage: indexparse <index file>\n");
        return 1;
    }

    register_protocol(&file_protocol);
    if (argc == 3) {
        if (url_fopen(&mpeg1, argv[2], URL_RDONLY) < 0) {
            printf("error opening file %s\n", argv[2]);
            return 1;
        }
        mpeg = &mpeg1;
    }

    if (url_fopen(pb, argv[1], URL_RDONLY) < 0) {
        printf("error opening file %s\n", argv[1]);
        return 1;
    }


/*#define DUMP(offset) \
    url_fseek(mpeg, offset, SEEK_SET); \
    get_buffer(mpeg, buffer, 16); \
    av_hex_dump(stdout, buffer, 16); \*/
    printf("magic %llx\n", get_le64(pb));
    printf("Version : %d\n", get_byte(pb));
    while (!url_feof(pb)) {
        printf("-----------------------\n");
        printf("pts %lld\n", get_le64(pb));
        printf("dts %lld\n", get_le64(pb));
        printf("pes_offset %lld\n", get_le64(pb));
        printf("frame type %d\n", get_byte(pb));
        printf("Timecode : %02d:%02d:%02d:%02d\n", get_byte(pb), get_byte(pb), get_byte(pb), get_byte(pb));
    }
    url_fclose(pb);
    return 0;
}
