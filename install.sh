#!/bin/sh
#Shared sj_index librarie installation

# compiling the library
cd libsjindex && make

# copy library in /usr/local/ and headers in /usr/local/include
cp *.h /usr/local/include
cp libsjindex.so.* /usr/local/

# fixing up symbolic link and set up soname
/sbin/ldconfig -n /usr/local/ 

# setting up linker name
ln -sf /usr/local/libsjindex.so.0 /usr/local/libsjindex.so

cd .. && make
