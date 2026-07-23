#ifndef DSYNC_HASH_H
#define DSYNC_HASH_H

#include "uthash.h"
#include <stdint.h>

// Hashable structure
struct wddir {
        int wd;                 // key
        const char* path;       // value
        UT_hash_handle hh;      // Makes structure hashable via uthash
};      
        
// Hashable structure
struct cookie_event {
        uint32_t cookie;        // key
        int wd;                 // value #1
	uint32_t mask;
	const char* name;
        UT_hash_handle hh;      // Makes structure hashable via uthash
};

// Global wddir hashmap
extern struct wddir* wddir_hm;
        
// Global cookie event hashmap  
extern struct cookie_event* cookie_event_hm;



// ========================================================
// uthash.h hashmap ADD methods
// ========================================================

// Key: watch desriptor 
// Value(s): file path
void hm_add_wddir(int wd, const char* path);

// Key: inotify_event cookie
// Value(s): watch descriptor, event bitmask, file path
void hm_add_cookie_event(uint32_t cookie_key, int wd, uint32_t mask, const char* name);



// ========================================================
// uthash.h hashmap FIND methods
// ========================================================

// Indexes wddir hashmap
struct wddir* hm_find_wddir(int wd_key);

// Indexes cookie_event hashmap
struct cookie_event* hm_find_cookie_event(uint32_t cookie_key);



// ========================================================
// uthash.h hashmap DELETE methods
// ========================================================

// Deletes wddir hashmap entry
// Frees watch descriptor from inotify instance via ininst_fd
void hm_delete_wddir(struct wddir* entry, int ininst_fd, int rmwatch);

// Deletes cookie_event hashmap entry
void hm_delete_cookie_event(struct cookie_event* entry);
       
// Deletes ALL wddir hashmap entries 
// Frees ALL watch descriptors from inotify instance via ininst_fd
void hm_delete_all_wddir(int ininst_fd, int rmwatch);
       
// Deletes ALL cookie_event hashmap entries 
void hm_delete_all_cookie_event();

// Removes a directory and all of its subdirectories from the hashmap and inotify instance(s)
// Takes in null terminated path
void hm_delete_tree_wddir(const char* root_path, int ininst_fd);

#endif
