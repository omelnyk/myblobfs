#!/bin/bash
gcc myblobfs.c -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -pthread -lfuse -lrt -lz -lmysqlclient -L/usr/lib/mysql/ -o myblobfs
