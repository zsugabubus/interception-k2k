CFLAGS += -std=c99 -O3 -g -Wall -Wextra -Werror -Wno-type-limits
TIMEOUT ?= 10

CONFIG_DIR ?= examples
OUT_DIR := out
INSTALL_DIR ?= /opt/interception

TARGETS := $(addprefix $(OUT_DIR)/,$(notdir $(wildcard $(CONFIG_DIR)/*)))

.PHONY: all
all: $(TARGETS)

$(OUT_DIR)/%: k2k.c $(CONFIG_DIR)/%/map-rules.h.in $(CONFIG_DIR)/%/tap-rules.h.in $(CONFIG_DIR)/%/multi-rules.h.in | $(OUT_DIR)
	$(CC) $(CFLAGS) -I$(CONFIG_DIR) -I$(CONFIG_DIR)/$* $< -o $@

$(OUT_DIR):
	mkdir $@

%-rules.h.in:
	touch $@

.PHONY: clean
clean:
	rm -rf $(OUT_DIR)

.PHONY: install
install:
	# If you have run `make test` then do not forget to run `make clean` after. Otherwise you may install with debug logs on.
	install -D --strip -t $(INSTALL_DIR) $(TARGETS)

.PHONY: test
test:
	CFLAGS=-DVERBOSE make
	make install
	timeout $(TIMEOUT) udevmon -c /etc/udevmon.yaml
