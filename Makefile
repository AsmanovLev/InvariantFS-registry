# InvariantFS-registry — WP60 Makefile
#
#   make            build the manager + static helpers + refresh index
#   make manager    build tools/invfs-pack (host C11)
#   make helpers    build the C packs statically (musl preferred)
#   make index      regenerate per-pack sha256 + codecpacks/index.json
#   make check      gen-index --check (validate required fields)
#   make test       run tools/test-invf-pack.sh (file:// repo, no VM)
#   make clean      remove built helpers + manager

CC      ?= cc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra
BIN     ?= build

.PHONY: all manager helpers index check test clean

all: manager helpers index check

manager: $(BIN)/invfs-pack

$(BIN)/invfs-pack: tools/invfs-pack.c
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -o $@ $<

helpers:
	@command -v musl-gcc >/dev/null 2>&1 && echo "musl-gcc: present" || echo "musl-gcc: absent (will try -static)"
	CFLAGS="$(CFLAGS)" bash tools/build-helpers.sh

index:
	python3 tools/gen-index.py --root . --platform "$${INVFS_PACK_PLATFORM:-$$(uname -m)-linux-musl}"

check:
	python3 tools/gen-index.py --root . --platform "$${INVFS_PACK_PLATFORM:-$$(uname -m)-linux-musl}" --check

test: $(BIN)/invfs-pack
	bash tools/test-invf-pack.sh

clean:
	rm -rf $(BIN)
	find codecpacks -type d -name bin -prune -exec rm -rf {} +
