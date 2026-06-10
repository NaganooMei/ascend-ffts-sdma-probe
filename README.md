# Ascend FFTS SDMA Probe

这个小工具用来验证 Ascend FFTS Plus 的 SDMA descriptor 是否能搬运以下地址组合：

- `d2d-sdma`: device source 到 device destination。
- `h2d-sdma`: registered mapped host source 到 device destination。
- `h2h-sdma`: registered mapped host source 到 registered mapped host destination。

`h2d-sdma` 和 `h2h-sdma` 只保留已经验证更有意义的 mapped host 地址路径。普通 host 地址、未 mapped 地址和 raw `aclrtMallocHost` 地址路径已经从命令行选项里删除。

## 构建

```bash
cmake -S . -B build -DASCEND_ROOT=/usr/local/Ascend/ascend-toolkit/latest
cmake --build build -j
```

如果 Ascend toolkit 不在默认路径，把 `ASCEND_ROOT` 指到 `ascend-toolkit/latest`。

## 参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--device` | `0` | Ascend device id。 |
| `--mode` | `all` | `d2d-sdma`、`h2d-sdma`、`h2h-sdma` 或 `all`。 |
| `--bytes` | `1048576` | 单个 SDMA IO 的字节数，支持 `K`、`M`、`G` 后缀。 |
| `--frags` | `1` | 独立 SDMA IO descriptor 个数。 |
| `--lanes` | `8` | FFTS ready context 上限；`0` 表示自动等于本次 descriptor 数。 |
| `--warmup` | `1` | 不计时 warmup 次数。 |
| `--repeat` | `10` | 计时次数。 |
| `--host-mem` | `aclrt-registered-mapped` | `h2d-sdma` 和 `h2h-sdma` 的 host buffer 类型。 |

支持的 `--host-mem`：

| 类型 | 分配方式 | 注册方式 | FFTS descriptor 使用的地址 |
| --- | --- | --- | --- |
| `registered-mapped` | `posix_memalign(4096)` | `ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED` | `aclrtHostGetDevicePointer` 返回的 mapped 地址 |
| `mmap-registered-mapped` | `mmap` 匿名映射 | `ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED` | `aclrtHostGetDevicePointer` 返回的 mapped 地址 |
| `aclrt-registered-mapped` | `aclrtMallocHost` | `ACL_HOST_REG_MAPPED` | `aclrtHostGetDevicePointer` 返回的 mapped 地址 |

## 推荐 Case

下面每个 case 都给两组默认配置：

- 大 IO：单个 IO 为 4 MiB，一次提交 1000 个 descriptor，`--lanes 8`。
- 小 IO：单个 IO 为 32 KiB，一次提交 1000 个 descriptor，其他配置相同。

### D2D

```bash
./build/ffts_sdma_probe --device 0 --mode d2d-sdma --bytes 4M --frags 1000 --lanes 8 --warmup 1 --repeat 10
./build/ffts_sdma_probe --device 0 --mode d2d-sdma --bytes 32K --frags 1000 --lanes 8 --warmup 1 --repeat 10
```

### H2D

```bash
./build/ffts_sdma_probe --device 0 --mode h2d-sdma --bytes 4M --frags 1000 --lanes 8 --warmup 1 --repeat 10 --host-mem aclrt-registered-mapped
./build/ffts_sdma_probe --device 0 --mode h2d-sdma --bytes 32K --frags 1000 --lanes 8 --warmup 1 --repeat 10 --host-mem aclrt-registered-mapped
```

如果要比较三种 mapped host 内存，把 `--host-mem` 换成：

```bash
--host-mem registered-mapped
--host-mem mmap-registered-mapped
--host-mem aclrt-registered-mapped
```

### H2H

```bash
./build/ffts_sdma_probe --device 0 --mode h2h-sdma --bytes 4M --frags 1000 --lanes 8 --warmup 1 --repeat 10 --host-mem aclrt-registered-mapped
./build/ffts_sdma_probe --device 0 --mode h2h-sdma --bytes 32K --frags 1000 --lanes 8 --warmup 1 --repeat 10 --host-mem aclrt-registered-mapped
```

## 结果和校验

输出中的 `avg_us` 和 `bandwidth_gib_s` 只覆盖 FFTS launch 到 stream synchronize 的区间，不包含辅助初始化和校验 memcpy。

每个模式都会做数据一致性校验：

- `d2d-sdma`: host pattern 先通过 ACL copy 写入 device source，FFTS D2D 后再读回 destination 校验。
- `h2d-sdma`: host source 写 pattern，FFTS H2D 后读回 device destination 校验。
- `h2h-sdma`: host source 写 pattern，FFTS H2H 后直接校验 host destination。
