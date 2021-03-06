CFLAGS += -D_GNU_SOURCE # HACK
CFLAGS += -g3 -gstrict-dwarf -std=c99 -fno-eliminate-unused-debug-types -O2
CFLAGS += -DUSE_STARTUP_BRK

THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))

export CFLAGS

UNIQTYPES_BASE ?= /usr/lib/allocsites
ALLOCSITES_BASE ?= /usr/lib/allocsites
LIBALLOCS ?= $(realpath $(dir $(THIS_MAKEFILE))/../../liballocs.hg)

export UNIQTYPES_BASE
export ALLOCSITES_BASE

export CC := $(realpath $(dir $(THIS_MAKEFILE))/../frontend/c/bin/crunchcc)
export CXX := $(realpath $(dir $(THIS_MAKEFILE))/../frontend/c++/bin/crunchc++)
export CFLAGS += -I$(realpath $(dir $(THIS_MAKEFILE)))/../include -I$(LIBALLOCS)/include
export LDFLAGS += -L$(LIBALLOCS)/lib

ifneq ($(USE_FAKE_LIBUNWIND),)
CFLAGS += -fno-omit-frame-pointer
CXXFLAGS += -fno-omit-frame-pointer
LIBUNWIND_LDLIBS :=
else
LIBUNWIND_LDLIBS := -lunwind -lunwind-`uname -m` 
endif

ifeq ($(CC),)
$(error Could not find crunchcc)
endif
ifeq ($(CXX),)
$(error Could not find crunchc++)
endif

cases := $(filter-out checks.sh,$(wildcard [-a-z]*))

ifneq ($(LIBCRUNCH_USE_PRELOAD),no)
LIBCRUNCH := $(realpath $(dir $(THIS_MAKEFILE))/../lib/libcrunch_preload.so)
else
LIBCRUNCH := $(realpath $(dir $(THIS_MAKEFILE))/../lib/libcrunch.so)
endif
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(LIBCRUNCH)),)
        $(error You must first build libcrunch{.so,_preload.so} and link them from ../lib)
endif
endif

ifneq ($(LIBCRUNCH_USE_PRELOAD),no)
export PRELOAD := "$(LIBCRUNCH)"
else
LIBHEAP_INDEX_HOOKS ?= $(realpath $(dir $(THIS_MAKEFILE))/../../libpmirror.hg/lib/libheap_index_fast_hooks.so)
# FIXME: do we really need to preload libcrunch in this case?
export PRELOAD := "$(LIBHEAP_INDEX_HOOKS) $(LIBCRUNCH)"
endif

INCLUDE_MK_INC = `if test -e $(dir $(realpath $(THIS_MAKEFILE)))/$*/mk.inc; then /bin/echo -f mk.inc; else true; fi`

default: checkall

checkall: $(patsubst %,checkrun-%,$(cases))

# Some cases are expected to fail
checkrun-fail-%:
	@echo "expected-fail case is fail-$*" && \
	out=$$( echo -n "out is " 1>&2; mktemp | tee /dev/stderr ) && \
	err=$$( echo -n "err is " 1>&2; mktemp | tee /dev/stderr ) && \
	$(MAKE) cleanrun-fail-$* 1>$$out 2>$$err && \
		. ./checks.sh && tail -n26 "$$err" | \
		    match "checks failed otherwise" '[1-9][0-9]*' >/dev/null || \
		(tail -n26 $$err; false)

# Most test cases should output a libcrunch summary in which 
# -- zero or more checks passed (trivially or by lazy typing)
# -- zero checks aborted
# -- zero checks failed
checkrun-%:
	@echo "case is $*" && \
	out=$$( echo -n "out is " 1>&2; mktemp | tee /dev/stderr ) && \
	err=$$( echo -n "err is " 1>&2; mktemp | tee /dev/stderr ) && \
	$(MAKE) cleanrun-$* 1>$$out 2>$$err && \
		. ./checks.sh && tail -n26 "$$err" | \
		    match "checks nontrivially passed" '[1-9][0-9]*' | \
		    match "checks failed inside"        '0' | \
		    match "checks failed otherwise"     '0' >/dev/null || \
		(tail -n26 $$err; false)

# Some cases are special and require only an exit status of zero
checkrun-hello: 
	$(MAKE) cleanrun-hello >/dev/null 2>&1
checkrun-hello-c++: 
	$(MAKE) cleanrun-hello-c++ >/dev/null 2>&1
checkrun-hello-errno: 
	$(MAKE) cleanrun-hello-errno >/dev/null 2>&1
checkrun-hello-qualified-char:
	$(MAKE) cleanrun-hello-qualified-char >/dev/null 2>&1
checkrun-random:
	$(MAKE) cleanrun-random >/dev/null 2>&1
checkrun-pointer-degree:
	$(MAKE) cleanrun-pointer-degree >/dev/null 2>&1

# Some cases are special and have their own matching criteria
checkrun-lazy-typing:
	@echo "case is lazy-typing" && \
	out=$$( echo -n "out is " 1>&2; mktemp | tee /dev/stderr ) && \
	err=$$( echo -n "err is " 1>&2; mktemp | tee /dev/stderr ) && \
	$(MAKE) cleanrun-lazy-typing 1>$$out 2>$$err && \
		. ./checks.sh && tail -n26 "$$err" | \
		    match "of which did lazy heap type assignment" '[1-9][0-9]*' | \
		    match "checks failed inside"        '0' | \
		    match "checks failed otherwise"     '0' >/dev/null || \
		(tail -n26 $$err; false)

_onlyrun-%:
	LD_PRELOAD=$(PRELOAD) ./$*

_onlygdbrun-%:
	gdb --eval-command "set environment LD_PRELOAD=$(PRELOAD)" --args ./$* ./$*

build-%:
	$(MAKE) -C "$*" $(INCLUDE_MK_INC) "$*" 

run-%:
	$(MAKE) build-$* && ( $(MAKE) -C "$*" $(INCLUDE_MK_INC) -f ../Makefile _onlyrun-$* )

cleanbuild-%:
	$(MAKE) -C $* $(INCLUDE_MK_INC) -f ../Makefile clean && \
	$(MAKE) build-$* 

cleanrun-%: 
	$(MAKE) -C $* $(INCLUDE_MK_INC) -f ../Makefile clean && \
	$(MAKE) run-$*

gdbrun-%: # run the test case with itself as input
	$(MAKE) -C "$*" $(INCLUDE_MK_INC) "$*" && ( cd "$*" && $(MAKE) $(INCLUDE_MK_INC) -f ../Makefile _onlygdbrun-$* )

gdbcleanrun-%:
	$(MAKE) -C $* $(INCLUDE_MK_INC) -f ../Makefile clean && $(MAKE) gdbrun-$*

clean-%:
	$(MAKE) -C "$*" $(INCLUDE_MK_INC) -f $(realpath $(THIS_MAKEFILE)) clean

default:
	for case in $(cases); do \
            $(MAKE) run-$$case; \
        done

# generic clean rule that we can run from test dirs too (with $(MAKE) -f ../Makefile)
clean: # (delete anything whose name is a prefix of a .c file's and doesn't contain a dot)
	rm -f $(filter-out .,$(patsubst %.c,%,$(shell find -name '*.c')))
	rm -f $(filter-out .,$(patsubst %.cc,%,$(shell find -name '*.cc')))
	find -name '*.cil.*'  -o -name '*.i' -o -name '*.o' -o -name '*.s' -o -name '*.allocs' | xargs rm -f
