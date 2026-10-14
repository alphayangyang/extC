// 启动探针（task-23 阶段 2 口径）：只做 JVM 初始化 + main 返回，什么都不干。
// 用途：把 `digraph.java` 的"冷值"拆成「JVM 启动 + 第一遍」，让冷/暖差归因得清。
public final class empty {
    public static void main(String[] args) { }
}
