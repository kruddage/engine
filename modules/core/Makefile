CC	= clang
TARGET	= --target=wasm32-unknown-unknown
CFLAGS	= $(TARGET) -nostdlib -O2 -Wall -Wextra
LDFLAGS	= -Wl,--no-entry -Wl,--export=engine_ping

OUTDIR	= ../../public/wasm
OUT	= $(OUTDIR)/core.wasm
SRC	= engine.c

.PHONY: all clean

all: $(OUT)

$(OUTDIR):
	mkdir -p $(OUTDIR)

$(OUT): $(SRC) engine.h | $(OUTDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(OUT) $(SRC)

clean:
	rm -f $(OUT)
