# Ascend FFTS SDMA Probe

这个小工具只验证 Ascend FFTS Plus 的 SDMA descriptor 能否跑几种地址组合：

- `d2d-sdma`: device source 到 device destination。
- `h2d-sdma`: host source 到 device destination。
- `h2h-sdma`: host source 到 host destination。

普通 ACL memcpy 只用于准备输入和读回校验结果，不作为被测路径，也不计入 FFTS SDMA 耗时。

## 构建

```bash
cmake -S . -B build -DASCEND_ROOT=/usr/local/Ascend/ascend-toolkit/latest
cmake --build build -j
```

如果 Ascend toolkit 不在默认路径，把 `ASCEND_ROOT` 指到 `ascend-toolkit/latest`。

## 运行

先跑最小单 descriptor：

```bash
./build/ffts_sdma_probe --device 0 --mode all --bytes 1048576 --frags 1 --lanes 1 --warmup 1 --repeat 10
```

再跑多 descriptor 和多 ready lane。这里是 8 个 1 MiB IO：

```bash
./build/ffts_sdma_probe --device 0 --mode d2d-sdma --bytes 1048576 --frags 8 --lanes 8 --warmup 1 --repeat 10
./build/ffts_sdma_probe --device 0 --mode h2d-sdma --bytes 1048576 --frags 8 --lanes 8 --warmup 1 --repeat 10
./build/ffts_sdma_probe --device 0 --mode h2h-sdma --bytes 1048576 --frags 8 --lanes 8 --warmup 1 --repeat 10
```

如果直接传 host 地址失败，可以继续比较注册后的 host 地址和 mapped device-visible 地址：

```bash
./build/ffts_sdma_probe --device 0 --mode h2d-sdma --bytes 1048576 --frags 1 --lanes 1 --host-mem registered
./build/ffts_sdma_probe --device 0 --mode h2d-sdma --bytes 1048576 --frags 1 --lanes 1 --host-mem registered-mapped
./build/ffts_sdma_probe --device 0 --mode h2d-sdma --bytes 1048576 --frags 1 --lanes 1 --host-mem aclrt-registered-mapped
```

## 参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--device` | `0` | Ascend device id。 |
| `--mode` | `all` | `d2d-sdma`、`h2d-sdma`、`h2h-sdma` 或 `all`。 |
| `--bytes` | `1048576` | 每个 SDMA IO 的字节数，支持 `K`、`M`、`G` 后缀。 |
| `--frags` | `1` | 独立 SDMA IO descriptor 个数。`--bytes 1M --frags 8` 表示一次提交 8 个 1 MiB IO。 |
| `--lanes` | `1` | FFTS ready context 数上限。 |
| `--warmup` | `1` | 不计时 warmup 次数。 |
| `--repeat` | `10` | 计时次数。 |
| `--host-mem` | `aclrt` | `h2d-sdma` 被测 source buffer 的 host 内存类型，支持 `aclrt`、`malloc`、`registered`、`registered-mapped`、`aclrt-registered` 和 `aclrt-registered-mapped`。 |

## 结果判断

- `d2d-sdma` 成功，说明 FFTS SDMA descriptor、launch 和 stream 同步基础路径可用。
- `h2h-sdma` 使用 `aclrtMallocHost` 申请 source 和 destination，不注册 host memory，也不获取 mapped pointer。
- `h2d-sdma` launch 失败，通常说明当前 runtime 不接受 host source 地址进入 FFTS SDMA descriptor。
- `--host-mem registered` 会用普通 host buffer，再调用 `aclrtHostRegisterV2(..., ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED)`，FFTS descriptor 仍使用原始 host 地址。
- `--host-mem registered-mapped` 会使用 `aclrtHostGetDevicePointer` 返回的 mapped 地址作为 FFTS descriptor 的 source。
- `--host-mem aclrt-registered` 会用 `aclrtMallocHost` 申请锁页 host buffer，再调用 `aclrtHostRegisterV2(..., ACL_HOST_REG_MAPPED)`，FFTS descriptor 仍使用原始 host 地址。
- `--host-mem aclrt-registered-mapped` 会使用 `aclrtMallocHost` + `aclrtHostRegisterV2(..., ACL_HOST_REG_MAPPED)` + `aclrtHostGetDevicePointer`，FFTS descriptor 使用 mapped 地址。

输出中的 `avg_us` 和 `bandwidth_gib_s` 只覆盖 FFTS launch 到 stream synchronize 的区间，不包含辅助初始化和校验 memcpy。

如果要“一次传多个 IO”，增加 `--frags`。此时 `--bytes` 仍然是单个 IO 的大小：

```bash
./build/ffts_sdma_probe --device 0 --mode h2d-sdma --bytes 1048576 --frags 8 --lanes 8 --host-mem aclrt-registered-mapped
```

这会创建 8 个独立 SDMA context，每个 context 搬 1 MiB，总搬运量按 8 MiB 计算。输出里的 `bandwidth_gib_s` 使用总搬运量除以一次 FFTS launch + synchronize 的平均耗时。

注册和 mapped 地址相关的诊断日志会输出到 stderr，包括 host pointer、4K 对齐结果、注册 flag、注册返回值、mapped pointer 和最终写入 FFTS descriptor 的 source 地址。如果要保存日志，可以这样运行：

```bash
./build/ffts_sdma_probe --device 0 --mode h2d-sdma --bytes 1048576 --frags 1 --lanes 1 --host-mem aclrt-registered-mapped 2>&1 | tee h2d_aclrt_registered_mapped.log
```
