CC=gcc
CFLAGS=-Wall -O3 -fomit-frame-pointer
LDFLAGS=-lavformat
DESTDIR = /
all:		indexer indexparse

indexer: indexer.o
		$(CC) $(CFLAGS) $^  -o $@ $(LDFLAGS)

indexparse: indexparse.o
		$(CC) $(CFLAGS) $^  -o $@ $(LDFLAGS)

.c.o:
		$(CC) $(CFLAGS) -c $< -o $@

cleanall:	clean

install: indexer indexparse
		install -d $(DESTDIR)
		install -m 755 indexer indexparse $(DESTDIR)/usr/bin

clean:
		rm -f *.o *~
		rm -f indexer indexparse

tags:
		etags *.c *.h
