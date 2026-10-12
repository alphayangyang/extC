#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/escape-fuzz/gen.py — 生成「像用 C++ 堆那样用 new」的语料。

系统组合这些维度（见 README 的「维度」一节）：

  结构 family   : list（单链表/自引用结构体）/ dlist（双向）/ tree（parent↔child）/
                  ring（环）/ selfref（自引用字段成环）
  目的地 dest   : lfield（本地引用变量的字段 a.next）/ dfield（经解引用 (*cur).next）/
                  ff（字段的字段 bx.mid.next）/ aelem（数组元素 arr[0]）/
                  glob（全局）/ pfield（经 mut ref 参数 (*out).next）
  流动 flow     : direct（new 后直接存）/ temp（经中间变量）/ ret（经函数返回）/
                  param（经 mut ref 参数）/ cont（存进 varArray）
  位置 loc      : main（无 home）/ home（返回引用的函数）/ nohome（不逃逸的函数）
  赋值次序 order: before（目标引用在语句之前已被赋过值）/ after（只在语句之后赋）
  收窄 narrow   : ifnull（if x != null）/ old（var o = x 后再收窄）

产出一对文件：cases/<name>.extc 与 cases/<name>.cpp（裸 new、不 delete 的等价堆式写法），
外加 cases/index.tsv。语料是**提交进仓库**的；run.sh 只读语料，不重新生成。
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "cases")

FAMILIES = {
    "list": {
        "ext": "struct node { value: i64  next: ?mut ref node }",
        "cpp": "struct node { long long value; node *next; };",
        "fields": ("next",),
        "walk": ("chain", "next", None),
    },
    "dlist": {
        "ext": "struct node { value: i64  next: ?mut ref node  prev: ?mut ref node }",
        "cpp": "struct node { long long value; node *next; node *prev; };",
        "fields": ("next", "prev"),
        "walk": ("chain", "next", None),
    },
    "tree": {
        "ext": ("struct node { value: i64  left: ?mut ref node  right: ?mut ref node"
                "  parent: ?mut ref node }"),
        "cpp": "struct node { long long value; node *left; node *right; node *parent; };",
        "fields": ("left", "right", "parent"),
        "walk": ("tree", "left", None),
    },
    "ring": {
        "ext": "struct node { value: i64  next: ?mut ref node }",
        "cpp": "struct node { long long value; node *next; };",
        "fields": ("next",),
        "walk": ("chain", "next", 6),
    },
    "selfref": {
        "ext": "struct node { value: i64  self: ?mut ref node }",
        "cpp": "struct node { long long value; node *self; };",
        "fields": ("self",),
        "walk": ("chain", "self", 4),
    },
}


class Prog(object):
    """一边 extC 行，一边等价 C++ 行（None = 这一边没有对应行）。"""

    def __init__(self):
        self.e = []
        self.c = []

    def l(self, e=None, c=None):
        if e is not None:
            self.e.append(e)
        if c is not None:
            self.c.append(c)


def indent(prog, n=1):
    out = Prog()
    for line in prog.e:
        out.e.append("    " * n + line)
    for line in prog.c:
        out.c.append("    " * n + line)
    return out


def pod(fam, var, val, flow="direct"):
    """把一个新节点放进 `<var>`（flow=temp 时放进 `<var>t`）；返回 (extc var, cpp var)。"""
    p = Prog()
    if flow == "ret":
        p.l("var %s: mut ref node = mk(%s)" % (var, val), "node *%s = mk(%s);" % (var, val))
        return p, var, var
    p.l("var %s: mut ref node = new node" % var, "node *%s = new node;" % var)
    p.l("%s.value = %s" % (var, val), "%s->value = %s;" % (var, val))
    for f in FAMILIES[fam]["fields"]:
        p.l("%s.%s = null" % (var, f), "%s->%s = nullptr;" % (var, f))
    if flow == "temp":
        p.l("var %st: mut ref node = %s" % (var, var), "node *%st = %s;" % (var, var))
        return p, var + "t", var + "t"
    return p, var, var


def helpers_for(fam, kinds):
    p = Prog()
    for k in kinds:
        if k == "mk":
            p.l("fn mk(v: i64) -> mut ref node {", "node *mk(long long v) {")
            p.l("    var n: mut ref node = new node", "    node *n = new node;")
            p.l("    n.value = v", "    n->value = v;")
            for f in FAMILIES[fam]["fields"]:
                p.l("    n.%s = null" % f, "    n->%s = nullptr;" % f)
            p.l("    return n", "    return n;")
            p.l("}", "}")
        elif k == "link3":
            p.l("fn link3(out: mut ref node, q: mut ref node) {",
                "void link3(node *out, node *q) {")
            p.l("    (*out).next = q", "    out->next = q;")
            p.l("}", "}")
        elif k == "vec":
            p.l(None, "#include <vector>")
    return p


# ---------------------------------------------------------------------------
# 走一遍并把和放进 t
# ---------------------------------------------------------------------------

def walk_prog(fam, head_e, head_c):
    kind, field, steps = FAMILIES[fam]["walk"]
    p = Prog()
    p.l("var c: ?mut ref node = %s" % head_e, "node *c = %s;" % head_c)
    p.l("var t: i64 = 0", "long long t = 0;")
    if kind == "tree":
        p.l("if c != null {", "if (c != nullptr) {")
        p.l("    t = t + (*c).value", "    t += c->value;")
        p.l("    if (*c).left != null { t = t + (*(*c).left!).value }",
            "    if (c->left != nullptr) { t += c->left->value; }")
        p.l("    if (*c).right != null { t = t + (*(*c).right!).value }",
            "    if (c->right != nullptr) { t += c->right->value; }")
        p.l("}", "}")
        return p
    if steps is None:
        p.l("while c != null {", "while (c != nullptr) {")
        p.l("    t = t + (*c).value", "    t += c->value;")
        p.l("    c = (*c).%s" % field, "    c = c->%s;" % field)
        p.l("}", "}")
    else:
        p.l("var s: i64 = 0", "long long s = 0;")
        p.l("while s < %d {" % steps, "while (s < %d) {" % steps)
        p.l("    if c != null {", "    if (c != nullptr) {")
        p.l("        t = t + (*c).value", "        t += c->value;")
        p.l("        c = (*c).%s" % field, "        c = c->%s;" % field)
        p.l("    }", "    }")
        p.l("    s = s + 1", "    s++;")
        p.l("}", "}")
    return p


def build_case(fam, loc, body, head_e, head_c, helpers, expect, top=None):
    """body: Prog（函数体），helpers: 需要的外部函数 kind 列表，top: 顶层声明。"""
    out = Prog()
    out.l(None, "#include <iostream>")
    out.l("use std::io")
    out.l(FAMILIES[fam]["ext"], FAMILIES[fam]["cpp"])
    if top is not None:
        out.e.extend(top.e)
        out.c.extend(top.c)
    hp = helpers_for(fam, helpers)
    out.e.extend(hp.e)
    out.c.extend(hp.c)
    if loc == "home":
        out.l("fn build() -> ?mut ref node {", "node *build() {")
        b = indent(body)
        out.e.extend(b.e)
        out.c.extend(b.c)
        out.l("    return %s" % head_e, "    return %s;" % head_c)
        out.l("}", "}")
        w = walk_prog(fam, "h", "h")
        out.l("fn main() -> i32 {", "int main() {")
        out.l("    var h: ?mut ref node = build()", "    node *h = build();")
        b = indent(w)
        out.e.extend(b.e)
        out.c.extend(b.c)
        out.l('    io::cout << t << "\\n"', '    std::cout << t << "\\n";')
        out.l("    return 0", "    return 0;")
        out.l("}", "}")
    elif loc == "nohome":
        out.l("fn burn() -> i64 {", "long long burn() {")
        b = indent(body)
        out.e.extend(b.e)
        out.c.extend(b.c)
        w = walk_prog(fam, head_e, head_c)
        b = indent(w)
        out.e.extend(b.e)
        out.c.extend(b.c)
        out.l("    return t", "    return t;")
        out.l("}", "}")
        out.l("fn main() -> i32 {", "int main() {")
        out.l('    io::cout << burn() << "\\n"', '    std::cout << burn() << "\\n";')
        out.l("    return 0", "    return 0;")
        out.l("}", "}")
    else:
        out.l("fn main() -> i32 {", "int main() {")
        b = indent(body)
        out.e.extend(b.e)
        out.c.extend(b.c)
        w = walk_prog(fam, head_e, head_c)
        b = indent(w)
        out.e.extend(b.e)
        out.c.extend(b.c)
        out.l('    io::cout << t << "\\n"', '    std::cout << t << "\\n";')
        out.l("    return 0", "    return 0;")
        out.l("}", "}")
    return out


# ---------------------------------------------------------------------------
# 骨架（返回 body Prog, head_e, head_c, helpers, expect）
# ---------------------------------------------------------------------------

def narrow_decl(p, narrow, var):
    """返回 (标识名, 关闭行) —— narrow=old 时先拷一份再判空。"""
    if narrow == "old":
        p.l("var o%s: ?mut ref node = %s" % (var, var),
            "node *o%s = %s;" % (var, var))
        p.l("if o%s != null {" % var, "if (o%s != nullptr) {" % var)
        return "o" + var
    p.l("if %s != null {" % var, "if (%s != nullptr) {" % var)
    return var


def skel_chain(fam, dest, flow, order, narrow, n=4):
    p = Prog()
    helpers = ["mk"] if flow == "ret" else []
    if dest == "lfield":
        p.l("var a: mut ref node = new node", "node *a = new node;")
        p.l("a.value = 0", "a->value = 0;")
        for f in FAMILIES[fam]["fields"]:
            p.l("a.%s = null" % f, "a->%s = nullptr;" % f)
        p.l("var head: ?mut ref node = a", "node *head = a;")
        p.l("var i: i64 = 1", "long long i = 1;")
        p.l("while i <= %d {" % n, "while (i <= %d) {" % n)
        sub, ve, vc = pod(fam, "p", "i", flow)
        p.e.extend("    " + x for x in sub.e)
        p.c.extend("    " + x for x in sub.c)
        p.l("    a.next = %s" % ve, "    a->next = %s;" % vc)
        p.l("    a = %s" % ve, "    a = %s;" % vc)
        p.l("    i = i + 1", "    i++;")
        p.l("}", "}")
        return p, "head", "head", helpers, n * (n + 1) // 2, None
    # dfield：经 (*cur) 写
    p.l("var head: ?mut ref node = null", "node *head = nullptr;")
    p.l("var cur: ?mut ref node = null", "node *cur = nullptr;")
    p.l("var i: i64 = 0", "long long i = 0;")
    p.l("while i < %d {" % n, "while (i < %d) {" % n)
    sub, ve, vc = pod(fam, "p", "i + 1", flow)
    p.e.extend("    " + x for x in sub.e)
    p.c.extend("    " + x for x in sub.c)
    p.l("    if head == null {", "    if (head == nullptr) {")
    p.l("        head = %s" % ve, "        head = %s;" % vc)
    if order == "before":
        p.l("        cur = %s" % ve, "        cur = %s;" % vc)
    p.l("    } else {", "    } else {")
    cv = narrow_decl(p, narrow, "cur")
    cv_e = "cur" if narrow == "ifnull" else "ocur"
    p.l("        (*%s).next = %s" % (cv_e, ve), "        %s->next = %s;" % (cv_e, vc))
    if fam == "dlist":
        p.l("        (*%s).prev = cur" % ve, "        %s->prev = cur;" % vc)
    p.l("    }", "    }")
    if order == "before":
        p.l("        cur = %s" % ve, "        cur = %s;" % vc)
        # 上面的 cur= 落在 else 体内（收窄块之后），语法上仍在 else 里
    p.l("    }", "    }")
    if order == "after":
        p.l("    cur = %s" % ve, "    cur = %s;" % vc)
    p.l("    i = i + 1", "    i++;")
    p.l("}", "}")
    return p, "head", "head", helpers, n * (n + 1) // 2, None


def skel_straight(fam, dest, flow):
    p = Prog()
    helpers = ["mk"] if flow == "ret" else []
    if dest == "ff":
        top = Prog()
        top.l("struct inner { next: ?mut ref node }", "struct inner { node *next; };")
        top.l("struct outer { mid: inner }", "struct outer { inner mid; };")
        p.l("var bx: outer = { mid: { next: null } }", "outer bx; bx.mid.next = nullptr;")
        sub, ve, vc = pod(fam, "p", "7", flow)
        p.e.extend(sub.e)
        p.c.extend(sub.c)
        p.l("bx.mid.next = %s" % ve, "bx.mid.next = %s;" % vc)
        return p, "bx.mid.next", "bx.mid.next", helpers, 7, top
    p.l("var arr: [2]?mut ref node", "node *arr[2];")
    p.l("arr[0] = null", "arr[0] = nullptr;")
    p.l("arr[1] = null", "arr[1] = nullptr;")
    sub, ve, vc = pod(fam, "p", "7", flow)
    p.e.extend(sub.e)
    p.c.extend(sub.c)
    p.l("arr[0] = %s" % ve, "arr[0] = %s;" % vc)
    return p, "arr[0]", "arr[0]", helpers, 7, None


def skel_tree(fam, dest, flow):
    p = Prog()
    helpers = ["mk"] if flow == "ret" else []
    if dest == "lfield":
        p.l("var root: mut ref node = new node", "node *root = new node;")
        p.l("root.value = 1", "root->value = 1;")
        for f in FAMILIES[fam]["fields"]:
            p.l("root.%s = null" % f, "root->%s = nullptr;" % f)
        sub, le, lc = pod(fam, "l", "2", flow)
        p.e.extend(sub.e)
        p.c.extend(sub.c)
        sub, re, rc = pod(fam, "r", "3", flow)
        p.e.extend(sub.e)
        p.c.extend(sub.c)
        p.l("root.left = %s" % le, "root->left = %s;" % lc)
        p.l("root.right = %s" % re, "root->right = %s;" % rc)
        p.l("%s.parent = root" % le, "%s->parent = root;" % lc)
        p.l("%s.parent = root" % re, "%s->parent = root;" % rc)
        return p, "root", "root", helpers, 6, None
    p.l("var root0: mut ref node = new node", "node *root0 = new node;")
    p.l("var root: ?mut ref node = root0", "node *root = root0;")
    p.l("root0.value = 1", "root0->value = 1;")
    for f in FAMILIES[fam]["fields"]:
        p.l("root0.%s = null" % f, "root0->%s = nullptr;" % f)
    sub, le, lc = pod(fam, "l", "2", flow)
    p.e.extend(sub.e)
    p.c.extend(sub.c)
    sub, re, rc = pod(fam, "r", "3", flow)
    p.e.extend(sub.e)
    p.c.extend(sub.c)
    p.l("if root != null {", "if (root != nullptr) {")
    p.l("    (*root).left = %s" % le, "    root->left = %s;" % lc)
    p.l("    (*root).right = %s" % re, "    root->right = %s;" % rc)
    p.l("}", "}")
    p.l("%s.parent = root" % le, "%s->parent = root;" % lc)
    p.l("%s.parent = root" % re, "%s->parent = root;" % rc)
    return p, "root", "root", helpers, 6, None


def skel_ring(fam, dest, flow):
    p = Prog()
    helpers = ["mk"] if flow == "ret" else []
    if dest == "lfield":
        sub, ae, ac = pod(fam, "a", "1", flow)
        p.e.extend(sub.e)
        p.c.extend(sub.c)
        sub, be, bc = pod(fam, "b", "2", flow)
        p.e.extend(sub.e)
        p.c.extend(sub.c)
        sub, ce, cc = pod(fam, "ca", "3", flow)
        p.e.extend(sub.e)
        p.c.extend(sub.c)
        p.l("%s.next = %s" % (ae, be), "%s->next = %s;" % (ac, bc))
        p.l("%s.next = %s" % (be, ce), "%s->next = %s;" % (bc, cc))
        p.l("%s.next = %s" % (ce, ae), "%s->next = %s;" % (cc, ac))
        return p, ae, ac, helpers, 12, None
    sub, q1e, q1c = pod(fam, "q1", "1", flow)
    p.e.extend(sub.e)
    p.c.extend(sub.c)
    sub, q2e, q2c = pod(fam, "q2", "2", flow)
    p.e.extend(sub.e)
    p.c.extend(sub.c)
    sub, q3e, q3c = pod(fam, "q3", "3", flow)
    p.e.extend(sub.e)
    p.c.extend(sub.c)
    p.l("var a: ?mut ref node = %s" % q1e, "node *a = %s;" % q1c)
    p.l("var b: ?mut ref node = %s" % q2e, "node *b = %s;" % q2c)
    p.l("var cc0: ?mut ref node = %s" % q3e, "node *cc0 = %s;" % q3c)
    p.l("if a != null { (*a).next = b }", "if (a != nullptr) { a->next = b; }")
    p.l("if b != null { (*b).next = cc0 }", "if (b != nullptr) { b->next = cc0; }")
    p.l("if cc0 != null { (*cc0).next = a }", "if (cc0 != nullptr) { cc0->next = a; }")
    return p, "a", "a", helpers, 12, None


def skel_selfref(fam, dest, flow):
    p = Prog()
    helpers = ["mk"] if flow == "ret" else []
    if dest == "lfield":
        p.l("var a: mut ref node = new node", "node *a = new node;")
        p.l("var b: mut ref node = new node", "node *b = new node;")
        p.l("a.value = 1", "a->value = 1;")
        p.l("b.value = 2", "b->value = 2;")
        p.l("a.self = null", "a->self = nullptr;")
        p.l("b.self = null", "b->self = nullptr;")
        p.l("a.self = b", "a->self = b;")
        p.l("b.self = a", "b->self = a;")
        return p, "a", "a", helpers, 6, None
    p.l("var a0: mut ref node = new node", "node *a0 = new node;")
    p.l("var a: ?mut ref node = a0", "node *a = a0;")
    p.l("var b0: mut ref node = new node", "node *b0 = new node;")
    p.l("var b: ?mut ref node = b0", "node *b = b0;")
    p.l("a0.value = 1", "a0->value = 1;")
    p.l("b0.value = 2", "b0->value = 2;")
    p.l("a0.self = null", "a0->self = nullptr;")
    p.l("b0.self = null", "b0->self = nullptr;")
    p.l("if a != null { (*a).self = b }", "if (a != nullptr) { a->self = b; }")
    p.l("if b != null { (*b).self = a }", "if (b != nullptr) { b->self = a; }")
    return p, "a", "a", helpers, 6, None


def skel_param(fam, flow, n=4):
    p = Prog()
    helpers = ["link3"]
    if flow == "ret":
        helpers.insert(0, "mk")
    p.l("var a: mut ref node = new node", "node *a = new node;")
    p.l("a.value = 0", "a->value = 0;")
    for f in FAMILIES[fam]["fields"]:
        p.l("a.%s = null" % f, "a->%s = nullptr;" % f)
    p.l("var head: ?mut ref node = a", "node *head = a;")
    p.l("var i: i64 = 1", "long long i = 1;")
    p.l("while i <= %d {" % n, "while (i <= %d) {" % n)
    sub, ve, vc = pod(fam, "p", "i", flow)
    p.e.extend("    " + x for x in sub.e)
    p.c.extend("    " + x for x in sub.c)
    p.l("    link3(ref *a, %s)" % ve, "    link3(a, %s);" % vc)
    p.l("    a = %s" % ve, "    a = %s;" % vc)
    p.l("    i = i + 1", "    i++;")
    p.l("}", "}")
    return p, "head", "head", helpers, n * (n + 1) // 2, None


def skel_cont(fam, flow, order, n=4):
    p = Prog()
    helpers = ["vec"]
    if flow == "ret":
        helpers.insert(0, "mk")
    p.l("var v: varArray<mut ref node> = varArray<mut ref node>::withCap(8)",
        "std::vector<node *> v;")
    p.l("var i: i64 = 0", "long long i = 0;")
    p.l("while i < %d {" % n, "while (i < %d) {" % n)
    sub, ve, vc = pod(fam, "p", "i + 1", flow)
    p.e.extend("    " + x for x in sub.e)
    p.c.extend("    " + x for x in sub.c)
    p.l("    v.push(%s)" % ve, "    v.push_back(%s);" % vc)
    p.l("    i = i + 1", "    i++;")
    p.l("}", "}")
    p.l("var head: ?mut ref node = null", "node *head = nullptr;")
    p.l("var cur: ?mut ref node = null", "node *cur = nullptr;")
    p.l("var k: i64 = 0", "long long k = 0;")
    p.l("while k < v.size() {", "while (k < (long long)v.size()) {")
    p.l("    var q: mut ref node = v.get(k)!", "    node *q = v[k];")
    p.l("    if head == null {", "    if (head == nullptr) {")
    p.l("        head = q", "        head = q;")
    if order == "before":
        p.l("        cur = q", "        cur = q;")
    p.l("    } else {", "    } else {")
    p.l("        if cur != null { (*cur).next = q }",
        "        if (cur != nullptr) { cur->next = q; }")
    if order == "before":
        p.l("        cur = q", "        cur = q;")
    p.l("    }", "    }")
    if order == "after":
        p.l("    cur = q", "    cur = q;")
    p.l("    k = k + 1", "    k++;")
    p.l("}", "}")
    return p, "head", "head", helpers, n * (n + 1) // 2, None


# ---------------------------------------------------------------------------

CASES_INDEX = []


def add_case(name, fam, dest, flow, loc, order, narrow, expect, skel):
    body, head_e, head_c, helpers, ex, top = skel
    full = build_case(fam, loc, body, head_e, head_c, helpers, ex, top)
    with open(os.path.join(CASES, name + ".extc"), "w", encoding="utf-8") as f:
        f.write("\n".join(full.e) + "\n")
    with open(os.path.join(CASES, name + ".cpp"), "w", encoding="utf-8") as f:
        f.write("\n".join(full.c) + "\n")
    CASES_INDEX.append((name, fam, dest, flow, loc, order, narrow, str(expect)))


def gen_chain_matrix():
    fam = "list"
    for loc in ("main", "home", "nohome"):
        for flow in ("direct", "temp", "ret"):
            add_case("m_%s_lfield_%s" % (loc, flow), fam, "lfield", flow, loc,
                     "before", "ifnull", 0,
                     skel_chain(fam, "lfield", flow, "before", "ifnull"))
            for order in ("before", "after"):
                add_case("m_%s_dfield_%s_%s" % (loc, flow, order), fam, "dfield", flow,
                         loc, order, "ifnull", 0,
                         skel_chain(fam, "dfield", flow, order, "ifnull"))
            add_case("m_%s_dfield_%s_old" % (loc, flow), fam, "dfield", flow, loc,
                     "after", "old", 0,
                     skel_chain(fam, "dfield", flow, "after", "old"))
            for d in ("ff", "aelem"):
                add_case("m_%s_%s_%s" % (loc, d, flow), fam, d, flow, loc,
                         "before", "ifnull", 0, skel_straight(fam, d, flow))
        for flow in ("direct", "ret"):
            add_case("m_%s_pfield_%s" % (loc, flow), fam, "pfield", flow, loc,
                     "before", "ifnull", 0, skel_param(fam, flow))
        for order in ("before", "after"):
            add_case("m_%s_cont_%s" % (loc, order), fam, "cont", "direct", loc,
                     order, "ifnull", 0, skel_cont(fam, "direct", order))


def gen_structures():
    for fam in ("dlist", "tree", "ring", "selfref"):
        for loc in ("main", "home", "nohome"):
            for dest in ("lfield", "dfield"):
                for flow in ("direct", "ret"):
                    if fam == "dlist":
                        skel = skel_chain(fam, dest, flow, "before", "ifnull")
                    elif fam == "tree":
                        skel = skel_tree(fam, dest, flow)
                    elif fam == "ring":
                        skel = skel_ring(fam, dest, flow)
                    else:
                        skel = skel_selfref(fam, dest, flow)
                    add_case("s_%s_%s_%s_%s" % (fam, loc, dest, flow), fam, dest, flow,
                             loc, "before", "ifnull", 0, skel)


GLOBALS = [
    ("g_ref_global", """use std::io
struct node { value: i64  next: ?mut ref node }
var g: ?mut ref node
fn main() -> i32 {
    var p: mut ref node = new node
    p.value = 7
    g = p
    if g != null { io::cout << (*g).value << "\\n" }
    return 0
}
"""),
    ("g_struct_global", """use std::io
struct node { value: i64  next: ?mut ref node }
struct slot { r: ?mut ref node }
var g: slot = { r: null }
fn main() -> i32 {
    var p: mut ref node = new node
    p.value = 7
    g.r = p
    if g.r != null { io::cout << (*g.r!).value << "\\n" }
    return 0
}
"""),
    ("g_array_global", """use std::io
struct node { value: i64  next: ?mut ref node }
var g: [2]?mut ref node
fn main() -> i32 {
    g[0] = null
    g[1] = null
    var p: mut ref node = new node
    p.value = 7
    g[0] = p
    if g[0] != null { io::cout << (*g[0]!).value << "\\n" }
    return 0
}
"""),
]
GLOBAL_CPP = """#include <iostream>
struct node { long long value; node *next; };
node *g = nullptr;
int main() {
    node *p = new node;
    p->value = 7;
    p->next = nullptr;
    g = p;
    std::cout << g->value << "\\n";
    return 0;
}
"""


def write_globals():
    for name, text in GLOBALS:
        with open(os.path.join(CASES, name + ".extc"), "w", encoding="utf-8") as f:
            f.write(text)
        with open(os.path.join(CASES, name + ".cpp"), "w", encoding="utf-8") as f:
            f.write(GLOBAL_CPP)
        CASES_INDEX.append((name, "list", "glob", "direct", "main", "before",
                            "ifnull", "7"))


# 「该拒」对照：真的会悬垂的形状。它们的深度错不是误拒，而是判据的本职。
EXTRAS = [
    ("r_deep_store", """use std::io
struct node { val: i64  next: ?mut ref node }
struct list { head: ?mut ref node }
fn main() -> i32 {
    var l: list = { head: null }
    {
        var x: node = { val: 1, next: null }
        l.head = ref x
    }
    return 0
}
""", """#include <iostream>
struct node { long long val; node *next; };
struct list { node *head; };
int main() {
    list l; l.head = nullptr;
    {
        node *x = new node;
        x->val = 1;
        x->next = nullptr;
        l.head = x;
    }
    std::cout << l.head->val << "\\n";
    return 0;
}
""", 1),
    ("r_deep_store_call", """use std::io
struct node { val: i64  next: ?mut ref node }
struct list { head: ?mut ref node }
fn stash(l: mut ref list, cell: ?mut ref node) {
    l.head = cell
}
fn main() -> i32 {
    var l: list = { head: null }
    {
        var x: node = { val: 1, next: null }
        stash(ref l, ref x)
    }
    return 0
}
""", """#include <iostream>
struct node { long long val; node *next; };
struct list { node *head; };
void stash(list *l, node *cell) { l->head = cell; }
int main() {
    list l; l.head = nullptr;
    {
        node *x = new node;
        x->val = 1;
        x->next = nullptr;
        stash(&l, x);
    }
    std::cout << l.head->val << "\\n";
    return 0;
}
""", 1),
]


def write_extras():
    for name, extc, cpp, expect in EXTRAS:
        with open(os.path.join(CASES, name + ".extc"), "w", encoding="utf-8") as f:
            f.write(extc)
        with open(os.path.join(CASES, name + ".cpp"), "w", encoding="utf-8") as f:
            f.write(cpp)
        CASES_INDEX.append((name, "list", "lfield", "direct", "main", "before",
                            "ifnull", str(expect)))


def main():
    if not os.path.isdir(CASES):
        os.makedirs(CASES)
    for f in os.listdir(CASES):
        if f.endswith(".extc") or f.endswith(".cpp"):
            os.remove(os.path.join(CASES, f))
    del CASES_INDEX[:]
    gen_chain_matrix()
    gen_structures()
    write_globals()
    write_extras()
    with open(os.path.join(CASES, "index.tsv"), "w", encoding="utf-8") as f:
        f.write("name\tfamily\tdest\tflow\tloc\torder\tnarrow\texpect\n")
        for row in CASES_INDEX:
            f.write("\t".join(row) + "\n")
    print("generated %d cases in %s" % (len(CASES_INDEX), CASES))


if __name__ == "__main__":
    main()
