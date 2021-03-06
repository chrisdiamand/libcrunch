THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))

LIBALLOCS_BASE ?= $(realpath ../../../liballocs.hg)

OCAMLFIND ?= ocamlfind
CILLY ?= cilly
CIL_TOOLS := trumptr
OCAMLFLAGS +=  -package findlib -package cil
OCAMLFLAGS += -I $(dir $(THIS_MAKEFILE))/lib -I $(LIBALLOCS_BASE)/tools/lang/c/lib #-I $(dir $(wildcard $(shell which $(CILLY))/../lib/ocaml/*/cil))

default: src lib cil

.PHONY: cil
cil: $(foreach t,$(CIL_TOOLS),$(t)/$(t).cmxs)

# until cilly can --load multiple plugins, we have to do this
%.cmxs: $(LIBALLOCS_BASE)/tools/lang/c/lib/cilallocs.cmx $(LIBALLOCS_BASE)/tools/lang/c/dumpallocs/dumpallocs.cmx %.cmx 
%.cmxs: $(LIBALLOCS_BASE)/tools/lang/c/lib/cilallocs.cmx %.cmx 
	$(OCAMLFIND) ocamlopt -shared -o "$@" $(OCAMLOPTFLAGS) $(OCAMLFLAGS) $+
%.cmx %.cmi: %.ml
	$(OCAMLFIND) ocamlopt -o "$@" $(OCAMLOPTFLAGS) $(OCAMLFLAGS) -c "$<"
%.cmo %.cmi: %.ml
	$(OCAMLFIND) ocamlc -o "$@" $(OCAMLFLAGS) -c "$<"

# for testing
%.cil.o: %.c
	cd "$(dir $<)" && $(CILLY) --do$$( echo $(dir $<) | tr -d '/' ) --save-temps -c -o "$(notdir $@)" "$(notdir $<)"

clean:
	for dir in $(CIL_TOOLS); do (cd $$dir && rm -f *.o *.cmo *.cma *.cmi *.cmx *.cmxa *.cmxs *.cil.c *.i ); done
	$(MAKE) -C src clean

.PHONY: sanity-tests
sanity-tests: | src lib cil
	$(MAKE) -C ../../test cleanrun-hello cleanrun-hello-errno cleanrun-hello-static cleanrun-hello-stack cleanrun-hello-heap

.PHONY: src
src:
	$(MAKE) -C src
