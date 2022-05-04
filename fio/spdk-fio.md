# spdk fio

<https://github.com/spdk/spdk/tree/master/examples/nvme/fio_plugin>

## build

按照上述文档：拷贝fio代码并编译、编译spdk。

## Usage

要将 SPDK fio 插件与 fio 结合使用，请在运行 fio 时使用 `LD_PRELOAD` 指定插件二进制文件，并在 fio 配置文件中设置 `ioengine=spdk`（请参阅与此 README 位于同一目录中的 example_config.fio）。

```shell
LD_PRELOAD=<path to spdk repo>/build/fio/spdk_nvme fio <jobfile>
```

默认情况下，fio 会为每个作业分叉一个单独的进程。它还支持在同一进程中为每个作业生成一个单独的线程。**SPDK fio 插件仅限于后一种线程使用模型，因此 fio 作业在使用 SPDK fio 插件时还必须指定 thread=1**。SPDK fio 插件支持多个线程 - 在这种情况下，“1”仅表示“使用线程模式”。

```shell
sudo LD_PRELOAD=../../spdk/build/fio/spdk_nvme fio ./examples/spdk-example.fio
```

如果通过 ioengine 参数指定引擎的完整路径来动态加载 ioengine，fio 当前在关闭时存在竞争条件 - 建议使用 LD_PRELOAD 来避免这种竞争条件。

**测试随机工作负载时，建议设置 norandommap=1**。fio 的随机映射处理会消耗额外的 CPU 周期，这会随着时间的推移降低 fio_plugin 的性能，因为所有 I/O 都在单个 CPU 内核上提交和完成。

**在使用 SPDK 插件在多个 NVMe SSD 上测试 FIO 时，建议在 FIO 配置中使用多个作业**。据观察，在测试多个 NVMe SSD 时，FIO（启用了 SPDK 插件）和 SPDK perf（examples/nvme/perf/perf）之间存在一些性能差距。如果您使用为 FIO 测试配置的一项作业（即使用一个 CPU 内核），则针对许多 NVMe SSD 的性能比 SPDK perf（也使用一个 CPU 内核）差。但是如果你使用多个作业进行 FIO 测试，FIO 的性能与 SPDK perf 相似。分析此现象后，我们认为是FIO架构造成的。主要是 FIO 可以扩展多线程（即，使用 CPU 内核），但对多个 I/O 设备使用一个线程并不好。

配置文件example：

```txt
[global]
filename=trtype=PCIe traddr=0000.1b.00.0 ns=1
ioengine=spdk
thread=1
group_reporting=1
direct=1
verify=0
time_based=1
ramp_time=0
runtime=5
iodepth=128
norandommap=1
# rw=randrw
rw=randwrite

[test]
numjobs=1
```
