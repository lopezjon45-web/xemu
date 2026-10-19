/*
 * CPU gate: xemu with the null renderer, no OpenGL and no interface, that
 * writes once a second how many frames the guest flipped, then quits by
 * itself. It answers whether a console's CPU can run a game at speed before
 * anyone writes a renderer for that console. Built with -DXEMU_CPU_GATE=1,
 * see the XEMU_CPU_GATE blocks in ui/xemu.c.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/core/cpu.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "hw/xbox/nv2a/debug.h"
#include "xemu-cpu-gate.h"
#include "xemu-settings.h"
#include "xemu-os-utils.h"
#include "xemu-version.h"
#include <SDL3/SDL.h>
#include <windows.h>

static double filetime_seconds(FILETIME kernel, FILETIME user)
{
    ULARGE_INTEGER k = { .LowPart = kernel.dwLowDateTime, .HighPart = kernel.dwHighDateTime };
    ULARGE_INTEGER u = { .LowPart = user.dwLowDateTime, .HighPart = user.dwHighDateTime };
    return (k.QuadPart + u.QuadPart) / 1e7;
}

/* CPU seconds used so far by the thread that runs the emulated CPU (TCG), or -1 before it exists */
static double vcpu_cpu_seconds(void)
{
    static HANDLE vcpu;
    if (!vcpu) {
        CPUState *cpu = first_cpu;
        if (!cpu || !cpu->thread_id) {
            return -1;
        }
        vcpu = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, cpu->thread_id);
        if (!vcpu) {
            return -1;
        }
    }
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(vcpu, &created, &exited, &kernel, &user)) {
        return -1;
    }
    return filetime_seconds(kernel, user);
}

static double process_cpu_seconds(void)
{
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return -1;
    }
    return filetime_seconds(kernel, user);
}

char *xemu_cpu_gate_log_path(const char *dir)
{
    return g_build_filename(dir, "cpu_gate.log", NULL);
}

void xemu_cpu_gate_run(const char *log_path, int seconds, bool *qemu_exiting)
{
    FILE *log = fopen(log_path, "w");
    if (!log) {
        fprintf(stderr, "cpu gate: cannot write %s\n", log_path);
    }
    const char *disc = g_config.sys.files.dvd_path;
    fprintf(stderr, "cpu gate: %s for %d s, log %s\n", disc && *disc ? disc : "(no disc)", seconds, log_path);
    if (log) {
        fprintf(log, "# xemu cpu gate: version %s commit %s, cpu \"%s\", %d logical cores, renderer null, "
                     "run %d s, disc %s\n",
                xemu_version, xemu_commit, xemu_get_cpu_info(), SDL_GetNumLogicalCPUCores(), seconds,
                disc && *disc ? disc : "(none)");
        fprintf(log, "# t: seconds since start; flips: guest frames flipped in that second (null renderer); "
                     "virt/real: emulated clock advance over wall clock; vcpu_cpu, proc_cpu: host CPU seconds "
                     "used in that second by the TCG thread and by the whole process; run: 1 while the "
                     "emulated console runs\n");
        fflush(log);
    }

    int64_t start_real = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t last_real = start_real;
    int64_t last_virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned last_flips = qatomic_read(&g_nv2a_stats.frame_count);
    double last_vcpu = vcpu_cpu_seconds();
    double last_proc = process_cpu_seconds();
    int t = 0;
    bool asked_to_stop = false;
    int64_t stop_asked_at = 0;

    while (!qatomic_read(qemu_exiting)) {
        SDL_PumpEvents();
        SDL_Delay(20);
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (now - last_real < NANOSECONDS_PER_SECOND) {
            continue;
        }
        t++;
        int64_t virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        unsigned flips = qatomic_read(&g_nv2a_stats.frame_count);
        double vcpu = vcpu_cpu_seconds();
        double proc = process_cpu_seconds();
        double real_s = (now - last_real) / 1e9;
        if (log) {
            fprintf(log, "t=%d flips=%u virt/real=%.3f vcpu_cpu=%.2f proc_cpu=%.2f run=%d\n", t,
                    flips - last_flips, (virt - last_virt) / 1e9 / real_s,
                    vcpu < 0 || last_vcpu < 0 ? -1.0 : vcpu - last_vcpu,
                    proc < 0 || last_proc < 0 ? -1.0 : proc - last_proc, runstate_is_running());
            fflush(log);
        }
        last_real = now;
        last_virt = virt;
        last_flips = flips;
        last_vcpu = vcpu;
        last_proc = proc;

        if (!asked_to_stop && now - start_real >= (int64_t)seconds * NANOSECONDS_PER_SECOND) {
            asked_to_stop = true;
            stop_asked_at = now;
            if (log) {
                fprintf(log, "# done: %u flips in %d s, quitting\n", flips, t);
                fflush(log);
            }
            fprintf(stderr, "cpu gate: done, quitting\n");
            shutdown_action = SHUTDOWN_ACTION_POWEROFF;
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
        } else if (asked_to_stop && now - stop_asked_at > 15 * NANOSECONDS_PER_SECOND) {
            if (log) {
                fprintf(log, "# the emulator did not stop within 15 s, exiting hard\n");
                fclose(log);
            }
            exit(0);
        }
    }
    if (log) {
        fclose(log);
    }
}
