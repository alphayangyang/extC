# extC 编译器（C 实现）
#
# 写这份 C 代码的规矩 = extC 将来要强制的规矩，详见 src/base.h 顶部。

CC       ?= gcc
CSTD     ?= -std=c11
CFLAGS   ?= $(CSTD) -Wall -Wextra -Wpedantic -O1 -g
SRCDIR   := src
BUILDDIR := build
SRCS     := $(wildcard $(SRCDIR)/*.c)
OBJS     := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))
BIN      := $(BUILDDIR)/extc

all: $(BIN)

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

# 便利目标：对 examples 里的文件 dump token
tokens: $(BIN)
	./$(BIN) --dump-tokens examples/hello.extc

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean tokens
