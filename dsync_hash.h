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
        uint64_t timestamp;     // value #1
        int wd;                 // value #2
        uint32_t mask;          // value #3
        const char* path;       // value #4
        UT_hash_handle hh;      // Makes structure hashable via uthash
};

// Global wddir hashmap
struct wddir* wddir_hm = NULL;
        
// Global cookie event hashmap  
struct cookie_event* cookie_event_hm = NULL;



// ========================================================
// uthash.h hashmap ADD methods
// ========================================================

// Key: watch desriptor 
// Value(s): file path
void add_wddir(int wd, const char* path);

// Key: inotify_event cookie
// Value(s): watch descriptor, event bitmask, file path
void add_cookie_event(uint32_t cookie, int wd, uint32_t mask, const char* path);



// ========================================================
// uthash.h hashmap FIND methods
// ========================================================

// Indexes wddir hashmap
struct wddir* find_dir(int wd_key);

// Indexes cookie_event hashmap
struct cookie_event* find_event(uint32_t cookie);



// ========================================================
// uthash.h hashmap DELETE methods
// ========================================================

// Deletes wddir hashmap entry
// Frees watch descriptor from inotify instance via ininst_fd
void delete_wddir(struct wddir* entry, int ininst_fd);

// Deletes cookie_event hashmap entry
void delete_cookie_event(struct cookie_event* entry);
       
// Deletes ALL wddir hashmap entries 
// Frees ALL watch descriptors from inotify instance via ininst_fd
void delete_all_wddir(int ininst_fd);
       
// Deletes ALL cookie_event hashmap entries 
void delete_all_cookie_event();

#endif
