LDLIBS=-ltelldus-core -lmosquitto
CFLAGS=-Wall -std=gnu99 -g3 -Werror

all:	td-mqtt

td-mqtt:	td-mqtt.o

install:	td-mqtt
	install -d 		${DESTDIR}${prefix}/bin/
	install -s td-mqtt	${DESTDIR}${prefix}/bin/

clean:
	rm -f td-mqtt
	rm -f td-mqtt.o

.PHONY:	all install clean


