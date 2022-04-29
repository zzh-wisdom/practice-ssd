.PHONY: all clean

SOURCE = $(wildcard *.c)
TARGETS = $(patsubst %.c,%,$(SOURCE))

SPDK_DIR = /home/zzh/spdk
PKG_CONFIG_CMD = PKG_CONFIG_PATH=$(SPDK_DIR)/build/lib/pkgconfig pkg-config --libs
SPDK_NVME_LIB = $(shell $(PKG_CONFIG_CMD) spdk_nvme)
SPDK_ENV_DPDK_LIB = $(shell $(PKG_CONFIG_CMD) spdk_env_dpdk)

CC = gcc
CFLAGS += -I$(SPDK_DIR)/include -g
# LDFLAGS = -lpthread -Wl,--whole-archive $(SPDK_NVME_LIB) $(SPDK_ENV_DPDK_LIB) -Wl,--no-whole-archive $(SPDK_SYS_LIB)
LDFLAGS = -Wl,--whole-archive -L/home/zzh/spdk/build/lib \
	-lspdk_nvme -lspdk_sock -lspdk_sock_posix -lspdk_trace -lspdk_rpc \
	-lspdk_jsonrpc -lspdk_json -lspdk_util -lspdk_log -L/home/zzh/spdk/build/lib \
	-L/home/zzh/spdk/dpdk/build/lib -lspdk_env_dpdk -lrte_eal \
	-lrte_mempool -lrte_ring -lrte_mbuf -lrte_bus_pci -lrte_pci -lrte_mempool_ring \
	-lrte_telemetry -lrte_kvargs -lrte_power -lrte_ethdev -lrte_net -lrte_vhost \
	-lrte_cryptodev -lrte_hash -lrte_rcu -Wl,--no-whole-archive -L/home/zzh/spdk/isa-l/.libs \
	-lisal -lrt -luuid -lcrypto -lm -laio -lnuma -ldl -lspdk_vmd -lpthread
BUILD_DIR := ./build

all: ${BUILD_DIR} $(TARGETS)

%:%.c
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/$@ $< $(LDFLAGS)

# 创建build目录
${BUILD_DIR}:
	$(shell if [ ! -e $(BUILD_DIR) ];then mkdir -p $(BUILD_DIR); fi)

clean:
	@echo $(LDFLAGS)
	rm -rf $(BUILD_DIR) *.o

PKG_CONFIG_PATH = $(SPDK_DIR)/build/lib/pkgconfig
SPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_nvme)
DPDK_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs spdk_env_dpdk)
SYS_LIB := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH)" pkg-config --libs --static spdk_syslibs)

app:
	$(CC) -o hello_world hello_world.c -pthread -Wl,--whole-archive $(SPDK_LIB) $(DPDK_LIB) -Wl,--no-whole-archive $(SYS_LIB)
