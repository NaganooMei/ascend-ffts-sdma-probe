# Ascend FFTS SDMA Probe

这个小工具只验证 Ascend FFTS Plus 的 SDMA descriptor 能否跑两种地址组合：

- `d2d-sdma`: device source 到 device destination。
- `h2d-sdma`: host source 到 device destination。

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

再跑多 descriptor 和多 ready lane：

```bash
./build/ffts_sdma_probe --device 0 --mode d2d-sdma --bytes 2097152 --frags 64 --lanes 8 --warmup 1 --repeat 10
./build/ffts_sdma_probe --device 0 --mode h2d-sdma --bytes 2097152 --frags 64 --lanes 8 --warmup 1 --repeat 10
```

## 参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--device` | `0` | Ascend device id。 |
| `--mode` | `all` | `d2d-sdma`、`h2d-sdma` 或 `all`。 |
| `--bytes` | `1048576` | 总搬运字节数，支持 `K`、`M`、`G` 后缀。 |
| `--frags` | `1` | 拆成多少个 SDMA context。 |
| `--lanes` | `1` | FFTS ready context 数上限。 |
| `--warmup` | `1` | 不计时 warmup 次数。 |
| `--repeat` | `10` | 计时次数。 |
| `--host-mem` | `aclrt` | host buffer 类型，支持 `aclrt` 和 `malloc`。 |

## 结果判断

- `d2d-sdma` 成功，说明 FFTS SDMA descriptor、launch 和 stream 同步基础路径可用。
- `h2d-sdma` launch 失败，通常说明当前 runtime 不接受 host source 地址进入 FFTS SDMA descriptor。
- `h2d-sdma` launch 成功但校验失败，需要继续比较 `--host-mem aclrt` 和 `--host-mem malloc`，判断是否和 host 地址类型或 DMA 可见性有关。

输出中的 `avg_us` 和 `bandwidth_gib_s` 只覆盖 FFTS launch 到 stream synchronize 的区间，不包含辅助初始化和校验 memcpy。
