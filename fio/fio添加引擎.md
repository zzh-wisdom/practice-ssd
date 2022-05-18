# fio 添加引擎

fio的代码解耦合做的比较好，通常，只需要将自己代码库生成对应的共享库`*.so`，然后fio代码仓库的`engines`目录下创建单独的.c文件对接自己的引擎接口，最后修改相应位置的配置/选项让fio发现你的引擎即可。

可见，这里有两个关键的部分：

1. `engines`目录下实现自己io引擎的接口
2. 对接接口，让fio发现引擎

以添加引擎名`dhms`为例，在实践中，可以参考其中已实现引擎的实现方式，如libaio，在相应的位置添加/修改即可。

## 实现 ioengine 接口

重中之重，实现并注册引擎。重点是实现结构体ioengine_ops：

```cpp
struct ioengine_ops {
	struct flist_head list;
	const char *name;
	int version;
	int flags;
	void *dlhandle;
	int (*setup)(struct thread_data *);
	int (*init)(struct thread_data *);
	int (*post_init)(struct thread_data *);
	int (*prep)(struct thread_data *, struct io_u *);
	enum fio_q_status (*queue)(struct thread_data *, struct io_u *);
	int (*commit)(struct thread_data *);
	int (*getevents)(struct thread_data *, unsigned int, unsigned int, const struct timespec *);
	struct io_u *(*event)(struct thread_data *, int);
	char *(*errdetails)(struct io_u *);
	int (*cancel)(struct thread_data *, struct io_u *);
	void (*cleanup)(struct thread_data *);
	int (*open_file)(struct thread_data *, struct fio_file *);
	int (*close_file)(struct thread_data *, struct fio_file *);
	int (*invalidate)(struct thread_data *, struct fio_file *);
	int (*unlink_file)(struct thread_data *, struct fio_file *);
	int (*get_file_size)(struct thread_data *, struct fio_file *);
	int (*prepopulate_file)(struct thread_data *, struct fio_file *);
	void (*terminate)(struct thread_data *);
	int (*iomem_alloc)(struct thread_data *, size_t);
	void (*iomem_free)(struct thread_data *);
	int (*io_u_init)(struct thread_data *, struct io_u *);
	void (*io_u_free)(struct thread_data *, struct io_u *);
	int (*get_zoned_model)(struct thread_data *td,
			       struct fio_file *f, enum zbd_zoned_model *);
	int (*report_zones)(struct thread_data *, struct fio_file *,
			    uint64_t, struct zbd_zone *, unsigned int);
	int (*reset_wp)(struct thread_data *, struct fio_file *,
			uint64_t, uint64_t);
	int (*get_max_open_zones)(struct thread_data *, struct fio_file *,
				  unsigned int *);
	int option_struct_size;
	struct fio_option *options;
};
```

通常是初始化一个静态的ioengine_ops变量：

```cpp
static struct ioengine_ops ioengine = {
    .name = "dhms",
    .version = FIO_IOOPS_VERSION,
    .init = fio_dhms_init,
    .get_file_size = fio_dhms_get_file_size,
    .queue = fio_dhms_queue,
    .open_file = fio_dhms_open_file,
    .close_file = fio_dhms_close_file,
    .flags = FIO_DISKLESSIO | FIO_SYNCIO,
    .options = options,
    .option_struct_size = sizeof(struct dhms_options_values),
};
```

然后把它注册到fio中：

```cpp
// 其他地方的定义
// #define fio_init	__attribute__((constructor))
// #define fio_exit	__attribute__((destructor))

static void fio_init fio_dhms_register(void) {
  register_ioengine(&ioengine);
}
static void fio_exit fio_dhms_unregister(void) {
  unregister_ioengine(&ioengine);
}
```

其中变量ioengine就是我们实现的引擎。这两个函数实现引擎的注册和注销。注意到宏fio_init和fio_exit，这很关键，表示这两个函数分别程序初始化和退出时执行。

> 这样的代码风格，实现完全的解耦合，不需要修改fio原来的代码。
> 更多细节可以参考：<https://blog.51cto.com/xiamachao/2366984>


## 让fio发现引擎

注意fio发现引擎，实际上是通过名字`name`来发现的，fio会在注册的列表中逐一对比已经注册engine的name，知道找到匹配的，否则报错。所以上面在实现变量ioengine_ops时，注意保持`name`全局唯一，并保持一致。

### 修改 ./configure

修改配置文件，让配置脚本检测到ioengin `dhms`

合适的位置添加如下内容，具体位置可以参考其他引擎的实现方式。

```shell
##########################################
# dhms probe
if test "$dhms" != "yes" ; then
  dhms="no"
fi
cat > $TMPC << EOF
#include <stdio.h>
#include <dhms.h>
int main(int argc, char **argv)
{
  dhms_addr addr = 0UL;
  return 0;
}
EOF
if compile_prog "" "-ldhms" "dhms"; then
    dhms="yes"
fi
print_config "dhms" "$dhms"

# ...
# ...

if test "$dhms" = "yes"; then
  output_sym "CONFIG_DHMS"
fi
```

实际上，只有最后的三句有用，它会在特定的文件定义对应的C宏：`CONFIG_DHMS`。
从而在编译fio时，可以将引擎dhms编译进去。

### 修改 `./Makefile`

```makefile
ifdef CONFIG_DHMS
  dhms_SRCS = engines/dhms.c
  dhms_LIBS = -ldhms
  ENGINES += dhms
endif
```

稍微解释上述的作用。这里将引擎dhms添加到变量`ENGINES`。后续会有语句检索该变量，对每个引擎进行处理，如下所示。

```makefile
# 定义 engine_template函数，前者是动态构建，即makefile帮助我们生成*.so共享库，忽略。
# 我们采用的是：自己事先生成好对应的libdhms.so库，编译fio时直接链接即可
ifdef CONFIG_DYNAMIC_ENGINES
 DYNAMIC_ENGS := $(ENGINES)
define engine_template =
$(1)_OBJS := $$($(1)_SRCS:.c=.o)
$$($(1)_OBJS): CFLAGS := -fPIC $$($(1)_CFLAGS) $(CFLAGS)
engines/fio-$(1).so: $$($(1)_OBJS)
	$$(QUIET_LINK)$(CC) $(LDFLAGS) -shared -rdynamic -fPIC -Wl,-soname,fio-$(1).so.1 -o $$@ $$< $$($(1)_LIBS)
ENGS_OBJS += engines/fio-$(1).so
endef
else # !CONFIG_DYNAMIC_ENGINES
define engine_template =
SOURCE += $$($(1)_SRCS)
LIBS += $$($(1)_LIBS)
override CFLAGS += $$($(1)_CFLAGS)
endef
endif

$(foreach eng,$(ENGINES),$(eval $(call engine_template,$(eng))))
```

可以看到对于每个添加到`ENGINES`的引擎，都会调用函数`engine_template`进行处理。将对应的源文件和链接库分别添加到变量`SOURCE`和`LIBS`。在后续编译中会用到：

```makefile
fio: $(FIO_OBJS)
    $(QUIET_LINK)$(CC) $(LDFLAGS) -o $@ $(FIO_OBJS) $(LIBS) $(HDFSLIB)
```

## 参考

1. <https://blog.csdn.net/weixin_38428439/article/details/121642171>
2.
