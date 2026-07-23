#include "dsync_hash.h"
#include <sys/inotify.h>
#include <stdlib.h>
#include <string.h>

// Hashmap definitions
struct wddir* wddir_hm = NULL;
struct cookie_event* cookie_event_hm = NULL; 


// ========================================================
// uthash.h hashmap ADD methods
// ========================================================

void hm_add_wddir(int wd_key, const char* path) {
        struct wddir* entry = (struct wddir*)malloc(sizeof *entry);
        entry->wd = wd_key;
        entry->path = strdup(path);             /* Copy path to the heap */
        HASH_ADD_INT(wddir_hm, wd, entry);      /* wd: name of key field */
}

void hm_add_cookie_event(uint32_t cookie_key, int wd, uint32_t mask, const char* name) {
        struct cookie_event* entry = (struct cookie_event*)malloc(sizeof *entry);
        entry->cookie = cookie_key;
        entry->wd = wd;
	entry->mask = mask;
	entry->name = strdup(name);					/* Copy name to the heap */	
	
        HASH_ADD(hh, cookie_event_hm, cookie, sizeof(uint32_t), entry); /* cookie: name of key field */
}

// ========================================================
// uthash.h hashmap FIND methods
// ========================================================

struct wddir* hm_find_wddir(int wd_key) {
        struct wddir* entry;

        HASH_FIND_INT(wddir_hm, &wd_key, entry);  /* entry: desired output pointer */
        return entry;
}

struct cookie_event* hm_find_cookie_event(uint32_t cookie_key) {
        struct cookie_event* event;

        HASH_FIND(hh, cookie_event_hm, &cookie_key, sizeof(cookie_key), event);
        return event;
}

// ========================================================
// uthash.h hashmap DELETE methods
// ========================================================

void hm_delete_wddir(struct wddir* entry, int ininst_fd, int rmwatch) {
	// removes watch from inotify instance if specified
        if (rmwatch)
		inotify_rm_watch(ininst_fd, entry->wd); 
        
	HASH_DEL(wddir_hm, entry);              /* deletes entry from hashmap */
        free((void*)entry->path);               /* frees the heap allocated path */
        free(entry);
}

void hm_delete_cookie_event(struct cookie_event* entry) {
        HASH_DEL(cookie_event_hm, entry);

	free((void*)entry->name);
        free(entry);
}

void hm_delete_all_wddir(int ininst_fd, int rmwatch) {
        struct wddir *current_wddir, *tmp;
        HASH_ITER(hh, wddir_hm, current_wddir, tmp) {
                hm_delete_wddir(current_wddir, ininst_fd, rmwatch);         /* delete; entry advances to next */
                //HASH_DEL(wddir_hm, current_entry);
                //free(current_entry);        
        }
}

void hm_delete_all_cookie_event() {
        struct cookie_event *current_event, *tmp;
        HASH_ITER(hh, cookie_event_hm, current_event, tmp) {
                hm_delete_cookie_event(current_event);
        }
}

void hm_delete_tree_wddir(const char* root_path, int ininst_fd) {
	struct wddir *current_wddir, *tmp;
	size_t root_path_len = strlen(root_path);

	HASH_ITER(hh, wddir_hm, current_wddir, tmp) {
		// Checks if the current dir path is a subpath of root dir (By checking root_path_len amt of bytes)
		// Checks for an exact match ('\0') or strictly a subdirectory match ('/')
		if (strncmp(current_wddir->path, root_path, root_path_len) == 0 &&
		   (current_wddir->path[root_path_len] == '\0' || current_wddir->path[root_path_len] == '/'))
			
			// 1 indicates inotify_rm_watch() will be called
			hm_delete_wddir(current_wddir, ininst_fd, 1);		
	}
}

