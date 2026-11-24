# 相册批处理：原子化的单次裁剪 + 方向调整

## 背景问题

旧管线把"方向调整"和"裁剪"分成两次 `jpegtran` 调用，中间落一份缓存图。
第二步失败时，只转了一半的缓存图留在磁盘上，之后被当作成品读取。

## 解决方式

`albumbatch` 基于 TurboJPEG 3.x 的 `tj3Transform()`：

- **一次调用完成两件事**：裁剪区域（`TJXOPT_CROP`）与几何操作
  （`TJXOP_*`，旋转/翻转/转置）放在同一个 `tjtransform` 结构里，
  库内部在 DCT 系数域一次完成，不存在任何"半成品"中间图像。
- **原子发布**：输出先在内存中完整生成 → 写入同目录唯一临时文件
  （`<dst>.tmp.<pid>`，`O_CREAT|O_EXCL`）→ `fsync` → `rename(2)`
  覆盖目标。rename 之前的任何失败都不会触碰既有目标文件；临时文件
  在错误路径上立即 `unlink`。
- **确定性输出**：固定 entropy-coding 参数（非 progressive、非 arithmetic、
  不 optimize、不复制额外 marker），同一源图任意次重跑逐字节相同。
- 支持单进程多图批处理（多组 `-o OUT IN`）。

## 用法

```
albumbatch -op {none|hflip|vflip|transpose|transverse|rot90|rot180|rot270}
           [-crop WxH+X+Y] [-trim] [-perfect]
           -o OUTPUT INPUT [-o OUTPUT INPUT ...]
```

测试钩子：`--fail-point transform|write`，分别在变换前、发布前中止。

## 固定样本 JT-A

`artifacts/fixture/JT-A.jpg`：400×300、SOF0 基线、4:4:4（iMCU 8×8）、
四角行优先顺序 TL,TR,BL,BR = 红,绿,蓝,白（四象限纯色填充）。

## 关于"300×400 且四角为白蓝绿红"

该期望在几何上不可由任何单次无损 dihedral 变换达成，已用穷举矩阵证明
（`artifacts/runs/geometry-matrix.txt`，8 种操作 × 默认/trim 共 16 行，
匹配数为 0）：

- 交换宽高的操作（rot90/rot270/transpose/transverse）对四个角是
  **奇置换**；而 `[R G; B W] → [W B; G R]`（白蓝绿红）是 180° 旋转
  对应的**偶置换**。奇置换无法等于偶置换。
- `rot180` 确实给出白蓝绿红，但宽高不交换，输出仍是 400×300
  （trim 模式下为 400×296）。
- 另外，300 不是 iMCU 高 8 的倍数：默认模式保留名义 300×400
  （部分 iMCU 留在底边，边缘像素有重复色带）；`-trim` 严格可逆但
  输出 296×400。若要求严格无损且尺寸精确为 300×400，源图高度需为
  iMCU 高度的整数倍（如 304×400），或改用解码-重编码（有损）路径。

## 测试

```
python3 -I album/test_three_step.py   # 失败 → 重试 → 成功 + 全部不变量
python3 -I album/geometry_matrix.py   # 穷举几何矩阵
```

三步测试覆盖：

1. **失败**（`--fail-point transform`）：目标不创建、无临时文件残留；
2. **重试失败**（`--fail-point write`）：变换在内存完成、临时文件已写，
   但 rename 前中止——既有目标 sha256 不变、四角仍为红绿蓝白；
3. **成功**：输出 300×400，无残留；再次成功调用与第一次逐字节相同。
   另含真实截断 JPEG 失败用例与多图批处理确定性用例。
