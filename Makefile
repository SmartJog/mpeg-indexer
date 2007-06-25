CC=gcc
CFLAGS=-Wall -O3 -fomit-frame-pointer
LDFLAGS=-lavformat -lavcodec -lavutil -lm
DESTDIR = /
all:		indexer indexparse search_idx

indexer: indexer.o
		$(CC) $(CFLAGS) $^  -o $@ $(LDFLAGS)

indexparse: indexparse.o
		$(CC) $(CFLAGS) $^  -o $@ $(LDFLAGS)

search_idx: search_idx.o
		$(CC) $(CFLAGS) $^  -o $@ $(LDFLAGS)
.c.o:
		$(CC) $(CFLAGS) -c $< -o $@

cleanall:	clean

install: indexer indexparse search_idx
		install -d $(DESTDIR)
		install -m 755 indexer indexparse search_idx $(DESTDIR)/usr/bin

clean:
		rm -f *.o *~
		rm -f indexer indexparse search_idx

tags:
		etags *.c *.h
