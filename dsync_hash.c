#include "dsync_hash.h"
#include <stdlib.h>
#include <string.h>

// ========================================================
// uthash.h hashmap ADD methods
// ========================================================

void add_wddir(int wd, const char* path) {
        struct wddir* entry = (struct wddir*)malloc(sizeof *entry);
        entry->wd = wd;
        entry->path = strdup(path);             /* Copy path to the heap */
        HASH_ADD_INT(wddir_hm, wd, entry);      /* wd: name of key field */
}

void add_cookie_event(uint32_t cookie, int wd, uint32_t mask, const char* path) {
        struct cookie_event* entry = (struct cookie_event*)malloc(sizeof *entry);
        entry->cookie = cookie;                         /* unsigned cookie as signed key is actually ok, still unique keys */
        entry->wd = wd;
        entry->mask = mask;
        entry->path = strdup(path);                     /* Copy path to the heap */
        HASH_ADD_INT(cookie_event_hm, cookie, entry);   /* cookie: name of key field */
}

// ========================================================
// uthash.h hashmap FIND methods
// ========================================================

struct wddir* find_dir(int wd_key) {
        struct wddir* entry;

        HASH_FIND_INT(wddir_hm, &wd_key, entry);  /* entry: desired output pointer */
        return entry;
}

struct cookie_event* find_event(uint32_t cookie) {
        struct cookie_event* event;

        HASH_FIND_INT(cookie_event_hm, &cookie, event);
        return entry;
}

// ========================================================
// uthash.h hashmap DELETE methods
// ========================================================

void delete_wddir(struct wddir* entry, int ininst_fd) {
        HASH_DEL(wddir_hm, entry);              /* deletes entry from hashmap */
        inotify_rm_watch(ininst_fd, entry->wd); /* removes watch from inotify instance */

        free((void*)entry->path);               /* frees the heap allocated path */
        free(entry);
}

void delete_cookie_event(struct cookie_event* entry) {
        HASH_DEL(cookie_event_hm, entry);

        free((void*)entry->path);
        free(entry);
}

void delete_all_wddir(int ininst_fd) {
        struct wddir *current_entry, *tmp;
        HASH_ITER(hh, wddir_hm, current_entry, tmp) {
                delete_wddir(current_entry, ininst_fd);         /* delete; entry advances to next */
                //HASH_DEL(wddir_hm, current_entry);
                //free(current_entry);        
        }
}

void delete_all_cookie_event() {
        struct cookie_event *current_entry, *tmp;
        HASH_ITER(hh, cookie_event_hm, current_entry, tmp) {
                delete_cookie_event(current_entry);
        {
}

