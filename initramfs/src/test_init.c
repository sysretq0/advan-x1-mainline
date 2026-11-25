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
	assert(cfg.rd_shell == true);
	assert(strcmp(cfg.real_init, "/sbin/openrc-init") == 0);
	assert(cfg.noswitch == false);

	/* Dracut root= normalization, blacklist, rd.shell=0, rd.timeout, and -- args */
	parse_cmdline_str(
		"root=block:/dev/disk/by-label/Advan\\x20Root "
		"module_blacklist=panfrost,snd-soc-fs1599 rd.blacklist=uhid.ko "
		"rd.shell=0 rd.emergency=poweroff rd.timeout=12 rootwait "
		"-- systemd.unit=rescue.target --log-level=debug",
		&cfg);
	assert(strcmp(cfg.root, "LABEL=Advan Root") == 0);
	assert(is_mod_blacklisted(&cfg, "panfrost") == true);
	assert(is_mod_blacklisted(&cfg, "snd_soc_fs1599") == true);
	assert(is_mod_blacklisted(&cfg, "drivers/hid/uhid.ko") == true);
	assert(is_mod_blacklisted(&cfg, "chipone_icnl9922c") == false);
	assert(cfg.rd_shell == false);
	assert(strcmp(cfg.rd_emergency, "poweroff") == 0);
	assert(cfg.rootdelay == 12);
	assert(cfg.n_init_args == 2);
	assert(strcmp(cfg.init_args[0], "systemd.unit=rescue.target") == 0);
	assert(strcmp(cfg.init_args[1], "--log-level=debug") == 0);

	char norm_root[128];
	normalize_root_spec("/dev/disk/by-uuid/e8f59895-03e3-4f1c-a25a-2d7a8cd3b050",
			    norm_root, sizeof(norm_root));
	assert(strcmp(norm_root, "UUID=e8f59895-03e3-4f1c-a25a-2d7a8cd3b050") == 0);
	normalize_root_spec("/dev/disk/by-partlabel/rootfs", norm_root, sizeof(norm_root));
	assert(strcmp(norm_root, "PARTLABEL=rootfs") == 0);
	normalize_root_spec("block:/dev/mmcblk0p2", norm_root, sizeof(norm_root));
	assert(strcmp(norm_root, "/dev/mmcblk0p2") == 0);
}

static void test_mount_flags_split(void)
{
	unsigned long flags = 0;
	char data[128];

	parse_mount_flags("defaults,noatime,subvol=@,compress=zstd,nodiratime,rw",
			  true, &flags, data, sizeof(data));
	assert(flags == (MS_NOATIME | MS_NODIRATIME));
	assert(strcmp(data, "subvol=@,compress=zstd") == 0);

	parse_mount_flags("relatime,nosuid,discard,lazytime",
			  true, &flags, data, sizeof(data));
	assert(flags == (MS_RDONLY | MS_RELATIME | MS_NOSUID | MS_LAZYTIME));
	assert(strcmp(data, "discard") == 0);

	parse_mount_flags("", true, &flags, data, sizeof(data));
	assert(flags == MS_RDONLY);
	assert(data[0] == '\0');
}

static void zero_file(int fd, off_t sz)
{
	assert(ftruncate(fd, 0) == 0);
	assert(ftruncate(fd, sz) == 0);
}

static void test_multi_fs_probe_and_gpt_sector_detect(void)
{
	char tmpl[] = "/tmp/advan-init-test-XXXXXX";
	int fd = mkstemp(tmpl);
	assert(fd >= 0);
	unlink(tmpl);

	const uint8_t uuid_bytes[16] = {
		0xe8, 0xf5, 0x98, 0x95, 0x03, 0xe3, 0x4f, 0x1c,
		0xa2, 0x5a, 0x2d, 0x7a, 0x8c, 0xd3, 0xb0, 0x50
	};
	const char *uuid_expected = "e8f59895-03e3-4f1c-a25a-2d7a8cd3b050";
	struct fs_info fi;

	/* GPT sector size detection */
	zero_file(fd, 8192);
	assert(pwrite(fd, "EFI PART", 8, 4096) == 8);
	assert(detect_gpt_sector_size_fd(fd, 4096) == 4096);

	zero_file(fd, 8192);
	assert(pwrite(fd, "EFI PART", 8, 512) == 8);
	assert(detect_gpt_sector_size_fd(fd, 4096) == 512);

	zero_file(fd, 8192);
	assert(detect_gpt_sector_size_fd(fd, 4096) == 0);

	/* 1. ext4 (with stale Btrfs at 64 KiB -> ext4 MUST win) */
	zero_file(fd, 0x11000);
	uint8_t btrfs_sb[BTRFS_SUPER_READ_SZ];
	memset(btrfs_sb, 0, sizeof(btrfs_sb));
	memcpy(btrfs_sb + BTRFS_MAGIC_OFF, "_BHRfS_M", 8);
	memcpy(btrfs_sb + BTRFS_UUID_OFF, uuid_bytes, 16);
	memcpy(btrfs_sb + BTRFS_LABEL_OFF, "btrfs-root", 10);
	assert(pwrite(fd, btrfs_sb, sizeof(btrfs_sb), BTRFS_SUPER_OFFSET) ==
	       (ssize_t)sizeof(btrfs_sb));

	uint8_t sb[EXT4_SUPER_SIZE];
	memset(sb, 0, sizeof(sb));
	sb[EXT4_MAGIC_OFF] = 0x53;
	sb[EXT4_MAGIC_OFF + 1] = 0xEF;
	memcpy(sb + EXT4_UUID_OFF, uuid_bytes, 16);
	memcpy(sb + EXT4_LABEL_OFF, "debian-root", 11);
	assert(pwrite(fd, sb, sizeof(sb), EXT4_SUPER_OFFSET) == (ssize_t)sizeof(sb));

	assert(probe_fs_fd(fd, &fi) == true);
	assert(strcmp(fi.fstype, "ext4") == 0);
	assert(strcmp(fi.uuid, uuid_expected) == 0);
	assert(strcmp(fi.label, "debian-root") == 0);

	/* 2. Btrfs alone (after wiping 1 KiB ext4 magic) */
	sb[EXT4_MAGIC_OFF] = 0x00;
	assert(pwrite(fd, sb, sizeof(sb), EXT4_SUPER_OFFSET) == (ssize_t)sizeof(sb));
	assert(probe_fs_fd(fd, &fi) == true);
	assert(strcmp(fi.fstype, "btrfs") == 0);
	assert(strcmp(fi.uuid, uuid_expected) == 0);
	assert(strcmp(fi.label, "btrfs-root") == 0);

	/* 3. F2FS (UTF-16LE label) */
	zero_file(fd, 8192);
	memset(sb, 0, sizeof(sb));
	sb[0] = 0x10; sb[1] = 0x20; sb[2] = 0xF5; sb[3] = 0xF2;
	memcpy(sb + F2FS_UUID_OFF, uuid_bytes, 16);
	const char *f2fs_lbl = "f2fs-root";
	for (size_t i = 0; f2fs_lbl[i]; i++) {
		sb[F2FS_LABEL_OFF + i * 2] = (uint8_t)f2fs_lbl[i];
		sb[F2FS_LABEL_OFF + i * 2 + 1] = 0;
	}
	assert(pwrite(fd, sb, sizeof(sb), F2FS_SUPER_OFFSET) == (ssize_t)sizeof(sb));
	assert(probe_fs_fd(fd, &fi) == true);
	assert(strcmp(fi.fstype, "f2fs") == 0);
	assert(strcmp(fi.uuid, uuid_expected) == 0);
	assert(strcmp(fi.label, "f2fs-root") == 0);

	/* 4. EROFS */
	zero_file(fd, 8192);
	memset(sb, 0, sizeof(sb));
	sb[0] = 0xE2; sb[1] = 0xE1; sb[2] = 0xF5; sb[3] = 0xE0;
	memcpy(sb + EROFS_UUID_OFF, uuid_bytes, 16);
	memcpy(sb + EROFS_LABEL_OFF, "erofs-sys", 9);
	assert(pwrite(fd, sb, sizeof(sb), EROFS_SUPER_OFFSET) == (ssize_t)sizeof(sb));
	assert(probe_fs_fd(fd, &fi) == true);
	assert(strcmp(fi.fstype, "erofs") == 0);
	assert(strcmp(fi.uuid, uuid_expected) == 0);
	assert(strcmp(fi.label, "erofs-sys") == 0);

	/* 5. XFS */
	zero_file(fd, 8192);
	uint8_t sb0[256];
	memset(sb0, 0, sizeof(sb0));
	memcpy(sb0, "XFSB", 4);
	memcpy(sb0 + XFS_UUID_OFF, uuid_bytes, 16);
	memcpy(sb0 + XFS_LABEL_OFF, "xfs-root", 8);
	assert(pwrite(fd, sb0, sizeof(sb0), 0) == (ssize_t)sizeof(sb0));
	assert(probe_fs_fd(fd, &fi) == true);
	assert(strcmp(fi.fstype, "xfs") == 0);
	assert(strcmp(fi.uuid, uuid_expected) == 0);
	assert(strcmp(fi.label, "xfs-root") == 0);

	/* 6. SquashFS */
	zero_file(fd, 8192);
	memset(sb0, 0, sizeof(sb0));
	memcpy(sb0, "hsqs", 4);
	assert(pwrite(fd, sb0, sizeof(sb0), 0) == (ssize_t)sizeof(sb0));
	assert(probe_fs_fd(fd, &fi) == true);
	assert(strcmp(fi.fstype, "squashfs") == 0);
	assert(fi.uuid[0] == '\0' && fi.label[0] == '\0');

	/* 7. Empty/unrecognized returns false */
	zero_file(fd, 0x11000);
	assert(probe_fs_fd(fd, &fi) == false);

	close(fd);
}

static void test_mod_normalize_and_uevent(void)
{
	char norm[64];

	normalize_mod_name("kernel/sound/soc/mediatek/mt8186/snd-soc-mt8186-afe.ko",
			   norm, sizeof(norm));
	assert(strcmp(norm, "snd_soc_mt8186_afe") == 0);

	normalize_mod_name("chipone-icnl9922c.ko.gz", norm, sizeof(norm));
	assert(strcmp(norm, "chipone_icnl9922c") == 0);

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

static void rm_rf_tmp(const char *path)
{
	struct stat st;
	if (lstat(path, &st) < 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				char child[PATH_MAX];
				if (strcmp(de->d_name, ".") == 0 ||
				    strcmp(de->d_name, "..") == 0)
					continue;
				if (join_path2(child, sizeof(child), path, de->d_name))
					rm_rf_tmp(child);
			}
			closedir(d);
		}
		rmdir(path);
	} else {
		unlink(path);
	}
}

static void test_stage_initramfs_firmware(void)
{
	char root[] = "/tmp/advan-fw-test-XXXXXX";
	char src[PATH_MAX], src_sub[PATH_MAX], src_bin[PATH_MAX], src_lnk[PATH_MAX];
	char dst1[PATH_MAX], dst2[PATH_MAX], dst3[PATH_MAX], missing[PATH_MAX];
	char param1[PATH_MAX], param2[PATH_MAX], param3[PATH_MAX];
	char out_bin[PATH_MAX], out_lnk[PATH_MAX], link_buf[64], buf[256];
	const unsigned char payload[] = { 0x00, 0x99, 0x22, 0xc0, 0xff, 0x0a, 0x42 };
	unsigned char readback[sizeof(payload)];
	int fd;

	assert(mkdtemp(root) != NULL);
	assert(join_path2(src, sizeof(src), root, "src"));
	assert(join_path2(src_sub, sizeof(src_sub), src, "chipone"));
	assert(join_path2(src_bin, sizeof(src_bin), src_sub, "icnl9922c.bin"));
	assert(join_path2(src_lnk, sizeof(src_lnk), src_sub, "latest.bin"));
	assert(mkdir_p(src_sub, 0755) == 0);

	fd = open(src_bin, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, payload, sizeof(payload)) == (ssize_t)sizeof(payload));
	close(fd);
	assert(symlink("icnl9922c.bin", src_lnk) == 0);

	/* Case 1: empty sysfs param ("\n") -> stages tree and sets param to dst1 */
	assert(join_path2(dst1, sizeof(dst1), root, "run-fw-1"));
	assert(join_path2(param1, sizeof(param1), root, "param1"));
	fd = open(param1, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, "\n", 1) == 1);
	close(fd);

	assert(stage_initramfs_firmware_from(src, dst1, param1) == 2);
	assert(join_path3(out_bin, sizeof(out_bin), dst1, "chipone", "icnl9922c.bin"));
	assert(join_path3(out_lnk, sizeof(out_lnk), dst1, "chipone", "latest.bin"));
	fd = open(out_bin, O_RDONLY);
	assert(fd >= 0);
	assert(read(fd, readback, sizeof(readback)) == (ssize_t)sizeof(readback));
	close(fd);
	assert(memcmp(readback, payload, sizeof(payload)) == 0);

	ssize_t llen = readlink(out_lnk, link_buf, sizeof(link_buf) - 1);
	assert(llen > 0);
	link_buf[llen] = '\0';
	assert(strcmp(link_buf, "icnl9922c.bin") == 0);

	assert(read_file_str(param1, buf, sizeof(buf)) > 0);
	strip_newline(buf);
	assert(strcmp(buf, dst1) == 0);

	/* Case 2: pre-populated param (cmdline override) -> stages tree, keeps param */
	assert(join_path2(dst2, sizeof(dst2), root, "run-fw-2"));
	assert(join_path2(param2, sizeof(param2), root, "param2"));
	fd = open(param2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, "/custom/fw\n", 11) == 11);
	close(fd);

	assert(stage_initramfs_firmware_from(src, dst2, param2) == 2);
	assert(read_file_str(param2, buf, sizeof(buf)) > 0);
	strip_newline(buf);
	assert(strcmp(buf, "/custom/fw") == 0);

	/* Case 3: missing source dir -> returns 0, does not create dst or touch param */
	assert(join_path2(missing, sizeof(missing), root, "does-not-exist"));
	assert(join_path2(dst3, sizeof(dst3), root, "run-fw-3"));
	assert(join_path2(param3, sizeof(param3), root, "param3"));
	fd = open(param3, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, "\n", 1) == 1);
	close(fd);

	assert(stage_initramfs_firmware_from(missing, dst3, param3) == 0);
	assert(access(dst3, F_OK) < 0);
	assert(read_file_str(param3, buf, sizeof(buf)) >= 0);
	strip_newline(buf);
	assert(buf[0] == '\0');

	rm_rf_tmp(root);
}

int main(void)
{
	test_cmdline_parser();
	test_mount_flags_split();
	test_multi_fs_probe_and_gpt_sector_detect();
	test_mod_normalize_and_uevent();
	test_stage_initramfs_firmware();
	printf("init unit tests: ALL PASSED\n");
	return 0;
}
