CC = gcc
CFLAGS = -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -pthread -lfuse -lrt -lz -lmysqlclient -L/usr/lib/mysql/
LDFLAGS = -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -pthread -lfuse -lrt -lz -lmysqlclient -L/usr/lib/mysql/
BINDIR = /usr/local/bin
MANDIR = /usr/share/man/man1
OWNER = bin
GROUP = bin

all: src/myblobfs src/myblobfs.o

src/myblobfs.o: src/myblobfs.c

clean:
	rm -f src/myblobfs.o src/myblobfs

install: src/myblobfs
	install -c -o ${OWNER} -g ${GROUP} -m 755 src/myblobfs ${BINDIR}
	install -c -o ${OWNER} -g ${GROUP} -m 644 doc/myblobfs.man ${MANDIR}/myblobfs.1

