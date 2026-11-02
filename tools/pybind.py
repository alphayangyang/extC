#!/usr/bin/env python3
"""pybind.py —— 把 `@export` 出来的 extC 函数包成 CPython 扩展模块（`pip install` 之后 `import`）。

用法：

    # ① 先把 extC 编译成 C 产物（这一步是正常的构建，跟平时一样）
    ./build/extc -w --no-line-map -o build/mathmod.c mathmod.extc

    # ② 生成 CPython 包装层
    tools/pybind.py --name extcmod --product build/mathmod.c -o build/pymod.c \\
        --fn add:add(i64,i64)->i64 \\
        --fn scale:scale(f64,f64)->f64 \\
        --fn isEven:isEven(i64)->bool

    # ③ 编译成扩展模块
    cc -shared -fPIC $(python3-config --includes) build/mathmod.c build/pymod.c \\
        -o build/extcmod$(python3-config --extension-suffix)

    # ④ 用
    PYTHONPATH=build python3 -c "import extcmod; print(extcmod.add(2, 3))"

`--scaffold <dir>` 会把 ②③ 的结果连同一个 `pyproject.toml` + `setup.py` 写进一个**可以直接
`pip install .`** 的项目目录（C 产物是预先烤好的：安装时不需要 extc，跟 cbindgen 的 `--check`
是同一个思路 —— 生成物入库，构建可复现）。

为什么是"生成 C 包装"而不是"手写一个 Python C 扩展"：extC 本来就编译到 C，`@export` 出来的
符号就是普通的 C 函数（名字不带模块前缀），所以这一层只是 `PyArg_ParseTuple` + 调用 + 装箱，
没有任何运行期粘合、没有跨语言对象、没有第二套内存管理。
"""
import argparse
import os
import sys

# 每个 extC 类型：C 声明、`PyArg_ParseTuple` 格式字符、从 C 值装成 PyObject 的表达式、
# 以及"从 `Py_buffer` 取实参"的表达式（只有视图类型用得到）。
TYPES = {
    "i64": dict(c="int64_t", fmt="L", to_c="(int64_t){a}", to_py="PyLong_FromLongLong((long long){v})"),
    "i32": dict(c="int32_t", fmt="i", to_c="(int32_t){a}", to_py="PyLong_FromLong((long){v})"),
    "u32": dict(c="uint32_t", fmt="I", to_c="(uint32_t){a}", to_py="PyLong_FromUnsignedLong((unsigned long){v})"),
    "f64": dict(c="double", fmt="d", to_c="{a}", to_py="PyFloat_FromDouble({v})"),
    "f32": dict(c="float", fmt="f", to_c="(float){a}", to_py="PyFloat_FromDouble((double){v})"),
    "bool": dict(c="bool", fmt="p", to_c="({a} != 0)", to_py="PyBool_FromLong({v} ? 1 : 0)"),
    # `bytes`：`@export` 不能收 `slice<u8>`（那是**两个** C 参数），所以惯例是把字节按
    # `(ref u8, i64)` 两个参数交给 C；这一项替用户把 `bytes` 拆成那两半。
    "bytes": dict(c="const uint8_t *", fmt="y*",
                  to_c=["(const uint8_t *)__buf{idx}.buf", "(int64_t)__buf{idx}.len"]),
}


def parse_type(name, where):
    t = TYPES.get(name)
    if t is None:
        sys.exit("%s: 不认识的类型 `%s`（支持：%s）" % (where, name, ", ".join(sorted(TYPES))))
    return t


def parse_fn(spec):
    """`pyname:extcname(i64,f64)->f64` -> dict。"""
    try:
        head, rest = spec.split(":", 1)
        name, tail = rest.split("(", 1)
        args, ret = tail.split(")", 1)
        if not ret.startswith("->"):
            raise ValueError
        ret = ret[2:]
    except ValueError:
        sys.exit("--fn 的写法是 `pyname:extcname(i64,f64)->f64`，收到 `%s`" % spec)
    args = [a.strip() for a in args.split(",") if a.strip()]
    return dict(py=head, c=name, args=args, ret=ret.strip())


def emit_fn(f):
    py, c, args, ret = f["py"], f["c"], f["args"], f["ret"]
    out = []
    out.append("static PyObject *py_%s(PyObject *__self, PyObject *__args) {" % py)
    # 声明 C 实参，并按 PyArg_ParseTuple 的格式串逐个取出来
    fmt = ""
    decls = []
    call = []
    for i, t in enumerate(args):
        ty = parse_type(t, py)
        fmt += ty["fmt"]
        if "y*" == ty["fmt"]:
            decls.append("    Py_buffer __buf%d;" % i)
            call.extend(t.format(idx=i) for t in ty["to_c"])
        elif ty["fmt"] in "p":
            decls.append("    int __raw%d = 0;" % i)
            call.append(ty["to_c"].format(a="__raw%d" % i))
        else:
            decls.append("    %s __a%d;" % (ty["c"], i))
            call.append(ty["to_c"].format(a="__a%d" % i))
    out.extend(decls)
    ptrs = ", ".join("&__buf%d" % i if parse_type(t, py)["fmt"] == "y*" else
                     ("&__raw%d" % i if parse_type(t, py)["fmt"] == "p" else "&__a%d" % i)
                     for i, t in enumerate(args))
    parse_fmt = '"%s:%s"' % (fmt, py) if fmt else '"":%s' % py
    out.append("    if (!PyArg_ParseTuple(__args, %s%s)) return NULL;"
               % (parse_fmt, (", " + ptrs) if ptrs else ""))
    calltxt = "%s(%s)" % (c if ret == "void" else c, ", ".join(call))
    if ret == "void":
        out.append("    %s;" % calltxt)
        out.append("    Py_RETURN_NONE;")
    else:
        ty = parse_type(ret, py)
        out.append("    %s __r = %s;" % (ty["c"], calltxt))
        out.append("    return %s;" % ty["to_py"].format(v="__r"))
    out.append("}")
    return "\n".join(out)


def emit_module(name, fns, doc):
    lines = []
    lines.append("/* 生成物：不要手改。`tools/pybind.py` 把 `@export` 的 extC 函数包成 CPython 方法。 */")
    lines.append("#include <Python.h>")
    lines.append("#include <stdint.h>")
    lines.append("#include <stdbool.h>")
    lines.append("")
    for f in fns:
        tail = {"bytes": ["const uint8_t *", "int64_t"]}
        parts = []
        for t in f["args"]:
            parts.extend(tail[t] if t in tail else [TYPES[t]["c"]])
        sig = ", ".join(parts) or "void"
        ret = "void" if f["ret"] == "void" else TYPES[f["ret"]]["c"]
        lines.append("/* extC `@export fn %s` —— 产物里的外部符号就叫这个名字。 */" % f["c"])
        lines.append("extern %s %s(%s);" % (ret, f["c"], sig))
    lines.append("")
    for f in fns:
        lines.append(emit_fn(f))
        lines.append("")
    lines.append("static PyMethodDef extc_methods[] = {")
    for f in fns:
        summary = "%s(%s) -> %s" % (f["py"], ", ".join(f["args"]), f["ret"])
        lines.append('    {"%s", py_%s, METH_VARARGS, "%s"},' % (f["py"], f["py"], summary))
    lines.append("    {NULL, NULL, 0, NULL},")
    lines.append("};")
    lines.append("")
    lines.append("static struct PyModuleDef extc_module = {")
    lines.append("    PyModuleDef_HEAD_INIT,")
    lines.append('    "%s",' % name)
    lines.append('    "%s",' % doc.replace('"', '\\"'))
    lines.append("    -1,")
    lines.append("    extc_methods,")
    lines.append("};")
    lines.append("")
    lines.append("PyMODINIT_FUNC PyInit_%s(void) { return PyModule_Create(&extc_module); }" % name)
    lines.append("")
    return "\n".join(lines)


SETUP_PY = '''"""生成物：`pip install .` 用。C 产物是预先烤好的（安装时不需要 extc）。"""
from setuptools import setup, Extension

setup(
    name="%(name)s",
    version="0.0.1",
    ext_modules=[
        Extension(
            "%(name)s",
            sources=["src/%(name)s.c", "src/pybind_%(name)s.c"],
        )
    ],
)
'''

PYPROJECT = '''[build-system]
requires = ["setuptools>=61"]
build-backend = "setuptools.build_meta"
'''


def main():
    ap = argparse.ArgumentParser(description="把 @export 的 extC 函数包成 CPython 扩展模块")
    ap.add_argument("--name", required=True, help="模块名（`import <name>`）")
    ap.add_argument("--product", help="extC 生成的 C 产物路径（`--scaffold` 时会被复制进去）")
    ap.add_argument("-o", "--out", help="生成的包装 C 文件")
    ap.add_argument("--fn", action="append", default=[], help="`pyname:extcname(i64,f64)->f64`，可重复")
    ap.add_argument("--doc", default="extC 编译出来的模块", help="模块 docstring")
    ap.add_argument("--scaffold", help="写成可以直接 `pip install .` 的项目目录")
    a = ap.parse_args()
    if not a.fn:
        sys.exit("至少给一个 --fn")
    fns = [parse_fn(s) for s in a.fn]
    for f in fns:
        for t in list(f["args"]) + ([f["ret"]] if f["ret"] != "void" else []):
            parse_type(t, f["py"])
    seen = set()
    for f in fns:
        if f["py"] in seen:
            sys.exit("重复的 Python 名字 `%s`" % f["py"])
        seen.add(f["py"])
    text = emit_module(a.name, fns, a.doc)
    if a.out:
        os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
        open(a.out, "w", encoding="utf-8").write(text)
    if a.scaffold:
        if not a.product:
            sys.exit("--scaffold 需要 --product（要复制进去的 extC C 产物）")
        src = os.path.join(a.scaffold, "src")
        os.makedirs(src, exist_ok=True)
        open(os.path.join(src, "pybind_%s.c" % a.name), "w", encoding="utf-8").write(text)
        open(os.path.join(src, "%s.c" % a.name), "w", encoding="utf-8").write(open(a.product, encoding="utf-8").read())
        open(os.path.join(a.scaffold, "setup.py"), "w", encoding="utf-8").write(SETUP_PY % dict(name=a.name))
        open(os.path.join(a.scaffold, "pyproject.toml"), "w", encoding="utf-8").write(PYPROJECT)
        print("scaffold: %s（`cd %s && pip install .`）" % (a.scaffold, a.scaffold))


if __name__ == "__main__":
    main()
