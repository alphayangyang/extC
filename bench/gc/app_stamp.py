#!/usr/bin/env python3
"""bench/gc/app_stamp.py —— 业务单位/停顿时间戳插桩（可复用；task-86）
用法: app_stamp.py <gen.c> <输出.c> <单位循环条件关键字(如 ops|units)>
- helper 插到**文件最顶**（第一个函数之前）
- 只按**原文匹配**的循环插 UNITSTAMP（匹配数 != 1 就报错退出，不猜）
- collect 入口插 CSTAMP
"""
import re,sys
src,dst,key=sys.argv[1],sys.argv[2],sys.argv[3]
s=open(src).read()
helper='''
#include <time.h>
#include <stdio.h>
static long long hp_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (long long)t.tv_sec*1000000000LL+t.tv_nsec; }
'''
i=s.index('#include'); j=s.index('\n',i)+1
s=s[:j]+helper+s[j:]
cands=[m for m in re.finditer(r'(?:for|while)\s*\(([^)]*)\)\s*\{', s) if re.search(key, m.group(1))]
if len(cands)!=1:
    print(f"UNIT_LOOP_CANDIDATES={len(cands)} ⇒ 不插（不猜）"); 
else:
    m=cands[0]; cond=m.group(1)
    mm=re.search(r'(?:int64_t|int|long|size_t)\s+(\w+)\s*=', cond) or re.search(r'(\w+)\s*<', cond)
    var=mm.group(1)
    body='\n        fprintf(stderr,"UNITSTAMP v=%lld t=%lld\\n",(long long)%s,hp_now());'%var
    if m.group(0).lstrip().startswith('while'):
        # while 循环体在 { 之后 ⇒ 同样插在 { 之后即可
        pass
    s=s[:m.end()]+body+s[m.end():]
    print(f"UNIT_LOOP_INSERTED kind={m.group(0).lstrip().split('(')[0].strip()} var={var} cond={cond.strip()}")
m2=re.search(r'\n([^\n]*extc_rc_collect[a-zA-Z0-9_.]*\([^\n]*\)\s*\{)', s)
if m2: s=s[:m2.end()]+'\n    fprintf(stderr,"CSTAMP t=%lld\\n",hp_now());'+s[m2.end():]; print("CSTAMP_INSERTED")
else: print("CSTAMP_NOT_FOUND")
open(dst,'w').write(s)
