#define _POSIX_C_SOURCE 200809L // getline (posix func from POSIX.1-2008 standard)

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>		// open() flags
#include <fcntl.h>		// fcntl()
#include <sys/inotify.h>	
#include <limits.h>		// NAME_MAX / PATH_MAX
#include <sys/stat.h>		// lstat()
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>		// opendir() / readdir()
#include <poll.h>
#include <fnmatch.h>		// fnmatch()
#include <syslog.h>		// syslog()
#include "dsync_hash.h"

#define DSYNC_WARNING -2
#define DSYNC_ERROR   -3

#define CONF_PATH  "/etc/dsync_blcklst.conf"
#define WATCH_PATH "/home/matt/Projects"
#define BACKUP_DIR "/mnt/SharedDrive"

#define EVENT_BATCH_COUNT 100	// Amt of events that can be read from kernel at once
#define BLACKLIST_MAX 256


// Bitmask for inotify watch events to track
static const uint32_t event_mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_MOVE_SELF | IN_CREATE | IN_DELETE | IN_DELETE_SELF;

static volatile sig_atomic_t shutdown_requested = 0;

// Stores blacklisted patterns (Does not watch files/directories containing these patterns)
static char* blacklist[BLACKLIST_MAX];
static int blacklist_counter = 0;



// ========================================================
// Recursively add all sub dirs of path to inotifty watch
// ========================================================

int parse_config() {
	FILE* fp = fopen(CONF_PATH, "r");
	if (!fp) {
		syslog(LOG_WARNING, "Unable to open dirsyncd config file: %m");
		return DSYNC_WARNING;
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
			line[nread - 1] =  '\0';
		
		// Skip empty strings / comments
		if (line[0] == '\0' || line[0] == '#')
			continue;
	
		char* entry = strdup(line);
		if (!entry) {
			syslog(LOG_WARNING, "strdup() failure during config parse: %m");
			continue;
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
	if (!dir) {
		syslog(LOG_WARNING, "Unable to open directory for watching: %m");
		return DSYNC_WARNING;
	}

	// Ignore dir if it matches a blacklisted pattern
        // Pattern MUST include a leading zero to ignore hidden files (e.g. ".git/")
	// The pattern matches input paths across nested directories as well (e.g. /test/dir/.entry)
        for (int i = 0; i < blacklist_counter; i++)
        	if (fnmatch(blacklist[i], base_path, FNM_PERIOD) == 0)
                	return 0;


	// Add dir to inotify watch if no blacklisted pattern is found
	int wd = inotify_add_watch(ininst_fd, base_path, event_mask);
	if (wd < 0) {
		closedir(dir);
		syslog(LOG_ERR, "Ran out of watch descriptors: %m");
		return DSYNC_ERROR;
	}

	// Add entry to wddir_hm if not already added
	if (!hm_find_dir(wd))
        	hm_add_wddir(wd, base_path);

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
		if (lstat(entry_path, &sb) == 0 && S_ISDIR(sb.st_mode))
			dir_scan_recursive(entry_path, ininst_fd);
		
		else
			syslog(LOG_WARNING, "lstat() failure: %m");
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
	if (src_fd < 0) {
		syslog(LOG_WARNING, "Error opening files for copying: %m");
		return DSYNC_WARNING;
	}

	// Equivalent to open() with flags: O_CREAT | O_TRUNC | O_WRONLY
	// 0644 -> Permissions (Owner: read/write, Group/Others: read)
	int dest_fd = creat(dest_path, 0644);
	if (dest_fd < 0) {
		syslog(LOG_WARNING, "Error opening files for copying: %m");
		close(src_fd);
		return DSYNC_WARNING;
	}
	

	// Stops reading on EOF or error
	char cpy_buf[8192]; // 8KB page buffer
	while (1) {		
		ssize_t bytes_read = read(src_fd, cpy_buf, sizeof cpy_buf);		
		if (bytes_read < 0) {
			if (errno == EINTR)
				continue;
			
			// Error --> exist			
			syslog(LOG_WARNING, "Error reading file during copying: %m");
			close(src_fd);
			close(dest_fd);
			return DSYNC_WARNING;
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
				syslog(LOG_WARNING, "Error writing file during copying: %m");
				close(src_fd);
				close(dest_fd);
				return DSYNC_WARNING;
			}
			
			bytes_written += result;
		}
	}

	close(src_fd);
	close(dest_fd);
	return 0;
}

// Takes a watched directory, and an event struct
// Creates new file, backs up all contents
int backup_new_file(struct wddir* watched_dir, struct inotify_event* event) {
	char file_path[PATH_MAX];
        snprintf(file_path, PATH_MAX, "%s/%s", watched_dir->path, event->name);

        char backup_path[PATH_MAX];
        snprintf(backup_path, PATH_MAX, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);

        // Safely copies contents into backup directory
        int rc;
	if ((rc = safe_copy(backup_path, file_path)) < 0)
		return rc;

	return 0;
}

// Takes a watched directory, and an event struct
// The passed event mask should contain IN_CREATE & IN_ISDIR
int backup_and_watch_dir(int ininst_fd, struct wddir* watched_dir, struct inotify_event* event) {
	if (!(event->mask & IN_CREATE) || !(event->mask & IN_ISDIR))
		return -1;

	// Use lstat() to read permissions of newly created dir
        char new_dir[PATH_MAX];
        snprintf(new_dir, PATH_MAX, "%s/%s", watched_dir->path, event->name);

        struct stat dir_stat;
        if (lstat(new_dir, &dir_stat) == 0) {
        	// use mkdir to copy dir w/ same permissions
                char backup_path[PATH_MAX];
                snprintf(backup_path, PATH_MAX, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);

                // Explicitly apply permissions via chmod to override the system umask
                mode_t target_mode = dir_stat.st_mode & 07777;
                if (mkdir(backup_path, target_mode) == 0) {
                	chmod(backup_path, target_mode);
                }
		else {
			syslog(LOG_WARNING, "Unable to make directory: %m");
			return DSYNC_WARNING;
		}
		
                // Watch new dir
                int wd = inotify_add_watch(ininst_fd, new_dir, event_mask);
		if (wd < 0) {
			syslog(LOG_WARNING, "Unable to watch new directory: %m");
			return DSYNC_WARNING;
		}

                hm_add_wddir(wd, new_dir);
	}

	else {
		syslog(LOG_WARNING, "Error with lstat(): %m");
		return DSYNC_WARNING;
	}

	return 0;
}

// After specified poll() timeout, treat unmatched IN_MOVE_FROM events as deletions
void handle_unmatched_movefrom_events(int ininst_fd) {
	// Iterate through the cookie : event hashmap
	struct cookie_event* event = cookie_event_hm;
	while (event != NULL) {	
		struct wddir* watched_dir = hm_find_dir(event->wd);
		
		// Delete directory     
        	if (event->mask & IN_ISDIR) {
                	char backup_path[PATH_MAX];
                	snprintf(backup_path, PATH_MAX, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);
  			
                	hm_delete_wddir(watched_dir, ininst_fd);   
                	rmdir(backup_path);
        	}

        	// Delete file
        	else {
                	char backup_path[PATH_MAX];
                	snprintf(backup_path, PATH_MAX, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);

                	unlink(backup_path);
        	}
		
		// Remove the IN_MOVED_FROM event from the hashmap, iterate to next
		struct cookie_event* prev = event;
		event = event->hh.next;
		hm_delete_cookie_event(prev);
	}
}


void sys_shutdown_handler(int sig) {
	(void)sig;
	shutdown_requested = 1;
}


int main() {
	// Sets return value for success/failure
	int return_status = 0;	

	struct sigaction sa = {0};
	sa.sa_flags = 0;			// Prevent SA_RESTART on blocking calls upon EINTR
	sa.sa_handler = sys_shutdown_handler;
	sigaction(SIGTERM, &sa, NULL);
	
	// Opens connection to system log
	openlog(NULL, LOG_PID, LOG_DAEMON);

	int ininst_fd = inotify_init();
	if (ininst_fd < 0) {
		syslog(LOG_ERR, "Unable to create inotify instance: %m");
		closelog();
		return -1;
	}
	

	// Parse system config file for blacklisted patterns
	if (parse_config() == DSYNC_ERROR) {
		closelog();
		close(ininst_fd);
		return -1;
	}
	
	// Track dir and all sub dirs via linux kernel's inotify subsystem
	if (dir_scan_recursive(WATCH_PATH, ininst_fd) == DSYNC_ERROR) {
		closelog();
		close(ininst_fd);
		free_blacklist();
		return -1;	
	}

	// ========================================================
	// Drain and process all directory events --> Daemon task
	// ========================================================

	struct pollfd pfd;
        pfd.fd = ininst_fd;
        pfd.events = POLLIN;
	int timeout_ms = 50;		// 50 ms timeout --> processed unmatched IN_MOVED_FROM events

	// Enough space for event + file name + null terminator	
	size_t event_size = sizeof(struct inotify_event) + NAME_MAX + 1;
	char ebuf[EVENT_BATCH_COUNT * event_size];

	while (!shutdown_requested) {
		
		int ret = poll(&pfd, 1, timeout_ms);
		
		if (shutdown_requested)
			break;
	
		// Poll() error. POLLERR / POLLHUP almost never set 
		// Those errors manifest as inotify event errors like IN_Q_OVERFLOW	
		if (ret < 0) {
			if (errno == EINTR)
				continue;

			syslog(LOG_ERR, "Poll() error: %m");
			return_status = -1;
			goto run_err_cleanup;
		}

		// Poll timedout --> no events are present
		else if (ret == 0) {
			handle_unmatched_movefrom_events(ininst_fd);
			timeout_ms = -1;
			continue;
		}


		// inotify kernel subsystem requires the buffer & requested byte size to be atleast sizeof(struct inotify_event)
		int length = read(ininst_fd, ebuf, EVENT_BATCH_COUNT * event_size);
		if (length < 0) {
			syslog(LOG_ERR, "Error while reading inotify events: %m");
			return_status = -1;
			goto run_err_cleanup;
		}
		

		// Handle events read from kernel inotify queue
		int i = 0;
		while (i < length) {
			struct inotify_event* event = (struct inotify_event*)(ebuf + i);
			struct wddir* watched_dir = hm_find_dir(event->wd);

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
				backup_and_watch_dir(ininst_fd, watched_dir, event);
			}

			// File is closed after a write (Also triggered right after creation)
			else if (event->mask & IN_CLOSE_WRITE)
				backup_new_file(watched_dir, event);

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
				snprintf(backup_path, PATH_MAX, "%s%s", BACKUP_DIR, watched_dir->path);

				hm_delete_wddir(watched_dir, ininst_fd);
				rmdir(backup_path);
			}

			// 'Move from' event
			// File is either moved out of watched directory or renamed (move from triggers before move to)
			else if (event->mask & IN_MOVED_FROM) {
				hm_add_cookie_event(event->cookie, event->wd, event->mask, event->name);	
				timeout_ms = 50;
			}

			// 'Move to' event
			// File is either moved into watched directory or renamed (always triggered AFTER a move from event)
			else if (event->mask & IN_MOVED_TO) {
				// Hashmap entry corresponding to a IN_MOVED_FROM event is found 
				// This IN_MOVED_FROM and IN_MOVED_TO pair means a rename occured
				struct cookie_event* entry;
				if ((entry = hm_find_event(event->cookie))) {
					// Rename the file and remove entry from hashmap
					char old_path[PATH_MAX], new_path[PATH_MAX];
					snprintf(old_path, PATH_MAX, "%s%s/%s", BACKUP_DIR, watched_dir->path, entry->name);
					snprintf(new_path, PATH_MAX, "%s%s/%s", BACKUP_DIR, watched_dir->path, event->name);
					rename(old_path, new_path);		
					
					hm_delete_cookie_event(entry);
				}
			
				// File/Dir was moved from an unwatched directory
				// treat as creation
				else {
					// Dir creation --> add to inotify watch
					if (event->mask & IN_ISDIR) 
						backup_and_watch_dir(ininst_fd, watched_dir, event);
					else
						backup_new_file(watched_dir, event);	
				}	
			}

			// Kernel overflowed with events
			else if (event->mask & IN_Q_OVERFLOW) {
				// Step 1: Force the fd into non-blocking mode to safely drain it
				// This is because an inotify fd blocks indefinitely until it can return atleast 1 event (never EOF)
        			int def_flags = fcntl(ininst_fd, F_GETFL, 0);
        			fcntl(ininst_fd, F_SETFL, def_flags | O_NONBLOCK);
				
				// Step 2: Drain event queue (read until empty / EAGAIN / EWOULDNOTBLOCK err)
				while (read(ininst_fd, ebuf, EVENT_BATCH_COUNT * event_size) > 0) {
					// Deliberately drain all events
				}

				// Step 3: Restore original blocking flags
				fcntl(ininst_fd, F_SETFL, def_flags);
				
				// Step 4: Clear current inotify tracking tree
				hm_delete_all_wddir(ininst_fd);
				
				// Step 5: Rebuild inotify tracking tree
				dir_scan_recursive(WATCH_PATH, ininst_fd);

				// Exit the event queue read loop --> Go back to polling ininst_fd
				break;
			}

			// Watched directory was unmounted --> remove from hashmap			
			else if (event->mask & IN_UNMOUNT) {
				syslog(LOG_NOTICE, "Dir unmounted: %s/%s", watched_dir->path, event->name);
				hm_delete_wddir(watched_dir, ininst_fd);
			}

			// Ignore IN_IGNORED	
		
			i += sizeof(struct inotify_event) + event->len;
		}	
	}

	run_err_cleanup:
	hm_delete_all_wddir(ininst_fd);	// Frees all wddir_hm entries & removes all watch descriptors from kernel's inotify instance
	hm_delete_all_cookie_event();	// Frees all cookie_event entries
	close(ininst_fd);		// Closes inotify instance
	free_blacklist();

	// Closes connection to system log
	closelog();
	return return_status;	
}

