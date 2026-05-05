/*
 * Xg233ai TCG Profiler — lightweight inline profiling for RISC-V target
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TARGET_RISCV_PROFILER_H
#define TARGET_RISCV_PROFILER_H

#include "qemu/osdep.h"

void profiler_init(void);
void profiler_count_insn(void);           /* normal RISC-V instruction */
void profiler_count_xg233ai(int idx);     /* Xg233ai instruction 0..9 */
void profiler_count_tb_trans(void);       /* TB translated */
void profiler_count_tb_exec(void);        /* TB executed */
void profiler_do_report(void);            /* print report at exit */

#endif
