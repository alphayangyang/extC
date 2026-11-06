# logstat — 一个能跑的真程序

读一份 TSV 日志，流式聚合成一张报表；自带数据生成器，也能把报表用 HTTP 提供出来。
一个文件、零依赖，`extc run examples/logstat -- …` 就能跑。

```
ts <TAB> path <TAB> status <TAB> lat_us <TAB> bytes
```

## 跑

```sh
./build/extc run examples/logstat -- gen access.tsv 2000000    # 写 ~78 MB
./build/extc run examples/logstat -- summary access.tsv
./build/extc run examples/logstat -- serve access.tsv 8080 &   # 报表已渲染，直接应答
curl -s localhost:8080/summary     # JSON
curl -s localhost:8080/text        # 与人看的同一张表
```

## 它演示了什么

- **内存不随输入长**：256 KiB 块流式读，行在原地解析，聚合表有界（状态表 600 项、
  延迟 32 桶、路径 4096 条封顶后计入 `other`）。实测峰值 RSS **2.2 MB**，
  0.7 MB 与 388 MB 的输入一样。
- **没有逐记录分配**：每行不 `new` 任何东西，解析是扫字节；报表只在最后渲染一次。
- **协程服务**：`serve` 一个线程、一个 epoll 循环、每连接一个协程，没有线程池；
  应答从预渲染的缓冲里拷出去。
- **数字可复核**：`tests/logstat/run.sh` 钉住 gen/summary 的一致性，Python 侧对照
  过 records / bytes / 状态分布 / p50 / p99 / top-5（逐项相同）。

## 诚实的边界

- p50/p99 是 **1 ms 桶**的近似（延迟直方图 32 桶），不是精确分位数。
- `stats` 与渲染缓冲是全局的：本程序一次只处理一份文件，所以够用；
  要同时处理多份就得把它们变成参数。
- 排序 top-5 是选择排序，O(5·n_paths)，路径数封顶 4096。
- 无模板、无框架：所有数字都是手写十进制格式化（`putInt`），没有依赖格式化库。
