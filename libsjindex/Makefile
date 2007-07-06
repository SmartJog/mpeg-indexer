CC=gcc
CFLAGS=-fPIC -Wall -g -std=c99
LIBFLAGS=-g -shared -Wl
LDFLAGS=-lavformat -lavcodec -lavutil -lm 
DESTDIR = /usr/local/
all:	libsjindex	

.c:
		$(CC) $(CFLAGS) -c $< 

libsjindex:	sj_search_index.o
		$(CC) $(LIBFLAGS),-soname,$@.so.0 -o $@.so.0.0 $^ -lc

cleanall:	clean

install: 
		cp *.h $(DESTDIR)/include
		cp libsjindex.so.* $(DESTDIR)
		/sbin/ldconfig -n $(DESTDIR)
		ln -sf $(DESTDIR)/libsjindex.so.0 $(DESTDIR)/libsjindex.so

clean:
		rm -f *.o *~
		rm -f *.so* 

tags:
		etags *.c *.h
