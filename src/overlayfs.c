/* src/overlayfs.c */
#include "overlayfs.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h> 

bool check_overlayfs_support(const char *test_dir) {
    FILE *fp = fopen("/proc/filesystems", "r");
    if (!fp) return false;
    
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "overlay")) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

int mount_overlayfs(const char *base_path, 
                    const char *module_path, 
                    const char *mount_point,
                    const char *unused) {
    
    char opts[PATH_MAX * 2 + 64];
    
    int len = snprintf(opts, sizeof(opts), "lowerdir=%s:%s", module_path, base_path);
             
    if (len >= (int)sizeof(opts)) {
        LOGE("OverlayFS: Mount options too long");
        return -1;
    }

    if (mount("overlay", mount_point, "overlay", MS_RDONLY, opts) != 0) {
        LOGE("OverlayFS: mount failed: %s -> %s (%s)", opts, mount_point, strerror(errno));
        return -1;
    }

    LOGI("OverlayFS (RO-Stack) mounted: %s", mount_point);
    return 0;
}