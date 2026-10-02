// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Debug build of host_main that prints the last 64 CPU PCs on exit:
//   cc -O1 -DTRACE -o host_trace host_trace.c && ./host_trace rom.a78 frames
#define main host_main_
#include "host_main.c"
#undef main
int main(int argc, char **argv) {
    int r = host_main_(argc, argv);
    for (int i = 0; i < 64; i++) printf("%04x ", trace_pc[(trace_n + i) & 63]);
    printf("\n");
    return r;
}
