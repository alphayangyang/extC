#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extC GC 策略 Roofline 模型 v1.4（P1 落地后；配套 docs/analysis/MODEL-UPDATE-P1.md、GC-ROOFLINE.md、CALIBRATION.md）

一键重生成：
    python3 docs/analysis/model/roofline.py
产出：
    docs/analysis/model/roofline.png          十联图（5x2，panel a-j）
    docs/analysis/model/roofline-table.md     数值表（数值 | 构建 | 档位 | 量具）
无网络依赖；退出码 0 即成功。只读仓库，不写源码。

相对 v1.0 的改动（全部来自 task-13 标定，见 CALIBRATION.md §9）：
    s_bar 80 -> 42.50 B；L 777840 -> 753504 B (p50)；H_trig 1.049 -> 2.130 MB；
    sigma 0.7415 -> 0.345（且改用 sigma_prev 判墙）；r 1.349 -> 2.83；
    tau_t 14.4 -> 17.49 ns/obj；tau_s 0.63 -> 0.458 ns/slot；c_t 0.18 -> 0.4114 ns/B；
    c_s 0.045 -> 0.0108 ns/B；kappa 20us -> 0（不需要）；a_peak 5.5 -> >=10.2 GB/s；
    a_m(digraph 1M) 0.515 -> 0.309 GB/s；lambda_GC 0.212 -> 0.2508 ns/B；
    成本函数 lambda(r) = (c_t + c_s*F_slab*r)/(r-1)（带 slab 乘数），实测误差 -2.0%~0.0%。

构建口径（重要）：本脚本所有实测锚点来自 2026-11-09 task-13 标定时的
build-th/extc md5 72c05dff0ef4f9052d145246f0b2d4c3（src/back/reclaim.c b220c448... 未提交 patch 态）。
源码 FREEZE（base 85b8871b）之后未重跑 task-13 标定 ⇒ 第 A-E 段的实测锚点一律按 build-th 口径读。

v1.4（P1 落地后，task-30）：新增锚点 **build-p1/extc b9fd57843f2c734c16756f6850ace9c5**，
冻结口径 v7：src/back/reclaim.c e17080d1c80513b27ca06f2bc6224b0f（4031 行）、
src/back/codegen.c d339382889b87c0dd18b357c69e2e779（16017 行）、
tests/reclaim/driver.c 7b9ac61a55c140640a5964ab74a8780e（1523 行）。
改动：图 4x2 -> 5x2（新增 (i) seg 通道、(j) regionFree 通道）；表新增 P 段（P1 修正）与 J 段（G-P1 门禁）；
P 由单一 SCAN_NS_W 常数改为 per-shape 拟合 c_fix/c_scan + markResume 快路径 W*/T/a_max；
作废「SCAN_CHUNK 8192->128 是 5us 门禁硬前提」。推导与旧->新替换表见 docs/analysis/MODEL-UPDATE-P1.md。
"""

import math
import os

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
PNG = os.path.join(HERE, 'roofline.png')
MD = os.path.join(HERE, 'roofline-table.md')

BUILD = 'HEAD 85b8871b / FREEZE v7 (reclaim e17080d1, codegen d3393828)'
BINMD5 = 'extc md5 b9fd57843f2c734c16756f6850ace9c5 (build-p1)'

# =================== 1. 参数表（每个常量带来源标签） ===================
MB = 1 << 20
MB_DEC = 1e6          # 十进制 MB：文档与本脚本的 M 预算单位都是 1e6

# --- 实测：几何/相位（CALIBRATION.md 第 3 节，digraph 1M 默认档） ---
SBAR = 42.50          # [实测] 平均对象载荷字节 = Sigma marked_bytes / Sigma marked_objs
L50 = 753504.0        # [实测] 活集 p50 字节
L_PEAK = 2332880.0    # [实测] 活集最大字节（决定内存界）
H50 = 2130240.0       # [实测] 触发堆 p50 字节
M_PEAK_1M = 6439536.0 # [实测] 全程 r->bytes 峰值
SIGMA = 0.345         # [实测] sigma = L_current/H（p50）；注意判墙要用 sigma_prev
SIGMA_PREV = 0.354    # [实测] sigma_prev = L_prev/H（p50）
R_MEAS = 2.83         # [实测] r = H/L_prev（p50）；min = 2.425
R_MIN_MEAS = 2.425    # [实测] min(H/L_prev)
SIGMA_PREV_MAX = 0.412  # [实测] max(L_prev/H)
F_SLAB = 1.30         # [实测] cap/H（1M）；250k 1.26、p4 1.05

# --- 实测：每单位成本（CALIBRATION.md 第 4/5/7 节） ---
TAU_T = 17.49e-9      # [实测] 每对象 trace 成本 s（250k 16.10）
TAU_S = 0.458e-9      # [实测] 每槽 sweep 成本 s（250k 0.495、p4 0.66）
C_T = 0.4114          # [实测] 每被 trace 载荷字节 ns/B（250k 0.3790）
C_S = 0.0108          # [实测] 每被 sweep cap 字节 ns/B（0.0105-0.0121）
C_R = 0.23            # [待标定] 每分配字节计数成本 ns/B（0.117-0.411，无直接实测）
C_R_LO, C_R_HI = 0.117, 0.411
G_PTR = 2.76          # [历史实测] 每对象平均推入指针数（MARK-MODEL.md；新 HEAD 未复测，见 CALIBRATION §11#3）

# --- 实测：吞吐 ---
A_PEAK = 10.2         # [实测下界] GB/s，spectrum_p1 纯 arena 512B*200k/0.01s
A_M_1M = 0.309        # [实测] digraph 1M a_all GB/s（墙钟口径）
A_MUT_1M = 0.335      # [实测] digraph 1M a_mut GB/s（扣 GC 时间）
A_M_250K = 0.915      # [实测] digraph 250k a_all
A_M_P4 = 3.166        # [实测] spectrum_p4 8k a_all
LAM_1M = 0.2508       # [实测] digraph 1M lambda_GC ns/B
LAM_250K = 0.1433     # [实测] digraph 250k
LAM_P4 = 0.0245       # [实测] spectrum_p4 8k
A_STEP = 224.0        # [实测] 每个 mark 步之间的分配次数（1M：32,235 alloc / 143.7 step）

# --- 源码常量（行号见 GC-ROOFLINE.md §8） ---
K_TICK = 64           # [源码] MARK_EVERY      reclaim.c:901-902
B_MAX = 256           # [源码] MARK_BATCH_MAX  reclaim.c:910-911
W_SWEEP = 8           # [源码] SWEEP_BUDGET   reclaim.c:887-889
FLOOR = 1 << 20       # [源码] EXTC_RC_FLOOR  reclaim.c:140-141
SCALE_MAX = 1 << 40   # [源码] EXTC_RC_SCALE_MAX reclaim.c:143-144
THR_MUL = 2           # [源码] thrUpd: thr = max(2*live, floor)*scale  reclaim.c:2636-2642

# --- 约束（来自门禁/预算，非实测） ---
KAPPA_M = 0.772          # [实测] 峰值商 (peak-L)/H，digraph 1M（§3.6）
KAPPA_M_250K = 0.825     # [实测] digraph 250k
KAPPA_M_P4 = 1.010       # [实测] spectrum_p4 8k（活集 << floor，几乎不回收）
R_OVER = 0.40            # [实测] 峰值轮 r=2.43 -> 全程 r_eff p50=2.83 的差
P_MAX = 5e-6          # [门禁] GC-ROOFLINE.md:463 G1 的 p99 <= 5us
RHO_MAX = 0.30        # [预算] GC 允许占用的墙钟上限（画三条屋顶用）


# =================== 2. 模型函数 ===================
def lam(r, c_t=C_T, c_s=C_S, f_slab=F_SLAB):
    """每分配字节的 GC 成本 [ns/B]：mark 摊到 (r-1)L 个垃圾 + sweep F_slab*r*L。"""
    return (c_t + c_s * f_slab * r) / (r - 1.0)


def r_min(b=float(B_MAX), g=G_PTR, k=K_TICK):
    """trace-rate 墙：每 K 次分配一步、一步 b 个 pop => r >= 1 + g*K/b。"""
    return 1.0 + g * k / b


def sigma_max(b=float(B_MAX), g=G_PTR, k=K_TICK):
    """trace-rate 墙的 sigma 形式：sigma_prev <= b/(b+gK)。"""
    return b / (b + g * k)


def b_opt(p_max=P_MAX, tau_t=TAU_T):
    """停顿墙：b = min(MARK_BATCH_MAX, (P_max - sweep_pause)/tau_t)。"""
    return min(B_MAX, (p_max - W_SWEEP * 64.0 * TAU_S) / tau_t)


def P_of(b, tau_t=TAU_T, tau_s=TAU_S):
    """单步停顿：b 个 pop + 8 个 word 的扫描。"""
    return b * tau_t + W_SWEEP * 64.0 * tau_s


def M_of_L(L, r):
    """内存界（thr=2L 与 roundOk 信用）：M >= L*(1+r)。"""
    return L * (1.0 + r)


def M_of_L_floor(L):
    """floor 主导区（2L < F）：M >= L + F。"""
    return L + FLOOR


def r_pol(M, L, kappa=KAPPA_M):
    """内存界允许的最大「政策 r」：M = L + kappa*(r-1)L => r = 1 + (M/L - 1)/kappa。

    kappa=KAPPA_M(0.772，实测) 为典型口径；kappa=1.0 为保守上界。
    """
    return (M / L - 1.0) / kappa


def r_eff(r_policy):
    """政策 r -> 实测生效 r = H/L_prev（实测 r_policy=2 时 r_eff=2.83，差 R_OVER）。"""
    return r_policy + R_OVER


def r_mem(M, L):
    """保守口径（kappa=1）：给定 M 与 L 允许的最大 r。仅用于保证不超内存。"""
    return M / L - 1.0


def r_cross(c_r=C_R, c_t=C_T):
    """S2 清扫 vs S3 计数的交叉触发比：r = 1 + c_t/c_r（c_s 项可忽略）。"""
    return 1.0 + c_t / c_r


def a_roof(lam_ns, rho=RHO_MAX):
    """GC 屋顶：tax = lambda*a <= rho => a <= rho/lambda [GB/s]。"""
    return rho / lam_ns


def tax(lam_ns, a_gbs):
    """GC 税 = lambda * a（ns/B * GB/s 无量纲）。"""
    return lam_ns * a_gbs


plt.rcParams['font.sans-serif'] = ['Microsoft YaHei', 'SimHei', 'Noto Sans CJK SC',
                                   'WenQuanYi Zen Hei', 'Droid Sans Fallback', 'DejaVu Sans']
plt.rcParams['axes.unicode_minus'] = False

fig = plt.figure(figsize=(15.6, 24.4))
TITLE = 'extC GC Roofline v1.4（标定后 + memory-first + quota + P1 通道化）| x=每分配字节 GC 成本 lambda  y=分配率 a | s_bar=42.5B c_t=0.411 c_s=0.0108 tau_t=17.5ns | v7 锚点 build-p1 b9fd5784'
fig.suptitle(TITLE, fontsize=12.0)

# =================== 3. 面板 (a) Roofline 平面 ===================
ax = fig.add_subplot(5, 2, 1)
lam_axis = np.logspace(-2.2, 0.7, 500)
for rho, style in [(0.05, ':'), (0.15, '--'), (RHO_MAX, '-')]:
    ax.plot(lam_axis, rho / lam_axis, style, lw=1.2, color='tab:red', alpha=0.75,
            label='GC 屋顶 rho=%.0f%%：a=rho/lambda' % (100 * rho))
ax.fill_between(lam_axis, 1e-2, np.minimum(RHO_MAX / lam_axis, A_PEAK), color='tab:red', alpha=0.05)
ax.axhline(A_PEAK, color='green', lw=1.8, label='S1/硬件平台 a_peak>=%.1f GB/s [实测下界 p1]' % A_PEAK)
ax.axhline(A_MUT_1M, color='tab:brown', lw=1.1, ls='-.', label='digraph 1M a_mut=%.2f GB/s（扣 GC 后）' % A_MUT_1M)

RIDGE = RHO_MAX / A_PEAK
ax.plot([RIDGE], [A_PEAK], 'D', ms=8, color='purple', label='ridge 点 lambda*=rho/a_peak=%.3f ns/B' % RIDGE)
ax.axvline(RIDGE, color='purple', ls=':', lw=0.9, alpha=0.7)

PTS = [('digraph 1M', LAM_1M, A_M_1M, 'tab:blue'),
       ('digraph 250k', LAM_250K, A_M_250K, 'tab:cyan'),
       ('spectrum_p4', LAM_P4, A_M_P4, 'tab:olive')]
for nm, l_, a_, col in PTS:
    ax.plot(l_, a_, 'o', ms=8, mfc='white', mec=col, mew=1.8)
    ax.annotate('%s\na=%.2f, lam=%.3f, tax=%.1f%%' % (nm, a_, l_, 100 * tax(l_, a_)),
                (l_, a_), textcoords='offset points', xytext=(8, 7), fontsize=7.4, color=col)
ax.annotate('S1: 无 GC，lambda->0，点落在平台线上（p1 512B*200k/0.01s）', (0.0105, A_PEAK),
            textcoords='offset points', xytext=(6, 6), fontsize=7.2, color='green')

CONFIGS = [('M=7MB (=3L_peak)', 7 * MB_DEC), ('M=12MB', 12 * MB_DEC), ('M=21MB', 21 * MB_DEC)]
for nm, M in CONFIGS:
    rp = r_pol(M, L_PEAK)          # 典型 kappa_M 下峰值轮允许的 r
    re = r_eff(rp)                 # 全程 r_eff -> 代价
    la = lam(re)
    ax.axvline(la, color='tab:purple', ls='-.', lw=1.0, alpha=0.8,
               label='%s: r_peak=%.2f -> r_eff=%.2f, lam=%.3f ns/B' % (nm, rp, re, la))
ax.set_xscale('log'); ax.set_yscale('log')
ax.set_xlim(0.006, 4.0); ax.set_ylim(0.06, 30)
ax.set_xlabel('lambda = 每分配字节 GC 成本 [ns/B] = (c_t + c_s*F_slab*r)/(r-1)')
ax.set_ylabel('a = 受管分配率 [GB/s]')
ax.set_title('(a) Roofline 平面：点离 GC 屋顶的垂直距离 = log(1/tax)；横轴由 r 的选择决定')
ax.grid(True, which='both', alpha=0.25)
ax.legend(fontsize=6.8, loc='lower left')

# =================== 4. 面板 (b) 内存屋顶 ===================
ax = fig.add_subplot(5, 2, 2)
Ls = np.logspace(math.log10(0.05 * MB_DEC), math.log10(32 * MB_DEC), 400)
ax.plot(Ls / MB_DEC, 3.0 * Ls / MB_DEC, '-', color='tab:red', lw=1.8, label='M = 3L（2L > floor 区，默认 thr=2L）')
ax.plot(Ls / MB_DEC, M_of_L_floor(Ls) / MB_DEC, '--', color='tab:orange', lw=1.6, label='M = L + floor（2L < floor 区）')
ax.plot(Ls / MB_DEC, 2.69 * Ls / MB_DEC, ':', color='tab:green', lw=1.3, label='M = 2.69L（r_min=1.69 的最小可行）')
MEAS = [('digraph 250k', 586160.0, 2145816.0), ('digraph 1M', 2332880.0, 6439536.0),
        ('spectrum_p4', 12256.0, 1085152.0)]
for nm, Lpk, pk in MEAS:
    ax.plot(Lpk / MB_DEC, pk / MB_DEC, 'o', ms=8, mfc='white', mec='tab:blue', mew=1.8)
    ax.annotate('%s\nL_peak=%.2fMB peak=%.2fMB (%.2fL)' % (nm, Lpk / MB_DEC, pk / MB_DEC, pk / Lpk),
                (Lpk / MB_DEC, pk / MB_DEC), textcoords='offset points', xytext=(8, -14), fontsize=7.2, color='tab:blue')
ax.set_xscale('log'); ax.set_yscale('log')
ax.set_xlim(0.05, 32); ax.set_ylim(0.05, 40)
ax.set_xlabel('活集峰值 L_peak [MB（1e6），对数]')
ax.set_ylabel('堆峰值 M [MB（1e6），对数]')
ax.annotate('floor 区（L 接近/小于 floor）实测 peak 高于 L+floor：\n含 in-flight 标记与下一轮预分配（250k +31%）',
            (0.07, 6.0), fontsize=6.8, color='tab:gray')
ax.set_title('(b) 内存屋顶：M 由 thr 倍数（2）而非 slab 决定；floor 只在小堆区兜底')
ax.grid(True, which='both', alpha=0.25)
ax.legend(fontsize=7.0, loc='upper left')
# =================== 5. 面板 (c) 触发比 r 的两面墙 ===================
ax = fig.add_subplot(5, 2, 3)
rr = np.linspace(1.05, 14.0, 700)
ax.plot(rr, lam(rr), '-', color='tab:blue', lw=1.9, label='lambda(r) = (c_t + c_s*F_slab*r)/(r-1) [ns/B]（左轴）')
ax.plot(rr, np.full_like(rr, C_S * F_SLAB), ':', color='tab:blue', lw=1.1,
        label='lambda 地板 c_s*F_slab=%.4f ns/B' % (C_S * F_SLAB))
ax.axvline(r_min(), color='tab:red', ls='--', lw=1.3, label='trace-rate 墙 r_min=%.2f（b=256）' % r_min())
ax.axvspan(r_min(), 14.0, color='tab:green', alpha=0.06,
           label='keep-up 可行区 r >= r_min=%.2f' % r_min())
# 内存约束：M = L + kappa*(r-1)L => 生效 r 的上界（按 L_peak 与实测 kappa）
for M, col in [(7 * MB_DEC, 'tab:red'), (12 * MB_DEC, 'tab:green'), (21 * MB_DEC, 'tab:purple')]:
    r_hi = r_pol(M, L_PEAK)          # 典型 kappa_M
    ax.axvline(r_hi, color=col, ls='--', lw=1.0,
               label='内存墙 r_eff<=%.2f（M=%.0fMB, L=L_peak, kappa=%.2f）' % (r_hi, M / MB_DEC, KAPPA_M))
ax.plot([R_MEAS], [LAM_1M], '*', ms=15, color='tab:red',
        label='digraph 1M 实测 (r=%.2f, lam=%.4f)' % (R_MEAS, LAM_1M))
ax.annotate('口径：政策 r=2 -> 实测 r_eff=2.83（星点）；峰值轮 r=2.43 @ L=2.24MB 才对应内存墙', (1.12, 0.62), fontsize=6.6, color='tab:gray')
ax.annotate('实测：lambda=%.4f，公式给 %.4f（差 %+.1f%%）'
            % (LAM_1M, lam(R_MEAS), 100 * (lam(R_MEAS) / LAM_1M - 1)),
            (R_MEAS, LAM_1M), textcoords='offset points', xytext=(10, -22), fontsize=7.2, color='tab:red')
ax.set_xlim(1.0, 14.0); ax.set_ylim(0.008, 1.5)
ax.set_xlabel('触发比 r = H/L_prev（= 1/sigma_prev）')
ax.set_ylabel('lambda [ns/B]（左轴，对数）')
ax.set_yscale('log')
ax2 = ax.twinx()
ax2.plot(rr, 100.0 * tax(lam(rr), A_M_1M), '--', color='tab:brown', lw=1.3,
          label='GC 税 = lambda*a_all（a_all=%.2f GB/s）[%%]（右轴）' % A_M_1M)
ax2.set_ylabel('GC 税 [%]（右轴）', color='tab:brown')
ax2.tick_params(axis='y', colors='tab:brown')
ax2.set_ylim(0, 40)
h1, l1 = ax.get_legend_handles_labels()
h2, l2 = ax2.get_legend_handles_labels()
ax.legend(h1 + h2, l1 + l2, fontsize=6.6, loc='upper right')
ax.set_title('(c) r 增大：lambda 单调降、内存线性涨 => 内存预算决定最优 r*')
ax.grid(True, which='both', alpha=0.25)

# =================== 6. 面板 (d) b 的取舍：停顿 vs trace-rate ===================
ax = fig.add_subplot(5, 2, 4)
bs = np.array([16, 24, 32, 48, 64, 96, 128, 160, 192, 224, 256, 320, 384, 448, 512], dtype=float)
pp = np.array([P_of(b) * 1e6 for b in bs])
ax.plot(bs, pp, '-o', ms=4, color='tab:blue', lw=1.7, label='P(b) = b*tau_t + 8*64*tau_s [us]（左轴）')
for b in (32, 64, 128, 224, 256, 512):
    i = int(np.argmin(np.abs(bs - b)))
    ax.annotate('b=%d' % b, (bs[i], pp[i]), textcoords='offset points', xytext=(5, -9), fontsize=7.4)
ax.axhline(P_MAX * 1e6, color='tab:purple', ls='--', lw=1.2, label='P_max=%.1f us（G1 p99 门禁）' % (P_MAX * 1e6))
ax.axvline(B_MAX, color='tab:gray', ls=':', lw=1.0, label='MARK_BATCH_MAX=%d（源码上限）' % B_MAX)
bstar = b_opt()
ax.plot([bstar], [P_of(bstar) * 1e6], 's', ms=9, color='tab:red',
        label='推荐 b*=%.0f：P=%.2f us，r_min=%.2f' % (bstar, P_of(bstar) * 1e6, r_min(bstar)))
ax.set_xscale('log'); ax.set_yscale('log')
ax.set_xlim(14, 560); ax.set_ylim(0.2, 20)
ax.set_xlabel('单步批量 b（pops/步，对数）')
ax.set_ylabel('单步停顿 P [us]（左轴，对数）')
ax2 = ax.twinx()
ax2.plot(bs, [r_min(b) for b in bs], '--', color='tab:green', lw=1.4, label='r_min(b) = 1 + g*K/b（右轴）')
ax2.axhline(R_MIN_MEAS, color='tab:olive', ls=':', lw=1.1,
            label='实测 min r\' = %.3f（1M）' % R_MIN_MEAS)
ax2.set_ylabel('r_min(b)（右轴）', color='tab:green')
ax2.tick_params(axis='y', colors='tab:green')
ax2.set_ylim(1.0, 12.0)
h1, l1 = ax.get_legend_handles_labels()
h2, l2 = ax2.get_legend_handles_labels()
ax.legend(h1 + h2, l1 + l2, fontsize=6.6, loc='upper right')
ax.set_title('(d) b 越大：停顿线性涨、r_min 下降；b=256 时 P=4.7us 贴 5us 门禁')
ax.grid(True, which='both', alpha=0.25)


# =================== 6.5 面板 (e)(f)：memory-first 分支（task-20 / E16） ===================
# 数据来源：docs/analysis/MEMORY-FIRST-MODEL.md §2；构建 build-th/extc md5 c371e5253a0aa152af968298dfcfb9d1
# （与共享 build-rf 行为等价：callgrind Ir 逐位一致）；停顿/GC 拆分为 rdtsc 包裹 markStep/sweepStep 的副本量具
# （仓库未改，脚本在 /tmp/e16/instrument.py），TSC=3.071987 GHz；wall 为 9 次 perf_counter 最小值（含 ~2-4ms 进程启动）。
TSC_HZ = 3.071987e9
CY2MS = 1e3 / TSC_HZ          # cycles -> ms

# shape: (wall_ms_min, wall_ms_med, mark_cyc_med, sweep_cyc_med, allocs, rss_kb, thr_max, mmax, alloc_mb,
#         ir_total_M, ir_mark_M, ir_sweep_M, step_total_us)
MF = {
    'promote':   (37.91, 41.01, 35.83e6, 10.77e6, 4550001, 6492, 3418672, 5128008, 109.2, 124.68, 19.40, 9.54, 16816.9),
    'sizeratio': (21.56, 23.89,  2.39e6,  4.48e6, 2000001, 3240, 1048576,  344064, 160.0,  23.12,  1.75, 2.21,  2268.2),
    'chase':     (144.16, 161.09, 18.16e6, 0.0415e6, 1111077, 73692, 63438892, 69206064, 53.3, 22.21,  6.31, 0.02,  6071.2),
    'cycleratio':(13.11, 13.59,  0.841e6,  3.83e6, 1821442, 3036, 1048576,   94728,  43.2, 137.27,  2.79, 9.79,  1554.2),
}

def mf_components(nm):
    wall, wall_med, mc, sc, allc, rss, thr, mmax, amb, irt, irm, irs, step_us = MF[nm]
    mark = mc * CY2MS
    sweep = sc * CY2MS
    gc = mark + sweep
    return dict(wall=wall, wall_med=wall_med, mark=mark, sweep=sweep, gc=gc,
                allocs=allc, rss_kb=rss, thr=thr, mmax=mmax, alloc_mb=amb,
                ir_total=irt, ir_mark=irm, ir_sweep=irs, step_us=step_us,
                non_gc=wall - gc, allocs_per_ms=allc / wall)

MF_ORDER = ['promote', 'sizeratio', 'chase', 'cycleratio']

# L1 可达性：Lead 的 slab 面积探针（FREEZE v4 生成 C；sweepBegin 快照每 slab popcount(occ)，sweepWord 完成该 slab 时重算，
# 差 = 该轮该死槽数；累计 stderr）。数据见 docs/analysis/MEMORY-FIRST-MODEL.md §5.1，脚本在
# docs/analysis/P1-7-SWEEP-ELIDE.md（runtime-facts 落盘）。口径 = align 参数族（promote 200k），不用于替换 panel(e) 的收益数值。
MF_L1_PROBE = {
    'promote':    dict(slabs=879,  full_dead=0,   full_dead_slots=0,       dead_slots=1748760, live_slots=481901, words=36117),
    'sizeratio':  dict(slabs=2394, full_dead=3,   full_dead_slots=2171,    dead_slots=1995915, live_slots=195544, words=44289),
    'chase':      dict(slabs=0,    full_dead=0,   full_dead_slots=0,       dead_slots=0,       live_slots=0,      words=0),
    'cycleratio': dict(slabs=756,  full_dead=651, full_dead_slots=1739978, dead_slots=1778862, live_slots=54651,  words=30870),
}

# E18：floor 下探（Lead；FREEZE v4 生成 C 重编 -DEXTC_RC_FLOOR='((int64_t)1<<18)' / 1<<16，
# /tmp/v2/v2run --pin 6，每变体 3 reps）。shape: {floor_bytes: (rss_kb_lo, rss_kb_hi, wall_ms_lo, wall_ms_hi)}；wall=None 表示无变化。
MF_FLOOR = {
    'promote':    {1 << 20: (6492, 6492, 41.6, 43.1), 1 << 18: (6492, 6492, None, None), 1 << 16: (6492, 6492, None, None)},
    'sizeratio':  {1 << 20: (3244, 3372, 21.9, 23.9), 1 << 18: (2088, 2092, 26.4, 27.2), 1 << 16: (1896, 1900, 30.1, 31.5)},
    'cycleratio': {1 << 20: (3036, 3036, 13.0, 14.4), 1 << 18: (2076, 2076, 13.7, 13.7), 1 << 16: (1692, 1692, 18.6, 19.1)},
}

def mf_l1_reach(nm):
    """L1b（全死 slab 整块重置）能覆盖的死槽占比；chase 无 sweep 返回 0。"""
    p = MF_L1_PROBE[nm]
    return (p['full_dead_slots'] / p['dead_slots']) if p['dead_slots'] else 0.0
MF_PHI_MARK = 0.50        # [估算] 指针位图 word-AND + dirty-slab 只扫有分配的 slab，可望砍 mark 的 30-50%
MF_PHI_ALLOC = 0.40       # [上界/估算] alloc 快路（含置零与屏障检查）可归因份额，需 E17 标定
MF_GATE_WALL = 0.10       # [门禁草案] 固定 M=3L 下墙钟降幅目标 >=10%

print('== memory-first 预算（M=3*L 固定，r_policy=2 不变） ==')
mf_rows = []
for nm in MF_ORDER:
    d = mf_components(nm)
    mf_rows.append((nm, d))
    print('%-11s wall=%6.2fms  mark=%6.2f (%.1f%%)  sweep=%5.2f (%.1f%%)  nonGC=%6.2f (%.1f%%)  allocs=%.2fM'
          % (nm, d['wall'], d['mark'], 100 * d['mark'] / d['wall'], d['sweep'], 100 * d['sweep'] / d['wall'],
             d['non_gc'], 100 * d['non_gc'] / d['wall'], d['allocs'] / 1e6))

ax = fig.add_subplot(5, 2, 5)
ypos = np.arange(len(MF_ORDER))
mk = np.array([mf_components(n)['mark'] for n in MF_ORDER])
sw = np.array([mf_components(n)['sweep'] for n in MF_ORDER])
ng = np.array([mf_components(n)['non_gc'] for n in MF_ORDER])
ax.barh(ypos, mk, color='tab:blue', label='mark（trace）实测')
ax.barh(ypos, sw, left=mk, color='tab:orange', label='sweep 实测（elision 仅 cycleratio 可达）')
ax.barh(ypos, ng, left=mk + sw, color='tab:gray', alpha=0.35, label='非 GC（alloc 快路 + 形状自身 + 缺页）')
ax.set_yticks(ypos)
ax.set_yticklabels(MF_ORDER)
for i, nm in enumerate(MF_ORDER):
    d = mf_components(nm)
    rc = mf_l1_reach(nm)
    ub = 100.0 * d['sweep'] / d['wall']
    if rc > 0.5:
        lbl = 'sweep->0 可达 -%.1f%%（L1b，%.0f%% 死槽成块）' % (rc * ub, 100 * rc)
        col = 'tab:orange'
    elif ub > 1.0:
        lbl = 'sweep->0 上界 -%.1f%%（不可达：full_dead=0）' % ub
        col = 'tab:gray'
    else:
        lbl = 'sweep≈0'
        col = 'tab:gray'
    ax.annotate(lbl, (d['mark'] + d['sweep'], i), textcoords='offset points', xytext=(4, 3), fontsize=7.0, color=col)
    ax.annotate('mark -%.0f%%: -%.1f%% wall' % (100 * MF_PHI_MARK, 100 * MF_PHI_MARK * d['mark'] / d['wall']),
                (d['mark'], i), textcoords='offset points', xytext=(4, -10), fontsize=6.8, color='tab:blue')
ax.set_xlabel('墙钟预算 [ms]（同一形状内三段相加 = wall_min）')
ax.set_title('(e) memory-first 预算：sweep 只能被「全死 slab 重置」拿走（仅 cycleratio 可达）')
ax.legend(fontsize=7.0, loc='lower right')
ax.grid(True, axis='x', alpha=0.25)

ax = fig.add_subplot(5, 2, 6)
xs = np.arange(len(MF_ORDER))
rss_mb = np.array([MF[n][5] * 1024.0 / MB_DEC for n in MF_ORDER])   # kB -> 1e6 B
mmax_mb = np.array([MF[n][7] / MB_DEC for n in MF_ORDER])
ratio_m = rss_mb / np.maximum(mmax_mb, 1e-9)
floor_mb = np.full(len(MF_ORDER), FLOOR / MB_DEC)
w = 0.36
ax.bar(xs - w / 2, rss_mb, w, color='tab:purple', label='实测峰值 RSS [MB(1e6)]')
ax.bar(xs + w / 2, np.maximum(mmax_mb, 1e-9), w, color='tab:red', alpha=0.55, label='M_max = 3*L（隐式内存墙）')
ax.plot(xs, floor_mb, 'k--', lw=1.2, label='EXTC_RC_FLOOR = 1 MiB（E18：256 KiB 换 -32~35% RSS）')
for i, nm in enumerate(MF_ORDER):
    if mmax_mb[i] < floor_mb[i]:
        ax.annotate('M_max=%.2fMB < floor（墙不可达，thr 用 floor）' % mmax_mb[i],
                    (xs[i], rss_mb[i]), textcoords='offset points', xytext=(0, 5), fontsize=6.8, ha='center', color='tab:red')
    else:
        ax.annotate('RSS/M_max=%.2fx' % ratio_m[i], (xs[i], max(rss_mb[i], mmax_mb[i])),
                    textcoords='offset points', xytext=(0, 5), fontsize=7.0, ha='center')
    ax.annotate('RSS/floor=%.2fx' % (rss_mb[i] / floor_mb[i]), (xs[i], max(rss_mb[i], floor_mb[i])),
                textcoords='offset points', xytext=(0, -14), fontsize=6.8, ha='center', color='tab:gray')
ax.set_xticks(xs)
ax.set_xticklabels(MF_ORDER)
ax.set_ylabel('堆 [MB(1e6)，线性]')
ax.set_title('(f) 占用分解：大活集形状 RSS≈3L（1.09-1.30x M_max）；小活集由 floor 决定')
ax.legend(fontsize=7.0, loc='upper left')
ax.grid(True, axis='y', alpha=0.25)

# =================== 6.7 P1 分支：P 通道化（seg / 配额 / regionFree）+ markResume 快路径（task-30 / MODEL-UPDATE-P1.md） ===================
# 口径 v7：锚点 build-p1/extc b9fd5784；本段数字均为 Lead/队友实测（本轮未跑计时程序）。
SEG_CHUNKS = [128, 256, 512, 8192]
SEG_PD_P999 = {128: 1.65, 256: 1.84, 512: 8.19, 8192: 34.08}    # [实测] [segsteps] dl800 pd, p99.9 us
SEG_DENSE_P999 = {128: 1.39, 256: 1.98, 8192: 56.40}            # [实测] dl800 dense
SEG_PD_DL0 = {128: 0.65, 256: 0.89, 512: 1.68, 8192: 87.28}     # [实测] dl0 pd
C_SCAN_PD = 4.02e-9       # [实测拟合] s/word：pd 两端点 128/8192
C_FIX_PD = 1.14e-6        # [实测拟合] s
C_SCAN_DENSE = 6.82e-9    # [实测拟合] s/word：dense 128/8192
C_FIX_DENSE = 0.52e-6
SEG_SAFE = 2.0            # 保守倍数：chunk=512 点偏离两点拟合 +157%
Q_SHIFT_MARK = 3
Q_MAX_MARK = 1 << 20
Q_MAX_WORDS = 8192
Q_DEADLINE_US = 800
RELEASE_BUDGET = 2048
FREE_BUDGET_SLABS = 8
FREE_DEADLINE_US = 100
FREE_QUEUE_BYTES = 8 << 20
FREE_HARD_DEADLINE_US = 1000
W_SWEEP_U = 1032                # 合法 slab：8*pm + ceil(24672/64) 上限 1024
W1_NS_WORD = 64.0 * TAU_S * 1e9  # 一 word=64 槽逐槽链写 => 29.31 ns/word
RF = {  # (slabs, sync_us, queue_us, verdict)
    'pd_bigobj200': (4, 0.5, 662.5, 'PASS'),
    'dense_G18': (1, 0.0, 1708.4, 'REG 大块 munmap 不可切'),
    'pb_escape100M': (24510, 13755.6, 1005.0, 'RED sync 未闭合'),
}
RF_ORDER = ['pd_bigobj200', 'dense_G18', 'pb_escape100M']
RF_C_SYNC_US = 561.0e-3          # [实测拟合] us/slab（pb 两点；pd 4 slab <=125 ns/slab）
P1_WALL_RATIO = {'promote': 1.054, 'sizeratio': 1.050, 'chase': 1.004, 'cycleratio': 1.007}  # [实测] 相对 HEAD
ELIDE = {'ir_v0_M': 273.19, 'ir_v1_M': 284.59, 'ir_net_M': 8.73,
         'wall_v0_ms': 15.51, 'wall_v1_ms': 15.38, 'slices_v0': 1221, 'slices_v1': 255}
P_SEG_GATE_US = 64.0
P_WARM_P999_US = 10.0
P_COLD_P999_US = 1000.0

def P_seg(chunk_w, c_fix=C_FIX_PD, c_scan=C_SCAN_PD):
    """续扫段步上界 [s]：c_fix + c_scan*chunk（按实测两点拟合）。"""
    return c_fix + c_scan * np.asarray(chunk_w, dtype=float)

def seg_eff(chunk_w, c_fix=C_FIX_PD, c_scan=C_SCAN_PD):
    """段效率：每步有 c_fix 固定开销，只有余下部分才对应有效扫描字数。"""
    c = np.asarray(chunk_w, dtype=float)
    return c * c_scan / (c_fix + c * c_scan)

def W_star(p_max=P_MAX, c_fix=C_FIX_PD, c_scan=C_SCAN_PD):
    """快路径下 P_step<=p_max 允许的单步字数（与 chunk 解耦）。"""
    return max(0.0, (p_max - c_fix) / c_scan)

def T_words(w, c_fix=C_FIX_PD, c_scan=C_SCAN_PD):
    return w / (c_fix + c_scan * w)

def a_max_gbs(w, sigma=1.0, c_fix=C_FIX_PD, c_scan=C_SCAN_PD):
    """keep-up 上限 [GB/s]：需求扫描率 a*sigma/8 word/s <= T(w) => a <= 8T/sigma。"""
    return 8.0 * T_words(w, c_fix, c_scan) / sigma / 1e9

def P_rf(slabs, forced=False):
    """regionFree 通道单步上界 [us]：同步段 O(#slabs) + 排空预算（100 us 常态 / 1000 us 强制）。"""
    b = FREE_HARD_DEADLINE_US if forced else FREE_DEADLINE_US
    return RF_C_SYNC_US * slabs + b

# =================== 6.6 面板 (g)(h)：quota 分支（P 字节节奏配额 + B 惰性 freelist；task-25 / QUOTA-MODEL.md） ===================
# 数据源：docs/analysis/QUOTA-MODEL.md、docs/analysis/P0-PROTOTYPE.md（build-lead5 = FREEZE v5 生成 C 原型）、
#         GC-ROOFLINE.md §3（lambda/P/b 模型）、MEMORY-MODEL-CURRENT.md §18（内存界）
K_SWEEP_QUOTA = 6          # [原型] sweep words = max(8*pressure, n>>6)
K_MARK_QUOTA = 10          # [原型] mark advances = n>>10（每批多扫一个 SCAN_CHUNK 窗口）
SCAN_CHUNK_W = 8192        # [源码] EXTC_RC_SCAN_CHUNK 8192 words = 64 KiB
SCAN_NS_W = 11.0e-9        # [实测] 8192-word 段 78-98 us => 9.5-12 ns/word（取 11，MEMORY-MODEL §19.1）
W_WORD = 64 * TAU_S        # 29.31 ns/word（sweep 一 word 64 槽）
ADV_NS = B_MAX * TAU_T     # 4477 ns/advance（b=256 pops）
C_A = C_T + C_S * F_SLAB   # 0.42544 ns/B = 每字节一次 trace + 一次 sweep
C_STAR = lam(2.0)          # 0.43948 ns/B = thr=max(2L,floor) 合法下界处的所需单价
Q_LIN = 1e9 * (W_WORD / 2 ** K_SWEEP_QUOTA + ADV_NS / 2 ** K_MARK_QUOTA)   # 4.8305 ns/B 线性渐近
KS_MAX = float(np.log2(64 * SBAR / F_SLAB))         # 11.031
KM_MAX = float(np.log2(B_MAX * SBAR / G_PTR))       # 11.945（pops 通道，比 scan 通道更紧）
BUF_L = C_T / C_STAR - 0.25                         # 0.6861：内存界的 L 系数
MF_P0 = {   # (step_total_base_us, step_total_B_us, stepmax_base_us, stepmax_B_us, wall_base_ms, wall_B_ms)
    'promote':    (17752.0, 15528.0, 160.4, 57.5, 42.78, 43.77),
    'sizeratio':  (2581.0, 1247.0, 51.8, 9.5, 23.54, 23.89),
    'cycleratio': (471.8, 432.0, 39.0, 16.9, 13.72, 12.43),
    'chase':      (0.0, 0.0, 0.0, 0.0, 172.66, 169.50),
}
MF_P0_ORDER = ['promote', 'sizeratio', 'cycleratio', 'chase']

def q_sweep_words(n, k=K_SWEEP_QUOTA):
    """配额推进的 sweep word 数（按分配字节数步进）。"""
    n = np.asarray(n, dtype=float)
    return np.maximum(float(W_SWEEP), np.floor(n / 2 ** k))

def q_mark_adv(n, k=K_MARK_QUOTA):
    """配额推进的 mark advance 批次（每批多扫一个 SCAN_CHUNK 窗口）。"""
    return np.floor(np.asarray(n, dtype=float) / 2 ** k)

def quota_capacity_ns(n, k_s=K_SWEEP_QUOTA, k_m=K_MARK_QUOTA):
    """一次分配 n 字节预付的回收服务能力 [ns] = q_sweep*W_WORD + q_mark*ADV_NS。"""
    return 1e9 * (W_WORD * q_sweep_words(n, k_s) + ADV_NS * q_mark_adv(n, k_m))

def P_quota(n, b=B_MAX, chunk_w=SCAN_CHUNK_W, cap_w=None, cap_b=None,
            k_s=K_SWEEP_QUOTA, k_m=K_MARK_QUOTA, tau_t=TAU_T):
    """配额开启后的单步停顿上界 [s]（原型语义：固定 pops 预算 + sweep 增量 + advance 的 chunk 扫描）。"""
    n = np.asarray(n, dtype=float)
    sw = q_sweep_words(n, k_s)
    if cap_w is not None:
        sw = np.minimum(sw, float(cap_w))
    extra = np.maximum(0.0, sw - float(W_SWEEP))
    adv = q_mark_adv(n, k_m)
    if cap_b is not None:
        adv = np.minimum(adv, float(cap_b))
    return b * tau_t + extra * W_WORD + adv * float(chunk_w) * SCAN_NS_W

print('== quota 自检（P 字节配额 + B；FREEZE v5 常量 c_t=0.4114 c_s=0.0108 F=1.30 tau_t=17.49ns tau_s=0.458ns b=256） ==')
print('  c_A=c_t+c_s*F=%.5f ns/B；c*=lambda(r=2)=%.5f ns/B；Q_lin=%.4f ns/B => 过配 %.2fx（c*）/ %.2fx（c_A）'
      % (C_A, C_STAR, Q_LIN, Q_LIN / C_STAR, Q_LIN / C_A))
print('  keep-up 容许的最松位移：k_s<=%.2f、k_m<=%.2f（pops 通道更紧）；原型 6/10 => 余量 sweep %.1fx / pops %.2fx'
      % (KS_MAX, KM_MAX, 2 ** (KS_MAX - K_SWEEP_QUOTA), 2 ** (KM_MAX - K_MARK_QUOTA)))
print('  内存界：M_peak <= M_max + max(L,floor) + %.3f L + n_max（合计 %.3f L + max(L,floor) + n_max）'
      % (BUF_L, 1 + BUF_L))

# ------------------- 面板 (g)：配额函数与需求线 -------------------
ax = fig.add_subplot(5, 2, 7)
n_axis = np.logspace(2.0, 7.3, 500)          # 100 B .. 20 MB
ax.loglog(n_axis, quota_capacity_ns(n_axis) / n_axis, 'tab:blue', lw=2.0,
          label='Q(n)/n 原型（k_s=6, k_m=10）')
ax.loglog(n_axis, quota_capacity_ns(n_axis, k_s=11, k_m=11) / n_axis, 'tab:cyan', lw=1.2, ls='--',
          label='k_s=11, k_m=11（keep-up 上界附近）')
ax.axhline(C_STAR, color='crimson', lw=1.4, label='c* = lambda(r=2) = %.3f ns/B' % C_STAR)
ax.axhline(C_A, color='darkred', lw=1.0, ls=':', label='c_A = c_t + c_s*F = %.3f ns/B' % C_A)
ax.axhline(LAM_1M, color='tab:green', lw=1.0, ls='-.', label='现状所需 lambda(r_eff=2.83) = %.3f' % LAM_1M)
ax.annotate('过配 %.1fx = 每字节预付 %.1f 次 trace+sweep\n（锯齿=floor(n/64) 量化，包络 = Q_lin）' % (Q_LIN / C_STAR, Q_LIN / C_STAR),
            xy=(3e5, Q_LIN), xytext=(6.0e2, 7.2), fontsize=7.0, color='tab:blue',
            arrowprops=dict(arrowstyle='->', color='tab:blue', lw=0.8))
ax.set_ylim(0.15, 12.0)
ax.set_xlabel('单次分配字节数 n [B]')
ax.set_ylabel('每字节服务能力 Q(n)/n [ns/B]')
ax.set_title('(g) 配额函数：阶梯 -> 线性渐近 %.2f ns/B，是 c* 的 %.1fx' % (Q_LIN, Q_LIN / C_STAR))
ax.grid(True, which='both', alpha=0.25)
ax.legend(fontsize=6.8, loc='upper right')

# ------------------- 面板 (h)：配额步的停顿 vs 5 us 门禁 -------------------
ax = fig.add_subplot(5, 2, 8)
ax.axhline(P_MAX * 1e6, color='crimson', lw=1.8, label='P_max = %.0f us（门禁 G1）' % (P_MAX * 1e6))
ax.loglog(n_axis, np.full_like(n_axis, P_of(B_MAX) * 1e6), color='tab:gray', lw=1.6,
          label='固定预算 b=256（无配额）%.2f us' % (P_of(256) * 1e6))
cfg = [('原型字面：chunk=8192 字、无 CAP', dict(b=float(B_MAX), chunk_w=SCAN_CHUNK_W), 'tab:red', '-'),
       ('旧推荐 A（chunk 结论已否）：b=224、cap_w=16', dict(b=224.0, chunk_w=SCAN_CHUNK_W, cap_w=16, cap_b=0), 'tab:orange', '--'),
       ('旧推荐 B（已否）：b=192、chunk=128 字、cap_b=1', dict(b=192.0, chunk_w=128, cap_w=8, cap_b=1), 'tab:blue', '-.'),
       ('旧推荐 C（已否）：b=160、chunk=64 字、cap_b=1', dict(b=160.0, chunk_w=64, cap_w=8, cap_b=1), 'tab:green', ':')]
for lbl, kw, col, st in cfg:
    yy = P_quota(n_axis, **kw) * 1e6
    ax.loglog(n_axis, yy, color=col, ls=st, lw=1.5, label='%s（max %.1f us）' % (lbl, float(yy.max())))
ax.set_ylim(2.0, 3e3)
ax.set_xlabel('单次分配字节数 n [B]')
ax.set_ylabel('该分配点的单步停顿上界 P_step [us]（对数）')
ax.set_title('(h) 配额停顿（v1.3 口径）：chunk=8192 在 n>=1 KiB 即 breakout；「降 chunk」结论已作废，见 (i)')
ax.grid(True, which='both', alpha=0.25)
ax.legend(fontsize=6.4, loc='upper left')
ax.annotate('原型字面：每 n>>10 个 advance 各扫一个 64 KiB 窗口\n=> P ∝ floor(n/1024)，无上界', xy=(2e4, 1.4e3),
            xytext=(1.3e2, 1.2e2), fontsize=6.6, color='tab:red',
            arrowprops=dict(arrowstyle='->', color='tab:red', lw=0.8))

# ------------------- 面板 (i)：seg 通道 P_step vs chunk + markResume 快路径 -------------------
ax = fig.add_subplot(5, 2, 9)
ch = np.array(SEG_CHUNKS, dtype=float)
ax.loglog(ch, [SEG_PD_P999[c] for c in SEG_CHUNKS], 'o-', color='tab:blue', lw=1.5, ms=5,
          label='pd p99.9 实测（dl800）')
_dn = [c for c in SEG_CHUNKS if c in SEG_DENSE_P999]
ax.loglog(_dn, [SEG_DENSE_P999[c] for c in _dn], 's--', color='tab:purple', lw=1.4, ms=5,
          label='dense p99.9 实测（dl800）')
cw = np.logspace(np.log10(64), np.log10(16384), 200)
ax.loglog(cw, P_seg(cw) * 1e6, color='tab:blue', lw=1.1, alpha=0.75,
          label='P_seg = %.1f us + %.2f ns/word（pd 拟合）' % (C_FIX_PD * 1e6, C_SCAN_PD * 1e9))
ax.loglog(cw, P_seg(cw, C_FIX_DENSE, C_SCAN_DENSE) * 1e6, color='tab:purple', lw=1.1, alpha=0.75,
          label='P_seg = %.1f us + %.2f ns/word（dense 拟合）' % (C_FIX_DENSE * 1e6, C_SCAN_DENSE * 1e9))
ax.axhline(P_MAX * 1e6, color='crimson', ls='--', lw=1.4, label='旧单值门禁 5 us（v1.3 口径）')
ax.axhline(P_WARM_P999_US, color='tab:green', ls='-.', lw=1.3, label='warm p99.9 门禁 10 us')
ax.axhline(P_SEG_GATE_US, color='tab:gray', ls=':', lw=1.3, label='seg 通道门禁 64 us')
ax.axvline(W_star(), color='crimson', ls=':', lw=1.1)
ax.annotate('快路径 W*(5us)=%.0f 字（pd）\nE=%.1f%%、T=%.0f word/us'
            % (W_star(), 100 * seg_eff(W_star()), T_words(W_star()) / 1e6),
            xy=(W_star(), P_MAX * 1e6), xytext=(70, 22), fontsize=6.6, color='crimson',
            arrowprops=dict(arrowstyle='->', color='crimson', lw=0.8))
ax.annotate('chunk=8192：34.08 us（dl0 87.28 us）\n=> 保持 8192，不靠降 chunk 换 5 us',
            xy=(8192, 34.08), xytext=(1500, 1.35), fontsize=6.6, color='tab:blue',
            arrowprops=dict(arrowstyle='->', color='tab:blue', lw=0.8))
ax.set_ylim(0.8, 160)
ax.set_xlabel('续扫段长度 chunk [word]（对数）')
ax.set_ylabel('续扫段步 p99.9 [us]（对数）')
ax.set_title('(i) seg 通道：小 chunk 被 c_fix 主导、大 chunk 被 c_scan 主导；快路径按字数计费')
ax.grid(True, which='both', alpha=0.25)
ax.legend(fontsize=5.8, loc='upper left', framealpha=0.92)

# ------------------- 面板 (j)：regionFree 通道（sync + 排空） -------------------
ax = fig.add_subplot(5, 2, 10)
ys = np.arange(len(RF_ORDER))
_sy = [RF[k][1] for k in RF_ORDER]
_qu = [RF[k][2] for k in RF_ORDER]
ax.barh(ys - 0.18, _sy, height=0.34, left=0.1, color='tab:red',
        label='同步段 sync（O(#slabs)，不可切）')
ax.barh(ys + 0.18, _qu, height=0.34, left=0.1, color='tab:blue', label='排空 queue（有预算）')
ax.set_xscale('log')
ax.set_xlim(0.1, 2e8)
for _i, _k in enumerate(RF_ORDER):
    _S, _s, _q, _v = RF[_k]
    ax.text(max(_s, _q) * 1.9 + 0.5, _i, 'S=%d  %.1f+%.1f=%.1f us  [%s]' % (_S, _s, _q, _s + _q, _v),
            va='center', fontsize=6.2)
ax.axvline(FREE_DEADLINE_US, color='tab:green', ls='--', lw=1.3, label='常态排空预算 100 us/步')
ax.axvline(FREE_HARD_DEADLINE_US, color='tab:orange', ls='-.', lw=1.3, label='强制 1000 us/步（队列>8 MiB）')
ax.axvline(1000.0, color='tab:red', ls=':', lw=1.2, label='1 ms 契约线（总耗）')
ax.set_yticks(ys)
ax.set_yticklabels(RF_ORDER, fontsize=7)
ax.set_ylim(2.62, -0.62)
ax.set_xlabel('regionFree 实测总耗时 [us]（对数；柱是整轮累计，不是单步上界）')
ax.set_title('(j) regionFree 通道：pd 已闭合、dense 单块 munmap 登记、pb 的 sync 项未闭合')
ax.grid(True, axis='x', which='both', alpha=0.25)
ax.legend(fontsize=5.8, loc='upper right')

fig.tight_layout(rect=[0, 0, 1, 0.972])
fig.savefig(PNG, dpi=150)
print('saved', PNG, os.path.getsize(PNG), 'B')

# =================== 7. 数值表（四列规范：数值 | 构建 | 档位 | 量具） ===================
rows = []
def row(q, v, tier, tool):
    rows.append((q, v, BUILD + ' / ' + BINMD5, tier, tool))

rows.append(('== A. 实测锚点（task-13 标定，tier2 默认档） ==', '', '', '', ''))
row('标定构建 vs FREEZE 构建', 'calprof 用 %s；FREEZE v4 构建 build-rf/extc md5 9fd0dcd861374668be3c74c342beac6f（3607 行源码，常量未随 v4 重测）'
    % BINMD5, 'T2', 'md5sum + make BUILDDIR=build-rf')
row('digraph 1M 轮数 / 分配字节', '85 / 135,901,368 B', 'T2 默认（RECLAIM=1, NOCOUNT=1, SHARDS=1）', 'calprof.py 轮表')
row('s_bar 平均对象字节', '42.50 B（250k 42.49）', 'T2 digraph', 'calprof：Sigma marked_bytes/Sigma marked_objs')
row('L / H_trig p50', '753,504 B / 2,130,240 B', 'T2 digraph 1M', 'calprof 轮表 p50')
row('L_peak / M_peak', '2,332,880 B / 6,439,536 B（2.76 L）', 'T2 digraph 1M', 'calprof 峰值')
row('sigma / sigma_prev', '0.345 / 0.354（max 0.412）', 'T2 digraph 1M', 'calprof 轮表')
row('r = H/L_prev p50 / min', '2.83 / 2.425', 'T2 digraph 1M', 'calprof 轮表')
row('F_slab = cap/H', '1.30（250k 1.26、p4 1.05）', 'T2 digraph', 'calprof cap_bytes/H')
row('tau_t / tau_s', '17.49 ns/obj / 0.458 ns/slot', 'T2 digraph 1M', 'calprof：Sigma mark_ns/Sigma marked_objs、Sigma sweep_ns/Sigma cap_slots')
row('c_t / c_s', '0.4114 ns/B / 0.0108 ns/B', 'T2 digraph 1M', 'calprof 同口径；区间 0.379-0.457 / 0.0105-0.0121')
row('lambda_GC', '0.2508 ns/B（GC 占墙钟 7.76%）', 'T2 digraph 1M', 'calprof：(mark+sweep)_ns/alloc_bytes')
row('a_all / a_mut', '0.309 / 0.335 GB/s', 'T2 digraph 1M', 'calprof：alloc/wall、alloc/(wall-GC)')
row('单步停顿 mark p99（三 rep）', '4.36 / 4.79 / 6.42 us', 'T2 digraph 1M', 'calprof per-step 数组（进程内计时）')
row('单步停顿 mark max（三 rep）', '170 / 52.5 / 490 us（不可用）', 'T2 digraph 1M', 'calprof；WSL2 抢占噪声')
row('a_peak（S1 纯 arena）', '>= 10.2 GB/s；p3 5-6 GB/s', 'T0 不发射运行期（p1/p2/p3 无 region）', '/usr/bin/time + VmHWM')
row('spectrum_p4 lambda / a_all / a_mut / s_bar', '0.0245 ns/B / 3.166 / 3.433 GB/s / 32.00 B', 'T2 spectrum_p4 8k', 'calprof')
row('峰值商 kappa_M = (peak-L)/H', '%.3f（1M）/ %.3f（250k）/ %.3f（p4）'
    % (KAPPA_M, KAPPA_M_250K, KAPPA_M_P4), 'T2 digraph + p4', 'calprof 峰值轮（CALIBRATION §3.6）')

rows.append(('== B. 模型常量（推导，非实测） ==', '', '', '', ''))
row('r_min = 1 + g*K/b', '%.3f（g=2.76, K=64, b=256）' % r_min(), 'T2', '模型：CALIBRATION §8')
row('sigma_max = b/(b+gK)', '%.3f（实测 sigma_prev max %.3f，裕度 %.2fx）'
    % (sigma_max(), SIGMA_PREV_MAX, sigma_max() / SIGMA_PREV_MAX), 'T2', '模型 + calprof')
row('P(b) = b*tau_t + 8*64*tau_s', 'b=256 -> %.2f us（门禁 %.1f us）' % (P_of(256) * 1e6, P_MAX * 1e6), 'T2', '模型')
row('b*(P_max)', '%.0f（被 MARK_BATCH_MAX=256 截断）' % b_opt(), 'T2', '模型')
row('M(r) = L*(1+r)', 'digraph 1M: 3L = 7.00 MB（实测 6.44，-8%）', 'T2', '模型 + calprof')
row('peak = L + kappa_M*H', 'kappa_M = 0.772（1M）/ 0.825（250k）/ 1.010（p4）', 'T2', 'calprof 峰值轮')
row('lambda(r) 误差', '-2.0% ~ 0.0%（6 数据集）', 'T2', 'calprof vs 模型')

rows.append(('== C. M 预算 -> 推荐参数（模型输出） ==', '', '', '', ''))
for M in (7 * MB_DEC, 9 * MB_DEC, 12 * MB_DEC, 21 * MB_DEC, 28 * MB_DEC):
    rc = r_pol(M, L_PEAK, 1.0)     # 保守（保证不超）
    rt = r_pol(M, L_PEAK)          # 典型 kappa_M=0.772
    re = r_eff(rt)                 # 典型全程 r_eff（用于 lambda）
    row('M=%.1f MB' % (M / MB_DEC),
        'r_peak: 保守<=%.2f / 典型<=%.2f -> r_eff=%.2f, sigma_eff=%.3f, lambda=%.4f ns/B, tax=%.1f%%, 可行=%s'
        % (rc, rt, re, 1.0 / re, lam(re), 100 * tax(lam(re), A_M_1M),
           'YES' if rc >= 0 and re >= r_min() else 'NO(r_eff<r_min)'),
        'T2 目标', '模型')
row('M < 最小可行', 'M_min(保守) = L_peak*(1+r_min) = %.2f MB' % (L_PEAK * (1 + r_min()) / MB_DEC), 'T2', '模型')

rows.append(('== D. 策略交叉：S2 清扫 vs S3 计数 ==', '', '', '', ''))
for cr in (C_R_LO, C_R, C_R_HI):
    rc = r_cross(cr)
    row('c_r=%.3f ns/B（%s）' % (cr, '待标定' if cr == C_R else '区间端'),
        'r_cross=1+c_t/c_r=%.2f => M=%.1f MB 时计数开始占优' % (rc, L_PEAK * (1 + rc) / MB_DEC),
        'T2 vs T1', '模型')
row('当前 r=2.83 的位置', 'r_cross(c_r=0.23)=%.2f => 计数与清扫打平' % r_cross(C_R), 'T2 vs T1', '模型')

rows.append(('== E. 与旧值（v1.0）的差异 ==', '', '', '', ''))
row('s_bar / sigma / r', '80 -> 42.50；0.7415 -> 0.345；1.349 -> 2.83', 'T2', 'calprof（旧值无实测支撑）')
row('c_t / c_s / tau_t', '0.18 -> 0.4114；0.045 -> 0.0108；14.4 -> 17.49', 'T2', 'calprof')
row('lambda_GC / a_m', '0.212 -> 0.2508 ns/B；0.515 -> 0.309 GB/s', 'T2', 'calprof')
row('kappa', '20 us -> 0（不需要该项）', 'T2', '模型拟合，误差 <=2%')
row('a_peak', '5.5 -> >=10.2 GB/s', 'T0', 'p1 纯 arena')


rows.append(('== F. memory-first 预算（M=3L 固定、r_policy=2；panel (e)(f) 的数据源） ==', '', '', '', ''))
MF_BUILD = 'build-th/extc md5 c371e5253a0aa152af968298dfcfb9d1（FREEZE v4 源码；与 build-rf 行为等价）'
for nm in MF_ORDER:
    d = mf_components(nm)
    rows.append((nm + ' 墙钟/GC 分解',
                 'wall=%.2fms; mark=%.2f (%.1f%%); sweep=%.2f (%.1f%%); nonGC=%.2f (%.1f%%); allocs=%.2fM'
                 % (d['wall'], d['mark'], 100*d['mark']/d['wall'], d['sweep'], 100*d['sweep']/d['wall'],
                    d['non_gc'], 100*d['non_gc']/d['wall'], d['allocs']/1e6),
                 'T2 默认 bench 参数', MF_BUILD + '；rdtsc 包裹 markStep/sweepStep 的副本量具'))
for nm in MF_ORDER:
    d = mf_components(nm)
    rc = mf_l1_reach(nm)
    ub = 100.0 * d['sweep'] / d['wall']
    p = MF_L1_PROBE[nm]
    if p['slabs'] == 0:
        l1 = 'L1 n/a（该形状 sweep≈0）'
    elif rc > 0.5:
        l1 = 'L1b 可达 -%.1f%% wall（%.1f%% 死槽落在 full-dead slab：%d/%d slab）' % (rc * ub, 100 * rc, p['full_dead'], p['slabs'])
    else:
        l1 = 'L1 不可达（full_dead=%d/%d slab，仅覆盖 %.2f%% 死槽）；-%.1f%% 只能当假设上界' % (p['full_dead'], p['slabs'], 100 * rc, ub)
    rows.append((nm + ' 杠杆可达性',
                 l1 + '；L2 mark-%.0f%%(phi 估算): -%.1f%%；L4 alloc-%.0f%%(上界): -%.1f%%'
                 % (100*MF_PHI_MARK, 100*MF_PHI_MARK*d['mark']/d['wall'], 100*MF_PHI_ALLOC, 100*MF_PHI_ALLOC*d['non_gc']/d['wall']),
                 'T2 默认 / 探针 T2', 'slab 面积探针（Lead，FREEZE v4 生成 C）+ rdtsc 拆分 + 模型'))
LAM_R2 = C_T + 2 * C_S * F_SLAB
TAX_R2 = 100.0 * LAM_R2 / (LAM_R2 + 1.0 / A_MUT_1M)
TAX_NOW = 100.0 * LAM_1M / (LAM_1M + 1.0 / A_MUT_1M)
WALL_R2 = (1.0 / A_MUT_1M + LAM_R2) / (1.0 / A_MUT_1M + LAM_1M)
rows.append(('M=3L 的 lambda 地板（r=2）',
             'lam=c_t+2*c_s*F_slab=%.4f ns/B => 税 %.1f%%（a_mut=%.2f）；现状 r_eff=2.83: lam=%.4f / 税 %.1f%%'
             % (LAM_R2, TAX_R2, A_MUT_1M, LAM_1M, TAX_NOW), 'T2 模型', '模型（c_t/c_s/F_slab 实测）'))
rows.append(('r_eff 2.83 -> 2 的交换',
             '省堆 %.0f%%（H: %.2fL -> 2L，M=L+kappa_M*H: %.2fL -> %.2fL）；付墙钟 %+.1f%%'
             % (100*(1-2.0/R_MEAS), R_MEAS, 1+KAPPA_M*R_MEAS, 1+KAPPA_M*2.0, 100*(WALL_R2-1)),
             'T2 模型', '模型 + KAPPA_M 实测'))
C_T_5 = 0.05 * (1.0 / A_MUT_1M) / 0.95 - 2*C_S*F_SLAB   # 5% 税所需 c_t（解 lam/(lam+1/a_mut)=0.05）
rows.append(('5% 税在 M=3L 下不可达',
             '需 c_t <= %.3f ns/B（实测 %.3f，-%d%%）；即使 sweep->0 也只能到 lam=c_t=%.3f => 税 %.1f%%'
             % (C_T_5, C_T, round(100*(1-C_T_5/C_T)), C_T, 100.0*C_T/(C_T+1.0/A_MUT_1M)),
             'T2 模型', '模型（不可达证明）'))
for nm in MF_ORDER:
    wall, wall_med, mc, sc, allc, rss, thr, mmax, amb, irt, irm, irs, step_us = MF[nm]
    rows.append((nm + ' 占用比',
                 'RSS=%.2fMB; M_max=%.2fMB (RSS/M_max=%.2fx); RSS/floor=%.2fx'
                 % (rss*1024.0/MB_DEC, mmax/MB_DEC, rss*1024.0/max(mmax,1e-9) if mmax >= FLOOR else float('nan'),
                    rss*1024.0/FLOOR),
                 'T2 默认', '/usr/bin/time -v 的 Maximum resident set size'))
rows.append(('大活集形状 M 已在下界带',
             'M_min=L*(1+r_min)=%.2fL；但实测 min r_eff=%.3f => 保守 M>=%.2fL 才能 keep-up；现行 M_max=3L 与峰值 %.2fL_peak（digraph 1M）'
             % (1+r_min(), R_MIN_MEAS, 1+R_MIN_MEAS, M_PEAK_1M/L_PEAK), 'T2 模型+实测', '模型 + calprof'))
rows.append(('小活集形状内存由 floor 决定',
             'sizeratio RSS/floor=3.16x、cycleratio 2.96x、promote 6.34x；M_max 甚至 < floor（墙不可达）',
             'T2 默认', '/usr/bin/time -v + [mmax] 日志'))

rows.append(('== F2. E18 floor 下探实测（Lead；FREEZE v4 生成 C 重编 -DEXTC_RC_FLOOR，--pin 6，3 reps） ==', '', '', '', ''))
for nm in ['sizeratio', 'cycleratio', 'promote']:
    b = MF_FLOOR[nm][1 << 20]
    br = 0.5 * (b[0] + b[1])
    bw = 0.5 * (b[2] + b[3])
    cells = []
    for f_, tag in ((1 << 20, '1MiB'), (1 << 18, '256KiB'), (1 << 16, '64KiB')):
        lo, hi, wlo, whi = MF_FLOOR[nm][f_]
        if wlo is None:
            cells.append('%s: %d kB（无变化）' % (tag, lo))
        else:
            cells.append('%s: %d-%d kB / %.1f-%.1f ms' % (tag, lo, hi, wlo, whi))
    rows.append((nm + ' floor 下探', '；'.join(cells), 'T2 默认（重编）', 'v2run --pin 6 / 3 reps（Lead）'))
rows.append(('E18 结论', 'floor 只影响「活集 ≪ floor」的小堆形状（promote 活集 1.8MB > floor => 无关）；256 KiB 是拐点（64 KiB 再省 0.2-0.4MB 但墙钟 +35~38%）=> 暴露为运行期旋钮、默认 1 MiB 不变', 'T2 默认', 'E18'))
rows.append(('G-MF4 判据修正', 'floor 下探后 RSS/floor 自动变大（256KiB 时 8x）=> 判据改成绝对 MB：sizeratio/cycleratio 峰值 RSS <=2.2 MB（256KiB 实测 2.09/2.08 MB）', '草案', 'panel (f) + E18'))

rows.append(('== G. memory-first 门禁草案（固定 M=3L，不得抬 r/M_max） ==', '', '', '', ''))
rows.append(('G-MF1 墙钟', '每形状 wall 降幅 >=10%（按**可达**杠杆判）：promote 只有 L2(15.4%) 可过；cycleratio L1b 6~9.3% + L2 1.0% 临界；sizeratio L2 1.8% 不可过；chase GC 侧上限 4.1% 豁免', '草案', 'panel (e)'))
rows.append(('G-MF2 lambda', 'r_eff=2.83 下 lambda <=0.35 ns/B（当前 %.4f 已过）；M=3L(r=2) 下 <=0.44 已过但税 %.1f%%' % (LAM_1M, TAX_R2), '草案', '模型'))
rows.append(('G-MF3 RSS', 'RSS <=1.35x M_max（promote 1.30 贴线；chase 1.09 通过；sizeratio/cycleratio 走 G-MF4）', '草案', 'panel (f)'))
rows.append(('G-MF4 小堆', '峰值 RSS <=2.2 MB（绝对 MB；floor 下探后 RSS/floor 会变大到 8x）：floor=1MiB 时 3.24/3.04 MB，floor=256KiB 时 2.09/2.08 MB（E18）', '草案', 'E18 + panel (f)'))
rows.append(('G-MF5 停顿', 'p99 <=5us、max <=50us、>1ms = 0（沿用 GC-ROOFLINE G1）', '草案', 'calprof per-step'))
rows.append(('G-MF6 sweep 占比', 'sweep/wall <=5%，等效判据=「full-dead slab 覆盖 >=90% 死槽」。current: promote 9.2%（L1 不可达 => 现有元数据下无法通过）、cycleratio 9.5%（L1b 可达）、sizeratio 6.8%、chase 0.0%', '草案', 'panel (e) + slab 面积探针'))


rows.append(('== H. quota 模型（P 字节配额 + B 惰性 freelist；task-25 / QUOTA-MODEL.md 的数据源，panel (g)(h)） ==', '', '', '', ''))
rows.append(('c_A = c_t + c_s*F_slab', '%.5f ns/B（每字节一次 trace + 一次 sweep 的单价）' % C_A, '模型（FREEZE v5 常量）', 'QUOTA-MODEL §2 T-Q1'))
rows.append(("c* = lambda(r'=2)", '%.5f ns/B（thr=max(2L,floor) 的合法下界处所需单价 => c* 覆盖整个合法策略空间）' % C_STAR, '模型', 'QUOTA-MODEL §2 T-Q1'))
rows.append(('Q_lin = 64*tau_s/2^k_s + b*tau_t/2^k_m', '%.4f ns/B => 过配 c* 的 %.2fx / c_A 的 %.2fx（原型 k_s=6, k_m=10）' % (Q_LIN, Q_LIN / C_STAR, Q_LIN / C_A), '模型', 'QUOTA-MODEL §2'))
rows.append(('keep-up 容许的最松位移', 'k_s <= %.2f、k_m <= %.2f（pops 通道更紧）；原型 6/10 => 余量 sweep %.0fx / pops %.2fx' % (KS_MAX, KM_MAX, 2 ** (KS_MAX - 6), 2 ** (KM_MAX - 10)), '模型', 'QUOTA-MODEL §2'))
rows.append(('内存界（真界化）', 'M_peak <= M_max + max(L,floor) + %.3f L + n_max；L=753,504 B、M_max=3L=%.2f MB 时界 = %.2f MB' % (BUF_L, 3 * L50 / MB_DEC, 3 * L50 / MB_DEC + (FLOOR + BUF_L * L50) / MB_DEC), '模型 + 源码契约', 'QUOTA-MODEL §3 T-M2'))
rows.append(('P_step 固定预算（b=256）', '%.2f us；5 us 门禁余量 %.3f us = %.1f 个 sweep word => 纯 sweep 配额可容 n <= %.0f B' % (P_of(256) * 1e6, (P_MAX - P_of(256)) * 1e6, (P_MAX - P_of(256)) / W_WORD, 64 * (W_SWEEP + (P_MAX - P_of(256)) / W_WORD)), '模型（tau_t/tau_s 实测）', 'QUOTA-MODEL §4'))
rows.append(('停顿∝n 的可接受性', '该次分配自身 memset 0.125 ns/B，而 pointer 扫描 11 ns/word = 1.375 ns/B => 常数比 %.0fx，不能无条件接受 => CAP 必须存在' % 11.0, '估算（memset 带宽未实测）', 'QUOTA-MODEL §4.2'))
rows.append(('SCAN_CHUNK 与 b 的联合约束【已作废，见 P 段】', 'chunk=2048 字(16KiB)=22.5 us、512 字=5.63 us 均不可行；256 字 => max b=125；128 字 => max b=205；64 字 => max b=246 => 5 us 门禁要求 chunk <= 256 字且 b <= 128~205', '模型 + [实测 11 ns/word]', 'panel (h)'))
rows.append(('P 原型实测：pd_bigobj 60 轮', 'base 745.9 ms / 493,400 kB -> P 113.5 ms / 26,072 kB（时间 -84% / 内存 -97.6%）；200 轮 1,272.9 ms / 1,067,288 kB -> 202.4 ms / 26,072 kB', 'FREEZE v5 生成 C（build-lead5）', 'P0-PROTOTYPE §4.3'))
rows.append(('P 对 v2 四形状中性', 'wall 44.62->40.25 / 22.91->22.22 / 12.76->12.80 / 169.62->171.01 ms；RSS 均不变（5 reps 交错 median）', 'FREEZE v5 生成 C', 'P0-PROTOTYPE §4.4'))
rows.append(('B：GC 步总时降幅', 'promote -12.5%（17,752->15,528 us）、sizeratio -51.7%（2,581->1,247）、cycleratio -8.4%（471.8->432）', 'FREEZE v5 生成 C', 'P0-PROTOTYPE §1.3'))
rows.append(('B：单步 max 降幅', '160.4->57.5 us（-64%）、51.8->9.5（-82%）、39.0->16.9（-57%）', 'FREEZE v5 生成 C', 'P0-PROTOTYPE §1.3'))
rows.append(('B：s_link 拟合', 'promote 0.541 / sizeratio 0.793 / cycleratio 0.103（= -dT_step/phi_sweep，phi 用 build-th rdtsc 拆分）', '模型拟合（phi 为 tier2 实测）', 'QUOTA-MODEL §5.2'))
rows.append(('B：max 步预测 vs 实测', '预测 0.459/0.207/0.897 vs 实测 0.358/0.183/0.433 => 误差 +28%/+13%/+107%（cycleratio 失效：98% 死槽在 full-dead slab，v5 已有批量重置）', '模型 vs FREEZE v5 原型', 'QUOTA-MODEL §5.3'))
rows.append(('B：lambda 重算（r_eff=2.83）', 'delta_lambda = -0.0118 / -0.0172 / -0.0022 ns/B => 净变化在 harness 抖动（+-5%）内；实测墙钟 +2.3% / +1.5% / -9.4%', '模型 + FREEZE v5 原型', 'QUOTA-MODEL §5.4'))
rows.append(('B 适用范围（洞密度门槛）', '洞密度 > c_w/(64*b_link) = %.1f%%~%.1f%%；1 MiB slab（24,672 槽）=> nfree >= %.0f~%.0f 槽（c_w≈5 ns/word、b_link 1.3~2.0 ns/slot）' % (100 * 5 / (64 * 2.0), 100 * 5 / (64 * 1.3), (1048576 / 42.5 / 64) * 5 / 2.0, (1048576 / 42.5 / 64) * 5 / 1.3), '估算（c_w/b_link 未标定）', 'QUOTA-MODEL §5.5'))
rows.append(('== I. quota 门禁草案 ==', '', '', '', ''))
rows.append(('G-Q1 内存', 'pd_bigobj 200 轮峰值 RSS <= 3L + 8 MiB（原型 26,072 kB 已过；base 1,067,288 kB 不过）', '草案', 'P0-PROTOTYPE §4.3'))
rows.append(('G-Q2 停顿【已作废，改为通道化，见 J 段】', '配额开启后 p99 <= 5 us 且 max <= 50 us => 必须同时把 SCAN_CHUNK 降到 <= 256 字、b <= 128~205', '草案', 'panel (h)'))
rows.append(('G-Q3 中性', '四 v2 形状墙钟 +-3%、RSS 不变（原型已过：-9.8% / -3.0% / +0.3% / +0.8%）', '草案', 'P0-PROTOTYPE §4.4'))
rows.append(('G-Q4 B 正确性', 'G1/G2 的 max 与 p99.9 改善 >= 30%；计数档 218 断言 + ASan/UBSan 全绿 + 新断言「同一槽只入链一次」', '草案', 'P0-PROTOTYPE §1.4/§5'))

rows.append(('== P. P1 修正（v7 冻结口径；锚点 build-p1/extc b9fd5784，A 段锚点仍为 build-th） ==', '', '', '', ''))
rows.append(('P1：SCAN_CHUNK / 段步（pd p99.9）',
             '128/256/512/8192 字 = 1.65/1.84/8.19/34.08 us（dl800）；dl0 = 0.65/0.89/1.68/87.28 us => 旧断言「降到 128 是 5 us 前提」作废',
             'T2 v7', 'P1-RQ3-CHUNK §2 [segsteps]'))
rows.append(('P1：dense 段步（dl0 p99.9）',
             '128/256/8192 = 1.34/3.54/59.71 us => 冷密形状按 1 ms 口径判，不参与 5 us 门禁',
             'T2 v7', 'P1-RQ3 §2 表 A'))
rows.append(('P1：seg 通道拟合',
             'pd c_fix=1.14 us + 4.02 ns/word；dense 0.52 us + 6.82 ns/word（chunk=512 离群 +157% => 用 2x 保守界）',
             '模型', '本文件 §6.7；panel (i)'))
rows.append(('P1：段效率 E(chunk)',
             'pd 31.2/47.6/64.5/96.7%（128/256/512/8192）；dense 62.8/77.2/99.1% => chunk 越小固定开销占比越高',
             '模型', 'panel (i)'))
rows.append(('P1：markResume 快路径',
             '一步内连续续扫多段、按字数计费 => W*(5us)=961 字（pd）/657（dense），远大于 chunk=128；a 上限 1.54/1.05 GB/s（sigma=1）',
             '模型', 'MODEL-UPDATE-P1 §3.3'))
rows.append(('P1：chunk 的内存代价',
             'pd_bigobj 200 峰值 26.6 MB(8192) -> 215 MB(128) = 8.1x，而 deadline 窗口可扫字数比仅 3.1x => 反馈放大，3.1x 是下界',
             'T2 v7', 'P1-RQ3 表 B、§7'))
rows.append(('P1：配额记账不是成本',
             'sizeratio 2M 分配仅 174 次 markAdvance、deadline checks=0；-D 变体 Ir 差 <0.1% => 旧「sizeratio 回归 = 配额记账」作废',
             'T2 v7', 'P1-QUOTA-FASTPATH §0'))
rows.append(('P1：sizeratio Ir 回归真因',
             'B 惰性 freelist 复用链重建 2,020 次 / 1.89M 洞 ~ 19.5M Ir；B 默认关（EXTC_RC_LAZY_FREE=0）',
             'T2 v7', 'P1-QUOTA-FASTPATH §0'))
rows.append(('P1：elide 改 O(1) 卷计数器',
             'cycleratio Ir 273.19M -> ~264.3M（净省 8.73M，约 -3.1%），墙钟 15.51 -> 15.38 ms（-0.84%），切片 1221 -> 255（-79%）',
             'T2 v7', 'P1-QUOTA-FASTPATH §8'))
rows.append(('P1：regionFree 通道',
             'pd_bigobj200 18.6-22.7 ms -> 663 us（sync 0.5 + queue 662.5）；dense 1,708 us（单块 8 MiB munmap 不可切）；pb 100M 14,762 us（sync 13,755.6）=> RED',
             'T2 v7', 'P1-REGIONFREE-BATCH；panel (j)'))
rows.append(('P1：1 ms 契约的 slab 上限',
             'S <= 1782 slab（561 ns/slab）；超出需 region 级 closing 队列（O(1) 挂链 + closing 标志拒绝 owns）',
             '模型', 'MODEL-UPDATE-P1 §3.4'))
rows.append(('P1：吞吐代价（相对 HEAD）',
             'promote 1.054 / sizeratio 1.050 / chase 1.004 / cycleratio 1.007；callgrind promote 115.21M -> 129.13M Ir（+12.1%）= v4 内存墙 8.2% + quota 2.5% + 卷计数器 0.95%',
             'T2 v7', 'GC-ROOFLINE §6.7.5'))
rows.append(('P1：P 通道判据',
             'P_step = max(P_adv, P_seg, P_sweepQ, P_rf, P_rel)；单值 5 us 口径作废，按通道给界（见 J 段）',
             '模型', 'MODEL-UPDATE-P1 §3.1'))
rows.append(('== J. G-P1 门禁（P1 落地后的判据草案） ==', '', '', '', ''))
rows.append(('G-P1-1 seg 通道',
             'p99.9 <= min(1.5*c_seg*chunk+1us, 64us)；chunk=8192 时 1.5*4.02ns*8192+1 = 50.4 us；over1ms=0；n<1e5 判 N/A',
             '门禁', 'GC-ROOFLINE §6.7.4'))
rows.append(('G-P1-2 warm 尾部',
             '>50 us 占比 <= 1e-4 且 p99.9 <= 10 us（按 [segsteps]/[chan] 分通道；max 只作 OBS）',
             '门禁', 'GC-ROOFLINE §6.7.4'))
rows.append(('G-P1-3 cold-dense',
             'tau_t <= 2 us、p99.9 <= 1 ms、over1ms=0（不参与 5 us 门禁）',
             '门禁', 'GC-ROOFLINE §6.7.4'))
rows.append(('G-P1-4 regionFree',
             '常态 <= 100 us/步、强制 <= 1000 us/步；sync 段只承诺 S <= 1782 slab，更大 region 登记契约外',
             '门禁', 'MODEL-UPDATE-P1 §3.4'))
rows.append(('G-P1-5 配额记账',
             'B=0 时相对 Ir 回归 <= 0.5%（实测 <0.1%）',
             '门禁', 'P1-QUOTA-FASTPATH §0'))
rows.append(('G-P1-6 elide',
             '切片数 -70% 以上且 p99.9 不升；EXTC_RC_ELIDE_VERIFY=1 时 elideMismatch=0',
             '门禁', 'P1-QUOTA-FASTPATH §0、§4'))


with open(MD, 'w', encoding='utf-8') as f:
    f.write('# GC Roofline 模型数值表 v1.4（由 roofline.py 自动生成，勿手改）\n\n')
    f.write('口径：A 段实测锚点 = build-th/extc 72c05dff（FREEZE v4 源码）；P/J 段 = build-p1/extc b9fd57843f2c734c16756f6850ace9c5（FREEZE v7：reclaim.c e17080d1、codegen.c d3393828）。两套构建不可混算。\n\n')
    f.write('约定：每个性能数字读作「数值 | 构建(HEAD + extc md5) | 档位 | 量具」。\n')
    f.write('档位：T0=不发射运行期（arena-only 形状）；T2=清扫档（默认 NOCOUNT=1, SWEEP_FREE=0, SHARDS=1）；T1=事件档（NOCOUNT=0, SWEEP_FREE=1；分层实测见 docs/analysis/MEASUREMENTS-TIER1.md：16B 慢 2.50x、456B 打平、32KB 快 3x）。\n')
    f.write('标注「模型」的行是推导值；c_r 仍是待标定项（CALIBRATION §11#1）。\n\n')
    f.write('| 量 | 数值 | 构建(HEAD+md5) | 档位(0/1/2+开关) | 量具 |\n')
    f.write('|---|---|---|---|---|\n')
    for rr_ in rows:
        cells = list(rr_) + [''] * (5 - len(rr_))
        f.write('| %s | %s | %s | %s | %s |\n' % tuple(cells[:5]))

# =================== 8. 自检：模型 vs 实测（6 数据集） ===================
MEASURED = [('dg250-post', 4.13, 0.3790, 0.0116, 1.26, 0.1433),
            ('dg1M-post', 2.83, 0.4114, 0.0108, 1.30, 0.2508),
            ('dg1M-rep1', 2.83, 0.4142, 0.0105, 1.30, 0.2519),
            ('dg1M-rep2', 2.83, 0.4570, 0.0121, 1.30, 0.2788),
            ('p4-2k', 105.43, 0.2128, 0.0210, 1.05, 0.0245),
            ('p4-8k', 86.89, 0.2367, 0.0206, 1.04, 0.0245)]
print('== 成本函数自检：lambda(r) 模型 vs calprof 实测 ==')
worst = 0.0
for tag, r_, ct, cs, fs, lm in MEASURED:
    pred = lam(r_, c_t=ct, c_s=cs, f_slab=fs)
    err = 100 * (pred / lm - 1)
    worst = max(worst, abs(err))
    print('  %-10s r=%6.2f  model=%.4f  meas=%.4f  err=%+.1f%%' % (tag, r_, pred, lm, err))
print('  最差 |err| = %.1f%%  =>  判据 <=1.5x 通过' % worst)

print('== 两面墙的裕度（digraph 1M） ==')
print('  trace-rate: min r\'=%.3f vs r_min=%.3f => 裕度 %.2fx' % (R_MIN_MEAS, r_min(), R_MIN_MEAS / r_min()))
print('  sigma 形式: max sigma_prev=%.3f vs sigma_max=%.3f => 裕度 %.2fx'
      % (SIGMA_PREV_MAX, sigma_max(), sigma_max() / SIGMA_PREV_MAX))
print('  内存: M_peak=%.2f MB vs 3*L_peak=%.2f MB（差 %+.1f%%）'
      % (M_PEAK_1M / 1e6, 3 * L_PEAK / 1e6, 100 * (M_PEAK_1M / (3 * L_PEAK) - 1)))
print('  峰值商: kappa_M = %.3f（1M）/ %.3f（250k）/ %.3f（p4）'
      % (KAPPA_M, KAPPA_M_250K, KAPPA_M_P4))
print('  停顿: P(256)=%.2f us vs P_max=%.1f us => 裕度 %.2fx（p99 实测中位 %.2f us）'
      % (P_of(256) * 1e6, P_MAX * 1e6, P_MAX / P_of(256), 4.8))

print('== 各形状在 (lambda, a) 平面 ==')
for nm, l_, a_, col in PTS:
    print('  %-12s lam=%.4f ns/B  a=%.2f GB/s  tax=%.1f%%  GC 屋顶(rho=30%%)=%.2f GB/s'
          % (nm, l_, a_, 100 * tax(l_, a_), a_roof(l_)))
print('  ridge 点: lambda*=%.3f ns/B（rho=30%%）' % RIDGE)

print('== 产出 ==')
print('  %s (%d B)' % (PNG, os.path.getsize(PNG)))
print('  %s (%d B, %d 行)' % (MD, os.path.getsize(MD), len(rows) + 6))

print('== memory-first 自检（M=3L 固定） ==')
for nm in MF_ORDER:
    d = mf_components(nm)
    print('  %-11s sweep/wall=%.1f%%  mark/wall=%.1f%%  nonGC/wall=%.1f%%  RSS/M_max=%s  RSS/floor=%.2fx'
          % (nm, 100*d['sweep']/d['wall'], 100*d['mark']/d['wall'], 100*d['non_gc']/d['wall'],
             ('%.2fx' % (d['rss_kb']*1024.0/d['mmax'])) if d['mmax'] >= FLOOR else 'M_max<floor（不可达）',
             d['rss_kb']*1024.0/FLOOR))
print('  L1 可达性（Lead slab 面积探针，align 口径：promote 200k）：')
for nm in MF_ORDER:
    p = MF_L1_PROBE[nm]
    rc = mf_l1_reach(nm)
    d = mf_components(nm)
    if p['slabs'] == 0:
        print('  %-11s sweep≈0 => L1 n/a' % nm)
    else:
        print('  %-11s full_dead=%d/%d slab（覆盖 %.1f%% 死槽）=> L1 可达 %.1f%% / 上界 %.1f%% wall'
              % (nm, p['full_dead'], p['slabs'], 100*rc, rc*100*d['sweep']/d['wall'], 100*d['sweep']/d['wall']))
print('  M=3L(r=2) 的 lambda 地板 = %.4f ns/B（税 %.1f%%）；现状 r_eff=2.83 lam=%.4f（税 %.1f%%）' % (LAM_R2, TAX_R2, LAM_1M, TAX_NOW))
print('  r_eff 2.83 -> 2：省堆 %.0f%%，付墙钟 %+.1f%%' % (100*(1-2.0/R_MEAS), 100*(WALL_R2-1)))
print('  5%% 税需 c_t <= %.3f ns/B（实测 %.3f，-%d%%）；sweep->0 也只到税 %.1f%% => 不可达'
      % (C_T_5, C_T, round(100*(1-C_T_5/C_T)), 100.0*C_T/(C_T+1.0/A_MUT_1M)))
print('  E18 floor 下探（Lead，重编 -DEXTC_RC_FLOOR）：sizeratio 256KiB RSS -35%/墙钟 +15%；cycleratio -32%/+4%；promote 无变化 => 256 KiB 是拐点')
print('  判据：模型/实测 <=1.5x 通过；phi_mark=%.2f、phi_alloc=%.2f 是估算/上界，待 E17 标定' % (MF_PHI_MARK, MF_PHI_ALLOC))
print('== quota 自检（P 字节配额 + B；FREEZE v5 常量 c_t=0.4114 c_s=0.0108 F=1.30 tau_t=17.49ns tau_s=0.458ns b=256） ==')
print('  c_A=c_t+c_s*F=%.5f ns/B；c*=lambda(r=2)=%.5f ns/B；Q_lin=%.4f ns/B => 过配 %.2fx（c*）/ %.2fx（c_A）'
      % (C_A, C_STAR, Q_LIN, Q_LIN / C_STAR, Q_LIN / C_A))
print('  keep-up 容许的最松位移：k_s<=%.2f、k_m<=%.2f（pops 通道更紧）；原型 6/10 => 余量 sweep %.0fx / pops %.2fx'
      % (KS_MAX, KM_MAX, 2 ** (KS_MAX - K_SWEEP_QUOTA), 2 ** (KM_MAX - K_MARK_QUOTA)))
print('  内存界：M_peak <= M_max + max(L,floor) + %.3f L + n_max；L=753,504 B、M_max=3L (%.2f MB) => 界 %.2f MB'
      % (BUF_L, 3 * L50 / MB_DEC, 3 * L50 / MB_DEC + (FLOOR + BUF_L * L50) / MB_DEC))
print('  P(b=256)=%.2f us，5 us 余量 %.3f us = %.1f word => 纯 sweep 配额可容 n <= %.0f B；chunk=8192 字 = %.1f us 必降'
      % (P_of(256) * 1e6, (P_MAX - P_of(256)) * 1e6, (P_MAX - P_of(256)) / W_WORD,
         64 * (W_SWEEP + (P_MAX - P_of(256)) / W_WORD), SCAN_CHUNK_W * SCAN_NS_W * 1e6))
print('  chunk 预算（b*tau_t + chunk*11ns + 8*64*tau_s <= 5 us）：256 字 => b<=125；128 字 => b<=205；64 字 => b<=246')
print('  B：GC 步总时降幅 -12.5% / -51.7% / -8.4%；单步 max 160.4->57.5 / 51.8->9.5 / 39.0->16.9 us；s_link=0.541/0.793/0.103')
print('  B lambda 重算（r_eff=2.83）：-0.0118/-0.0172/-0.0022 ns/B => 在 +-5% harness 抖动内（实测墙钟 +2.3/+1.5/-9.4%）')
print('  判据：模型/实测 <=1.5x 通过；11 ns/word 是 9.5-12 的上沿，CAP 选择准则见 QUOTA-MODEL §4.3')
if True:
    print('== P1 自检（v7 冻结口径；锚点 build-p1 b9fd5784）==')
    print('  seg 拟合：pd c_fix=%.2f us / c_scan=%.2f ns/word；dense %.2f us / %.2f ns/word（倍数 %.1f）'
          % (C_FIX_PD * 1e6, C_SCAN_PD * 1e9, C_FIX_DENSE * 1e6, C_SCAN_DENSE * 1e9, SEG_SAFE))
    for _c in SEG_CHUNKS:
        _m = float(P_seg(_c)) * 1e6
        _dm = SEG_DENSE_P999.get(_c)
        print('  chunk=%5d word: pd 实测 %.2f us / 模型 %.2f us / 保守 %.2f us；dense %s'
              % (_c, SEG_PD_P999[_c], _m, _m * SEG_SAFE, ('%.2f us' % _dm) if _dm else '   -'))
    print('  段效率 E(chunk)：pd %.1f/%.1f/%.1f/%.1f%%；dense %.1f/%.1f/%.1f%%（128/256/512/8192 或省略 512）'
          % (100 * seg_eff(128), 100 * seg_eff(256), 100 * seg_eff(512), 100 * seg_eff(8192),
             100 * seg_eff(128, C_FIX_DENSE, C_SCAN_DENSE), 100 * seg_eff(256, C_FIX_DENSE, C_SCAN_DENSE),
             100 * seg_eff(8192, C_FIX_DENSE, C_SCAN_DENSE)))
    _wp = W_star()
    _wd = W_star(c_fix=C_FIX_DENSE, c_scan=C_SCAN_DENSE)
    print('  快路径：W*(5us)=%.0f 字（pd，E=%.1f%%）/ %.0f 字（dense，E=%.1f%%）'
          % (_wp, 100 * seg_eff(_wp), _wd, 100 * seg_eff(_wd, C_FIX_DENSE, C_SCAN_DENSE)))
    print('  快路径吞吐 T：%.0f word/us（pd）/ %.0f（dense）；keep-up a <= %.2f GB/s（sigma=1）/ %.2f（sigma=0.35）'
          % (T_words(_wp) / 1e6, T_words(_wd, C_FIX_DENSE, C_SCAN_DENSE) / 1e6,
             a_max_gbs(_wp), a_max_gbs(_wp, 0.35)))
    _wc = [int(Q_DEADLINE_US * 1e-6 / float(P_seg(c)) * c) for c in SEG_CHUNKS]
    _win = '  '.join('%d->%d' % (c, w) for c, w in zip(SEG_CHUNKS, _wc))
    print('  deadline=%d us 窗口可扫字数：%s（128 vs 8192 = %.1fx，实测内存比 8.1x 是反馈放大 => 3.1x 读作下界）'
          % (Q_DEADLINE_US, _win, _wc[-1] / float(_wc[0])))
    for _k in RF_ORDER:
        _S, _s, _q, _v = RF[_k]
        print('  regionFree %-13s S=%5d sync=%8.1f queue=%7.1f => %8.1f us / 单步上界 %.1f us [%s]'
              % (_k, _S, _s, _q, _s + _q, P_rf(_S), _v))
    print('  1 ms 契约的 slab 上限（561 ns/slab）= %d slab；P_step = max(通道界) 而非单值 5 us'
          % int(1000.0 / RF_C_SYNC_US))
    print('  elide：Ir %.2fM -> %.2fM（净省 %.2fM，约 -%.1f%%），墙钟 %.2f -> %.2f ms（-%.2f%%），切片 %d -> %d'
          % (ELIDE['ir_v0_M'], ELIDE['ir_v0_M'] - ELIDE['ir_net_M'], ELIDE['ir_net_M'],
             100 * ELIDE['ir_net_M'] / ELIDE['ir_v0_M'], ELIDE['wall_v0_ms'], ELIDE['wall_v1_ms'],
             100 * (ELIDE['wall_v0_ms'] - ELIDE['wall_v1_ms']) / ELIDE['wall_v0_ms'],
             ELIDE['slices_v0'], ELIDE['slices_v1']))
    print('  吞吐代价（相对 HEAD）：' + ' / '.join('%s %.3f' % (k, v) for k, v in sorted(P1_WALL_RATIO.items())))

print('OK')