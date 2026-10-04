# 内部成员一览（透明清单）

标准库成员**缺省公开**，编译器不阻止任何访问。本页列出**按约定属于内部**的成员：它们可达，
但不受兼容性承诺保护，重构时不预告。是否使用、如何承担风险，由使用者自行判断 ——
本手册的职责是把它们**列清楚**，而不是把它们锁起来。

生成方式：`tools/manual_surface.py`；`check.sh` 保证本页与源码一致。

公开面共 724 个成员，其中内部 208 个：

## prelude

- `pcg32.inc`（字段）
- `varArray.buf`（字段）
- `varArray.cap`（字段）

## std::b64

- `-.pDec`（方法）
- `-.pDec6`（方法）
- `-.pEnc`（方法）
- `-.pEnc6`（方法）

## std::coro::scheduler

- `loop.ep`（字段）

## std::hash::sha1

- `-.pRotl`（方法）
- `state.buf`（字段）
- `state.pBlockAt`（方法）

## std::hash::sha256

- `-.pRotr`（方法）
- `state.buf`（字段）
- `state.pBlockAt`（方法）

## std::heap

- `-.pRoundUp`（方法）

## std::http

- `conn.buf`（字段）
- `conn.buf`（字段）
- `session.buf`（字段）

## std::io

- `-.accDigit`（方法）
- `-.errWrite`（方法）
- `-.fillState`（方法）
- `-.flushCout`（方法）
- `-.fmtI64`（方法）
- `-.fmtI64Fast`（方法）
- `-.isSpaceByte`（方法）
- `-.nextI64State`（方法）
- `-.pairsReady`（方法）
- `reader.chunk`（字段）
- `reader.newlineAfter`（方法）
- `writer.buf`（字段）
- `writer.cap`（方法）

## std::json

- `-.pCount`（方法）
- `-.pFirst`（方法）
- `-.pHex`（方法）
- `-.pKeyEq`（方法）
- `-.pNext`（方法）
- `-.pNth`（方法）
- `-.pSetNext`（方法）
- `-.pUtf8`（方法）
- `builder.buf`（字段）
- `builder.n`（字段）

## std::loop

- `timers.n`（字段）

## std::term

- `terminal.rawbuf`（字段）
- `terminal.saved`（字段）

## stl::hashMap

- `hashMap.bucketOfDense`（字段）
- `hashMap.bucketOfDense`（字段）
- `hashMap.cap`（字段）
- `hashMap.cap`（字段）
- `hashMap.dead`（字段）
- `hashMap.dead`（字段）
- `hashMap.deadOf`（方法）
- `hashMap.denseLen`（方法）
- `hashMap.keyAtDense`（方法）
- `hashMap.keys`（字段）
- `hashMap.keys`（字段）
- `hashMap.liveAt`（方法）
- `hashMap.n`（字段）
- `hashMap.n`（字段）
- `hashMap.pPoison`（方法）
- `hashMap.pStale`（方法）
- `hashMap.pid`（字段）
- `hashMap.pid`（字段）
- `hashMap.pidGen`（字段）
- `hashMap.pidGen`（字段）
- `hashMap.pidOf`（方法）
- `hashMap.tagCensus`（方法）
- `hashMap.valAtDense`（方法）
- `hashMap.vals`（字段）
- `hashMap.vals`（字段）
- `hashMapI64.deadOf`（方法）
- `hashMapI64.denseLen`（方法）
- `hashMapI64.keyAtDense`（方法）
- `hashMapI64.liveAt`（方法）
- `hashMapI64.pidOf`（方法）
- `hashMapI64.tagCensus`（方法）
- `hashMapI64.valAtDense`（方法）

## stl::hashSet

- `hashSetI64.liveAt`（方法）

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

- `bnode.cnt`（字段）
- `bnode.keys`（字段）
- `bnode.kids`（字段）
- `bnode.leaf`（字段）
- `bnode.n`（字段）
- `bnode.vals`（字段）
- `map.bn`（方法）
- `map.borrowLeft`（方法）
- `map.borrowRight`（方法）
- `map.childSlot`（方法）
- `map.cntOf`（方法）
- `map.dropChild`（方法）
- `map.firstKey`（方法）
- `map.freeHead`（字段）
- `map.freeNode`（方法）
- `map.growStores`（方法）
- `map.height`（方法）
- `map.kd`（方法）
- `map.keys`（字段）
- `map.keys`（字段）
- `map.keys`（字段）
- `map.kids`（字段）
- `map.kids`（字段）
- `map.kids`（字段）
- `map.kidsCnt`（方法）
- `map.kn`（方法）
- `map.lastKey`（方法）
- `map.leafFor`（方法）
- `map.leafOf`（方法）
- `map.lowerIn`（方法）
- `map.mergeLeft`（方法）
- `map.mergeRight`（方法）
- `map.n`（字段）
- `map.n`（字段）
- `map.ncap`（字段）
- `map.newNode`（方法）
- `map.nnodes`（字段）
- `map.nnodes`（字段）
- `map.nodeCount`（方法）
- `map.nodes`（字段）
- `map.nodes`（字段）
- `map.nx`（方法）
- `map.pPoison`（方法）
- `map.pStale`（方法）
- `map.pid`（字段）
- `map.pid`（字段）
- `map.pidGen`（字段）
- `map.root`（字段）
- `map.setCnt`（方法）
- `map.setKey`（方法）
- `map.setKid`（方法）
- `map.setN`（方法）
- `map.setNext`（方法）
- `map.setVal`（方法）
- `map.side`（字段）
- `map.subMinKey`（方法）
- `map.vals`（字段）
- `map.vals`（字段）
- `map.vals`（字段）
- `map.vn`（方法）

## stl::pool

- `handle.ep`（字段）
- `handle.gen`（字段）
- `pool.atDense`（方法）
- `pool.cap`（字段）
- `pool.cap`（字段）
- `pool.cursor`（字段）
- `pool.cursor`（字段）
- `pool.ent`（字段）
- `pool.ent`（字段）
- `pool.epoch`（字段）
- `pool.epoch`（字段）
- `pool.gather`（方法）
- `pool.handleAtDense`（方法）
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

- `string.big`（字段）
- `string.big`（字段）
- `string.cap`（字段）
- `string.cap`（字段）
- `string.n`（字段）
- `string.n`（字段）
- `string.pPoison`（方法）
- `string.pStale`（方法）
- `string.pid`（字段）
- `string.pid`（字段）
- `string.pidGen`（字段）
- `string.pidGen`（字段）
- `string.small`（字段）

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

