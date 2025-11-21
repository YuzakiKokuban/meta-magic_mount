#include "overlayfs.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <libgen.h>
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

static int prepare_workdir(const char *module_path, char *workdir_out, size_t size) {
    
    char temp_path[PATH_MAX];
    if ((size_t)snprintf(temp_path, sizeof(temp_path), "%s", module_path) >= sizeof(temp_path)) {
        return -1;
    }
    
    char *path_dup = strdup(temp_path);
    if (!path_dup) return -1;

    char *dname = dirname(path_dup);
    
    char *path_dup2 = strdup(temp_path);
    if (!path_dup2) { free(path_dup); return -1; }
    char *bname = basename(path_dup2); 
    
    snprintf(workdir_out, size, "%s/.work_%s", dname, bname);
    
    free(path_dup);
    free(path_dup2);
    
    if (mkdir_p(workdir_out) != 0) {
        return -1;
    }
    
    struct stat st;
    if (stat(module_path, &st) == 0) {
        chmod(workdir_out, st.st_mode);
        chown(workdir_out, st.st_uid, st.st_gid);
        
        char *con = NULL;
        if (get_selinux(module_path, &con) == 0 && con) {
            set_selinux(workdir_out, con);
            free(con);
        }
    }
    
    return 0;
}

int mount_overlayfs(const char *base_path, 
                    const char *module_path, 
                    const char *mount_point,
                    const char *unused_root) {
    char workdir[PATH_MAX];
    
    if (prepare_workdir(module_path, workdir, sizeof(workdir)) != 0) {
        LOGE("OverlayFS: Failed to prepare workdir for %s", module_path);
        return -1;
    }

    char opts[4096];
    int len = snprintf(opts, sizeof(opts), "lowerdir=%s,upperdir=%s,workdir=%s", 
             base_path, module_path, workdir);
             
    if (len >= (int)sizeof(opts)) {
        LOGE("OverlayFS: Mount options too long");
        return -1;
    }

    if (mount("overlay", mount_point, "overlay", 0, opts) != 0) {
        LOGE("OverlayFS: mount failed: %s -> %s (%s)", opts, mount_point, strerror(errno));
        rmdir(workdir);
        return -1;
    }

    LOGI("OverlayFS mounted: %s", mount_point);
    return 0;
}
