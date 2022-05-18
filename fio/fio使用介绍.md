# fio 使用介绍

> 基本上是翻译官方文档

## Get Start

### fio build

github: <https://github.com/axboe/fio>

```shell
git clone git@github.com:axboe/fio.git
./configure
make
make install
```

请注意，GNU make 是必需的。在 BSD 上，它可以从 ports 目录中的 devel/gmake 获得；在 Solaris 上，它位于 SUNWgmake 软件包中。**在 GNU make 不是默认的平台上，键入gmake而不是make**。

### 文档

仓库中有描述如果获取文档。

如果系统安装了fio，可以直接查看man文档： `man fio`。

Fio 使用Sphinx从reStructuredText文件生成文档。要构建 HTML 格式的文档，请运行`make -C doc html`并将浏览器定向到:file:`./doc/output/html/index.html`。构建手册页运行 `make -C doc man`，然后`man doc/output/man/fio.1`. 要查看支持的其他输出格式，请运行`make -C doc help`.

```shell
brew install sphinx-doc # mac 安装 sphinx-build
# brew cleanup sphinx
sudo apt install python3-sphinxcontrib.apidoc/focal # linux 安装 sphinx-build
make -C doc html
```

文档已经事先生成好，参考[这里](./html-doc/index.html)

### 使用总览

运行fio最简单的方式是，传递一个jobfile（配置文件）作为参数给fio。

```shell
$ fio [options] [jobfile] ...
```

fio会按照jobfile的描述进行工作。**注意的是**，你可以在命令参数中给出多个jobfile，fio会串行执行这些文件。在内部，这与在参数section描述中使用 :option:\`stonewall\` 参数相同。

```fio
[seq-read]
rw=read
stonewall

[rand-read]
rw=randread
stonewall

[seq-write]
rw=write
stonewall
```

如果jobfile只包含一个工作，你也可以直接在命令行给出参数。命令行参数和jobfile中的参数基本相同（除了一些控制全局的参数），例如对于jobfile中的参数`iodepth=2`，命令行的镜像命令参数是 `--iodepth=2`或者`--iodepth 2`

实际上，你也可以使用命令行参数给出多个工作。对于 fio 看到的每个`--name <name>`选项，它将使用该名称启动一个新作业。`--name <name>`条目后面的命令行条目将应用于该作业，直到没有更多条目或出现新的条目`--name <name>`。这类似于作业文件选项，其中每个选项都适用于当前作业，直到看到新的 [] 作业条目。

fio 不需要以 root 身份运行，除非作业部分中指定的文件或设备需要这样做。其他一些选项也可能会受到限制，例如内存锁定、I/O 调度程序切换和降低 nice 值。

如果jobfile被指定为-，作业文件将从标准输入中读取。

### fio 工作方式

让 fio 模拟所需的 I/O 工作负载的第一步是编写描述该特定设置的作业文件(job file)。作业文件可以包含任意数量的线程和/或文件 – 作业文件的典型内容是定义共享参数的global部分，以及描述所涉及的作业的一个或多个作业部分。运行时，fio 会解析此文件并按所述设置所有内容。如果我们从上到下分解作业，它包含以下基本参数：

1. I/O type
   定义颁发给文件的 I/O 模式。我们可能只是按顺序从此文件读取，也可能是随机写入。甚至按顺序或随机混合读取和写入。我们应该执行缓冲 I/O，还是直接/原始 I/O？
2. Block size
   我们发出 I/O 的块有多大？这可能是单个值，**也可以描述一系列块大小**。即单次io的数据大小
3. I/O size
   我们将要读取/写入多少数据。即**数据集大小**
4. I/O engine
   我们如何发出 I/O？我们可以是内存映射文件，我们可以使用常规读/写，我们可以使用splice，异步I / O，甚至SG（SCSI通用sg）。
5. I/O depth
   如果I/O engine是异步，我们要保持多大的排队深度？
6. Target file/device
   我们将工作负载分散了多少个文件。
7. Threads, processes and job synchronization
   我们应该将此工作负载分散多少个线程或进程。

以上是为工作负荷定义的基本参数，此外，还有大量参数可以修改此作业行为的其他方面。

## Command line options

- --debug=type
  启用各种 fio 操作的详细跟踪类型。可以是所有类型或用逗号分隔的单个类型（例如 `--debug=file,mem` 将启用文件和内存调试）。目前，其他可用的日志记录有：
  - process（Dump info related to processes.）
  - file（Dump info related to file actions.）
  - io（Dump info related to I/O queuing.）
  - random（Dump info related to random offset generation.）
  - parse（Dump info related to option matching and parsing.）
  - ? or help（Show available debug options.）
  - 更多（参考官方文档）
- --parse-only
  只解释参数选项，不进行任何IO
- merge-blktrace-only
- --output=filename
  Write output to file filename.
- --output-format=format






