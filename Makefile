CFILES=m701.c
OFILES=${CFILES:.c=.o}
TARG=m701
CFLAGS=-std=c99
LIBS=-pthread -lncurses -lasound -lm -ldl
LDFLAGS= ${LIBS}
BINDIR?=/usr/local/bin

.SUFFIXES: .c .o
.PHONY: all clean install uninstall

.c.o:
	${CC} ${CFLAGS} -c $< -o $@

${TARG}: ${OFILES}
	${CC} ${OFILES} -o ${TARG} ${LDFLAGS}

all: ${TARG}

clean:
	rm -f ${TARG} ${OFILES}

install: all
	install -Dm755 ${TARG} ${BINDIR}/

uninstall:
	rm -f ${BINDIR}/${TARG}
