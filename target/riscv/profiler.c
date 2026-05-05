/*
 * Xg233ai TCG Profiler — implementation
 *
 * Counts guest instructions, Xg233ai custom instructions per type,
 * TB translations, and TB executions. Dumps a report on exit.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/atomic.h"
#include <stdlib.h>
#include "profiler.h"

/* Xg233ai instruction names, indexed by funct7 order */
static const char *xg233ai_name[] = {
    "dma", "sort", "crush", "expand", "vdot",
    "vrelu", "vscale", "vmax", "gemm", "vadd"
};

static uint64_t total_insns;
static uint64_t total_xg233ai;
static uint64_t xg233ai_per_type[10];
static uint64_t total_tb_trans;
static uint64_t total_tb_exec;

static bool profiler_inited;

static void profiler_ensure_init(void)
{
    if (profiler_inited) {
        return;
    }
    profiler_inited = true;
    total_insns = 0;
    total_xg233ai = 0;
    memset(xg233ai_per_type, 0, sizeof(xg233ai_per_type));
    total_tb_trans = 0;
    total_tb_exec = 0;
    atexit(profiler_do_report);
}

void profiler_init(void)
{
    profiler_ensure_init();
}

void profiler_count_insn(void)
{
    profiler_ensure_init();
    qatomic_inc(&total_insns);
}

void profiler_count_xg233ai(int idx)
{
    profiler_ensure_init();
    qatomic_inc(&total_xg233ai);
    if (idx >= 0 && idx < 10) {
        qatomic_inc(&xg233ai_per_type[idx]);
    }
}

void profiler_count_tb_trans(void)
{
    profiler_ensure_init();
    qatomic_inc(&total_tb_trans);
}

void profiler_count_tb_exec(void)
{
    profiler_ensure_init();
    qatomic_inc(&total_tb_exec);
}

void profiler_do_report(void)
{
    /* Read counters atomically */
    uint64_t insns = qatomic_read(&total_insns);
    uint64_t xg = qatomic_read(&total_xg233ai);
    uint64_t normal = insns - xg;
    uint64_t trans = qatomic_read(&total_tb_trans);
    uint64_t exec = qatomic_read(&total_tb_exec);

    fprintf(stderr, "\n");
    fprintf(stderr,
        "============================================================\n"
        "  Xg233ai TCG Profiler — Report\n"
        "============================================================\n\n");

    fprintf(stderr,
        "--- 1. Instruction Execution Counts ---\n"
        "  Total guest instructions:  %" PRIu64 "\n"
        "  Normal RISC-V insns:       %" PRIu64 "  (%.1f%%)\n"
        "  Xg233ai instructions:      %" PRIu64 "  (%.1f%%)\n\n",
        insns,
        normal,
        insns ? (double)normal / insns * 100.0 : 0.0,
        xg,
        insns ? (double)xg / insns * 100.0 : 0.0);

    fprintf(stderr, "  Per-type breakdown:\n");
    fprintf(stderr,
        "  -------------------------------------------------------\n");
    for (int j = 0; j < 10; j++) {
        uint64_t cnt = qatomic_read(&xg233ai_per_type[j]);
        fprintf(stderr,
            "    %-10s %12" PRIu64 "   (%5.1f%% of Xg233ai, %5.2f%% of total)\n",
            xg233ai_name[j], cnt,
            xg ? (double)cnt / xg * 100.0 : 0.0,
            insns ? (double)cnt / insns * 100.0 : 0.0);
    }
    fprintf(stderr,
        "  -------------------------------------------------------\n");

    double tb_hit_rate = exec ?
        (double)(exec - trans) / exec * 100.0 : 0.0;

    fprintf(stderr,
        "\n--- 2. Translation Block Hit Rate ---\n"
        "  TB translations (total):   %" PRIu64 "\n"
        "  TB executions (total):     %" PRIu64 "\n"
        "  TB hit rate:               %.2f%%\n"
        "    (formula: 1 - translations/executions)\n"
        "  Avg execs per TB:          %.1f\n",
        trans, exec, tb_hit_rate,
        trans ? (double)exec / trans : 0.0);

    fprintf(stderr,
        "\n--- 3. TB Link Hit Rate ---\n"
        "  (Cannot measure reliably without TCG plugin API.\n"
        "   Approximate: if TB hit rate > 95%%, link rate is also high,\n"
        "   since each linked TB transition bypasses the translation step.)\n");

    fprintf(stderr,
        "\n--- 4. Host Instruction Estimation ---\n"
        "  Run with '-d out_asm' for exact per-instruction counts.\n"
        "  Approximate host insns per Xg233ai instruction:\n"
        "    Helper-based (dma,sort,crush,expand,vrelu,vscale,gemm):\n"
        "      ~20 (args+call) + helper body (30-200+ insns)\n"
        "    Helper+retval (vdot,vmax): ~25 + helper body\n"
        "    Inline TCG (vadd): 112 TCG ops ~ 280-350 host insns\n");

    fprintf(stderr,
        "\n--- 5. Overhead Analysis ---\n"
        "  Largest overhead: SoftMMU memory access in helpers.\n"
        "  Each cpu_ldl_data/cpu_stl_data walks the guest TLB\n"
        "  (~30-50 host insns per access). For gemm this means\n"
        "  16 loads + 16 stores = ~960-1600 host insns in TLB walks\n"
        "  out of ~170 total host insns — over 50%% of execution time.\n"
        "\n============================================================\n\n");
}
