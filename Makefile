FILES = wrthog.c
OUT_BIN = wrthog
CCFLAGS = -std=gnu99 -Wall -pedantic

build: $(FILES)
				$(CC) $(CCFLAGS) -O3 -o $(OUT_BIN) $(FILES) -lcurl -pthread

clean:
				rm -f *.o wrthog

rebuild: clean build

debug: $(FILES)
				$(CC) $(CCFLAGS) -g -o $(OUT_BIN) $(FILES) -lcurl -pthread
