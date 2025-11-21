#ifndef OVERLAYFS_H
#define OVERLAYFS_H

#include <stdbool.h>

bool check_overlayfs_support(const char *test_dir);

int mount_overlayfs(const char *base_path, 
                    const char *module_path, 
                    const char *mount_point,
                    const char *workdir_root);

#endif