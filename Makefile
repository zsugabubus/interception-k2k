CFLAGS+=-std=c99 -O3 -Wall -Wextra -Wno-type-limits

TARGET:=interception-k2k

.PHONY: all
all: $(TARGET)

$(TARGET): k2k.c single-rules.h.in double-rules.h.in tap-rules.h.in
	$(CC) $(CFLAGS) k2k.c -o $@

.PHONY: clean
clean:
	rm $(TARGET)
