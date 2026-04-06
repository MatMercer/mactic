CC      = clang
CFLAGS  = -Wall -Wextra -O2
FRAMEWORKS = -framework CoreFoundation -framework IOKit
MT_FW   = /System/Library/PrivateFrameworks/MultitouchSupport.framework
LDFLAGS = -F$(dir $(MT_FW)) -framework MultitouchSupport $(FRAMEWORKS)

TARGET  = mactic

.PHONY: all clean

all: $(TARGET)

$(TARGET): mactic.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
