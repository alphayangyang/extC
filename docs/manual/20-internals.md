# 内部成员一览（透明清单）

标准库成员**缺省公开**，编译器不阻止任何访问。本页列出**按约定属于内部**的成员：它们可达，
但不受兼容性承诺保护，重构时不预告。是否使用、如何承担风险，由使用者自行判断 ——
本手册的职责是把它们**列清楚**，而不是把它们锁起来。

生成方式：`tools/manual_surface.py`；`check.sh` 保证本页与源码一致。

公开面共 749 个成员，其中内部 177 个：

## prelude

- `pcg32.buf`（字段）
- `pcg32.cap`（字段）
- `unit.buf`（字段）
- `unit.cap`（字段）
- `varArray.buf`（字段）
- `varArray.cap`（字段）

## std::io

- `writer.accDigit`（方法）
- `writer.buf`（字段）
- `writer.cap`（方法）

## stl::hashMap

- `hashMap.cap`（字段）
- `hashMap.cap`（字段）
- `hashMap.keys`（字段）
- `hashMap.keys`（字段）
- `hashMap.n`（字段）
- `hashMap.n`（字段）
- `hashMap.pPoison`（方法）
- `hashMap.pStale`（方法）
- `hashMap.pid`（字段）
- `hashMap.pid`（字段）
- `hashMap.pidGen`（字段）
- `hashMap.pidGen`（字段）
- `hashMap.vals`（字段）
- `hashMap.vals`（字段）

## stl::linmap

- `linMap.cap`（字段）
- `linMap.keys`（字段）
- `linMap.keys`（字段）
- `linMap.n`（字段）
- `linMap.n`（字段）
- `linMap.pPoison`（方法）
- `linMap.pStale`（方法）
- `linMap.pid`（字段）
- `linMap.pid`（字段）
- `linMap.pidGen`（字段）
- `linMap.vals`（字段）

## stl::map

- `bnode.bn`（方法）
- `bnode.borrowLeft`（方法）
- `bnode.borrowRight`（方法）
- `bnode.childSlot`（方法）
- `bnode.freeHead`（字段）
- `bnode.kd`（方法）
- `bnode.keys`（字段）
- `bnode.keys`（字段）
- `bnode.keys`（字段）
- `bnode.keys`（字段）
- `bnode.kids`（字段）
- `bnode.kids`（字段）
- `bnode.kids`（字段）
- `bnode.kids`（字段）
- `bnode.leafOf`（方法）
- `bnode.n`（字段）
- `bnode.n`（字段）
- `bnode.n`（字段）
- `bnode.nnodes`（字段）
- `bnode.nnodes`（字段）
- `bnode.pPoison`（方法）
- `bnode.pStale`（方法）
- `bnode.pid`（字段）
- `bnode.pid`（字段）
- `bnode.pidGen`（字段）
- `bnode.root`（字段）
- `bnode.vals`（字段）
- `bnode.vals`（字段）
- `bnode.vals`（字段）
- `bnode.vals`（字段）
- `map.bn`（方法）
- `map.borrowLeft`（方法）
- `map.borrowRight`（方法）
- `map.childSlot`（方法）
- `map.freeHead`（字段）
- `map.kd`（方法）
- `map.keys`（字段）
- `map.keys`（字段）
- `map.keys`（字段）
- `map.kids`（字段）
- `map.kids`（字段）
- `map.kids`（字段）
- `map.leafOf`（方法）
- `map.n`（字段）
- `map.n`（字段）
- `map.nnodes`（字段）
- `map.nnodes`（字段）
- `map.pPoison`（方法）
- `map.pStale`（方法）
- `map.pid`（字段）
- `map.pid`（字段）
- `map.pidGen`（字段）
- `map.root`（字段）
- `map.vals`（字段）
- `map.vals`（字段）
- `map.vals`（字段）

## stl::pool

- `handle.atDense`（方法）
- `handle.cap`（字段）
- `handle.cap`（字段）
- `handle.cursor`（字段）
- `handle.cursor`（字段）
- `handle.ent`（字段）
- `handle.ent`（字段）
- `handle.epoch`（字段）
- `handle.epoch`（字段）
- `handle.holeHead`（字段）
- `handle.holeHead`（字段）
- `handle.n`（字段）
- `handle.n`（字段）
- `handle.pDense`（方法）
- `handle.pEp`（方法）
- `handle.pEpMask`（方法）
- `handle.pFree`（方法）
- `handle.pGen`（方法）
- `handle.pLive`（方法）
- `handle.pLow`（方法）
- `handle.pPack`（方法）
- `handle.pPoisonSelf`（方法）
- `handle.pStaleSelf`（方法）
- `handle.pid`（字段）
- `handle.pid`（字段）
- `handle.pidGen`（字段）
- `handle.pidGen`（字段）
- `handle.slots`（字段）
- `handle.slots`（字段）
- `handle.vals`（字段）
- `handle.vals`（字段）
- `pool.atDense`（方法）
- `pool.cap`（字段）
- `pool.cap`（字段）
- `pool.cursor`（字段）
- `pool.cursor`（字段）
- `pool.ent`（字段）
- `pool.ent`（字段）
- `pool.epoch`（字段）
- `pool.epoch`（字段）
- `pool.holeHead`（字段）
- `pool.holeHead`（字段）
- `pool.n`（字段）
- `pool.n`（字段）
- `pool.pDense`（方法）
- `pool.pEp`（方法）
- `pool.pEpMask`（方法）
- `pool.pFree`（方法）
- `pool.pGen`（方法）
- `pool.pLive`（方法）
- `pool.pLow`（方法）
- `pool.pPack`（方法）
- `pool.pPoisonSelf`（方法）
- `pool.pStaleSelf`（方法）
- `pool.pid`（字段）
- `pool.pid`（字段）
- `pool.pidGen`（字段）
- `pool.pidGen`（字段）
- `pool.slots`（字段）
- `pool.slots`（字段）
- `pool.vals`（字段）
- `pool.vals`（字段）

## stl::string

- `string.buf`（字段）
- `string.buf`（字段）
- `string.cap`（字段）
- `string.cap`（字段）
- `string.maxSuffix`（方法）
- `string.n`（字段）
- `string.n`（字段）
- `string.pPoison`（方法）
- `string.pStale`（方法）
- `string.pid`（字段）
- `string.pid`（字段）
- `string.pidGen`（字段）
- `string.pidGen`（字段）

## stl::vector

- `vector.cap`（字段）
- `vector.cap`（字段）
- `vector.n`（字段）
- `vector.n`（字段）
- `vector.pPoison`（方法）
- `vector.pStale`（方法）
- `vector.pid`（字段）
- `vector.pid`（字段）
- `vector.pidGen`（字段）
- `vector.pidGen`（字段）
- `vector.vals`（字段）
- `vector.vals`（字段）

