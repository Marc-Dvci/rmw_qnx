# Builds pinv for QNX 8.0 x86_64 with qcc. Source qnxsdp-env.sh first.
OUT ?= ../../qnx/build/x86_64
CC = qcc -Vgcc_ntox86_64
CFLAGS = -O2 -Wall -Wextra -std=gnu11

$(OUT)/pinv: pinv.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $< -lsocket

clean:
	rm -f $(OUT)/pinv

.PHONY: clean
