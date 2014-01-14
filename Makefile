CFLAGS   = -Wall -O2 -g -Werror
LDLIBS   = -lpthread -lnuma

all: makefile.dep miniprof

makefile.dep: *.[Cch]
	(for i in *.[Cc]; do ${CC} -MM "$${i}" ${CFLAGS}; done) > $@
   
-include makefile.dep

miniprof: machine.o

tags: ${FILES}
	ctags --totals `find . -name '*.[ch]'`
	cscope -b -q -k -R -s.

clean:
	rm -f *.o miniprof tags cscope.*

.PHONY: all clean tags
