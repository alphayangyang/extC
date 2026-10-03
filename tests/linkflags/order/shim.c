/* 命令行经由 --ccflag 传进来的**源文件**：它引用 zlib。链接行上它必须排在模块带来的
 * `-lz` 之前（`--as-needed` 会丢掉"需要它的人还没出现"的那些库）—— 这条判据就是钉这个顺序的。 */
extern const char *zlibVersion(void);
const char *extc_order_ver(void) { return zlibVersion(); }
