# hello1_rust: hello in Rust, built by gccrs (docs/rust-gccrs.md).
# Its own toolchain image, mmi-gccrs (Dockerfile here, on top of mmi-sdk).
#
#   make image    build mmi-gccrs (once; ~25 min: builds GCC 16.2's compilers)
#   make          build ../../build/hello1_rust
#   make check    ELF header, program headers and NEEDED of the binary
#   make shell    shell in the mmi-gccrs container

ROOT  := $(abspath $(CURDIR)/../..)
IMAGE := mmi-gccrs
OUT   := build/hello1_rust
RUN   := docker run --rm -u $(shell id -u):$(shell id -g) -v $(ROOT):/src -w /src $(IMAGE)

.PHONY: all image check shell
all: $(ROOT)/$(OUT)

image:
	docker build -t $(IMAGE) .

$(ROOT)/$(OUT): main.rs
	mkdir -p $(ROOT)/build
	$(RUN) shle-nto-gccrs -O2 -g -o $(OUT) src/hello1_rust/main.rs

check: $(ROOT)/$(OUT)
	$(RUN) sh -c 'file $(OUT); sh4-linux-gnu-readelf -h -l -d $(OUT) | grep -E "Type:|Entry|interpreter|LOAD|NEEDED|HASH|FLAGS|Flags"; sh4-linux-gnu-nm -u $(OUT)'

shell:
	docker run --rm -it -u $(shell id -u):$(shell id -g) -v $(ROOT):/src -w /src $(IMAGE) bash
