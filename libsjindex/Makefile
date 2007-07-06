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
		cp *.h /usr/local/include
		cp libsjindex.so.* /usr/local/
		/sbin/ldconfig -n /usr/local/
		ln -sf /usr/local/libsjindex.so.0 /usr/local/libsjindex.so

clean:
		rm -f *.o *~
		rm -f *.so* 

tags:
		etags *.c *.h
