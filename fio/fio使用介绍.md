# fio 使用介绍

## fio build

github: <https://github.com/axboe/fio>

```shell
git clone git@github.com:axboe/fio.git
./configure
make
make install
```

请注意，GNU make 是必需的。在 BSD 上，它可以从 ports 目录中的 devel/gmake 获得；在 Solaris 上，它位于 SUNWgmake 软件包中。**在 GNU make 不是默认的平台上，键入gmake而不是make**。

## 文档

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

## 快速介绍

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

