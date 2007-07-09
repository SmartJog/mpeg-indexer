CC=gcc
CFLAGS=-fPIC -DPIC -Wall -g -std=c99
LIBFLAGS=-shared -Wl
LDFLAGS=-lavformat -lavcodec -lavutil -lm 
DESTDIR = /usr/local/lib

MAJOR = 0
MINOR = 0
PATCH_LEVEL = 1

LIBSONAME_MAJOR = libsjindex.so.$(MAJOR)
LIBSONAME_FULL = libsjindex.so.$(MAJOR).$(MINOR).$(PATCH_LEVEL)
LIBSONAME = libsjindex.so

all:	$(LIBSONAME_FULL)	

.c.o:
		$(CC) $(CFLAGS) -c $< -o $@

$(LIBSONAME_FULL):	sj_search_index.o
		$(CC) $(LIBFLAGS),-soname,$@ $^ -o $@

cleanall:	clean

install: 
		install -d $(DESTDIR)
		install -m 755 $(LIBSONAME_FULL) $(DESTDIR) 
		ln -sf $(LIBSONAME_FULL) $(DESTDIR)/$(LIBSONAME_MAJOR)
		ln -sf $(LIBSONAME_MAJOR) $(DESTDIR)/$(LIBSONAME)

clean:
		rm -f *.o *~
		rm -f *.so* 

tags:
		etags *.c *.h
