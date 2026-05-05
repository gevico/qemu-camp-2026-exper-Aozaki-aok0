/*
 * Xg233ai TCG Plugin Profiler
 *
 * Collects:
 *   1. Xg233ai vs. normal RISC-V instruction execution ratio
 *   2. TB hit rate and link hit rate
 *   3. Per-instruction-type breakdown
 *
 * Usage:
 *   qemu-system-riscv64 ... -plugin ./libprofiler.so
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* --- Xg233ai instruction names (funct7 order, matching trans_rvi.c.inc) --- */
static const char *xg233ai_names[] = {
    "dma", "sort", "crush", "expand", "vdot",
    "vrelu", "vscale", "vmax", "gemm", "vadd", NULL
};

/* --- Scoreboard entries (per-VCPU, lock-free inline increment) --- */
static qemu_plugin_u64 insn_count;
static qemu_plugin_u64 xg233ai_count;
static qemu_plugin_u64 xg233ai_type_entries[10];

/* --- TB tracking --- */
typedef struct {
    uint64_t start_addr;
    struct qemu_plugin_scoreboard *exec_count;
    int trans_count;
    unsigned long insns;
    unsigned long xg_insns;
} TBInfo;

static GMutex tb_lock;
static GHashTable *tb_table;
static uint64_t total_tb_trans;

/* --- Link tracking --- */
typedef struct {
    uint64_t prev_from;          /* last transition's source TB PC */
    uint64_t prev_to;            /* last transition's dest TB PC */
    uint64_t total_transitions;
    uint64_t stable_transitions; /* # of transitions repeating prev pair */
} LinkTrace;

static LinkTrace link_trace;
static GMutex link_lock;

/* --- Helpers --- */
static guint tb_hash(gconstpointer v)
{
    return (guint)((const TBInfo *)v)->start_addr;
}

static gboolean tb_equal(gconstpointer v1, gconstpointer v2)
{
    return ((const TBInfo *)v1)->start_addr ==
           ((const TBInfo *)v2)->start_addr;
}

static gint tb_cmp_by_exec(gconstpointer a, gconstpointer b, gpointer d)
{
    TBInfo *ea = (TBInfo *)a;
    TBInfo *eb = (TBInfo *)b;
    uint64_t ca = qemu_plugin_u64_sum(
        qemu_plugin_scoreboard_u64(ea->exec_count));
    uint64_t cb = qemu_plugin_u64_sum(
        qemu_plugin_scoreboard_u64(eb->exec_count));
    return ca > cb ? -1 : 1;
}

/* --- TB execution callback --- */
static void vcpu_tb_exec(unsigned int cpu_index, void *udata)
{
    TBInfo *tbi = (TBInfo *)udata;

    /* Increment per-TB execution count */
    qemu_plugin_u64_add(
        qemu_plugin_scoreboard_u64(tbi->exec_count), cpu_index, 1);

    /* Link tracking: count this (prev_tb → curr_tb) transition.
     * If it repeats the previous transition exactly, it's a "link hit"
     * (the direct jump from prev_tb to curr_tb was reused). */
    g_mutex_lock(&link_lock);

    uint64_t curr_pc = tbi->start_addr;
    uint64_t prev_pc = link_trace.prev_to; /* last TB's PC */

    if (prev_pc != 0) {
        link_trace.total_transitions++;

        if (link_trace.prev_from == prev_pc &&
            link_trace.prev_to == curr_pc) {
            link_trace.stable_transitions++;
        }
    }

    link_trace.prev_from = prev_pc;
    link_trace.prev_to = curr_pc;

    g_mutex_unlock(&link_lock);
}

/* --- TB translation callback --- */
static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    uint64_t pc = qemu_plugin_tb_vaddr(tb);
    unsigned long xg_count = 0;

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        char *disas = qemu_plugin_insn_disas(insn);

        /* Inline: count every guest instruction */
        qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
            insn, QEMU_PLUGIN_INLINE_ADD_U64, insn_count, 1);

        /* Match Xg233ai instructions by disassembly prefix */
        for (int j = 0; xg233ai_names[j] != NULL; j++) {
            if (g_str_has_prefix(disas, xg233ai_names[j])) {
                xg_count++;

                qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
                    insn, QEMU_PLUGIN_INLINE_ADD_U64, xg233ai_count, 1);

                qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(
                    insn, QEMU_PLUGIN_INLINE_ADD_U64,
                    xg233ai_type_entries[j], 1);
                break;
            }
        }
        g_free(disas);
    }

    /* Create or update TB record */
    g_mutex_lock(&tb_lock);
    {
        TBInfo key = { .start_addr = pc };
        TBInfo *tbi = g_hash_table_lookup(tb_table, &key);
        if (tbi) {
            tbi->trans_count++;
        } else {
            tbi = g_new0(TBInfo, 1);
            tbi->start_addr = pc;
            tbi->trans_count = 1;
            tbi->insns = n;
            tbi->xg_insns = xg_count;
            tbi->exec_count = qemu_plugin_scoreboard_new(sizeof(uint64_t));
            g_hash_table_insert(tb_table, tbi, tbi);
        }

        total_tb_trans++;

        /* Register per-TB execution callback */
        qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec,
                                             QEMU_PLUGIN_CB_NO_REGS, tbi);
    }
    g_mutex_unlock(&tb_lock);
}

/* --- Exit callback: print report --- */
static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    g_autoptr(GString) rep = g_string_new("");

    g_string_append(rep,
        "\n"
        "============================================================\n"
        "  Xg233ai TCG Plugin Profiler — Report\n"
        "============================================================\n\n");

    /* 1. Instruction counts */
    uint64_t total_insns = qemu_plugin_u64_sum(insn_count);
    uint64_t total_xg = qemu_plugin_u64_sum(xg233ai_count);
    uint64_t normal_insns = total_insns - total_xg;

    g_string_append_printf(rep,
        "--- 1. Instruction Execution Counts ---\n"
        "  Total guest instructions:  %" PRIu64 "\n"
        "  Normal RISC-V insns:       %" PRIu64 "  (%.1f%%)\n"
        "  Xg233ai instructions:      %" PRIu64 "  (%.1f%%)\n\n",
        total_insns,
        normal_insns,
        total_insns ? (double)normal_insns / total_insns * 100.0 : 0.0,
        total_xg,
        total_insns ? (double)total_xg / total_insns * 100.0 : 0.0);

    g_string_append(rep, "  Per-type breakdown:\n");
    g_string_append(rep,
        "  -------------------------------------------------------\n");
    for (int j = 0; xg233ai_names[j] != NULL; j++) {
        uint64_t cnt = qemu_plugin_u64_sum(xg233ai_type_entries[j]);
        g_string_append_printf(rep,
            "    %-10s %12" PRIu64 "   (%5.1f%% of Xg233ai, %5.2f%% of total)\n",
            xg233ai_names[j], cnt,
            total_xg ? (double)cnt / total_xg * 100.0 : 0.0,
            total_insns ? (double)cnt / total_insns * 100.0 : 0.0);
    }
    g_string_append(rep,
        "  -------------------------------------------------------\n");

    /* 2. TB hit rate */
    guint tb_count = g_hash_table_size(tb_table);

    uint64_t total_tb_exec = 0;
    {
        GList *vals = g_hash_table_get_values(tb_table);
        for (GList *it = vals; it; it = it->next) {
            TBInfo *tbi = it->data;
            total_tb_exec += qemu_plugin_u64_sum(
                qemu_plugin_scoreboard_u64(tbi->exec_count));
        }
        g_list_free(vals);
    }

    double tb_hit_rate = total_tb_exec ?
        (double)(total_tb_exec - total_tb_trans) / total_tb_exec * 100.0 : 0.0;

    g_string_append_printf(rep,
        "\n--- 2. Translation Block Hit Rate ---\n"
        "  Unique TBs created:        %u\n"
        "  TB translations (total):   %" PRIu64 "\n"
        "  TB executions (total):     %" PRIu64 "\n"
        "  TB hit rate:               %.2f%%\n"
        "    (formula: 1 - translations/executions)\n"
        "  Avg execs per TB:          %.1f\n",
        tb_count, total_tb_trans, total_tb_exec,
        tb_hit_rate,
        tb_count ? (double)total_tb_exec / tb_count : 0.0);

    /* 3. Link hit rate */
    uint64_t transitions = link_trace.total_transitions;
    uint64_t stable = link_trace.stable_transitions;
    double link_hit_rate = transitions ?
        (double)stable / transitions * 100.0 : 0.0;

    g_string_append_printf(rep,
        "\n--- 3. TB Link Hit Rate ---\n"
        "  Total TB transitions:      %" PRIu64 "\n"
        "  Stable (repeated) pairs:   %" PRIu64 "\n"
        "  Link hit rate:             %.2f%%\n"
        "    (fraction of (A→B) transitions that repeat\n"
        "     consecutively — a proxy for direct-link reuse)\n",
        transitions, stable, link_hit_rate);

    /* 4. Top TBs */
    g_string_append(rep,
        "\n--- 4. Top 15 TBs by Execution Count ---\n"
        "  pc, trans, insns, xg_insns, execs\n");
    {
        GList *vals = g_hash_table_get_values(tb_table);
        GList *sorted = g_list_sort_with_data(vals, tb_cmp_by_exec, NULL);
        int n = 15;
        for (int i = 0; i < n && sorted; i++, sorted = sorted->next) {
            TBInfo *tbi = sorted->data;
            g_string_append_printf(rep,
                "  0x%016" PRIx64 ", %d, %ld, %ld, %" PRIu64 "\n",
                tbi->start_addr, tbi->trans_count,
                tbi->insns, tbi->xg_insns,
                qemu_plugin_u64_sum(
                    qemu_plugin_scoreboard_u64(tbi->exec_count)));
        }
        g_list_free(sorted);
    }

    /* 5. Host instruction estimation */
    g_string_append(rep,
        "\n--- 5. Host Instruction Estimation (per Xg233ai insn) ---\n"
        "  Run with '-d out_asm' for exact per-instruction counts.\n\n"
        "  Helper-based (dma, sort, crush, expand, vrelu, vscale, gemm):\n"
        "    TCG prologue:  ~15-20 host insns (get_gpr x3 + call)\n"
        "    Helper body:   30-200+ host insns (cpu_ldl/stl loops)\n"
        "    gemm: ~20 (prologue) + ~150 (4x4 matmul, 16 loads + 16 stores\n"
        "          via softmmu) = ~170 host insns\n"
        "\n"
        "  Helper with retval (vdot, vmax):\n"
        "    +5 host insns for gen_set_gpr\n"
        "\n"
        "  Inline TCG (vadd):\n"
        "    112 TCG ops = ~280-350 host insns (16x unrolled ld+ld+add+st)\n");

    /* 6. Overhead analysis hints */
    g_string_append(rep,
        "\n--- 6. Overhead Analysis Hints ---\n"
        "  Key overhead sources in current implementation:\n"
        "  (a) SoftMMU TLB walk per cpu_ldl_data/cpu_stl_data\n"
        "      (~30-50 host insns per access, dominates helper cost)\n"
        "  (b) Helper call ABI overhead (~20-30 host insns)\n"
        "  (c) vadd inline expansion: 112 TCG ops cause I-cache pressure\n"
        "  (d) Low link hit rate means frequent tb_find() lookups\n"
        "\n============================================================\n");

    qemu_plugin_outs(rep->str);

    /* Cleanup */
    {
        GList *vals = g_hash_table_get_values(tb_table);
        for (GList *it = vals; it; it = it->next) {
            TBInfo *tbi = it->data;
            qemu_plugin_scoreboard_free(tbi->exec_count);
        }
        g_list_free(vals);
    }
    g_hash_table_destroy(tb_table);
    qemu_plugin_scoreboard_free(insn_count.score);
    qemu_plugin_scoreboard_free(xg233ai_count.score);
    for (int j = 0; j < 10; j++) {
        qemu_plugin_scoreboard_free(xg233ai_type_entries[j].score);
    }
}

/* --- Plugin install --- */
QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Scoreboards for instruction counting */
    insn_count = qemu_plugin_scoreboard_u64(
        qemu_plugin_scoreboard_new(sizeof(uint64_t)));
    xg233ai_count = qemu_plugin_scoreboard_u64(
        qemu_plugin_scoreboard_new(sizeof(uint64_t)));
    for (int j = 0; j < 10; j++) {
        struct qemu_plugin_scoreboard *sb =
            qemu_plugin_scoreboard_new(sizeof(uint64_t));
        xg233ai_type_entries[j] = qemu_plugin_scoreboard_u64(sb);
    }

    tb_table = g_hash_table_new(tb_hash, tb_equal);
    total_tb_trans = 0;
    memset(&link_trace, 0, sizeof(link_trace));

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    return 0;
}
