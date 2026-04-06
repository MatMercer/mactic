CC      = clang
CFLAGS  = -Wall -Wextra -O2
FRAMEWORKS = -framework CoreFoundation -framework IOKit
MT_FW   = /System/Library/PrivateFrameworks/MultitouchSupport.framework
LDFLAGS = -F$(dir $(MT_FW)) -framework MultitouchSupport $(FRAMEWORKS)

TARGET  = haptic

.PHONY: all clean

all: $(TARGET)

$(TARGET): haptic.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
