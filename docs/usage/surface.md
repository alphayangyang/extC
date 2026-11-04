# 内部成员一览（透明清单）

标准库成员**缺省公开**，源码写 `@private` 才是内部。本页列出这些内部成员：它们跨模块不可见，
也不受兼容性承诺保护，重构时不预告。

生成方式：`tools/manual_surface.py`；`check.sh` 保证本页与源码一致。

公开面共 545 个成员，其中内部 134 个：

## prelude

- `pcg32.inc`（字段）
- `varArray.cap`（字段）
- `varArray.pid`（字段）
- `varArray.vals`（字段）

## std::coro::scheduler

- `loop.ep`（字段）

## std::data::buf

- `borrowGuard.b`（字段）
- `borrowGuard.p`（字段）
- `buf.cap`（字段）
- `buf.n`（字段）
- `buf.pid`（字段）
- `buf.pidGen`（字段）
- `buf.pins`（字段）
- `buf.vals`（字段）

## std::data::hashMap

- `hashMap.bucketOfDense`（字段）
- `hashMap.cap`（字段）
- `hashMap.dead`（字段）
- `hashMap.deadOf`（方法）
- `hashMap.keys`（字段）
- `hashMap.n`（字段）
- `hashMap.pid`（字段）
- `hashMap.pidGen`（字段）
- `hashMap.pidOf`（方法）
- `hashMapI64.deadOf`（方法）
- `hashMapI64.pidOf`（方法）
- `hashMapI64.tagCensus`（方法）

## std::data::linmap

- `linMap.cap`（字段）
- `linMap.keys`（字段）
- `linMap.n`（字段）
- `linMap.pid`（字段）
- `linMap.pidGen`（字段）

## std::data::map

- `bnode.cnt`（字段）
- `bnode.keys`（字段）
- `bnode.kids`（字段）
- `bnode.leaf`（字段）
- `bnode.n`（字段）
- `map.bn`（方法）
- `map.cntOf`（方法）
- `map.freeHead`（字段）
- `map.kd`（方法）
- `map.keys`（字段）
- `map.kids`（字段）
- `map.kn`（方法）
- `map.leafOf`（方法）
- `map.n`（字段）
- `map.ncap`（字段）
- `map.nnodes`（字段）
- `map.nodes`（字段）
- `map.nx`（方法）
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
- `map.vn`（方法）

## std::data::pool

- `handle.ep`（字段）
- `pool.cap`（字段）
- `pool.cursor`（字段）
- `pool.ent`（字段）
- `pool.holeHead`（字段）
- `pool.n`（字段）
- `pool.pDense`（方法）
- `pool.pEp`（方法）
- `pool.pEpMask`（方法）
- `pool.pGen`（方法）
- `pool.pLow`（方法）
- `pool.pPack`（方法）
- `pool.pid`（字段）
- `pool.pidGen`（字段）
- `pool.slots`（字段）

## std::data::vector

- `vector.cap`（字段）
- `vector.n`（字段）
- `vector.pid`（字段）
- `vector.pidGen`（字段）

## std::fs

- `-.inSave`（方法）

## std::hash::digest

- `-.hmacSha1`（方法）
- `-.hmacSha256`（方法）
- `-.pRotl`（方法）
- `-.pRotr`（方法）
- `-.sha1Hex`（方法）
- `-.sha1Sum`（方法）
- `-.sha256Hex`（方法）
- `-.sha256Sum`（方法）
- `sha1State.buf`（字段）
- `sha256State.buf`（字段）

## std::io

- `-.accDigit`（方法）
- `-.errWrite`（方法）
- `-.fillState`（方法）
- `-.fmtI64`（方法）
- `-.isSpaceByte`（方法）
- `terminal.rawbuf`（字段）
- `terminal.saved`（字段）
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

- `timer.t0`（字段）
- `timers.n`（字段）

## std::mem

- `-.pRoundUp`（方法）

## std::net

- `-.eqCI`（方法）
- `-.findSub`（方法）
- `-.headerBlockEnd`（方法）
- `-.headerEnd`（方法）
- `-.hexVal`（方法）
- `-.isSpace`（方法）
- `-.lower`（方法）
- `-.pAppend`（方法）
- `-.pPow2`（方法）
- `-.putBytes`（方法）
- `-.putI64`（方法）
- `-.shiftToFront`（方法）
- `session.buf`（字段）

## std::text::string

- `-.pDec`（方法）
- `-.pDec6`（方法）
- `-.pEnc`（方法）
- `-.pEnc6`（方法）
- `string.big`（字段）
- `string.cap`（字段）
- `string.n`（字段）
- `string.pid`（字段）
- `string.pidGen`（字段）
- `string.small`（字段）

