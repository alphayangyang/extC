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
SRCS     := $(wildcard $(SRCDIR)/*.c)
OBJS     := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d)
BIN      := $(BUILDDIR)/extc
EMBED    := $(BUILDDIR)/embed

all: $(BIN)

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

# 把 stdlib/*.extc 嵌进编译器（prelude 机制，见 PLAN.md 的 T4b）。
# 用 C 写的工具 —— 不引入解释器依赖。
$(EMBED): $(TOOLDIR)/embed.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $< -o $@

$(BUILDDIR)/prelude_data.c: $(STDLIB)/prelude.extc $(EMBED)
	./$(EMBED) $< $@ extc_prelude_src

test: $(BIN)
	./tests/run.sh

clean:
	rm -rf $(BUILDDIR)

-include $(DEPS)

.PHONY: all test clean
