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
#define BACKUP_DIR ""

#define MAX_PATH_LEN 1024	
#define BACH_COUNT 100	// Amt of events that can be read from kernel at once


// Bitmask for inotify watch events to track
static uint32_t event_mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_MOVE_SELF | IN_CREATE | IN_DELETE | IN_DELETE_SELF;

// Hashable structure --> key/value pair
struct wddir {
	int wd;			// key
	const char* path;	// value
	UT_hash_handle hh;	// Makes structure hashable via uthash
};

// Global hashmap
struct wddir* hashmap = NULL;

// ========================================================
// uthash.h methods for hashmap handling
// ========================================================

void add_wddir(int wd, const char* path) {
    	struct wddir* entry = (struct wddir*)malloc(sizeof *entry);
    	entry->wd = wd;
    	entry->path = strdup(path);		/* Copy path to the heap */
	HASH_ADD_INT(hashmap, wd, entry);	/* wd: name of key field */
}

struct wddir* find_user(int wd_key) {
        struct wddir* entry;

	HASH_FIND_INT(hashmap, &wd_key, entry);  /* entry: desired output pointer */
	return entry;
}

void delete_wddir(struct wddir* entry, int ininst_fd) {
    	HASH_DEL(hashmap, entry);		/* deletes entry from hashmap */
	inotify_rm_watch(ininst_fd, entry->wd);	/* removes watch from inotify instance */
	
	free((void*)entry->path);		/* frees the heap allocated path */
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
// Reads linux system file to find watch descriptor max
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

// ========================================================
// Recursively add all sub dirs of path to inotifty watch
// ========================================================
	
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
		// lstat() does not follow symlinks
		struct stat sb;
		if (lstat(entry_path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
			dir_scan_recursive(entry_path, ininst_fd);
		}
	}

	// Directory layer is fully traversed
	// Close associated DIR*
	closedir(dir);
	return 0;
}


int safe_copy(const char* dest_path, const char* src_path) {
	int src_fd = open(src_path, O_RDONLY);
	if (src_fd < 0)
		return -1;

	// Equivalent to open() with flags: O_CREAT | O_TRUNC | O_WRONLY
	// 0644 -> Permissions (Owner: read/write, Group/Others: read)
	int dest_fd = creat(dest_path, 0644);
	if (dest_fd < 0) {
		close(src_fd);
		return -1;
	}
	

	// Stops reading on EOF or error
	char cpy_buf[8192]; // 8KB page buffer
	while (1) {		
		ssize_t bytes_read = read(src_fd, cpy_buf, sizeof cpy_buf);		
		if (bytes_read < 0) {
			if (errno == EINTR)
				continue;
			
			// Error --> exist			
			close(src_fd);
			close(dest_fd);
			return -1;
		}
		
		// EOF reached --> Copying done
		else if (bytes_read == 0)
			break;

		ssize_t bytes_written = 0;
		while (bytes_written < bytes_read) {
			ssize_t result = write(dest_fd, cpy_buf + bytes_written, bytes_read - bytes_written);
			
			if (result <= 0) {
				if (errno == EINTR)
					continue;
				
				// Write error
				close(src_fd);
				close(dest_fd);
				return -1;
			}
			
			bytes_written += result;
		}
	}

	close(src_fd);
	close(dest_fd);
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

	// Enough space for event + file name + null terminator	
	size_t event_size = sizeof(struct inotify_event) + NAME_MAX + 1;
	char ebuf[BATCH_COUNT * event_size] = {0};
	while (1) {
		// inotify kernel subsystem requires the buffer & requested byte size to be atleast sizeof(struct inotify_event)
		int length = read(ininst_fd, ebuf, BATCH_COUNT * event_size);
		if (length < 0) {
			// Syslog
			break;
		}

		// Handle events read from kernel inotify queue
		int i = 0;
		while (i < length) {
			struct inotify_event* event = (struct inotify_event*)(ebuf + i);
			struct wddir* watched_dir = find_user(event->wd);
		
			// Folder is created (Ignore file creation)
			if (event->mask & IN_CREATE && event->mask & IN_ISDIR) {
				// Use lstat() to read permissions of newly created dir
				char new_dir[MAX_PATH_LEN];
				snprintf(new_dir, MAX_PATH_LEN, "%s/%s", watched_dir->path, event->name);
				
				struct stat dir_stat;
				if (lstat(new_dir, &dir_stat) == 0 && S_ISDIR(dir_stat.st_mode)) {
					// use mkdir to copy dir w/ same permissions
					char backup_path[MAX_PATH_LEN];
					snprintf(backup_path, MAX_PATH_LEN, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);
			
                        		// Explicitly apply permissions via chmod to override the system umask
					mode_t target_mode = dir_stat.st_mode & 07777;
                			if (mkdir(backup_path, target_mode) == 0) {
                        			chmod(backup_path, target_mode);
                			}

					// Watch new dir
					int wd = inotify_add_watch(ininst_fd, new_dir, event_mask);
					add_wddir(wd, new_dir);
				}
			}

			// File is closed after a write (Also triggered right after creation)
			else if (event->mask & IN_CLOSE_WRITE) {
				char file_path[MAX_PATH_LEN];
				snprintf(file_path, MAX_PATH_LEN, "%s/%s", watched_dir->path, event->name);

				char backup_path[MAX_PATH_LEN];
				snprintf(backup_path, MAX_PATH_LEN, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);

				// Safely copies contents into backup directory
				safe_copy(backup_path, file_path);
			}

			// ONLY delete if file --> skip on directory
			else if (event->mask & IN_DELETE) {
				if (!(event->mask & IN_ISDIR)) {
					char backup_path[MAX_PATH_LEN];
                                	snprintf(backup_path, MAX_PATH_LEN, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);
					
					remove(backup_path);
				}
			}

			// Directory is deleted
			// Linux behavior guarantees all subcontents are deleted (Files via IN_DELETE subdirs via IN_DELETE_SELF)
			else if (event->mask & IN_DELETE_SELF) {
				char backup_path[MAX_PATH_LEN];
				snprintf(backup_path, MAX_PATH_LEN, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);

				delete_wddir(watched_dir, ininst_fd);
				rmdir(backup_path);
			}


			i += sizeof(struct inotify_event) + event->len;
		}	
	}
	
	delete_all_wddir(ininst_fd);	// Frees all hashmap entries & removes all watch descriptors from kernel's inotify instance
	close(ininst_fd);		// Closes inotify instance
	return 0;	
}

