# 内部成员一览（透明清单）

标准库成员**缺省公开**，编译器不阻止任何访问。本页列出**按约定属于内部**的成员：它们可达，
但不受兼容性承诺保护，重构时不预告。是否使用、如何承担风险，由使用者自行判断 ——
本手册的职责是把它们**列清楚**，而不是把它们锁起来。

生成方式：`tools/manual_surface.py`；`check.sh` 保证本页与源码一致。

公开面共 749 个成员，其中内部 47 个：

## std::io

- `writer.accDigit`（方法）

## stl::hashMap

- `hashMap.pPoison`（方法）
- `hashMap.pStale`（方法）

## stl::linmap

- `linMap.pPoison`（方法）
- `linMap.pStale`（方法）

## stl::map

- `bnode.bn`（方法）
- `bnode.borrowLeft`（方法）
- `bnode.borrowRight`（方法）
- `bnode.childSlot`（方法）
- `bnode.kd`（方法）
- `bnode.leafOf`（方法）
- `bnode.pPoison`（方法）
- `bnode.pStale`（方法）
- `map.bn`（方法）
- `map.borrowLeft`（方法）
- `map.borrowRight`（方法）
- `map.childSlot`（方法）
- `map.kd`（方法）
- `map.leafOf`（方法）
- `map.pPoison`（方法）
- `map.pStale`（方法）

## stl::pool

- `handle.atDense`（方法）
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
- `pool.atDense`（方法）
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

## stl::string

- `string.pPoison`（方法）
- `string.pStale`（方法）

## stl::vector

- `vector.pPoison`（方法）
- `vector.pStale`（方法）

