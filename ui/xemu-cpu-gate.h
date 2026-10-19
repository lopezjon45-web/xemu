#ifndef XEMU_CPU_GATE_H
#define XEMU_CPU_GATE_H

#include <stdbool.h>

/* <dir>/cpu_gate.log, free with g_free() */
char *xemu_cpu_gate_log_path(const char *dir);

/*
 * The interface thread's loop in a gate build: pumps the window's events,
 * appends one line a second to the log, asks the emulator to quit after
 * `seconds`, and returns once `*qemu_exiting` is set.
 */
void xemu_cpu_gate_run(const char *log_path, int seconds, bool *qemu_exiting);

#endif
