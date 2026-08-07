#pragma once

#include <stdint.h>

/* Where the frame loop currently is.
 *
 * When the loop wedges, both cores go idle - the task is parked in a driver
 * call, not spinning - so the task watchdog can only print a backtrace of
 * whatever idle task happens to be running, which says nothing about the
 * blocked task. A marker the loop updates as it goes, read by a monitor task
 * that is still schedulable, names the exact call instead. */
extern volatile const char *g_sand_stage;
extern volatile int32_t g_sand_detail;
extern volatile uint32_t g_sand_frame;

#define SAND_STAGE(s)           (g_sand_stage = (s))
#define SAND_STAGE_AT(s, n)     do { g_sand_detail = (n); g_sand_stage = (s); } while (0)
