/*
 * kv_tgt.c  --  带 kv_emu bdev 的 nvmf target
 *
 * 和 SPDK 的 app/nvmf_tgt/nvmf_main.c 一样,只是多链了 bdev_kv_emu.c
 * (它靠 SPDK_BDEV_MODULE_REGISTER 的构造函数自注册,不需要在这里调用)。
 * 用法和 nvmf_tgt 相同:
 *
 *   sudo ./kv_tgt -m 0x3 &
 *   ./setup_kv_target.sh
 *
 * 链接按 nvmf 需要的事件子系统(iobuf accel keyring vmd sock bdev nvmf)
 * 列,pkg-config 会顺带把 SPDK 自带的 bdev 模块也带进来 —— 所以 malloc/null
 * 这些照样能建,可以和 KV bdev 挂在同一个 target 上做对照。
 */

#include "spdk/stdinc.h"
#include "spdk/event.h"
#include "spdk/log.h"

static void
kv_tgt_usage(void)
{
}

static int
kv_tgt_parse_arg(int ch, char *arg)
{
	return 0;
}

static void
kv_tgt_started(void *arg)
{
	SPDK_NOTICELOG("kv_tgt 已启动,下一步:tst/kv_target/setup_kv_target.sh\n");
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "kv_tgt";
	rc = spdk_app_parse_args(argc, argv, &opts, "", NULL,
				 kv_tgt_parse_arg, kv_tgt_usage);
	if (rc != SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}
	rc = spdk_app_start(&opts, kv_tgt_started, NULL);
	spdk_app_fini();
	return rc;
}
