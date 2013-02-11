CFLAGS   = -Wall -O2 -g -Werror
LDLIBS   = -lpthread -lnuma
FILES    = miniprof.c miniprof.h

all: miniprof

miniprof: ${FILES}

tags: ${FILES}
	ctags --totals `find . -name '*.[ch]'`
	cscope -b -q -k -R -s.

clean:
	rm -f *.o miniprof tags cscope.*

.PHONY: all clean tags
