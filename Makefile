CFLAGS+=-std=c99 -O3 -Wall -Wextra -Werror -Wno-type-limits

TARGET:=interception-k2k

.PHONY: all
all: $(TARGET)

$(TARGET): k2k.c map-rules.h.in toggle-rules.h.in tap-rules.h.in
	$(CC) $(CFLAGS) k2k.c -o $@

.PHONY: clean
clean:
	rm -f $(TARGET)

.PHONY: install
install:
	install --strip $(TARGET) /opt

.PHONY: test
test:
	CFLAGS=-DVERBOSE make
	make install
	timeout 10 udevmon -c /etc/udevmon.yaml
