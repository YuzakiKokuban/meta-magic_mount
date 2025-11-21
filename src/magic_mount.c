/* src/magic_mount.c */
#include "magic_mount.h"
#include "utils.h"
#include "overlayfs.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/xattr.h>
#include <sys/statfs.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <ctype.h>

// --- Configuration Structures ---

typedef enum {
    MOUNT_MODE_DEFAULT = 0,
    MOUNT_MODE_OVERLAY,
    MOUNT_MODE_BIND
} MountMode;

struct PathConfig {
    char *path;
    MountMode mode;
};

static struct PathConfig *g_path_configs = NULL;
static int g_path_config_count = 0;

// --- Global Variables ---

MountStats g_stats = { 0 };

const char *g_module_dir   = DEFAULT_MODULE_DIR;
const char *g_mount_source = DEFAULT_MOUNT_SOURCE;

char **g_failed_modules      = NULL;
int   g_failed_modules_count = 0;

char **g_extra_parts      = NULL;
int   g_extra_parts_count = 0;

int global_fd = 0;

// --- Helper Functions for Config ---

static char *trim_string(char *s) {
    if (!s) return s;
    char *end;
    while(isspace((unsigned char)*s)) s++;
    if(*s == 0) return s;
    end = s + strlen(s) - 1;
    while(end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

static void load_hybrid_config(const char *config_path) {
    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        LOGI("No hybrid config found at %s, using default behavior", config_path);
        return;
    }

    LOGI("Loading hybrid config from %s", config_path);
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim_string(line);
        if (!*p || *p == '#') continue;

        char *pipe = strchr(p, '|');
        if (!pipe) continue;
        *pipe = '\0';
        
        char *path = trim_string(p);
        char *mode_str = trim_string(pipe + 1);

        g_path_configs = realloc(g_path_configs, (size_t)(g_path_config_count + 1) * sizeof(struct PathConfig));
        if (g_path_configs) {
            g_path_configs[g_path_config_count].path = strdup(path);
            
            if (strcasecmp(mode_str, "overlay") == 0) {
                g_path_configs[g_path_config_count].mode = MOUNT_MODE_OVERLAY;
                LOGD("Config rule: %s -> OVERLAY", path);
            } else if (strcasecmp(mode_str, "bind") == 0) {
                g_path_configs[g_path_config_count].mode = MOUNT_MODE_BIND;
                LOGD("Config rule: %s -> BIND", path);
            } else {
                g_path_configs[g_path_config_count].mode = MOUNT_MODE_DEFAULT;
            }
            g_path_config_count++;
        }
    }
    fclose(fp);
}

static MountMode get_mount_mode(const char *path) {
    for (int i = 0; i < g_path_config_count; ++i) {
        if (strcmp(g_path_configs[i].path, path) == 0) {
            return g_path_configs[i].mode;
        }
    }
    return MOUNT_MODE_DEFAULT;
}

// --- Original Helpers ---

static void grab_fd(void) { syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, (void *)&global_fd); }

static void send_unmountable(const char *mntpoint)
{ 
    struct ksu_add_try_umount_cmd cmd = {0};

    if (!global_fd)
        return;

    cmd.arg = (uint64_t)mntpoint;
    cmd.flags = 0x2;
    cmd.mode = 1;
    
    ioctl(global_fd, KSU_IOCTL_ADD_TRY_UMOUNT, &cmd);
}

static void register_module_failure(const char *module_name)
{
    if (!module_name)
        return;

    for (int i = 0; i < g_failed_modules_count; ++i) {
        if (strcmp(g_failed_modules[i], module_name) == 0)
            return;
    }

    char **arr =
        realloc(g_failed_modules,
                (size_t)(g_failed_modules_count + 1) * sizeof(char *));
    if (!arr)
        return;

    g_failed_modules = arr;
    g_failed_modules[g_failed_modules_count] = strdup(module_name);
    if (!g_failed_modules[g_failed_modules_count])
        return;

    g_failed_modules_count++;
}

void add_extra_part_token(const char *start, size_t len)
{
    while (len > 0 && (start[0] == ' ' || start[0] == '\t')) {
        start++;
        len--;
    }
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
        len--;

    if (len == 0)
        return;

    char *name = malloc(len + 1);
    if (!name)
        return;

    memcpy(name, start, len);
    name[len] = '\0';

    char **arr = realloc(g_extra_parts,
                         (size_t)(g_extra_parts_count + 1) * sizeof(char *));
    if (!arr) {
        free(name);
        return;
    }

    g_extra_parts = arr;
    g_extra_parts[g_extra_parts_count++] = name;
}

typedef enum {
    NFT_REGULAR,
    NFT_DIRECTORY,
    NFT_SYMLINK,
    NFT_WHITEOUT
} NodeFileType;

typedef struct Node {
    char *name;
    NodeFileType type;
    struct Node **children;
    size_t child_count;
    char *module_path;
    char *module_name;
    bool replace;
    bool skip;
    bool done;
} Node;

static Node *node_new(const char *name, NodeFileType t)
{
    Node *n = calloc(1, sizeof(Node));
    if (!n)
        return NULL;

    n->name = strdup(name ? name : "");
    n->type = t;
    return n;
}

static void node_free(Node *n)
{
    if (!n)
        return;

    for (size_t i = 0; i < n->child_count; ++i)
        node_free(n->children[i]);

    free(n->children);
    free(n->name);
    free(n->module_path);
    free(n->module_name);
    free(n);
}

static NodeFileType node_type_from_stat(const struct stat *st)
{
    if (S_ISCHR(st->st_mode) && st->st_rdev == 0)
        return NFT_WHITEOUT;
    if (S_ISREG(st->st_mode))
        return NFT_REGULAR;
    if (S_ISDIR(st->st_mode))
        return NFT_DIRECTORY;
    if (S_ISLNK(st->st_mode))
        return NFT_SYMLINK;
    return NFT_WHITEOUT;
}

static bool dir_is_replace(const char *path)
{
    char buf[8];
    ssize_t len = lgetxattr(path, REPLACE_DIR_XATTR, buf, sizeof(buf) - 1);
    if (len <= 0)
        return false;

    buf[len] = '\0';
    return (strcmp(buf, "y") == 0);
}

static Node *node_new_module(const char *name, const char *path,
                             const char *module_name)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return NULL;

    if (!(S_ISCHR(st.st_mode) || S_ISREG(st.st_mode) ||
          S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)))
        return NULL;

    NodeFileType t = node_type_from_stat(&st);
    Node *n = node_new(name, t);
    if (!n)
        return NULL;

    n->module_path = strdup(path);
    if (module_name)
        n->module_name = strdup(module_name);
    n->replace = (t == NFT_DIRECTORY) && dir_is_replace(path);

    g_stats.nodes_total++;
    return n;
}

static int node_add_child(Node *parent, Node *child)
{
    Node **arr = realloc(parent->children,
                         (parent->child_count + 1) * sizeof(Node *));
    if (!arr) {
        errno = ENOMEM;
        return -1;
    }

    parent->children = arr;
    parent->children[parent->child_count++] = child;
    return 0;
}

static Node *node_find_child(Node *parent, const char *name)
{
    for (size_t i = 0; i < parent->child_count; ++i) {
        if (strcmp(parent->children[i]->name, name) == 0)
            return parent->children[i];
    }
    return NULL;
}

static Node *node_take_child(Node *parent, const char *name)
{
    for (size_t i = 0; i < parent->child_count; ++i) {
        if (strcmp(parent->children[i]->name, name) == 0) {
            Node *n = parent->children[i];
            memmove(&parent->children[i], &parent->children[i + 1],
                    (parent->child_count - i - 1) * sizeof(Node *));
            parent->child_count--;
            return n;
        }
    }
    return NULL;
}

static bool module_disabled(const char *mod_dir)
{
    char buf[PATH_MAX];

    path_join(mod_dir, DISABLE_FILE_NAME, buf, sizeof(buf));
    if (path_exists(buf))
        return true;

    path_join(mod_dir, REMOVE_FILE_NAME, buf, sizeof(buf));
    if (path_exists(buf))
        return true;

    path_join(mod_dir, SKIP_MOUNT_FILE_NAME, buf, sizeof(buf));
    if (path_exists(buf))
        return true;

    return false;
}

static int node_collect(Node *self, const char *dir, const char *module_name,
                        bool *has_any)
{
    DIR *d = opendir(dir);
    if (!d) {
        LOGE("opendir %s: %s", dir, strerror(errno));
        return -1;
    }

    struct dirent *de;
    bool any = false;
    char path[PATH_MAX];

    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        if (path_join(dir, de->d_name, path, sizeof(path)) != 0) {
            closedir(d);
            return -1;
        }

        Node *child = node_find_child(self, de->d_name);
        if (!child) {
            Node *n = node_new_module(de->d_name, path, module_name);
            if (n) {
                if (node_add_child(self, n) != 0) {
                    node_free(n);
                    closedir(d);
                    return -1;
                }
                child = n;
            }
        }

        if (child) {
            if (child->type == NFT_DIRECTORY) {
                bool sub = false;
                if (node_collect(child, path, module_name, &sub) != 0) {
                    closedir(d);
                    return -1;
                }
                if (sub || child->replace)
                    any = true;
            } else {
                any = true;
            }
        }
    }

    closedir(d);
    *has_any = any;
    return 0;
}

static Node *collect_root(void)
{
    const char *mdir = g_module_dir;
    Node *root   = node_new("",       NFT_DIRECTORY);
    Node *system = node_new("system", NFT_DIRECTORY);

    if (!root || !system) {
        node_free(root);
        node_free(system);
        return NULL;
    }

    DIR *d = opendir(mdir);
    if (!d) {
        LOGE("opendir %s: %s", mdir, strerror(errno));
        node_free(root);
        node_free(system);
        return NULL;
    }

    struct dirent *de;
    bool has_any = false;
    char mod[PATH_MAX];
    char mod_sys[PATH_MAX];

    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        if (path_join(mdir, de->d_name, mod, sizeof(mod)) != 0) {
            closedir(d);
            node_free(root);
            node_free(system);
            return NULL;
        }

        struct stat st;
        if (stat(mod, &st) < 0 || !S_ISDIR(st.st_mode))
            continue;

        if (module_disabled(mod))
            continue;

        if (path_join(mod, "system", mod_sys, sizeof(mod_sys)) != 0) {
            closedir(d);
            node_free(root);
            node_free(system);
            return NULL;
        }

        if (!path_is_dir(mod_sys))
            continue;

        LOGD("collecting module %s", de->d_name);
        g_stats.modules_total++;

        bool sub = false;
        if (node_collect(system, mod_sys, de->d_name, &sub) != 0) {
            closedir(d);
            node_free(root);
            node_free(system);
            return NULL;
        }
        if (sub)
            has_any = true;
    }

    closedir(d);

    if (!has_any) {
        node_free(root);
        node_free(system);
        return NULL;
    }

    g_stats.nodes_total += 2;

    struct {
        const char *name;
        bool need_symlink;
    } builtin_parts[] = {
        { "vendor",     true },
        { "system_ext", true },
        { "product",    true },
        { "odm",        false },
    };

    char rp[PATH_MAX];
    char sp[PATH_MAX];

    for (size_t i = 0; i < sizeof(builtin_parts) / sizeof(builtin_parts[0]);
         ++i) {
        if (path_join("/", builtin_parts[i].name, rp, sizeof(rp)) != 0 ||
            path_join("/system", builtin_parts[i].name, sp, sizeof(sp)) != 0) {
            node_free(root);
            node_free(system);
            return NULL;
        }

        if (!path_is_dir(rp))
            continue;

        if (builtin_parts[i].need_symlink && !path_is_symlink(sp))
            continue;

        Node *child = node_take_child(system, builtin_parts[i].name);
        if (child && node_add_child(root, child) != 0) {
            node_free(child);
            node_free(root);