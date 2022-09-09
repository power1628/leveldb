FROM docker.io/library/ubuntu:focal

RUN apt update && \
		apt install -y software-properties-common

RUN apt-get install -y libgflags-dev libsnappy-dev zlib1g-dev libbz2-dev liblz4-dev libzstd-dev libbenchmark-dev 

RUN apt install -y net-tools vim git  build-essential cmake gdb

RUN apt install -y wget unzip

# install lmdb
RUN wget https://github.com/LMDB/lmdb/archive/refs/tags/LMDB_0.9.29.zip && \
    unzip LMDB_0.9.29.zip && \
    cd lmdb-LMDB_0.9.29/libraries/liblmdb && \
    make && make install  && \
    cd ../../../ && \
    rm LMDB_0.9.29.zip && \
    rm -rf lmdb-LMDB_0.9.29/ 

#RUN git clone	git@github.com:LMDB/lmdb.git && \
#    pushd libraries/liblmdb && \
#    make && make install


