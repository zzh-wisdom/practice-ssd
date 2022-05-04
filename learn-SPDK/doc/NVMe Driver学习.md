# NVMe Driver

<https://spdk.io/doc/nvme.html>

- [1. 注意点](#1-注意点)
- [2. NVMe over Fabrics Host Support](#2-nvme-over-fabrics-host-support)
- [3. NVMe 热插拔](#3-nvme-热插拔)
- [4. 编程实践](#4-编程实践)
	- [4.1. 基本情况](#41-基本情况)
	- [4.2. 环境env](#42-环境env)
	- [4.3. 设备控制器](#43-设备控制器)
	- [4.4. 传输ID（trid）](#44-传输idtrid)
	- [4.5. 命名空间ns](#45-命名空间ns)
	- [4.6. QPair操作](#46-qpair操作)
	- [4.7. IO操作](#47-io操作)
	- [其他特殊IO操作](#其他特殊io操作)
	- [4.8. 编程步骤](#48-编程步骤)
	- [其他接口汇总](#其他接口汇总)
	- [使用spdk_nvme_ctrlr_cmd_io_raw 进行 deallocate](#使用spdk_nvme_ctrlr_cmd_io_raw-进行-deallocate)
- [TODO](#todo)

## 1. 注意点

1. 融合操作（SPDK_NVME_IO_FLAGS_FUSE_FIRST、SPDK_NVME_IO_FLAGS_FUSE_SECOND）
2. NVMe 队列对 (struct spdk_nvme_qpair) 为 I/O 提供并行提交路径。I/O 可以从不同的线程同时提交到多个队列对上。但是，队列对不包含锁或原子，因此**给定的队列对一次只能由单个线程使用**，否则造成未定义行为

**扩展性上**：允许的队列对数由 NVMe SSD 本身决定。该规范允许上千个（64K），**但大多数设备支持 32 到 128 个**。该规范不保证每个队列对的可用性能，<font color=red>但实际上，一个设备的全部性能几乎总是可以使用一个队列对来实现</font>。例如，如果设备声称能够在队列深度 128 处每秒进行 450,000 次 I/O，实际上，这与“驱动程序是使用 4 个队列对，每个队列深度为 32，还是使用单个队列对，队列深度为128”无关。

鉴于上述情况，使用 SPDK 的应用程序最简单的线程模型是在池中**生成固定数量的线程**，并将单个 NVMe 队列对专用于每个线程。进一步的改进是将**每个线程固定到一个单独的 CPU 核心**，并且 SPDK 文档通常会互换使用“CPU 核心”和“线程”，因为我们已经考虑了这种线程模型。

NVMe 驱动程序在 I/O 路径中不使用任何锁，**因此只要队列对和 CPU 内核专用于每个新线程，它就可以在每个线程的性能方面线性扩展**。为了充分利用这种扩展，应用程序应考虑组织其内部数据结构，以便将数据专门分配给单个线程。所有需要数据的操作都应该通过向拥有线程发送请求来完成。这导致了**消息传递架构，而不是锁定架构**，并将导致跨 CPU 内核的卓越扩展。

**内存**上：驱动内不为IO命令实现任何数据buffer，即零拷贝，但某些管理命令存在数据拷贝。

每个队列对都有许多跟踪器，用于跟踪调用者提交的命令。I/O 队列的数量跟踪器取决于用户输入的队列大小和从控制器功能寄存器字段中读取的支持最大队列条目值（MQES，基于 0 的值）。每个跟踪器都有固定大小 4096 Bytes，因此每个 I/O 队列使用的最大内存为：(MQES + 1) * 4 KiB。

I/O 队列对可以分配在**主机内存**中，这用于大多数 NVMe 控制器，一些支持 Controller Memory Buffer 的 NVMe 控制器可能会将 I/O 队列对放在**控制器的 PCI BAR 空间**中，SPDK NVMe 驱动程序可以将 I/ O 提交队列到控制器内存缓冲区，它取决于用户的输入和控制器的能力。每个提交队列条目 (SQE) 和完成队列条目 (CQE) 分别消耗 64 字节和 16 字节。因此，每个 I/O 队列对使用的最大内存为 (MQES + 1) * (64 + 16) Bytes。

所以单个qp对占用内存的总大小最大为(MQES + 1)*(4096 + 64 + 16) B.

## 2. NVMe over Fabrics Host Support

见参考文档，以及 <https://spdk.io/doc/nvmf.html>

## 3. NVMe 热插拔

在 NVMe 驱动程序级别，我们为 Hotplug 提供以下支持：

1. 热插拔事件检测：NVMe 库的用户可以定期调用spdk_nvme_probe()来检测热插拔事件。检测到的每个新设备都将调用 probe_cb，然后是 attach_cb。用户还可以选择提供一个 remove_cb，如果系统上不再存在先前连接的 NVMe 设备，则将调用该 remove_cb。移除设备的所有后续 I/O 都将返回错误。
2. 带 IO 负载的热移除 NVMe：当发生 I/O 时热移除设备时，对 PCI BAR 的所有访问都将导致 SIGBUS 错误。NVMe 驱动程序通过安装 SIGBUS 处理程序并将 PCI BAR 重新映射到新的占位符内存位置来自动处理这种情况。这意味着在热删除期间进行中的 I/O 将完成并带有适当的错误代码，并且不会使应用程序崩溃。

也可以看看
[spdk_nvme_probe](https://spdk.io/doc/nvme_8h.html#a225bbc386ec518ae21bd5536f21db45d)

## 4. 编程实践

### 4.1. 基本情况

1. 为控制器分配的任何I/O qpair 都可以将 I/O 提交到该控制器上的任何命名空间。
2. 不提供任何qp同步，因此一个qp只能单个线程使用。
3. 用户负责调用 spdk_nvme_qpair_process_completions(ns_entry->qpair, 0) 轮询完成。

### 4.2. 环境env

```cpp
struct spdk_env_opts opts;
spdk_env_opts_init(&opts);
opts.name = "hello_world";
if (spdk_env_init(&opts) < 0) {
	fprintf(stderr, "Unable to initialize SPDK env\n");
	return 1;
}

// 释放环境库中spdk_env_init（）分配的任何资源。
// 此调用后，不得进行 SPDK env 函数调用。
// 预计此函数的常见用法是在终止进程之前或在同一进程中重新初始化环境库之前调用它。
spdk_env_fini();
```

### 4.3. 设备控制器

#### 4.3.1. attach/创建

```cpp
int
spdk_nvme_probe(const struct spdk_nvme_transport_id *trid, void *cb_ctx,
		spdk_nvme_probe_cb probe_cb, spdk_nvme_attach_cb attach_cb,
		spdk_nvme_remove_cb remove_cb)

rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
if (rc != 0) {
    fprintf(stderr, "spdk_nvme_probe() failed\n");
    rc = 1;
    goto exit;
}
```

trid – 指示要枚举的总线的传输 ID。如果 trtype 是 PCIe 或 trid 为 NULL，这将扫描本地 PCIe 总线。如果 tr 类型为 RDMA，则 traddr 和 trsvcid 必须指向 NVMe-oF 发现服务的位置。

枚举由 trid(传输ID) 指示的总线，并在需要时将用户空间 NVMe 驱动程序附加到找到的每个设备。此函数不是线程安全的，一次只能从一个线程调用，而没有其他线程主动使用任何 NVMe 设备。如果从辅助进程调用，则只会探测已连接到主进程中的用户空间驱动程序的设备。如果多次调用，则只会报告尚未连接到 SPDK NVMe 驱动程序的设备。要停止使用控制器并释放其关联的资源，请调用 spdk_nvme_detach（）。

probe_cb：每个NVMe设备被发现后的回调
attach_cb：每个NVMe设备probe_cb时返回true，且附加到用户空间驱动程序时的回调函数
remove_cb – 之前调用spdk_nvme_probe()并attach的设备，如果现在不想attach了，可以指定该回调函数，Optional; specify NULL if removal notices are not desired.

```cpp
typedef bool (*spdk_nvme_probe_cb)(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
				   struct spdk_nvme_ctrlr_opts *opts);
```

可以在probe_cb函数的opts指定相关参数，如队列个数、大小等等，库会为opts填充某些默认值，但不会填充完整，最终的值在attach_cb回调中得到。

```cpp
typedef void (*spdk_nvme_attach_cb)(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
				    struct spdk_nvme_ctrlr *ctrlr,
				    const struct spdk_nvme_ctrlr_opts *opts);
```

attach_cb中opts的值是控制器实际使用的参数，可能与用户开始配置的参数不一样，具体取决于控制器的支持。某个设备的默认参数：

```shell
Attaching to 0000:81:00.0
spdk_nvme_ctrlr_opts default:
        num_io_queues:31
        use_cmb_sqs:0
        io_queue_size:256
        io_queue_requests:512
```

通常队列的最大深度是 4096。

**其他连接到设备的函数**

```cpp
struct spdk_nvme_ctrlr *spdk_nvme_connect(const struct spdk_nvme_transport_id *trid,
		const struct spdk_nvme_ctrlr_opts *opts,
		size_t opts_size);
int spdk_nvme_ctrlr_disconnect(struct spdk_nvme_ctrlr *ctrlr);
```

连接到某个特定设备。

#### 4.3.2. detach/释放

```cpp
int
spdk_nvme_detach(struct spdk_nvme_ctrlr *ctrlr)
```

成功后，spdk_nvme_ctrlr句柄不再有效。此函数应从单个线程调用，而没有其他线程主动使用 NVMe 设备。
成功返回0，否则返回-1；

```cpp
int
spdk_nvme_detach_async(struct spdk_nvme_ctrlr *ctrlr,
		       struct spdk_nvme_detach_ctx **_detach_ctx)
// 用法
struct spdk_nvme_detach_ctx *detach_ctx = NULL;
spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
```

异步detach，用于detach多个设备。

**如果此调用是序列中分离的第一个成功开始，则分配上下文以跟踪多个控制器的分离，否则请使用传递的上下文**。然后，开始从 NVMe 驱动程序中分离spdk_nvme_probe（） 的attach_cb返回的指定设备，并将此分离追加到上下文中。用户必须调用 spdk_nvme_detach_poll_async（） 才能完成分离。如果在此调用之前未分配上下文，并且指定的设备已从调用方进程本地分离，但任何其他进程仍附加该设备或未能分离，则不会分配上下文。此函数应从单个线程调用，而没有其他线程主动使用 NVMe 设备。

```cpp
void spdk_nvme_detach_poll(struct spdk_nvme_detach_ctx *detach_ctx)

// 用法
if (detach_ctx) {
	// 轮询设备分离完成
	spdk_nvme_detach_poll(detach_ctx);
}
```

继续在内部调用 spdk_nvme_detach_poll_async（），直到返回 0。

### 4.4. 传输ID（trid）

```cpp
struct spdk_nvme_transport_id {
	/**
	 * NVMe transport string.
     *  传输类型字符串，如FC PCIE RDMA TCP
	 */
	char trstring[SPDK_NVMF_TRSTRING_MAX_LEN + 1];

	/**
	 * NVMe transport type.
	 */
	enum spdk_nvme_transport_type trtype;

	/**
	 * Address family of the transport address.
	 *
	 * For PCIe, this value is ignored.
	 */
	enum spdk_nvmf_adrfam adrfam;

	/**
	 * Transport address of the NVMe-oF endpoint. For transports which use IP
	 * addressing (e.g. RDMA), this should be an IP address. For PCIe, this
	 * can either be a zero length string (the whole bus) or a PCI address
	 * in the format DDDD:BB:DD.FF or DDDD.BB.DD.FF. For FC the string is
	 * formatted as: nn-0xWWNN:pn-0xWWPN” where WWNN is the Node_Name of the
	 * target NVMe_Port and WWPN is the N_Port_Name of the target NVMe_Port.
	 */
	char traddr[SPDK_NVMF_TRADDR_MAX_LEN + 1];

	/**
	 * Transport service id of the NVMe-oF endpoint.  For transports which use
	 * IP addressing (e.g. RDMA), this field should be the port number. For PCIe,
	 * and FC this is always a zero length string.
	 */
	char trsvcid[SPDK_NVMF_TRSVCID_MAX_LEN + 1];

	/**
	 * Subsystem NQN of the NVMe over Fabrics endpoint. May be a zero length string.
	 */
	char subnqn[SPDK_NVMF_NQN_MAX_LEN + 1];

	/**
	 * The Transport connection priority of the NVMe-oF endpoint. Currently this is
	 * only supported by posix based sock implementation on Kernel TCP stack. More
	 * information of this field can be found from the socket(7) man page.
	 */
	int priority;
};
```

对于PCI只用关注：trtype和traddr。典型trid的值为：

```txt
Attached to 0000:81:00.0
    trstring: PCIE
    trtype: 256
    adrfam: 0x0
    traddr: 0000:81:00.0
    trsvcid:
    subnqn:
    priority: 0
```

trstring当作trtype的human字符串即可。

可通过下面的函数将一个字符串转化成trid：

```cpp
int spdk_nvme_transport_id_parse(struct spdk_nvme_transport_id *trid, const char *str);
```

成功则返回0，否则返回负数。str是一个以0结尾的C string，并且包含一到多个由空格分割的 `key:value` 对。

Key          | Value
------------ | -----
trtype       | Transport type (e.g. PCIe, RDMA)
adrfam       | Address family (e.g. IPv4, IPv6)
traddr       | Transport address (e.g. 0000:04:00.0 for PCIe, 192.168.100.8 for RDMA, or WWN for FC)
trsvcid      | Transport service identifier (e.g. 4420)
subnqn       | Subsystem NQN

未指定的 trid 字段保持不变，因此调用方必须在调用此函数之前初始化 trid（例如，memset（） 到 0）。

举例：

```shell
# PCIe
perf -q 128 -o 4096 -w randread -r 'trtype:PCIe traddr:0000:04:00.0' -t 300
# NVMe of fabric
perf -q 128 -o 4096 -w randread -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420' -t 300
```

可能用到的**辅助函数**：

```cpp
void
spdk_nvme_trid_populate_transport(struct spdk_nvme_transport_id *trid,
				  enum spdk_nvme_transport_type trtype)
```

填充trid的trstring和trtype字段

### 4.5. 命名空间ns

每个控制器都有一个或多个命名空间。命名空间的编号从 1 到命名空间的总数。编号中永远不会有任何间隙。命名空间的数量是通过调用 spdk_nvme_ctrlr_get_num_ns（） 获得的。

获取命名空间

```cpp
int nsid;
struct spdk_nvme_ns *ns;
for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
	// 获取命名空间
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
	if (ns == NULL) {
		continue;
	}
	// register_ns(ctrlr, ns);
}
```

可以在probe_cb中获取命名空间。

ns在设备ctrl中通过红黑树管理
RB_HEAD(nvme_ns_tree, spdk_nvme_ns)	ns;
所以上面获取的命名空间只是树上节点的索引，不需要用户释放空间

一些查询API：

```cpp
spdk_nvme_ns_get_sector_size()  // 获取扇区大小
spdk_nvme_ns_get_extended_sector_size() // 获取给定命名空间的扩展扇区大小（以字节为单位）。此函数返回数据扇区加上元数据的大小
spdk_nvme_ns_get_md_size() // 获取元数据的大小
spdk_nvme_ns_supports_extended_lba(ns) // 检查启用端到端数据保护后，命名空间是否可以支持扩展 LBA。
spdk_nvme_ns_get_data(ns) // 获取 NVMe 规范定义的标识命名空间数据。
spdk_nvme_ns_get_pi_type(ns) // 获取给定命名空间的端到端数据保护（protection information）信息类型。
```

### 4.6. QPair操作

#### 4.6.1. 创建QP

```cpp
struct spdk_nvme_qpair *
spdk_nvme_ctrlr_alloc_io_qpair(struct spdk_nvme_ctrlr *ctrlr,
			       const struct spdk_nvme_io_qpair_opts *user_opts,
			       size_t opts_size)
```

分配 I/O 队列对（提交和完成队列）。默认情况下，此功能还执行新创建的 qpair 所需的任何连接活动。若要避免这种行为，用户应将选择结构中的create_only标志设置为 true。每个队列对一次只能从单个线程使用（互斥必须由用户强制执行）。

- ctrlr – NVMe controller for which to allocate the I/O queue pair.
- opts – I/O qpair creation options, or NULL to use the defaults as returned by spdk_nvme_ctrlr_get_default_io_qpair_opts().
- opts_size – Must be set to sizeof(struct spdk_nvme_io_qpair_opts), or 0 if opts is NULL.

```cpp
struct spdk_nvme_io_qpair_opts {
	// qp的优先级
	enum spdk_nvme_qprio qprio;
	/**
	 * 队列深度. Overrides spdk_nvme_ctrlr_opts::io_queue_size（这是默认值）.
	 */
	uint32_t io_queue_size;

	/**
	 * The number of requests to allocate for this NVMe I/O queue.
	 *
	 * Overrides spdk_nvme_ctrlr_opts::io_queue_requests.（默认值）
	 *
	 * This should be at least as large as io_queue_size.
	 *
	 * A single I/O may allocate more than one request, since splitting may be
	 * necessary to conform to the device's maximum transfer size, PRP list
	 * compatibility requirements, or driver-assisted striping.
	 */
	uint32_t io_queue_requests;

	/**
	 * When submitting I/O via spdk_nvme_ns_read/write and similar functions,
	 * don't immediately submit it to hardware. Instead, queue up new commands
	 * and submit them to the hardware inside spdk_nvme_qpair_process_completions().
	 *
	 * This results in better batching of I/O commands. Often, it is more efficient
	 * to submit batches of commands to the underlying hardware than each command
	 * individually.
	 *
	 * This only applies to PCIe and RDMA transports.
	 *
	 * The flag was originally named delay_pcie_doorbell. To allow backward compatibility
	 * both names are kept in unnamed union.
	 * 设置为true，则表示IO命令不会立即提交到硬件，而是等到调用spdk_nvme_qpair_process_completions时，一起提交。这样通过batch处理，可以提高性能（默认是false）
	 */
	union {
		bool delay_cmd_submit;
		bool delay_pcie_doorbell;
	};

	/**
	 * These fields allow specifying the memory buffers for the submission and/or
	 * completion queues.
	 * By default, vaddr is set to NULL meaning SPDK will allocate the memory to be used.
	 * If vaddr is NULL then paddr must be set to 0.
	 * If vaddr is non-NULL, and paddr is zero, SPDK derives the physical
	 * address for the NVMe device, in this case the memory must be registered.（spdk_zmalloc注册好）
	 * If a paddr value is non-zero, SPDK uses the vaddr and paddr as passed
	 * SPDK assumes that the memory passed is both virtually and physically
	 * contiguous.
	 * If these fields are used, SPDK will NOT impose any restriction
	 * on the number of elements in the queues.（不限制队列元素大小）
	 * The buffer sizes are in number of bytes, and are used to confirm
	 * that the buffers are large enough to contain the appropriate queue.
	 * These fields are only used by PCIe attached NVMe devices.  They
	 * are presently ignored for other transports.
	 */
	struct {
		struct spdk_nvme_cmd *vaddr;
		uint64_t paddr;
		uint64_t buffer_size;
	} sq;
	struct {
		struct spdk_nvme_cpl *vaddr;
		uint64_t paddr;
		uint64_t buffer_size;
	} cq;

	/**
	 * This flag indicates to the alloc_io_qpair function that it should not perform
	 * the connect portion on this qpair. This allows the user to add the qpair to a
	 * poll group and then connect it later.（只创建，不执行相关的连接操作，默认是false）
	 */
	bool create_only;

	/**
	 * This flag if set to true enables the creation of submission and completion queue
	 * asynchronously. This mode is currently supported at PCIe layer and tracks the
	 * qpair creation with state machine and returns to the user.Default mode is set to
	 * false to create io qpair synchronously.（默认同步）
	 */
	bool async_mode;
};
```

用户可以使用 spdk_nvme_ctrlr_get_default_io_qpair_opts（） 检索控制器创建 I/O 队列对的默认选项。

```cpp
spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
```

#### 连接 QPair

```cpp
int
spdk_nvme_ctrlr_connect_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
void spdk_nvme_ctrlr_disconnect_io_qpair(struct spdk_nvme_qpair *qpair);
int spdk_nvme_ctrlr_reconnect_io_qpair(struct spdk_nvme_qpair *qpair);// 连接的状态下重新连接，尝试重新连接给定的 qpair。 此函数旨在在已连接但已进入失败状态的 qpairs 上调用，如 spdk_nvme_qpair_process_completions 或 spdk_nvme_ns_cmd_* 函数之一的返回值 -ENXIO 所示
```

连接新创建的 I/O qpair。此函数执行新创建的 qpair 所需的任何连接活动。在调用spdk_nvme_ctrlr_alloc_io_qpair并在spdk_nvme_io_qpair_opts结构中将create_only标志设置为 true 后，应调用该函数。如果在已连接的 qpair 上执行此调用将失败。有关重新连接 qpair，请参阅spdk_nvme_ctrlr_reconnect_io_qpair。对于 TCP 和 RDMA 等结构，此函数实际上通过连接 qpair 的线路发送命令。对于 PCIe，此功能执行一些内部状态机操作。

#### 4.6.2. 销毁QPair

```cpp
int
spdk_nvme_ctrlr_free_io_qpair(struct spdk_nvme_qpair *qpair)
```

释放由 spdk_nvme_ctrlr_alloc_io_qpair（） 分配的 I/O 队列对。调用此函数后，不得访问 qpair。成功返回0。

### 4.7. IO操作

1. 读写大小需要是扇区的整数倍

#### 4.7.1. 内存分配

需要分配专门的内存，给SPDK进行IO操作，且内存区域需要支持DMA。

```cpp
void *
spdk_nvme_ctrlr_map_cmb(struct spdk_nvme_ctrlr *ctrlr, size_t *size)
```

映射以前保留的控制器内存缓冲区，以便从 CPU 中可以看到其数据。此操作并不总是可能的。有些设备内部不含内存。

通常使用下面的函数在host中分配内存：

```cpp
void *
spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags)
```

Allocate dma/sharable memory based on a given dma_flg. It is a memory buffer with the given size, alignment and socket id. Also, the buffer will be zeroed.

参数:
- size – Size in bytes.
- align – If non-zero, the allocated buffer is aligned to a multiple of align. In this case, it must be a power of two. **The returned buffer is always aligned to at least cache line size.**
- phys_addr – **Deprecated弃用**. Please use **spdk_vtophys()** for retrieving physical addresses. A pointer to the variable to hold the physical address of the allocated buffer is passed. If NULL, the physical address is not returned.
- socket_id – Socket ID to allocate memory on, or SPDK_ENV_SOCKET_ID_ANY for any socket.
- flags – Combination of SPDK_MALLOC flags (SPDK_MALLOC_DMA, SPDK_MALLOC_SHARE).

典型用法：

```cpp
spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
```

通常按Page的若干倍分配，按Page刷SSD能充分发挥写性能。

记得调用函数释放：

```cpp
void
spdk_free(void *buf)
```

#### IO操作

```cpp
int
spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		       void *buffer, uint64_t lba,
		       uint32_t lba_count, spdk_nvme_cmd_cb cb_fn, void *cb_arg,
		       uint32_t io_flags)
// 读
spdk_nvme_ns_cmd_read();
```

将写入 I/O 提交到指定的 NVMe 命名空间。该命令将提交到由 spdk_nvme_ctrlr_alloc_io_qpair（） 分配的 qpair。用户必须确保在任何给定时间只有一个线程在给定的 qpair 上提交 I/O。

- lba_count：写入操作的长度（以扇区lba为单位）。
- io_flags：很多标记信息，通常默认为0即可。

返回值：0 if successfully submitted, negated errnos on the following error conditions: -EINVAL: The request is malformed. -ENOMEM: The request cannot be allocated. -ENXIO: The qpair is failed at the transport level.

**带sgl的读写**：

```cpp
int spdk_nvme_ns_cmd_readv(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   uint64_t lba, uint32_t lba_count,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			   spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			   spdk_nvme_req_next_sge_cb next_sge_fn);
int spdk_nvme_ns_cmd_writev(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			    uint64_t lba, uint32_t lba_count,
			    spdk_nvme_cmd_cb cb_fn, void *cb_arg, uint32_t io_flags,
			    spdk_nvme_req_reset_sgl_cb reset_sgl_fn,
			    spdk_nvme_req_next_sge_cb next_sge_fn);
```

`reset_sgl_fn`将sgl重置到指定偏移。next_sge_fn用于迭代sgl，函数会返回起始地址和长度。

带md的读写：

```cpp
spdk_nvme_ns_cmd_write_with_md()
spdk_nvme_ns_cmd_writev_with_md()
spdk_nvme_ns_cmd_writev_ext()// 与内存区域有关，待学习

spdk_nvme_ns_cmd_read_with_md()
spdk_nvme_ns_cmd_readv_with_md()
spdk_nvme_ns_cmd_readv_ext()
```

```cpp
int spdk_nvme_ctrlr_cmd_io_raw(struct spdk_nvme_ctrlr *ctrlr,
			       struct spdk_nvme_qpair *qpair,
			       struct spdk_nvme_cmd *cmd,
			       void *buf, uint32_t len,
			       spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int spdk_nvme_ctrlr_cmd_io_raw_with_md(struct spdk_nvme_ctrlr *ctrlr,
				       struct spdk_nvme_qpair *qpair,
				       struct spdk_nvme_cmd *cmd,
				       void *buf, uint32_t len, void *md_buf,
				       spdk_nvme_cmd_cb cb_fn, void *cb_arg);
```

向控制器发送IO命令。低级接口，且正确性不做检查。非线程安全。
**注意**：在构建 nvme_command 时，不需要填写 PRP 列表/SGL 或 CID。 司机将为您处理这两个问题。

### 其他特殊IO操作

```cpp
spdk_nvme_ns_cmd_write_zeroes()  // 某些ssd不支持
spdk_nvme_ns_cmd_flush() // 相当于fence
int spdk_nvme_ctrlr_cmd_abort(struct spdk_nvme_ctrlr *ctrlr,
			      struct spdk_nvme_qpair *qpair,
			      uint16_t cid,
			      spdk_nvme_cmd_cb cb_fn,
			      void *cb_arg); // 中止之前提交的特定 NVMe 命令。
```

```cpp
// Write Uncorrectable 命令用于将逻辑块标记为无效。
// 在此操作之后读取指定的逻辑块时，将返回失败并显示未恢复的读取错误状态。
// 为了清除无效逻辑块状态，需要对这些逻辑块执行写操作。
int spdk_nvme_ns_cmd_write_uncorrectable(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
		uint64_t lba, uint32_t lba_count,
		spdk_nvme_cmd_cb cb_fn, void *cb_arg);
// 可以用来释放LBA
// 这是一个方便的包装器，它将自动分配和构造正确的数据缓冲区。
// 因此，范围不需要从固定内存中分配，可以放在堆栈上。
// 如果需要更高性能、零拷贝版本的 DSM，只需使用 spdk_nvme_ctrlr_cmd_io_raw() 构建并提交原始命令。
// 见最后的示例：用spdk_nvme_ctrlr_cmd_io_raw实现lba deallocate
// 该命令的其他功能，主要用来向 SSD控制器指示数据范围的访问情况，便于控制器可以进行某些优化，暂忽略
spdk_nvme_ns_cmd_dataset_management()
```

A logical block is allocated when it is written with a Write or Write Uncorrectable command. A
logical block may be deallocated using the Dataset Management command.

> **TODO**: 有必要看一下 NVMe 规范，学习如何提交原始命令。

```cpp
int spdk_nvme_ns_cmd_copy(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			  const struct spdk_nvme_scc_source_range *ranges,
			  uint16_t num_ranges,
			  uint64_t dest_lba,
			  spdk_nvme_cmd_cb cb_fn,
			  void *cb_arg);
```

将range中指定的LBA的范围，拷贝到dest_lba为起始位置的范围。这是一个方便的包装器，它将自动分配和构造正确的数据缓冲区。 因此，范围不需要从固定内存中分配，可以放在堆栈上。 如果需要更高性能、零拷贝版本的 SCC，只需使用 spdk_nvme_ctrlr_cmd_io_raw() 构建并提交原始命令。

```cpp
int spdk_nvme_ns_cmd_flush(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair,
			   spdk_nvme_cmd_cb cb_fn, void *cb_arg);
```

flush命令用于将非易失写缓存中的内容持久化，同时将之前提交的命令全部应用。

```cpp
int spdk_nvme_ns_cmd_compare(struct spdk_nvme_ns *ns, struct spdk_nvme_qpair *qpair, void *payload,
			     uint64_t lba, uint32_t lba_count, spdk_nvme_cmd_cb cb_fn,
			     void *cb_arg, uint32_t io_flags);
```

通过fuse操作可以实现，compare and write。

#### 轮询完成

```cpp
int32_t
spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
```

处理在队列对上提交的 I/O 的任何未完成完成项。此调用是非阻塞的，即它仅处理在此函数调用时准备就绪的完成。它不会等待未完成的命令完成。对于每个已完成的命令，**如果在提交请求时指定为非 NULL，则将调用请求的回调函数**。调用方必须确保一次只能从一个线程使用每个队列对。当控制器连接到 SPDK NVMe 驱动程序时，可以随时调用此功能。

回调函数的格式如下（在提交IO时指定）：

```cpp
typedef void (*spdk_nvme_cmd_cb)(void *ctx, const struct spdk_nvme_cpl *cpl);
```

cpl 指示完成的状态，相关的辅助函数/宏：

```cpp
spdk_nvme_cpl_is_error(cpl)
spdk_nvme_cpl_is_success(cpl)
spdk_nvme_cpl_get_status_string(&completion->status) // 获取状态的字符串
// 打印完成的消息
spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
```

spdk_nvme_qpair_print_completion需要将log等级设置为2（最高），默认是0；

```cpp
spdk_log_set_print_level(2);
spdk_log_set_level(2);
```

#### wq group轮询

创建一个新的轮询组，在一个线程使用多个qp的情况下，可以使用该结构体（但都单线程了，一个QP也OK了）

```cpp
struct spdk_nvme_poll_group *
spdk_nvme_poll_group_create(void *ctx, struct spdk_nvme_accel_fn_table *table)

// 销毁
int spdk_nvme_poll_group_destroy(struct spdk_nvme_poll_group *group)
```

- ctx – A user supplied context that can be retrieved later with spdk_nvme_poll_group_get_ctx
- table(暂时忽略) – The call back table defined by users which contains the accelerated functions which can be used to accelerate some operations such as crc32c.

```cpp
int
spdk_nvme_poll_group_add(struct spdk_nvme_poll_group *group, struct spdk_nvme_qpair *qpair)
```

将spdk_nvme_qpair添加到poll组。**qpairs只有在处于断开连接状态时才能添加到poll组中**;即，它们要么刚刚分配但尚未连接，要么它们已断开连接（调用spdk_nvme_ctrlr_disconnect_io_qpair）。

对于如何连接QP，参考 `spdk_nvme_ctrlr_connect_io_qpair`。

**轮询group**：

```cpp
int64_t
spdk_nvme_poll_group_process_completions(struct spdk_nvme_poll_group *group,
		uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
```

轮询此轮询组中所有 qpair 的完成情况。**对于轮询组中的所有断开连接的 qpair，将调用disconnected_qpair_cb，包括在此调用上下文中失败的 qpairs**。用户负责尝试重新连接或销毁这些 qpair。

- completions_per_qpair – The maximum number of completions per qpair.
- disconnected_qpair_cb – A callback function of type spdk_nvme_disconnected_qpair_cb. Must be non-NULL

返回值：return The number of completions across all qpairs, -EINVAL if no disconnected_qpair_cb is passed, or -EIO if the shared completion queue cannot be polled for the RDMA transport.

获取统计信息：

```cpp
spdk_nvme_poll_group_get_stats(group, &stat);
spdk_nvme_poll_group_free_stats(group, stat);
```

#### fuse操作

要“融合”两个命令，第一个命令应该设置 SPDK_NVME_IO_FLAGS_FUSE_FIRST io 标志，下一个命令应该设置 SPDK_NVME_IO_FLAGS_FUSE_SECOND。

此外，必须满足以下规则才能将两个命令作为一个原子单元执行：

- 这些命令应在同一提交队列中彼此相邻插入。
- 两个命令的 LBA 范围应该相同。

例如，要发送融合的比较和写入操作，用户必须调用 spdk_nvme_ns_cmd_compare，然后调用 spdk_nvme_ns_cmd_write，并确保在同一队列之间没有其他操作提交，如下例所示：

```cpp
rc = spdk_nvme_ns_cmd_compare(ns, qpair, cmp_buf, 0, 1, nvme_fused_first_cpl_cb,
                NULL, SPDK_NVME_CMD_FUSE_FIRST);
if (rc != 0) {
        ...
}

rc = spdk_nvme_ns_cmd_write(ns, qpair, write_buf, 0, 1, nvme_fused_second_cpl_cb,
                NULL, SPDK_NVME_CMD_FUSE_SECOND);
if (rc != 0) {
        ...
}
```

NVMe 规范目前将比较和写入定义为融合操作。对比较和写入的支持由控制器标志SPDK_NVME_CTRLR_COMPARE_AND_WRITE_SUPPORTED报告。

#### 管理命令

```cpp
int spdk_nvme_ctrlr_cmd_admin_raw(struct spdk_nvme_ctrlr *ctrlr,
				  struct spdk_nvme_cmd *cmd,
				  void *buf, uint32_t len,
				  spdk_nvme_cmd_cb cb_fn, void *cb_arg);
```

将给定的管理命令发送到 NVMe 控制器。这是用来直接提交管理命令的低级接口，是其他很多控制/查询功能的基础。
注意：cmd的正确性不会被检查。
When constructing the nvme_command it is not necessary to fill out the PRP list/SGL or the CID. The driver will handle both of those for you.
该函数是线程安全的。

```cpp
int32_t spdk_nvme_ctrlr_process_admin_completions(struct spdk_nvme_ctrlr *ctrlr);
```

轮询管理命令的完成。是线程安全的，而且可以等待任何其他线程发起命令的完成。
返回值：number of completions processed (may be 0) or negated on error

### 4.8. 编程步骤

1. 初始化环境env：spdk_env_opts_init，spdk_env_init
2. 连接设备：spdk_nvme_probe
3. 获取命名空间ns
4. 创建qp：spdk_nvme_ctrlr_alloc_io_qpair
5. 分配pin的页，用于IO
6. 提交IO操作到qp，如spdk_nvme_ns_cmd_write
7. 轮询完成spdk_nvme_qpair_process_completions
8. 释放qp：spdk_nvme_ctrlr_free_io_qpair
9. //命名空间不需要释放
10. detach设备
11. 释放环境env资源：spdk_env_fini

### 其他接口汇总

控制器选项opt相关的函数：

```cpp
void spdk_nvme_ctrlr_get_default_ctrlr_opts(struct spdk_nvme_ctrlr_opts *opts,
		size_t opts_size);  // 获取默认选项
const struct spdk_nvme_ctrlr_opts *spdk_nvme_ctrlr_get_opts(struct spdk_nvme_ctrlr *ctrlr); // 指定控制器的选项
```

NVMe 控制器相关：

```cpp
void spdk_nvme_ctrlr_set_remove_cb(struct spdk_nvme_ctrlr *ctrlr,
				   spdk_nvme_remove_cb remove_cb, void *remove_ctx); // 重新设置控制器的删除回调
int spdk_nvme_ctrlr_reset(struct spdk_nvme_ctrlr *ctrlr);   // 对 NVMe 控制器执行完全硬件重置。
void spdk_nvme_ctrlr_prepare_for_reset(struct spdk_nvme_ctrlr *ctrlr);
const struct spdk_nvme_ctrlr_data *spdk_nvme_ctrlr_get_data(struct spdk_nvme_ctrlr *ctrlr);

bool spdk_nvme_ctrlr_is_log_page_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t log_page);
bool spdk_nvme_ctrlr_is_feature_supported(struct spdk_nvme_ctrlr *ctrlr, uint8_t feature_code);

// ns 也有类似的函数
int spdk_nvme_ctrlr_cmd_set_feature(struct spdk_nvme_ctrlr *ctrlr,
				    uint8_t feature, uint32_t cdw11, uint32_t cdw12,
				    void *payload, uint32_t payload_size,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);
int spdk_nvme_ctrlr_cmd_get_feature(struct spdk_nvme_ctrlr *ctrlr,
				    uint8_t feature, uint32_t cdw11,
				    void *payload, uint32_t payload_size,
				    spdk_nvme_cmd_cb cb_fn, void *cb_arg);
uint64_t spdk_nvme_ctrlr_get_flags(struct spdk_nvme_ctrlr *ctrlr); // Get supported flags of the controller.
const struct spdk_nvme_transport_id *spdk_nvme_ctrlr_get_transport_id(
	struct spdk_nvme_ctrlr *ctrlr);
```

命名空间相关：

```cpp
uint32_t spdk_nvme_ctrlr_get_num_ns(struct spdk_nvme_ctrlr *ctrlr); // 获取最大的NSID，但在NVM 1.2以后，NSID不一定连续，所以这个函数变得不可靠了
const struct spdk_nvme_ns_data *spdk_nvme_ns_get_data(struct spdk_nvme_ns *ns);
uint32_t spdk_nvme_ns_get_id(struct spdk_nvme_ns *ns);
struct spdk_nvme_ctrlr *spdk_nvme_ns_get_ctrlr(struct spdk_nvme_ns *ns);
bool spdk_nvme_ns_is_active(struct spdk_nvme_ns *ns);
uint32_t spdk_nvme_ns_get_max_io_xfer_size(struct spdk_nvme_ns *ns);
uint32_t spdk_nvme_ns_get_sector_size(struct spdk_nvme_ns *ns);
// returns the size of the data sector plus metadata.
uint32_t spdk_nvme_ns_get_extended_sector_size(struct spdk_nvme_ns *ns);
uint64_t spdk_nvme_ns_get_num_sectors(struct spdk_nvme_ns *ns);
uint64_t spdk_nvme_ns_get_size(struct spdk_nvme_ns *ns);
uint32_t spdk_nvme_ns_get_md_size(struct spdk_nvme_ns *ns);
bool spdk_nvme_ns_supports_extended_lba(struct spdk_nvme_ns *ns);
bool spdk_nvme_ns_supports_compare(struct spdk_nvme_ns *ns);
// 确定读取释放块时返回的值。 如果解除分配的块返回 0，
// 则解除分配命令可用作 write_zeroes 命令的更有效替代方法，尤其是对于大型请求。
// 实验室的SSD都不支持，连 Write Zeroes Command 也不支持
enum spdk_nvme_dealloc_logical_block_read_value spdk_nvme_ns_get_dealloc_logical_block_read_value(
	struct spdk_nvme_ns *ns);
// 获取给定命名空间的最佳 I/O 边界（以块为单位）。 读取和写入命令不应跨越最佳 I/O 边界以获得最佳性能。
// 估计是每次IO的大小不要超过的边界, 0表示未报告
uint32_t spdk_nvme_ns_get_optimal_io_boundary(struct spdk_nvme_ns *ns);
const struct spdk_uuid *spdk_nvme_ns_get_uuid(const struct spdk_nvme_ns *ns);
enum spdk_nvme_ns_flags {
	SPDK_NVME_NS_DEALLOCATE_SUPPORTED	= 1 << 0, /**< The deallocate command is supported */
	SPDK_NVME_NS_FLUSH_SUPPORTED		= 1 << 1, /**< The flush command is supported */
	SPDK_NVME_NS_RESERVATION_SUPPORTED	= 1 << 2, /**< The reservation command is supported */
	SPDK_NVME_NS_WRITE_ZEROES_SUPPORTED	= 1 << 3, /**< The write zeroes command is supported */
	SPDK_NVME_NS_DPS_PI_SUPPORTED		= 1 << 4, /**< The end-to-end data protection is supported */
	SPDK_NVME_NS_EXTENDED_LBA_SUPPORTED	= 1 << 5, /**< The extended lba format is supported,
							      metadata is transferred as a contiguous
							      part of the logical block that it is associated with */
	SPDK_NVME_NS_WRITE_UNCORRECTABLE_SUPPORTED	= 1 << 6, /**< The write uncorrectable command is supported */
	SPDK_NVME_NS_COMPARE_SUPPORTED		= 1 << 7, /**< The compare command is supported */
};
// 实验室的ssd比较、write zero、和flush都不支持
uint32_t spdk_nvme_ns_get_flags(struct spdk_nvme_ns *ns);
```

### 使用spdk_nvme_ctrlr_cmd_io_raw 进行 deallocate

```cpp
	struct spdk_nvme_dsm_range* raw_ranges = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (raw_ranges == NULL) {
		printf("ERROR: write buffer allocation failed\n");
		return;
	}

	for(int i = 0; i < RANGE_NUM; ++i) {
		raw_ranges[i].attributes.raw = 0;
		raw_ranges[i].starting_lba = START_LBA_FOR_RAW + i * LBA_LEVEL;
		raw_ranges[i].length = LBA_LEVEL;
	}
	sequence.is_completed = 0;

	struct spdk_nvme_cmd cmd = {0};
	cmd.opc = SPDK_NVME_OPC_DATASET_MANAGEMENT;
	cmd.nsid = spdk_nvme_ns_get_id(ns_entry->ns);
	cmd.cdw10_bits.dsm.nr = RANGE_NUM - 1;
	cmd.cdw11 = SPDK_NVME_DSM_ATTR_DEALLOCATE;
	rc = spdk_nvme_ctrlr_cmd_io_raw(ns_entry->ctrlr, ns_entry->qpair, &cmd, raw_ranges,
		LBA_NUM * sizeof(struct spdk_nvme_dsm_range), test_deallocate_common_complete, &sequence);
	if (rc) {
		printf("Error in nvme command completion, values may be inaccurate.\n");
	}

	while (!sequence.is_completed) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}
	printf("spdk_nvme_ctrlr_cmd_io_raw over\n");
	spdk_free(raw_ranges);
```

## TODO

1. 学习SPDK内存管理API：<lib/env_dpdk/env.c>
2. 线程模型：<include/spdk/env.h>
   1. 学习SPDK 的绑核（已解决）
   2. 注意，子线程根本没有主动退出，是随着主线程的退出而退出的。
3. NVMe标准
   1. 端到端的数据保护
   2. 元数据
   3. 扩展LBA
4. 接口再过一遍 <spdk/nvme.h>
