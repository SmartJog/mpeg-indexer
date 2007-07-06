CC=gcc
CFLAGS=-fPIC -DPIC -Wall -g -std=c99
LIBFLAGS=-shared -Wl
LDFLAGS=-lavformat -lavcodec -lavutil -lm 
DESTDIR = /usr/local/lib


LIBSONAME_MAJOR = libsjindex.so.0
LIBSONAME_FULL = libsjindex.so.0.0.1
LIBSONAME = libsjindex.so

all:	$(LIBSONAME_MAJOR)	

.c.o:
		$(CC) $(CFLAGS) -c $< -o $@

$(LIBSONAME_MAJOR):	sj_search_index.o
		$(CC) $(LIBFLAGS),-soname,$@ $^ -o $@

cleanall:	clean

install: 
		install -d $(DESTDIR)
		install -m 755 $(LIBSONAME_MAJOR) $(DESTDIR) 
		ln -sf $(LIBSONAME_MAJOR) $(DESTDIR)/$(LIBSONAME)
		ln -sf $(LIBSONAME_MAJOR) $(DESTDIR)/$(LIBSONAME_FULL)

clean:
		rm -f *.o *~
		rm -f *.so* 

tags:
		etags *.c *.h
