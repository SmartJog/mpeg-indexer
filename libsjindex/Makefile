CC=gcc
CFLAGS=-fPIC -Wall -g -std=c99
LIBFLAGS=-g -shared -Wl
LDFLAGS=-lavformat -lavcodec -lavutil -lm 
DESTDIR = /usr/local/lib

LIBSONAME = libsjindex.so.0.0.1
LIBREALNAME = libsjindex.so.0
LIBLINKERNAME = libsjindex.so
all:	libsjindex	

.c:
		$(CC) $(CFLAGS) -c $< 

libsjindex:	sj_search_index.o
		$(CC) $(LIBFLAGS),-soname,$@.so.0 -o $@.so.0.0.1 $^ -lc

cleanall:	clean

install: 
		install -d $(DESTDIR)
		install -t $(DESTDIR) $(LIBSONAME)
		ln -sf $(LIBSONAME) $(DESTDIR)/$(LIBREALNAME)
		ln -sf $(LIBSONAME) $(DESTDIR)/$(LIBLINKERNAME)

clean:
		rm -f *.o *~
		rm -f *.so* 

tags:
		etags *.c *.h
