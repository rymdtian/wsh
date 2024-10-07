CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic -std=gnu18
LOGIN = rtian
SUBMITPATH = ~cs537-1/handin/$(LOGIN)/p3

.PHONY: all

all: wsh wsh-dbg

wsh: wsh.c wsh.h
	$(CC) $(CLFAGS) -O2 -o $@ $<

wsh-dbg: wsh.c wsh.h
	$(CC) $(CFLAGS) -Og	-ggdb -o $@ $<

clean:
	rm -rf wsh wsh-dbg*

submit:
	cp -r .. $(SUBMITPATH)
