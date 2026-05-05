# QEMU TCG 翻译性能分析报告

## 1. 测试环境

| 项目 | 值 |
|------|-----|
| QEMU 版本 | 10.2.50 (camp-2026) |
| Host CPU | x86_64 (GCC 11.4.0, LLD 22.1.2) |
| Guest 架构 | RISC-V 64 (g233) |
| 测题 | test-insn-gemm, test-insn-vdot |
| 分析方法 | 嵌入式 Profiler（target/riscv/profiler.c）+ objdump 静态分析 |

---

## 2. 指令执行概况

### test-insn-gemm

| 指标 | 数值 |
|------|------|
| 总 guest 指令数 | 3,298 |
| 普通 RISC-V 指令 | 3,296 (99.94%) |
| Xg233ai 指令 | 2 (0.06%) |
| 其中 gemm | 2 (100% of Xg233ai) |

### test-insn-vdot

| 指标 | 数值 |
|------|------|
| 总 guest 指令数 | 1,109 |
| 普通 RISC-V 指令 | 1,108 (99.91%) |
| Xg233ai 指令 | 1 (0.09%) |
| 其中 vdot | 1 (100% of Xg233ai) |

**分析**：Xg233ai 指令在测试中占比极低（<0.1%），因为每个测题只调用 1-2 次自定义指令，其余均为 C 运行时初始化、循环控制、内存访问等普通 RISC-V 指令。在 AI 推理等密集计算场景中，Xg233ai 占比会大幅上升。

---

## 3. 翻译块（TB）效率

### test-insn-gemm

| 指标 | 数值 |
|------|------|
| TB 翻译总次数 | 79 |
| TB 执行总次数 | 679 |
| **TB 命中率** | **88.37%** |
| 平均每 TB 执行次数 | 8.6 |

### test-insn-vdot

| 指标 | 数值 |
|------|------|
| TB 翻译总次数 | 52 |
| TB 执行总次数 | 258 |
| **TB 命中率** | **79.84%** |
| 平均每 TB 执行次数 | 5.0 |

**分析**：

- TB 命中率 79-88% 对于短小的测试程序属于合理范围。启动时的大量 TB 从未被复用（crt0 初始化代码），拉低了整体命中率。
- 稳态下（main 函数循环），命中率接近 100%，因为热点 TB 被反复执行。
- 链接命中率无法通过嵌入式 profiler 精确测量（需要 TCG Plugin API 的 TB 执行回调来追踪跨 TB 跳转对），但可通过 TB 命中率间接推断：若某个 TB 的 trans_count=1 而 exec_count≫1，说明其前驱 TB 的链接长期有效，无需重新查找。

---

## 4. 单条 Xg233ai 指令的 Host 指令膨胀

通过 `objdump -d` 对 QEMU 二进制进行静态分析：

### helper_gemm（110 条 host 指令）

| 组成部分 | host 指令数 | 说明 |
|----------|------------|------|
| 函数序言/结语 | ~15 | 栈帧、canary |
| 加载 mat_a（16 元素）| 16 × 17 = 272 | `cpu_ldl_le_data` TLB 快速路径 |
| 加载 mat_b（16 元素）| 16 × 17 = 272 | 同上 |
| 4×4×4 矩阵乘法 | ~70 | 纯寄存器/栈算术 |
| 存储结果（16 元素）| 16 × 17 = 272 | `cpu_stl_le_data` TLB 快速路径 |
| TCG 调用开销 | ~25 | get_gpr × 3 + call + profiler |

**gemm 总计：~951 host 指令**

### helper_vdot（48 条 host 指令）

| 组成部分 | host 指令数 |
|----------|------------|
| 函数体 | 48 |
| 加载 vec_a（16 元素）| 16 × 17 = 272 |
| 加载 vec_b（16 元素）| 16 × 17 = 272 |
| TCG 调用开销（含 gen_set_gpr）| ~30 |

**vdot 总计：~622 host 指令**

### 对比总结

| 指令 | 类型 | helper 体 | softmmu 调用 | 估算总 host 指令 | 膨胀比 |
|------|------|----------|-------------|-----------------|--------|
| gemm | helper | 110 | 48 次 | **~951** | 1:951 |
| vdot | helper+retval | 48 | 32 次 | **~622** | 1:622 |
| vadd | 内联 TCG | N/A | 96 TCG ops | **~280-350** | 1:300 |
| dma/sort/crush/expand | helper | 变长 | 变长 | 估计 ~500-2000+ | — |

---

## 5. 开销瓶颈分析

### 最大开销环节：SoftMMU 内存访问

**结论：`cpu_ldl_data` / `cpu_stl_data` 的 TLB 遍历是当前实现中最大的性能开销。**

**数据支撑**：

1. **占比分析**：以 gemm 为例，helper_gemm 总计 ~951 条 host 指令，其中 softmmu 访问占 48 × 17 = **816 条（85.8%）**。即超过 85% 的 host 指令消耗在 guest 内存访问的地址翻译上，而非实际计算。

2. **根源**：所有 Xg233ai helper 均通过 `cpu_ldl_data` / `cpu_stl_data` 逐元素访问 guest 内存。每次调用：
   - 提取 guest 虚拟地址 → 查找 TLB → 计算 host 物理地址 → 执行实际 load/store
   - TLB 命中快速路径：~17 条 host 指令
   - TLB 未命中慢速路径：触发 page table walk，可达数百条指令

3. **次大开销**：helper 函数调用 ABI 开销（~20-30 条指令），包括寄存器保存/恢复、参数传递。对于轻量级指令（如 vrelu），调用开销可能超过实际计算。

4. **vadd 的特殊情况**：vadd 使用内联 TCG 避免了 helper 调用开销，但将 16 次 load+add+store 完全展开为 112 条 TCG ops（~300 条 host 指令），造成 I-cache 膨胀。每个 `qemu_ld_i32` / `qemu_st_i32` TCG op 同样走 softmmu 路径，未解决根本问题。

### 优化建议

| 优先级 | 方向 | 预期收益 |
|--------|------|---------|
| P0 | 在 helper 内批量翻译地址（一次性 TLB lookup 覆盖连续内存范围），然后直接使用 host 指针访问 | 消除 80%+ 的 softmmu 开销 |
| P1 | 将 gemm/vdot 等高频指令改为 TCG 内联 + guest 地址预翻译 | 消除 helper 调用 + 减少 TLB 查询次数 |
| P2 | 使用 TCG vector ops（如果有 SIMD host 支持）批量处理 | 进一步压缩 host 指令数 |
| P3 | 改善 TB 链接稳定性（减少 CF_INVALID 触发） | 提升 TB 命中率至 95%+ |

---

## 6. 附：复现方法

```bash
# 1. 编译 QEMU
make -f Makefile.camp build

# 2. 运行测题（profiler 已嵌入）
./build/qemu-system-riscv64 -M g233 -m 2G -display none -semihosting \
    -device loader,file=build/tests/gevico/tcg/riscv64-softmmu/test-insn-gemm

# 3. 静态分析 host 指令数
objdump -d --disassemble=helper_gemm build/qemu-system-riscv64
objdump -d --disassemble=helper_vdot build/qemu-system-riscv64
objdump -d --disassemble=cpu_ldl_le_data build/qemu-system-riscv64

# 4. 运行时 host 代码跟踪
./build/qemu-system-riscv64 ... -d out_asm -D host_asm.log
```
