/* adopt.c: Flow was called ARK Browser up to 0.3.1 and lived in
   PSP/GAME/ARKBrowser. When the Plugin Manager updates it to Flow, it removes
   the old app's files but not what the user kept there (downloads, cookies),
   and a folder without an EBOOT.PBP shows in the Game column as corrupted
   data. Flow moves those files into its own folder and removes the old one.
   Nothing is touched while the old app is still installed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adopt.h"
#include "fs.h"
#include "util.h"

#define ADOPT_PATH 256
#define ADOPT_FILES 256 /* per start; any further ones move on the next */
#define ADOPT_SPARE (1024 * 1024) /* free space a copy leaves on the device */

typedef struct {
    char (*names)[104];
    int count;
} name_list;

static int collect(void *ud, const char *name, int is_dir)
{
    name_list *list = ud;
    /* collected first: a folder changing while it's read can skip entries */
    if (!is_dir && strlen(name) < sizeof(list->names[0]) && list->count < ADOPT_FILES)
        strcpy(list->names[list->count++], name);
    return 0;
}

/* dir + name, or -1 when the path doesn't fit */
static int join(char *out, const char *dir, const char *name)
{
    return snprintf(out, ADOPT_PATH, "%s%s", dir, name) < ADOPT_PATH ? 0 : -1;
}

/* Deletes a file. PPSSPP lists a lowercase 8.3 name in capitals, as the PSP
   does, but then can't delete the file by that name and still reports
   success, so a file that is still there is renamed in its folder first. */
static int remove_file(const char *path)
{
    char renamed[ADOPT_PATH];
    if (fs_remove(path) == 0 && !fs_exists(path)) return 0;
    if (join(renamed, path, ".old") < 0 || fs_rename(path, renamed) < 0) return -1;
    if (fs_remove(renamed) == 0) return 0;
    fs_rename(renamed, path);
    return -1;
}

/* The PSP renames a file only within its folder (a folder in the new name
   is ignored), so a file moves by being copied and then deleted. */
static int move_file(const char *from, const char *to)
{
    int64_t size = fs_size(from), space = fs_free_bytes(to);
    /* a copy that can't fit would fail again at every start */
    if (size < 0 || (space >= 0 && size > space - ADOPT_SPARE)) return -1;
    if (fs_copy(from, to) < 0 || fs_size(to) != size) {
        fs_remove(to);
        return -1;
    }
    if (remove_file(from) == 0) return 0;
    fs_remove(to); /* one copy is enough: the original stays */
    return -1;
}

/* "name", then "name.1" to "name.99" as downloads are named, or -1 */
static int free_name(const char *dir, const char *name, char *out, int size)
{
    for (int index = 0; index < 100; index++) {
        int n = index ? snprintf(out, size, "%s%s.%d", dir, name, index) : snprintf(out, size, "%s%s", dir, name);
        if (n < 0 || n >= size) return -1;
        if (!fs_exists(out)) return 0;
    }
    return -1;
}

static void move_downloads(const char *old, const char *app_dir, adopt_progress progress, void *ud, adopt_result *out)
{
    char from_dir[ADOPT_PATH], to_dir[ADOPT_PATH], from[ADOPT_PATH], to[ADOPT_PATH];
    if (join(from_dir, old, "downloads/") < 0 || join(to_dir, app_dir, "downloads/") < 0 || !fs_is_dir(from_dir)) return;
    name_list list = { malloc(ADOPT_FILES * sizeof(*list.names)), 0 };
    if (!list.names) return;
    if (fs_list(from_dir, collect, &list) == 0 && list.count) {
        int ready = fs_mkdirs(to_dir, NULL, NULL) == 0;
        for (int i = 0; i < list.count; i++) {
            if (progress) progress(ud, i, list.count);
            if (ready && join(from, from_dir, list.names[i]) == 0 &&
                    free_name(to_dir, list.names[i], to, sizeof(to)) == 0 && move_file(from, to) == 0)
                out->downloads++;
            else out->failed++;
        }
    }
    free(list.names);
    if (fs_dir_empty(from_dir)) fs_rmdir(from_dir);
}

int adopt_old_folder(const char *app_dir, const char *old_name, adopt_progress progress, void *ud, adopt_result *out)
{
    memset(out, 0, sizeof(*out));
    size_t len = strlen(app_dir);
    if (len < 2 || app_dir[len - 1] != '/') return -1;
    size_t parent = len - 1;
    while (parent > 0 && app_dir[parent - 1] != '/') parent--;
    char old[ADOPT_PATH], path[ADOPT_PATH], dest[ADOPT_PATH];
    if (!parent || snprintf(old, sizeof(old), "%.*s%s/", (int)parent, app_dir, old_name) >= (int)sizeof(old)) return -1;
    if (!pm_strcasecmp(old, app_dir) || !fs_is_dir(old)) return -1;
    if (join(path, old, "EBOOT.PBP") < 0 || fs_exists(path)) return -1;

    /* cookies Flow already has are newer: the old file then stays */
    if (join(path, old, "cookies.txt") == 0 && join(dest, app_dir, "cookies.txt") == 0 &&
            fs_exists(path) && !fs_exists(dest) && move_file(path, dest) == 0)
        out->cookies = 1;

    /* the old app's own files, in case its update left any, and its page cache */
    static const char *const leftovers[] = { "cacert.pem", "README.md", "COPYING", "QuickJS-LICENSE", ".cache/page.tmp" };
    for (size_t i = 0; i < sizeof(leftovers) / sizeof(leftovers[0]); i++)
        if (join(path, old, leftovers[i]) == 0 && fs_exists(path)) fs_remove(path);
    if (join(path, old, ".cache/") == 0 && fs_is_dir(path) && fs_dir_empty(path)) fs_rmdir(path);

    move_downloads(old, app_dir, progress, ud, out);
    out->removed = fs_dir_empty(old) && fs_rmdir(old) == 0;
    return 0;
}
