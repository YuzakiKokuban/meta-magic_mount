#ifndef MAGIC_MOUNT_H
#define MAGIC_MOUNT_H

#include <stdio.h>
#include <stdint.h>

#define DISABLE_FILE_NAME      "disable"
#define REMOVE_FILE_NAME       "remove"
#define SKIP_MOUNT_FILE_NAME   "skip_mount"

#define REPLACE_DIR_XATTR "trusted.overlay.opaque"

#define DEFAULT_MOUNT_SOURCE  "KSU"
#define DEFAULT_MODULE_DIR    "/data/adb/modules"

typedef struct {
    int modules_total;
    int nodes_total;
    int nodes_mounted;
    int nodes_skipped;
    int nodes_whiteout;
    int nodes_fail;
} MountStats;

extern MountStats g_stats;

extern const char *g_module_dir;
extern const char *g_mount_source;

extern char **g_failed_modules;
extern int   g_failed_modules_count;

extern char **g_extra_parts;
extern int   g_extra_parts_count;

int magic_mount(const char *tmp_root);

void add_extra_part_token(const char *start, size_t len);

// kernelsu api, atleast ones we care about
#define KSU_INSTALL_MAGIC1 0xDEADBEEF
#define KSU_INSTALL_MAGIC2 0xCAFEBABE

struct ksu_add_try_umount_cmd {
    uint64_t arg; // char ptr, this is the mountpoint
    uint32_t flags; // this is the flag we use for it
    uint8_t mode; // denotes what to do with it 0:wipe_list 1:add_to_list 2:delete_entry
};

#define KSU_IOCTL_ADD_TRY_UMOUNT _IOC(_IOC_WRITE, 'K', 18, 0)

// susfs
#define SUSFS_MAX_LEN_PATHNAME 256
#define KERNEL_SU_OPTION 0xDEADBEEF
#define CMD_SUSFS_ADD_TRY_UMOUNT 0x55580
struct st_susfs_try_umount {
	char                    target_pathname[SUSFS_MAX_LEN_PATHNAME];
	int                     mnt_mode;
};

#endif /* MAGIC_MOUNT_H */
