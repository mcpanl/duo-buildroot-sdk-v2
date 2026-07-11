#include <common.h>
#include <command.h>
#include <env.h>
#include <mmc.h>
#include <blk.h>
#include <cvipart.h>
#include <cvi_boot_mode_emmc.h>
#include <cvi_update.h>

static int slot_is_b(const char *slot)
{
	if (!slot || !slot[0])
		return 0;

	return slot[0] == 'b' || slot[0] == 'B' || slot[0] == '1';
}

static void emmc_ab_init_env(void)
{
	if (!env_get("boot_slot"))
		env_set("boot_slot", "a");
	if (!env_get("slot_a_priority"))
		env_set("slot_a_priority", "15");
	if (!env_get("slot_b_priority"))
		env_set("slot_b_priority", "14");
	if (!env_get("slot_a_tries"))
		env_set("slot_a_tries", "7");
	if (!env_get("slot_b_tries"))
		env_set("slot_b_tries", "7");
	if (!env_get("slot_a_successful"))
		env_set("slot_a_successful", "1");
	if (!env_get("slot_b_successful"))
		env_set("slot_b_successful", "1");
}

static boot_mode_t emmc_get_boot_mode(void)
{
	emmc_ab_init_env();

	if (slot_is_b(env_get("boot_slot")))
		return BOOT_MODE_B;

	return BOOT_MODE_A;
}

static int do_loadboot_emmc(struct cmd_tbl *cmdtp, int flag, int argc,
			    char *const argv[])
{
	boot_mode_t boot_mode;
	const char *part_offset = NULL;
	const char *part_size = NULL;
	const char *rootargs = NULL;
	struct mmc *mmc;
	struct blk_desc *bd;
	unsigned long offset;
	unsigned long size;
	unsigned long blkcnt;
	ulong n;

	boot_mode = emmc_get_boot_mode();

	switch (boot_mode) {
	case BOOT_MODE_A:
		part_offset = env_get("BOOT_PART_OFFSET");
		part_size = env_get("BOOT_PART_SIZE");
#ifdef ROOTARGSA
		rootargs = ROOTARGSA;
#else
		rootargs = env_get("root");
#endif
		printf("[%s] INFO: boot slot A\n", __func__);
		break;
	case BOOT_MODE_B:
		part_offset = env_get("BOOT_B_PART_OFFSET");
		part_size = env_get("BOOT_B_PART_SIZE");
#ifdef ROOTARGSB
		rootargs = ROOTARGSB;
#else
		rootargs = env_get("root");
#endif
		printf("[%s] INFO: boot slot B\n", __func__);
		break;
	default:
		printf("[%s] ERROR: invalid boot mode\n", __func__);
		return CMD_RET_FAILURE;
	}

	if (!part_offset || !part_size || !rootargs) {
		printf("[%s] ERROR: missing boot partition metadata\n", __func__);
		return CMD_RET_FAILURE;
	}

	if (env_set("root", rootargs) != 0) {
		printf("[%s] WARNING: failed to set root args\n", __func__);
	}

	mmc = find_mmc_device(0);
	if (!mmc) {
		printf("[%s] ERROR: mmc device 0 not found\n", __func__);
		return CMD_RET_FAILURE;
	}

	if (mmc_init(mmc)) {
		printf("[%s] ERROR: mmc init failed\n", __func__);
		return CMD_RET_FAILURE;
	}

	offset = simple_strtoul(part_offset, NULL, 16);
	size = simple_strtoul(part_size, NULL, 16);
	blkcnt = size;

	bd = mmc_get_blk_desc(mmc);
	if (!bd) {
		printf("[%s] ERROR: mmc block descriptor missing\n", __func__);
		return CMD_RET_FAILURE;
	}

	n = blk_dread(bd, offset, blkcnt, (void *)CVIMMAP_UIMAG_ADDR);
	if (n != blkcnt) {
		printf("[%s] ERROR: mmc read failed (%lu/%lu)\n", __func__, n,
		       blkcnt);
		return CMD_RET_FAILURE;
	}

	printf("load 0x%lx bytes from mmc offset 0x%lx to 0x%lx success\n",
	       size, offset, (unsigned long)CVIMMAP_UIMAG_ADDR);

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(loadboot_emmc, 1, 0, do_loadboot_emmc,
	   "loadboot_emmc - load kernel from active A/B boot partition",
	   "loadboot_emmc - load kernel from active A/B boot partition\n");
