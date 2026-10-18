#!/usr/bin/env python3
"""tools/check_shade_cache.py --- 守住「旋转散列着色缓存」的结构事实。

## 为什么需要这个门禁

着色缓存是回收运行期里唯一一处**性能提示**：命中就整段跳过屏障慢路径。它记的是
「这一代里这个指针黑过」，所以失效契约稍有偏差就会把白对象当黑（下一次清扫扫掉可达
对象）。历史实现（commit 1409de3）用 16 字节一格：{指针, 代际号}，命中要读两个字。

现在改成 8 字节一格：代际号**旋进格号**（格号 = (地址 >> 3) + gen），所以同一指针在
不同代落进不同格，「同格同指针」只可能同代 ⇒ 命中是精确的；代际号每 2048 代回绕一次，
extc_rc_shadeNextGen 在回绕前 memset 整表，回绕后旧记录不会复活。收益实测 cyclic
86,174,816 -> 84,575,386 Ir（-1.86%）、P5 Ir -0.59% 且 D1mr 反而更好（表小一半）。

这条门禁钉住下面几条，否则「回到 16 字节 + 两字比较」或「忘掉回绕清零」这类退化
只能靠指令数棘轮慢慢发现：

1. 一格只有一个 payload 字（没有 gen 字段）；
2. 格号把代际号旋进去，且表容量 = 掩码 + 1；
3. known/remember 只碰指针，不比较代际号；
4. NextGen 在掩码回绕处 memset 整表（回绕清零）；
5. 清 mark 的四条路径都只调 NextGen、没有裸 extc_rc_shadeGen++；
6. 缓存格子只被 known/remember 两处按下标访问（别处不许直接摸）。

--selftest 把上面每一条各注入一次，要求判据变红。
"""
import argparse
import ast
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CELL_DECL = 'typedef struct ExtcRcShadeCell { uintptr_t v; } ExtcRcShadeCell;'
KNOWN_LINE = 'return extc_rc_shadeCache[extc_rc_shadeHash(v)].v == (uintptr_t)v;'
REMEMBER_LINE = 'extc_rc_shadeCache[extc_rc_shadeHash(v)].v = (uintptr_t)v;'
NEXTGEN_HEAD = 'static inline void extc_rc_shadeNextGen(void) {'
HASH_HEAD = 'static inline size_t extc_rc_shadeHash(void *v) {'
INVALIDATION_CALL = 'extc_rc_shadeNextGen();'


def emitter_text():
    source = (ROOT / 'src/back/reclaim.c').read_text()
    start = source.index('RECLAIM_RT[]')
    end = source.index('\n};', start)
    return ''.join(ast.literal_eval(m.group())
                   for m in re.finditer(r'"(?:[^"\\]|\\.)*"', source[start:end]))


def body_of(text, needle):
    start = text.find(needle)
    if start < 0:
        return None
    a = text.index('{', start)
    depth = 1
    b = a + 1
    while b < len(text) and depth:
        depth += (text[b] == '{') - (text[b] == '}')
        b += 1
    return text[a:b]


def check(text):
    bad = []
    # 1. 一格只有一个 payload 字
    if CELL_DECL not in text:
        bad.append('着色缓存格不是单字 { uintptr_t v; }（退回 16 字节两字结构？）')
    if re.search(r'ExtcRcShadeCell\s*\{[^}]*\bgen\b', text):
        bad.append('着色缓存格里又出现了 gen 字段（代际号应旋进格号，不占第二字）')
    # 2. 表容量与掩码、格号旋入代际号
    m = re.search(r'static ExtcRcShadeCell extc_rc_shadeCache\[(\d+)\];', text)
    n = int(m.group(1)) if m else 0
    if not m:
        bad.append('找不到着色缓存表声明（形状变了就同步本门禁）')
    h = body_of(text, HASH_HEAD)
    mask = None
    if h is None:
        bad.append('extc_rc_shadeHash 不见了（签名变了就同步本门禁）')
    else:
        mm = re.search(r'\+ \(uintptr_t\)extc_rc_shadeGen\) & (\d+)u', h)
        if '+ (uintptr_t)extc_rc_shadeGen' not in h:
            bad.append('格号没把代际号旋进去（同一指针跨代会同格 ⇒ 必须靠第二字校验）')
        if not mm:
            bad.append('格号的掩码形状变了（应形如 ((地址 >> 3) + 代际号) & 2047u）')
        else:
            mask = int(mm.group(1))
            if n and n != mask + 1:
                bad.append('表容量 %d 与掩码 %d 不配（应 n == mask + 1）' % (n, mask))
    # 3. known/remember 只碰指针
    if KNOWN_LINE not in text:
        bad.append('extc_rc_shadeKnown 不再只比指针（不得回头比较代际号字段）')
    if REMEMBER_LINE not in text:
        bad.append('extc_rc_shadeRemember 不再只写指针')
    # 4. 回绕清零
    g = body_of(text, NEXTGEN_HEAD)
    if g is None:
        bad.append('extc_rc_shadeNextGen 不见了')
    else:
        want = 'if ((++extc_rc_shadeGen & %du) == 0u) memset(extc_rc_shadeCache, 0, sizeof extc_rc_shadeCache);' % (mask if mask is not None else 2047)
        if want not in g:
            bad.append('NextGen 没有在掩码回绕处 memset 整表（回绕后旧记录会复活）')
        if 'extc_rc_shadeCache' not in g:
            bad.append('NextGen 不再清表')
    # 5. 四条清 mark 的路径都只调 NextGen
    if text.count(INVALIDATION_CALL) != 4:
        bad.append('清 mark 的路径里 %s 出现 %d 次（应为 4：起轮/清扫起轮/收窄清扫/整区释放）'
                   % (INVALIDATION_CALL, text.count(INVALIDATION_CALL)))
    if re.search(r'extc_rc_shadeGen\+\+', text):
        bad.append('还有裸 extc_rc_shadeGen++（绕过回绕清零 ⇒ 旧记录可能复活）')
    # 6. 格子只被 known/remember 按下标访问
    hits = text.count('extc_rc_shadeCache[extc_rc_shadeHash(v)]')
    if hits != 2:
        bad.append('按下标访问着色缓存格的地方有 %d 处（应只有 known/remember 两处）' % hits)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true')
    args = ap.parse_args()
    text = emitter_text()
    bad = check(text)
    if args.selftest:
        assert not bad, bad
        cases = [
            ('cell grew a gen field',
             text.replace(CELL_DECL,
                          'typedef struct ExtcRcShadeCell { uintptr_t v; uint64_t gen; } ExtcRcShadeCell;', 1)),
            ('gen not rotated into slot index',
             text.replace('+ (uintptr_t)extc_rc_shadeGen', '+ 0u', 1)),
            ('table size does not match mask',
             text.replace('extc_rc_shadeCache[2048]', 'extc_rc_shadeCache[1024]', 1)),
            ('known compares generation again',
             text.replace(KNOWN_LINE,
                          'return extc_rc_shadeCache[extc_rc_shadeHash(v)].v == (uintptr_t)v && extc_rc_shadeGen != 0u;', 1)),
            ('nextgen does not clear on wrap',
             text.replace('if ((++extc_rc_shadeGen & 2047u) == 0u) memset(extc_rc_shadeCache, 0, sizeof extc_rc_shadeCache);', ';', 1)),
            ('one invalidation site bypasses nextgen',
             text.replace(INVALIDATION_CALL, 'extc_rc_shadeGen++;', 1)),
        ]
        for name, mutated in cases:
            assert mutated != text, name
            if not check(mutated):
                print('selftest FAILED: %s 注入后判据仍然绿' % name)
                return 1
        print('ok shade cache selftest: %d 条注入全部变红' % len(cases))
        return 0
    if bad:
        for b in bad:
            print('check 着色缓存结构：%s' % b)
        return 1
    print('ok shade cache structure')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
