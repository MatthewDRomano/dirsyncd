#define _POSIX_C_SOURCE >= 200809L // getline (posix func)

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <time.h>
#include <fnmatch.h>
#include "dsync_hash.h"

#define WATCH_PATH "/home/matt/Projects"
#define BACKUP_DIR ""

#define BATCH_COUNT 100	// Amt of events that can be read from kernel at once
#define BLACKLIST_MAX 256


// Bitmask for inotify watch events to track
static uint32_t event_mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_MOVE_SELF | IN_CREATE | IN_DELETE | IN_DELETE_SELF;

static volatile sig_atomic_t shutdown_requested = 0;

static const char* blacklist[BLACKLIST_MAX];
static int blacklist_counter = 0;

// ========================================================
// Reads linux system file to find watch descriptor max
// ========================================================
/*
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
*/
// ========================================================
// Recursively add all sub dirs of path to inotifty watch
// ========================================================

int parse_config() {
	FILE* fp = fopen("/etc/dirsyncd", "r");
	if (!fp) {
		// syslog
		return -1;
	}

	char* line = NULL;
	size_t size = 0;
	ssize_t nread;

	// Read & process all lines in config file
	while ((nread = getline(&line, &size, fp)) != -1) {
		
		// Ensure no buffer overflow 
		if (blacklist_counter >= BLACKLIST_MAX)
			break;		
	
		// Strip trailing newline
		if (nread > 0 && line[nread - 1] == '\n')
			line[nread - 1] ==  '\0';
		
		// Skip empty strings / comments
		if (line[0] == '\0' || line[0] == '#')
			continue;
	
		char* entry = strdup(line);
		if (!entry) {
			//syslog
			break;
		}
	
		blacklist[blacklist_counter++] = entry;
	}	

	// Always free line and close fp
	free(line);
	fclose(fp);
	return 0;
}

void free_blacklist() {
	for (int i = 0; i < blacklist_counter; i++)
		free(blacklist[i]);
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

	// Add entry to wddir_hm not already added
	if (!find_dir(wd))
        	add_wddir(wd, base_path);

	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		// Ignore . and ..
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		
		// Construct entry path
		char entry_path[PATH_MAX];
                snprintf(entry_path, PATH_MAX, "%s/%s", base_path, entry->d_name);
		
		// Entry is a dir --> Add to kernel's inotify watch list
		// lstat() does not follow symlinks
		struct stat sb;
		if (lstat(entry_path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
			// Ignore dir if it matches a blacklisted pattern
        	        // Pattern MUST include a leading zero to ignore hidden files (e.g. ".git/")
	                for (int i = 0; i < blacklist_counter; i++)
                	        if (fnmatch(blacklist[i], entry->d_name, FNM_PERIOD) == 0);
                        	        continue;

			dir_scan_recursive(entry_path, ininst_fd);
		}
	}

	// Directory layer is fully traversed
	// Close associated DIR*
	closedir(dir);
	return 0;
}

// ========================================================
// Safely copies file contents from one file to another
// ========================================================

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

uint64_t now_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sys_shutdown_handler(int sig) {
	(void)sig;
	shutdown_requested = 1;
}

int main() {
	
	struct sigaction sa = {0};
	sa.sa_flags = 0;			// Prevent SA_RESTART on blocking calls upon EINTR
	sa.sa_handler = sys_shutdown_handler;
	sigaction(SIGTERM, &sa, NULL);
	

	int ininst_fd = inotify_init();
	if (ininst_fd < 0) {
		// Log somewhere (errno available)
		return -1;
	}
	
	/*	
	int max_wd = sys_wdmax();
	if (max_wd < 0) {
		// Syslog
		return -1;
	}	
	*/

	// Track dir and all sub dirs via linux kernel's inotify subsystem
	dir_scan_recursive(WATCH_PATH, ininst_fd);	

	// ========================================================
	// Drain and process all directory events --> Daemon task
	// ========================================================

	// Enough space for event + file name + null terminator	
	size_t event_size = sizeof(struct inotify_event) + NAME_MAX + 1;
	char ebuf[BATCH_COUNT * event_size];

	while (!shutdown_requested) {
		// inotify kernel subsystem requires the buffer & requested byte size to be atleast sizeof(struct inotify_event)
		int length = read(ininst_fd, ebuf, BATCH_COUNT * event_size);
		if (length < 0) {
			// Syslog
			break;
		}
		
		if (shutdown_requested)
			break;

		// Handle events read from kernel inotify queue
		int i = 0;
		while (i < length) {
			struct inotify_event* event = (struct inotify_event*)(ebuf + i);
			struct wddir* watched_dir = find_dir(event->wd);

			// Ignore the entry if it matches a blacklisted pattern
			// Leading periods MUST be explicitly put in the blacklisted pattern
			for (int j = 0; j < blacklist_counter; j++)
				if (fnmatch(blacklist[j], event->name, FNM_PERIOD) == 0) {
					i += sizeof(struct inotify_event) + event->len;
					continue;
				}
					
			// ========================================================
        		// Process events on valid entries
        		// ========================================================
	
			// Folder is created (Ignore file creation)
			if (event->mask & IN_CREATE && event->mask & IN_ISDIR) {
				// Use lstat() to read permissions of newly created dir
				char new_dir[PATH_MAX];
				snprintf(new_dir, PATH_MAX, "%s/%s", watched_dir->path, event->name);
				
				struct stat dir_stat;
				if (lstat(new_dir, &dir_stat) == 0 && S_ISDIR(dir_stat.st_mode)) {
					// use mkdir to copy dir w/ same permissions
					char backup_path[PATH_MAX];
					snprintf(backup_path, PATH_MAX, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);
			
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
				char file_path[PATH_MAX];
				snprintf(file_path, PATH_MAX, "%s/%s", watched_dir->path, event->name);

				char backup_path[PATH_MAX];
				snprintf(backup_path, PATH_MAX, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);

				// Safely copies contents into backup directory
				safe_copy(backup_path, file_path);
			}

			// ONLY delete if file --> skip on directory
			else if (event->mask & IN_DELETE && !(event->mask & IN_ISDIR)) {
				char backup_path[PATH_MAX];
                               	snprintf(backup_path, PATH_MAX, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);
					
				unlink(backup_path);
			}

			// Directory is deleted
			// Linux behavior guarantees all subcontents are deleted (Files via IN_DELETE subdirs via IN_DELETE_SELF)
			else if (event->mask & IN_DELETE_SELF && event->mask & IN_ISDIR) {
				char backup_path[PATH_MAX];
				snprintf(backup_path, PATH_MAX, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);

				delete_wddir(watched_dir, ininst_fd);
				rmdir(backup_path);
			}

			// HANDLE RENAMES / MOVES

			// Kernel overflowed with events
			else if (event->mask & IN_Q_OVERFLOW) {
				// Step 1: Force the fd into non-blocking mode to safely drain it
				// This is because an inotify fd blocks indefinitely until it can return atleast 1 event (never EOF)
        			int def_flags = fcntl(ininst_fd, F_GETFL, 0);
        			fcntl(ininst_fd, F_SETFL, def_flags | O_NONBLOCK);
				
				// Step 2: Drain event queue (read until empty / EAGAIN / EWOULDNOTBLOCK err)
				while (read(ininst_fd, ebuf, BATCH_COUNT * event_size) > 0) {
					// Deliberately drain all events
				}

				// Step 3: Restore original blocking flags
				fcntl(ininst_fd, F_SETFL, def_flags);
				
				// Step 4: Clear current inotify tracking tree
				delete_all_wddir(ininst_fd);
				
				// Step 5: Rebuild inotify tracking tree
				dir_scan_recursive(WATCH_PATH, ininst_fd);				
			}	
		
			i += sizeof(struct inotify_event) + event->len;
		}	
	}
	
	delete_all_wddir(ininst_fd);	// Frees all wddir_hm entries & removes all watch descriptors from kernel's inotify instance
	close(ininst_fd);		// Closes inotify instance
	free_blacklist();
	
	return 0;	
}

