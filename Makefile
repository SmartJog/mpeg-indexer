CC=gcc
CFLAGS=-Wall -O3 -fomit-frame-pointer -std=c99
LDFLAGS=-lavformat -lavcodec -lavutil -lm -lsjindex
DESTDIR = /
all:		indexer indexparse search

indexer: indexer.o
		$(CC) $(CFLAGS) $^  -o $@ $(LDFLAGS)

indexparse: indexparse.o
		$(CC) $(CFLAGS) $^  -o $@ $(LDFLAGS)

search: search.o
		$(CC) $(CFLAGS) $^  -o $@ $(LDFLAGS)
.c.o:
		$(CC) $(CFLAGS) -c $< -o $@

cleanall:	clean

install: indexer indexparse search
		install -d $(DESTDIR)
		install -m 755 indexer indexparse search $(DESTDIR)/usr/bin

clean:
		rm -f *.o *~
		rm -f indexer indexparse search

tags:
		etags *.c *.h
