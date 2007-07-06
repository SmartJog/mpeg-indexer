#!/bin/sh
#Shared sjindex library installation

# compiling the library
cd libsjindex && make

# copy library in /usr/local/ and headers in /usr/local/include
cp *.h /usr/local/include
mkdir /usr/local/lib/sjindex
cp libsjindex.so.* /usr/local/lib/sjindex/

# fixing up symbolic link and set up soname
/sbin/ldconfig -n /usr/local/lib/sjindex/

# setting up linker name
ln -sf /usr/local/lib/sjindex/libsjindex.so.0 /usr/local/lib/sjindex/libsjindex.so

cd .. && make
