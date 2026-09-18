/* 把一个文件变成 C 字节数组。
 *
 * 用法: embed <输入文件> <输出的 .c> <符号名>
 *
 * 生成：
 *   const unsigned char <符号名>[] = { 0x73, 0x74, ... };
 *   const unsigned long <符号名>_len = 1234;
 *
 * 为什么用 C 写而不是 python：
 *   ① 这个项目的工具也应该是 C（这是 C 程设作业）
 *   ② 不引入任何解释器依赖 —— make 只要有一个 C 编译器就够
 *
 * 跟编译器一样：跑一次就退出，所以**不手动 free**。
 */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <input> <output.c> <symbol>\n", argv[0]);
        return 2;
    }
    const char *inPath  = argv[1];
    const char *outPath = argv[2];
    const char *sym     = argv[3];

    FILE *in = fopen(inPath, "rb");
    if (!in) {
        fprintf(stderr, "embed: cannot read `%s`\n", inPath);
        return 1;
    }
    if (fseek(in, 0, SEEK_END) != 0) { fclose(in); return 1; }
    long n = ftell(in);
    if (n < 0) { fclose(in); return 1; }
    rewind(in);

    unsigned char *buf = (unsigned char *)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) {
        fclose(in);
        fprintf(stderr, "embed: out of memory\n");
        return 1;
    }
    size_t rd = fread(buf, 1, (size_t)n, in);
    fclose(in);

    FILE *out = fopen(outPath, "wb");
    if (!out) {
        fprintf(stderr, "embed: cannot write `%s`\n", outPath);
        return 1;
    }

    fprintf(out, "/* 自动生成 —— 请勿手工编辑。源文件：%s */\n", inPath);
    fprintf(out, "const unsigned char %s[] = {\n", sym);
    for (size_t i = 0; i < rd; i++) {
        if (i % 16 == 0) fputs("    ", out);
        fprintf(out, "0x%02x,", buf[i]);
        if (i % 16 == 15 || i + 1 == rd) fputc('\n', out);
        else fputc(' ', out);
    }
    fputs("};\n", out);
    fprintf(out, "const unsigned long %s_len = %luUL;\n", sym, (unsigned long)rd);

    fclose(out);
    return 0;
}
