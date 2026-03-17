#include <stdio.h>
#include <stdint.h>

#define ITERS 1000000

// 使用 volatile 防止编译器将内存操作优化为寄存器操作
volatile uint32_t memory_array[1024];

int main() {
    uint32_t dep_sum = 0;
    uint32_t indep_sum = 0;

    printf("Starting Selective Replay Micro-benchmark...\n");

    for (uint32_t i = 0; i < ITERS; i++) {
        // 1. 生成伪随机索引，旨在迷惑 gem5 的 Store Set 预测器（内存依赖预测器）
        // 使得 Store 的地址计算变慢且不可预测
        uint32_t idx = (i * 73) % 1024;

        // 2. Store 操作
        memory_array[idx] = i;

        // 3. Load 操作 (潜在的内存依赖违例违规点 RAW Hazard)
        // 如果 O3CPU 预测器认为没有依赖，这个 Load 会越过上面的 Store 提前执行，导致拿到旧数据。
        // 当 Store 地址最终计算出来并发现冲突时，会触发 Squash。
        uint32_t val = memory_array[idx];

        // 4. 依赖指令链 (Dependent Chain)
        // 发生违例时，这些指令因为依赖了错误加载的 'val'，必须被 Selective Replay 重新执行
        uint32_t temp = val ^ 0xAAAAAAAA;
        temp = temp + (val << 2);
        dep_sum += temp;

        // 5. 独立指令链 (Independent Chain)
        // 核心测试点：这些指令只依赖 'i'，完全不依赖 'val'。
        // 在传统的 Full Flush (完全清空) 机制中，即使它们是正确的，也会因为在 Load 后面而被清空重做。
        // 在 Selective Replay 机制中，它们不应该被清空，从而节省大量的时钟周期！
        uint32_t indep_temp = i ^ 0x55555555;
        indep_temp = indep_temp + (i << 1);
        indep_sum += indep_temp;
    }

    // 打印结果，确保编译器不优化掉循环
    printf("Dependent Sum: %u\n", dep_sum);
    printf("Independent Sum: %u\n", indep_sum);
    printf("Benchmark Complete.\n");

    return 0;
}