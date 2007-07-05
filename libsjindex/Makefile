CC=gcc
CFLAGS=-fPIC -Wall -g -std=c99
LIBFLAGS=-g -shared -Wl
LDFLAGS=-lavformat -lavcodec -lavutil -lm 
DESTDIR = /
all:	libsjindex	

.c:
		$(CC) $(CFLAGS) -c $< 

libsjindex:	sj_search_index.o
		$(CC) $(LIBFLAGS),-soname,$@.so.0 -o $@.so.0.0 $^ -lc

cleanall:	clean

install: 

clean:
		rm -f *.o *~
		rm -f *.so* 

tags:
		etags *.c *.h
