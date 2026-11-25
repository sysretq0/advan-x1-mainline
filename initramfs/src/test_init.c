// SPDX-License-Identifier: GPL-2.0-only
/* Host unit tests for ADVAN X1 static C /init pure helpers. */

#define INIT_UNIT_TEST 1
#include "init.c"

#include <assert.h>

static void test_cmdline_parser(void)
{
	struct boot_cfg cfg;

	parse_cmdline_str(
		"earlycon=uart8250,mmio32,0x11002000 console=ttyGS0,115200 "
		"root=\"PARTLABEL=My Root\" rootfstype=ext4 rootflags=noatime,discard "
		"rootdelay=8 rootwait rw init=/sbin/init real_init=/lib/systemd/systemd "
		"noswitch rd.nomodules rd.break=mount",
		&cfg);
	assert(strcmp(cfg.root, "PARTLABEL=My Root") == 0);
	assert(strcmp(cfg.rootfstype, "ext4") == 0);
	assert(strcmp(cfg.rootflags, "noatime,discard") == 0);
	assert(cfg.rootwait == true);
	assert(cfg.rootdelay == 30);
	assert(cfg.root_ro == false);
	assert(strcmp(cfg.real_init, "/lib/systemd/systemd") == 0);
	assert(cfg.noswitch == true);
	assert(cfg.rd_nomodules == true);
	assert(strcmp(cfg.rd_break, "mount") == 0);

	/* Dracut default: ro unless rw is passed. */
	parse_cmdline_str("console=tty0 init=/sbin/openrc-init", &cfg);
	assert(cfg.root[0] == '\0');
	assert(cfg.rootdelay == 5);
	assert(cfg.root_ro == true);
	assert(strcmp(cfg.real_init, "/sbin/openrc-init") == 0);
	assert(cfg.noswitch == false);
}

static void test_ext4_uuid_and_gpt_sector_detect(void)
{
	char tmpl[] = "/tmp/advan-init-test-XXXXXX";
	int fd = mkstemp(tmpl);
	assert(fd >= 0);
	unlink(tmpl);

	assert(ftruncate(fd, 8192) == 0);
	assert(pwrite(fd, "EFI PART", 8, 4096) == 8);
	assert(detect_gpt_sector_size_fd(fd, 4096) == 4096);

	uint8_t zeros[8] = { 0 };
	assert(pwrite(fd, zeros, 8, 4096) == 8);
	assert(pwrite(fd, "EFI PART", 8, 512) == 8);
	assert(detect_gpt_sector_size_fd(fd, 4096) == 512);

	assert(pwrite(fd, zeros, 8, 512) == 8);
	assert(detect_gpt_sector_size_fd(fd, 4096) == 0);

	uint8_t sb[EXT4_SUPER_SIZE];
	memset(sb, 0, sizeof(sb));
	sb[EXT4_MAGIC_OFF] = 0x53;
	sb[EXT4_MAGIC_OFF + 1] = 0xEF;
	const uint8_t uuid_bytes[16] = {
		0xe8, 0xf5, 0x98, 0x95, 0x03, 0xe3, 0x4f, 0x1c,
		0xa2, 0x5a, 0x2d, 0x7a, 0x8c, 0xd3, 0xb0, 0x50
	};
	memcpy(sb + EXT4_UUID_OFF, uuid_bytes, 16);
	assert(pwrite(fd, sb, sizeof(sb), EXT4_SUPER_OFFSET) == (ssize_t)sizeof(sb));

	char uuid_str[37] = { 0 };
	assert(read_ext4_uuid_fd(fd, uuid_str) == true);
	assert(strcmp(uuid_str, "e8f59895-03e3-4f1c-a25a-2d7a8cd3b050") == 0);

	sb[EXT4_MAGIC_OFF] = 0x00;
	assert(pwrite(fd, sb, sizeof(sb), EXT4_SUPER_OFFSET) == (ssize_t)sizeof(sb));
	assert(read_ext4_uuid_fd(fd, uuid_str) == false);

	close(fd);
}

static void test_mod_normalize_and_uevent(void)
{
	char norm[64];

	normalize_mod_name("kernel/sound/soc/mediatek/mt8186/snd-soc-mt8186-afe.ko",
			   norm, sizeof(norm));
	assert(strcmp(norm, "snd_soc_mt8186_afe") == 0);

	normalize_mod_name("chipone-tddi.ko.gz", norm, sizeof(norm));
	assert(strcmp(norm, "chipone_tddi") == 0);

	struct part_info pi;
	const char *uev =
		"MAJOR=259\n"
		"MINOR=72\n"
		"DEVNAME=loop0p2\n"
		"DEVTYPE=partition\n"
		"PARTN=2\n"
		"PARTNAME=rootfs\n"
		"PARTUUID=1cd1f6a4-689e-4ce4-922c-749d9691cd48\n";
	assert(parse_uevent_buf(uev, &pi) == true);
	assert(pi.major == 259 && pi.minor == 72 && pi.partn == 2);
	assert(strcmp(pi.devname, "loop0p2") == 0);
	assert(strcmp(pi.partname, "rootfs") == 0);
	assert(strcmp(pi.partuuid, "1cd1f6a4-689e-4ce4-922c-749d9691cd48") == 0);
}

int main(void)
{
	test_cmdline_parser();
	test_ext4_uuid_and_gpt_sector_detect();
	test_mod_normalize_and_uevent();
	printf("init unit tests: ALL PASSED\n");
	return 0;
}
