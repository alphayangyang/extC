import re, pathlib

R = pathlib.Path('/home/alphayang/extC_Compiler')
groups = {
    'check':   ['src/check_top.c', 'src/check_escape.c', 'src/check_expr.c', 'src/check_lookup.c', 'src/check_stmt.c'],
    'codegen': ['src/codegen.c'],
    'front':   ['src/parser.c', 'src/lexer.c', 'src/ast.c'],
    'other':   ['src/modules.c', 'src/types.c', 'src/dataflow.c', 'src/main.c', 'src/pools.c', 'src/coroutine.c'],
}
texts = {g: '\n'.join((R / f).read_text(encoding='utf-8') for f in fs if (R / f).exists())
         for g, fs in groups.items()}

# 分析/计划候选（Expr / Sym / FuncDef / Stmt 上"不是语法事实"的那些）
cands = [
 # 计划：codegen 必须读
 'arenaArg', 'arenaArgPending', 'arenaLevel', 'zoneLevel', 'usesHome', 'makesPool',
 'needsHome', 'mayUseArena', 'homeDepth', 'instName', 'tmpl', 'func', 'used',
 'coroNeedsZone', 'coroBoxed', 'isCoro', 'yieldType', 'coroFrameType', 'coroProto',
 # 分析缓存（memo）
 'effState', 'effComplete', 'addrMask', 'contMask', 'otherMask', 'homeAddrMask',
 'homeContMask', 'addrFromLocal', 'freshCount', 'callees', 'effAddrMask', 'effContMask',
 'lexicalLevel', 'storedAt', 'refDepth', 'heldSrc', 'needTemp',
 # 身份 / 重指
 'origin', 'addressed', 'outOfFrame', 'otherDepth', 'nfields', 'fieldsComplete',
 'callSrc', 'callSrcDepth', 'callMinReq', 'paramSyms', 'nParamSyms', 'depth',
 'condAllocs', 'forStep', 'cname',
]

def count(f, mode, text):
    if mode == 'w':
        return len(re.findall(r'->' + f + r'\s*(?:=[^=]|\+\+|--|\+=|-=|\|=|&=)', text))
    return len(re.findall(r'->' + f + r'\b(?!\s*(?:=[^=]|\+\+|--|\+=|-=|\|=|&=))', text))

print(f"{'字段':<18}{'check写':>7}{'check读':>7}{'gen读':>6}{'gen写':>6}{'other写':>8}{'other读':>8}")
rows = []
for f in cands:
    cw, cr = count(f, 'w', texts['check']), count(f, 'r', texts['check'])
    gw, gr = count(f, 'w', texts['codegen']), count(f, 'r', texts['codegen'])
    ow, orr = count(f, 'w', texts['other']), count(f, 'r', texts['other'])
    if cw + cr + gw + gr + ow + orr == 0:
        continue
    rows.append((gr, f, cw, cr, gw, ow, orr))
rows.sort(reverse=True)
for gr, f, cw, cr, gw, ow, orr in rows:
    print(f"{f:<18}{cw:>7}{cr:>7}{gr:>6}{gw:>6}{ow:>8}{orr:>8}")
