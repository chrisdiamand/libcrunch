#CFLAGS += -std=gnu99 -g -O3 -fPIC
CFLAGS += -std=gnu99 -g -O0 -fPIC

# For maximum speed
CFLAGS   += -DUSE_SHARED_LIBRARY_MIN_ADDRESS_HACK
CXXFLAGS += -DUSE_SHARED_LIBRARY_MIN_ADDRESS_HACK

C_SRC := $(wildcard *.c)

default: libcrunch.so stubs.o

C_DEPS := $(patsubst %.c,.%.d,$(C_SRC))

DEPS :=$(C_DEPS)

$(C_DEPS): .%.d : %.c
	$(CC) -MM $(CFLAGS) "$<"  > "$@"

-include $(DEPS)

libcrunch.so: libcrunch.o
	$(CC) $(CFLAGS) -shared -o "$@" "$<" $(LDFLAGS) #-lheap_index_fast_hooks