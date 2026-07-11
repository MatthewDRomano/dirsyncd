#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>

#include "uthash.h"

#define WATCH_PATH "/home/matt/Projects"
#define MAX_PATH_LEN 1024	// 1KB

static uint32_t event_mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE |  IN_DELETE | IN_DELETE_SELF;

struct wddir {
	int wd;			// key
	const char* path;	
	UT_hash_handle hh;	// Makes structure hashable via uthash
};

struct wddir* hashmap = NULL;

// ========================================================
// uthash.h methods for hashmap handling
// ========================================================

void add_wddir(int wd, const char* path) {
    	struct wddir* entry;

    	entry = malloc(sizeof *entry);
    	entry->wd = wd;
    	strcpy(entry->path, path);
    	HASH_ADD_INT(hashmap, wd, entry);  /* wd: name of key field */
}

struct wddir* find_user(int wd_key) {
        struct wddir* entry;

	HASH_FIND_INT(hashmap, &wd_key, entry);  /* entry: desired output pointer */
	return entry;
}

void delete_wddir(struct wddir* entry, int ininst_fd) {
    	HASH_DEL(hashmap, entry);	/* entry: pointer to delete */
	inotify_rm_watch(ininst_fd, entry->wd);	/* removes watch from inotify instance */
	free(entry);            
}

void delete_all_wddir(int ininst_fd) {
 	struct wddir *current_entry, *tmp;
  	HASH_ITER(hh, hashmap, current_entry, tmp) {
    		delete_wddir(current_entry, ininst_fd);		/* delete; entry advances to next */
		//HASH_DEL(hashmap, current_entry);
		//free(current_entry);        
  	}
}

// ========================================================
// Reads linux system files to find watch descriptor max
// ========================================================

int sys_wdmax() {
	FILE* fp = fopen("/proc/sys/fs/inotify/max_user_watches", "r");
	if (!fp)
		return -1;

	char buf[32] = {0};
	if (!fgets(buf, 32, fp)) {
		fclose(fp);
		return -1;
	}

	char* end_ptr;
	long int max_wd = strtol(buf, &end_ptr, 10);	// Base 10
	// Invalid integer
	if (end_ptr == buf || (*end_ptr != '\n' &&  *end_ptr != '\0')) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return (uint32_t)max_wd;
}

int dir_scan_recursive(const char* base_path, int ininst_fd) {
	DIR* dir = opendir(base_path);
	
	// Unable to open directory
	if (!dir) 
		return -1;

	int wd = inotify_add_watch(ininst_fd, base_path, event_mask);
	if (wd < 0) {
		closedir(dir);
		return -1;
	}

	// Add to hashmap
        add_wddir(wd, base_path);

	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		// Ignore . and ..
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		
		// Construct entry path
		char entry_path[MAX_PATH_LEN];
                snprintf(entry_path, MAX_PATH_LEN, "%s/%s", base_path, entry->d_name);
		
		// Entry is a dir --> Add to kernel's inotify watch list
		struct stat sb;
		if (stat(entry_path, &sb) && S_ISDIR(sb.st_mode)) {
			dir_scan_recursive(entry_path, ininst_fd);	
		}
	}

	// Directory layer is fully traversed
	// Close associated DIR*
	closedir(dir);
	
	return 0;
}


int full_read(int fd, char* buf, int n) {
	size_t bytes_read = 0;

	while (bytes_read < n) {
		size_t result = read(fd, buf + bytes_read, n - bytes_read);

		if (result < 0) {	// Error
			if (errno == EINTR)
				continue;
			return -1;
		}

		else if (result == 0)	// EOF
			return -1;

		bytes_read += result;
	}
	
	return 0;
}


int main() {
	int ininst_fd = inotify_init();
	if (ininst_fd < 0) {
		// Log somewhere (errno available)
		return -1;
	}
		
	int max_wd = sys_wdmax();
	if (max_wd < 0) {
		// Syslog
		return -1;
	}	

	// Track dir and all sub dirs via linux kernel's inotify subsystem
	dir_scan_recursive(WATCH_PATH, ininst_fd);	

	// ========================================================
	// Drain and process all directory events --> Daemon task
	// ========================================================
	size_t event_size = sizeof(struct inotify_event);	
	char buf[event_size + NAME_MAX + 1] = {0};
	while (1) {
		// ENSURES FULL READ
		int rc = full_read(ininst_fd, buf, event_size);
		if (rc < 0) {
			// Syslog
			break;
		}

		
		uint32_t name_len = ((struct inotify_event*)buf)->len;
		rc = full_read(ininst_fd, buf + event_size, name_len);
		if (rc < 0) {
			// Syslog
			break;
		}

		// HANDLE EVENT
		struct inotify_event* event = (struct inotify_event*)buf;
		
	}
	
	delete_all_wddir(ininst_fd);	// Frees all hashmap entries & removes all watch descriptors from kernel's inotify instance
	close(ininst_fd);		// Closes inotify instance
	return 0;	
}

