# extC 编译器（C 实现）
#
# 写这份 C 代码的规矩 = extC 将来要强制的规矩，详见 src/base.h 顶部。
#
# ⚠️ `-MMD -MP` 生成头文件依赖，**不能去掉**。
#    踩过：改 ast.h 让 sizeof(StructDef) 变了，但只有部分 .o 重编，
#    结果新旧 ABI 混在一起 → 段错误，而且症状看起来像代码 bug。

CC       ?= gcc
CSTD     ?= -std=c11
CFLAGS   ?= $(CSTD) -Wall -Wextra -Wpedantic -O1 -g
DEPFLAGS := -MMD -MP
SRCDIR   := src
TOOLDIR  := tools
STDLIB   := stdlib
BUILDDIR := build
# `src/` is a tree, not a flat directory (`base/ front/ ast/ types/ plan/ check/ back/`), so
# the sources are found recursively. **The object path keeps the subdirectory name**: a flat
# `build/ast.o` would have collided the moment two directories had a file of the same name,
# and a silent collision is a wrong build, not a compile error.
SRCS     := $(shell find $(SRCDIR) -name '*.c' | sort)
GENSRC   := $(BUILDDIR)/prelude_data.c
# The objects live under `build/obj/`, **not** directly under `build/`: `extc --run` writes the
# program's generated C and binary into `build/` (`build/types.c`, `build/types`), and a
# subdirectory named after a source directory (`build/types/`) made the linker fail with
# "cannot open output file build/types: Is a directory". Two users of one directory is the bug;
# the object tree moving one level down is the fix.
OBJDIR   := $(BUILDDIR)/obj
OBJS     := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS)) $(OBJDIR)/prelude_data.o
# Every directory of the tree is on the include path, so a file may keep naming its
# neighbours by basename (`#include "ast.h"`) no matter which subdirectory they sit in.
INCDIRS  := $(sort $(dir $(shell find $(SRCDIR) -name '*.h')))
CFLAGS   += $(addprefix -I,$(INCDIRS))
DEPS     := $(OBJS:.o=.d)
BIN      := $(BUILDDIR)/extc
EMBED    := $(BUILDDIR)/embed

all: $(BIN)

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

# 把 stdlib/*.extc 嵌进编译器（prelude 机制，见 PLAN.md 的 T4b）。
# 用 C 写的工具 —— 不引入解释器依赖。
$(EMBED): $(TOOLDIR)/embed.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $< -o $@

$(BUILDDIR)/prelude_data.c: $(STDLIB)/prelude.extc $(EMBED)
	./$(EMBED) $< $@ extc_prelude

$(OBJDIR)/prelude_data.o: $(BUILDDIR)/prelude_data.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

test: $(BIN)
	./tests/run.sh

clean:
	rm -rf $(BUILDDIR)

-include $(DEPS)

.PHONY: all test clean
