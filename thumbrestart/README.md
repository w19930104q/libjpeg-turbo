# thumbrestart：复用编码器时的 restart 标记回归测试（JT-B）

## 背景问题

缩略图服务为了摊薄初始化成本，会用**同一个压缩对象**连续编码多张图。
历史上此类服务反复踩中同一类状态泄漏：第一幅图设置了 restart interval
（DRI + RSTn 标记），把 interval 改回 0 再编码第二幅时，上一幅的 restart
状态没有被清干净，输出里仍然带 DRI/RSTn 标记。

## 固定样本 JT-B

三幅 257×129 RGB 图，用同一个压缩对象按 **8、0、0** 的 restart interval
顺序连续编码（第 2、3 幅像素完全相同）：

| 序号 | restart interval | 期望输出 |
| --- | --- | --- |
| 1 | 8 MCU | 恰好 1 个 DRI 标记且值为 8，含 RSTn 标记 |
| 2 | 0 | 无 DRI、无 RSTn |
| 3 | 0 | 无 DRI、无 RSTn，且与第 2 份**逐字节相同** |

三份文件都必须仍能被解码器正常读取（旧解码器兼容）。

## 结论

对当前代码库（HEAD = 16317632）的系统排查表明，压缩侧的 restart 状态在
两条 API 路径上都已正确隔离，**无需修改库源码**：

- **TurboJPEG 路径**：`tj3Compress8()` 每次都经 `setCompDefaults()` →
  `jpeg_set_defaults()` 把 `restart_interval`/`restart_in_rows` 清零，再从
  实例参数（`turbojpeg.c` 的 `setCompDefaults()`）恢复；编码结束后
  `jpeg_abort_compress()` 释放 JPOOL_IMAGE，熵编码器的
  `restarts_to_go`/`next_restart_num` 在每个 pass 的 `start_pass`
  （`jchuff.c`、`jcphuff.c`、`jcarith.c`、`jclhuff.c`）重新初始化。
- **标记写入路径**：marker writer 每次压缩都从 JPOOL_IMAGE 重新分配，
  `jinit_marker_writer()` 与 SOI 处理（`jcmarker.c` 的
  `write_file_header()`）都会把 `last_restart_interval` 复位为 0，因此
  DRI 的“仅在值变化时写出”逻辑不会把上一幅的 DRI 带到下一幅。
- **原始 libjpeg C API 路径**：`jpeg_abort_compress()` 后复用同一
  `jpeg_compress_struct` 的场景同样验证通过。

本目录交付的是把上述约定固化下来的**回归工具**，防止以后的改动重新引入
状态泄漏。已用注入缺陷的库验证过该工具确实能抓到问题（见下）。

## 文件

- `thumbrestart.c` — 回归程序。一个压缩对象编码三幅 257×129 RGB 图，
  interval 依次为 8、0、0；自行解析 JPEG 字节流（正确处理熵数据中的
  `0xFF 0x00` 填充）统计 DRI/RSTn，校验第 2、3 份逐字节相同，并用
  TurboJPEG 解码器回读三份文件确认旧解码器可读。
  - `-m tj`（默认）：走 TurboJPEG 3 API（`tj3Init` + `tj3Compress8`）。
  - `-m capi`：走原始 libjpeg C API，`jpeg_abort_compress()` 复用同一对象。
- `build.sh` — 针对本地静态库（`build-local/libturbojpeg.a`、
  `libjpeg.a`）编译，工具链默认 `/home/liiing/toolchain/gcc.sh`。
- `test_jtb.py` — 用 `python3 -I` 运行；两种模式各跑一遍并断言全部
  JT-B 不变量，任一不满足即非零退出。
- `make_sample.py` — 确定性生成三幅 257×129 RGB PPM 样本（第 2、3 幅
  像素一致），仅用标准库。

## 用法

```sh
./thumbrestart/build.sh                 # 编译（需先在 build-local/ 建好库）
python3 -I thumbrestart/test_jtb.py     # 完整回归，两种 API 模式
./thumbrestart/thumbrestart             # 直接运行（默认 TurboJPEG 模式）
```

当前库的实测输出：

```
JT-B: one compressor reused for three 257x129 RGB images, restart intervals 8, 0, 0
  [TurboJPEG] run 1 (interval=8): 16475 bytes, DRI=1(value=8), RSTn=70 ... OK
  [TurboJPEG] run 2 (interval=0): 15855 bytes, DRI=0(value=0), RSTn=0 ... OK
  [TurboJPEG] run 3 (interval=0): 15855 bytes, DRI=0(value=0), RSTn=0 ... OK
PASS: all JT-B restart-marker invariants hold
```

## 缺陷捕获能力验证（变异测试）

为确认测试不是“永远通过”，在隔离的源码副本上注入过两类缺陷并重新建库：

1. 让 `jpeg_set_defaults()` 不再清零 restart 字段（等价于服务层漏清
   interval）→ TurboJPEG 模式下第 2、3 幅输出变成
   `DRI=1(value=8), RSTn=70 ... FAIL`，`test_jtb.py` 非零退出。
2. 让 marker writer 跨压缩复用同一子对象（`last_restart_interval`
   残留）→ C API 模式下第 2 幅多出残留 DRI、第 2/3 份字节不再相同，
   同样被抓出。

恢复正常库后两种模式全部通过。
