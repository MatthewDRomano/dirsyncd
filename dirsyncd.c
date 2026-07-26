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

#define CONF_PATH  "/etc/dirsyncd.conf"

#define EVENT_BATCH_COUNT 100	// Amt of events that can be read from kernel at once
#define BLACKLIST_MAX 256


// Bitmask for inotify watch events to track
static const uint32_t event_mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_MOVE_SELF | IN_CREATE | IN_DELETE | IN_DELETE_SELF;

static volatile sig_atomic_t shutdown_requested = 0;

// Stores blacklisted patterns (Does not watch files/directories containing these patterns)
static char* blacklist[BLACKLIST_MAX];
static int blacklist_counter = 0;

// Watch and backup paths set by user in dirsyncd.conf
static char* watch_root = NULL;
static char* backup_root = NULL;

// Config file keywords for watch_root and backup_root specifications
static const char* wroot_keyword = "WATCH_PATH=";
static const char* broot_keyword = "BACKUP_PATH=";


// Free heap allocated blacklisted patterns
void free_blacklist() {
        for (int i = 0; i < blacklist_counter; i++)
                free(blacklist[i]);
}


// ========================================================
// Parse system config file
//	|-> Finds user-set blacklisted file/dir patterns
//	|-> Sets global watch_root and backup_root paths
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

		/* 
		 * Removes trailing newline
		 * Skips empty strings / comments
	 	*/
		
		if (nread > 0 && line[nread - 1] == '\n')
			line[nread - 1] =  '\0';
		
		if (line[0] == '\0' || line[0] == '#')
			continue;
	
		/* 
		 * Check for watch/backup root path strings
		 * Sets global fields if valid, removing potentially trailing '/'
		*/
	
		char* tmp;
		size_t len;
		int keylen = strlen(wroot_keyword);
		if (strncmp(line, wroot_keyword, keylen) == 0) {
			if (!watch_root) {
				tmp = strdup(line + keylen);
				if (!tmp) {
					syslog(LOG_ERR, "Error allocating memory for watch_root: %m");
					goto err_inv_parse;
				}
				
				len = strlen(tmp);
				if (len <= 0) {
					free(tmp);
					continue;
				}

				watch_root = tmp;
				if (watch_root[len - 1] == '/')
					watch_root[len - 1] = '\0';
			}
			else
				syslog(LOG_WARNING, "Duplicate WATCH_PATH in config ignored");

			continue;
		}

		keylen = strlen(broot_keyword);
		if (strncmp(line, broot_keyword, keylen) == 0) {
			if (!backup_root) {
				tmp = strdup(line + keylen);
				if (!tmp) {
					syslog(LOG_ERR, "Error allocating memory for backup_root: %m");
					goto err_inv_parse;
				}

				len = strlen(tmp);
				if (len <= 0) {
					free(tmp);
					continue;
				}
				
				backup_root = tmp;
				if (backup_root[len - 1] == '/')
					backup_root[len - 1] = '\0';
			}
			else
				syslog(LOG_WARNING, "Duplicate BACKUP_PATH in config ignored");

			continue;
		}

		/*
		 * Otherwise, treats the config line as a blacklisted pattern
		 * Perform bounds checks, then add pattern to blacklist array
		*/
                
		if (blacklist_counter >= BLACKLIST_MAX) {
                	syslog(LOG_ERR, "Too many blacklisted patterns (max: %d)", BLACKLIST_MAX); 
			goto err_inv_parse;      
		}

		len = strlen(line);
		if (line[len - 1] == '/')
			line[len - 1] = '\0';
	
		char* entry = strdup(line);
		if (!entry) {
			syslog(LOG_WARNING, "strdup() failure during config parse: %m");
			continue;
		}
	
		blacklist[blacklist_counter++] = entry;
	}	


	// Error if watch/backup root path(s) not specified in config
	if (!watch_root || !backup_root) {
		syslog(LOG_ERR, "Watch path and/or backup path not specified in dirsyncd config");
		goto err_inv_parse;
	}

	// Invalid watch path
	struct stat sb;
	if (lstat(watch_root, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
		syslog(LOG_ERR, "Invalid watch path in config");
		goto err_inv_parse;
	}

	// Invalid backup path 
	if (lstat(backup_root, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
		syslog(LOG_ERR, "Invalid backup path in config");
		goto err_inv_parse;
	}


	// Always free line and close fp
	free(line);
	fclose(fp);
	return 0;


	// Cleanup path for config watch/backup root path errors & blacklist overflow
	err_inv_parse:
	free(watch_root);	// Safe preventive free call
	free(backup_root);	// Safe preventive free call
	free_blacklist();	

	free(line);
	fclose(fp);
	
	return DSYNC_ERROR;
}


// Returns a pointer to the relative path fragment inside abs_path by stripping 
// the leading watch_root or backup_root prefix. Returns "" if abs_path is the root itself, 
// or NULL if it falls under neither. 
// Do NOT free() the returned pointer; it aliases abs_path and shares its lifetime.
const char* constr_rel_path(const char* abs_path) {
	int wroot_path_len = strlen(watch_root);
	int broot_path_len = strlen(backup_root);

	// The absolute path is a watched directory
	if (strncmp(abs_path, watch_root, wroot_path_len) == 0) {
		const char* rel_path = abs_path + wroot_path_len;
		if (*rel_path  == '/')
			return rel_path + 1;

		else if (*rel_path == '\0')
			return rel_path;

		// else: false-positive prefix match (e.g. "/data_backup" vs "/data") -- fall through
	}

	// The absolute path is a backup directory
	if (strncmp(abs_path, backup_root, broot_path_len) == 0) {
		const char* rel_path = abs_path + broot_path_len;
		if (*rel_path == '/')
			return rel_path + 1;
		
		else if (*rel_path == '\0')
			return rel_path;

		// else: same false positive fall through behavior as above
	}

	return NULL;
}


// ========================================================
// Recursively add all sub dirs of path to inotify watch
// ========================================================
	
int watch_tree(const char* base_path, int ininst_fd) {
	DIR* dir = opendir(base_path);
	
	// Unable to open directory
	if (!dir) {
		syslog(LOG_WARNING, "Unable to open directory for watching: %m");
		return DSYNC_WARNING;
	}

	// Ignore dir if it matches a blacklisted pattern
        // Pattern MUST include a leading period to ignore hidden files (e.g. ".git/")
	// The pattern matches input paths across nested directories as well (e.g. /test/dir/.entry)
	const char* base_name = strrchr(base_path, '/');		// sets pointer to last occurence of '/'
        base_name = (base_name) ? base_name + 1 : base_path;

	for (int i = 0; i < blacklist_counter; i++)
        	if (fnmatch(blacklist[i], base_name, FNM_PERIOD) == 0) {
			closedir(dir);
                	return 0;
		}

	// Add dir to inotify watch if no blacklisted pattern is found
	int wd = inotify_add_watch(ininst_fd, base_path, event_mask);
	if (wd < 0) {
		closedir(dir);
		if (errno == ENOSPC)
			syslog(LOG_ERR, "Out of inotify watch descriptors (fs.inotify.max_user_watches) -- aborting scan: %m");
		else
			syslog(LOG_WARNING, "Unable to add directory to watch: %m");

		return (errno == ENOSPC) ? DSYNC_ERROR : DSYNC_WARNING;
	}

	// Add entry to wddir_hm if not already added
	if (!hm_find_wddir(wd))
        	if (hm_add_wddir(wd, base_path) != 0) {
			syslog(LOG_ERR, "Failed to watch directory (wd: %d)", wd);
			inotify_rm_watch(ininst_fd, wd);
			return DSYNC_ERROR;
		}

	struct dirent* entry;
	int scan_rc = 0;
	while ((entry = readdir(dir)) != NULL) {
		// Ignore . and ..
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		
		// Construct entry path
		char entry_path[PATH_MAX];
                snprintf(entry_path, PATH_MAX, "%s/%s", base_path, entry->d_name);
		
		// lstat() does not follow symlinks
		struct stat sb;
		if (lstat(entry_path, &sb) != 0) {
			syslog(LOG_WARNING, "lstat() failure while scanning dir: %m");
			scan_rc = DSYNC_WARNING;
			continue;
		}
	
		// Entry is a dir --> Add to kernel's inotify watch list
		if (S_ISDIR(sb.st_mode)) {
			int child_rc = watch_tree(entry_path, ininst_fd);
					
			if (child_rc == DSYNC_ERROR) {
				scan_rc = DSYNC_ERROR; 
				break;
			}
			
			else if (child_rc == DSYNC_WARNING)
				scan_rc = DSYNC_WARNING;	
		
		}
	}

	// Directory layer is fully traversed
	// Close associated DIR*
	closedir(dir);
	return scan_rc;
}

// ========================================================
// Safely copies file contents from one file to another
// 	|-> Creates file if dest_path doesn't exist
// ========================================================

int safe_copy(const char* dest_path, const char* src_path) {
	int src_fd = open(src_path, O_RDONLY);
	if (src_fd < 0) {
		syslog(LOG_WARNING, "Error opening src file for copying (%s): %m", src_path);
		return DSYNC_WARNING;
	}

	// Copy src file perms for dest file
	struct stat sb;
	if (lstat(src_path, &sb) != 0) {
		syslog(LOG_WARNING, "Error w/ lstat() during copy: %m");
        	close(src_fd);
	        return DSYNC_WARNING;
	}

	// Equivalent to open() with flags: O_CREAT | O_TRUNC | O_WRONLY
	int dest_fd = creat(dest_path, 0600);
	if (dest_fd < 0) {
		syslog(LOG_WARNING, "Error opening dest file for copying(%s): %m", dest_path);
		close(src_fd);
		return DSYNC_WARNING;
	}
	
	// Ensure the source files permissions cary over --> overrides umask
	fchmod(dest_fd, 0777 & sb.st_mode);

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


// Takes a newly IN_MOVED_IN directory, and the corresponding event struct
// The passed event mask should contain IN_CREATE & IN_ISDIR
int backup_and_watch_new_dir(int ininst_fd, struct wddir* watched_dir, struct inotify_event* event) {
	if (!(event->mask & IN_CREATE) || !(event->mask & IN_ISDIR))
		return -1;

	// Use lstat() to read permissions of newly created dir
        char new_dir_path[PATH_MAX];
        snprintf(new_dir_path, PATH_MAX, "%s/%s", watched_dir->path, event->name);

        struct stat dir_stat;
        if (lstat(new_dir_path, &dir_stat) == 0) {
		
		// Watch new dir
                int wd = inotify_add_watch(ininst_fd, new_dir_path, event_mask);
                if (wd < 0) {
                        if (errno == ENOSPC)
                                syslog(LOG_ERR, "Out of inotify watch descriptors (fs.inotify.max_user_watches) -- aborting scan: %m");
                        else
                                syslog(LOG_WARNING, "Unable to watch new directory: %m");

                        return (errno == ENOSPC) ? DSYNC_ERROR : DSYNC_WARNING;
                }

		// Add to hashmap
                if (hm_add_wddir(wd, new_dir_path) != 0) { 
			inotify_rm_watch(ininst_fd, wd);
			return DSYNC_ERROR;
		}
        	
		// use mkdir to copy dir w/ same permissions
                char backup_path[PATH_MAX];
                snprintf(backup_path, PATH_MAX, "%s/%s", backup_root, constr_rel_path(new_dir_path));

                // Explicitly apply permissions via chmod to override the system umask
                mode_t target_mode = dir_stat.st_mode & 0777;
                if (mkdir(backup_path, target_mode) == 0) {
                	chmod(backup_path, target_mode);
                }
		else {
			syslog(LOG_WARNING, "Unable to make directory: %m");
			hm_delete_wddir(hm_find_wddir(wd), ininst_fd, 1);
			return DSYNC_WARNING;
		}
		
	}

	else {
		syslog(LOG_WARNING, "lstat() error reading new dir perms: %m");
		return DSYNC_WARNING;
	}

	return 0;
}


// Recursively backs up the files/directories in watch_path's subtree to backup_path
int backup_tree(const char* backup_path, const char* watch_path) {
	
	// Ensure watch_path is a valid directory
	struct stat sb;
	if (lstat(watch_path, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
		syslog(LOG_WARNING, "lstat() failure backing up moved-in dir tree: %m");
		return DSYNC_WARNING;
	}

	// Check if watch_path is blacklisted
	const char* base_name = strrchr(watch_path, '/');                // sets pointer to last occurence of '/'
        base_name = (base_name) ? base_name + 1 : watch_path;

        for (int i = 0; i < blacklist_counter; i++)
                if (fnmatch(blacklist[i], base_name, FNM_PERIOD) == 0)
                        return 0;

	// Backup the directory
	mode_t target_mode = sb.st_mode & 0777;
	if (mkdir(backup_path, target_mode) == 0)
		chmod(backup_path, target_mode);
	else {
		syslog(LOG_WARNING, "mkdir() failure backing up moved-in tree: %m");
		return DSYNC_WARNING;
	}


	// Begin recursively backup up directory subtree
	DIR* dir = opendir(watch_path);
	if (!dir) {
		syslog(LOG_WARNING, "opendir() failure backing up moved-in tree: %m");
		return DSYNC_WARNING;
	}

	struct dirent* entry;
	int scan_rc = 0;
	while ((entry = readdir(dir)) != NULL) {
		// Skip . and ..
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		
		char child_watch_path[PATH_MAX], child_backup_path[PATH_MAX];
		snprintf(child_watch_path,  PATH_MAX, "%s/%s", watch_path, entry->d_name);
		snprintf(child_backup_path, PATH_MAX, "%s/%s", backup_path, entry->d_name);

		struct stat child_sb;
		if (lstat(child_watch_path, &child_sb) == 0) {
			// Child is dir
			if (S_ISDIR(child_sb.st_mode)) {
				if (backup_tree(child_backup_path, child_watch_path) != 0)
					scan_rc = DSYNC_WARNING;
			}
	
			// Child is file
			else {
				if (safe_copy(child_backup_path, child_watch_path) != 0) {
					syslog(LOG_WARNING, "Fail to copy child within moved-in tree");
					scan_rc = DSYNC_WARNING;
				}
			}
		}

		else {
			syslog(LOG_WARNING, "lstat() failure on child backup within move-in tree: %m");
			scan_rc = DSYNC_WARNING;
		}

	}

	closedir(dir);
	return scan_rc;
}


// Recursively removes a backup path
int remove_backup_tree(const char* base_path) {
	DIR* dir = opendir(base_path);
	if (!dir) {
		syslog(LOG_WARNING, "Error opening backup dir for removal: %m");
		return DSYNC_WARNING;
	}

	struct dirent* entry;
	int rc = 0;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		char child_path[PATH_MAX];
		snprintf(child_path, PATH_MAX, "%s/%s", base_path, entry->d_name);
	
		// Read entry perms to determine if file/dir	
		struct stat sb;
		if (lstat(child_path, &sb) == 0) {
			// Recursively remove subtree if entry is a dir		
			if (S_ISDIR(sb.st_mode)) {
				if (remove_backup_tree(child_path) != 0)
					rc = DSYNC_WARNING;	
			}		

			// Otherwise file --> remove
			else {
				if (unlink(child_path) != 0) {
                	                syslog(LOG_WARNING, "unlink() error while cleaning backup path: %m");
        	                        rc = DSYNC_WARNING;
	                        }

			}
		}
		
		// lstat failure
		else {
			syslog(LOG_WARNING, "lstat() error while cleaning backup path: %m");
			rc = DSYNC_WARNING;
		}
	}

	closedir(dir);
	if (rmdir(base_path) != 0) {
		syslog(LOG_WARNING, "rmdir() error while cleaning backup path: %m");
		return DSYNC_WARNING;
	}
	return rc;
}


// After specified poll() timeout, treat unmatched IN_MOVE_FROM events as deletions
void handle_unmatched_movefrom_events(int ininst_fd) {
	struct cookie_event *current_event, *tmp;
	
	HASH_ITER(hh, cookie_event_hm, current_event, tmp) {	
		struct wddir* watched_dir = hm_find_wddir(current_event->wd);
		
		// The parent watch can vanish before the event's 50ms grace period 
		// (e.g. The parent dir was deleted or unmounted in the meantime) 
		// watched_dir would then be NULL
		if (!watched_dir) {
			syslog(LOG_WARNING, "Stale watch descriptor for pending IN_MOVED_FROM event; dropping");
			hm_delete_cookie_event(current_event);
			continue;
		}

		char child_watch_path[PATH_MAX], child_backup_path[PATH_MAX];
		snprintf(child_watch_path,  PATH_MAX, "%s/%s", watched_dir->path, current_event->name);
		snprintf(child_backup_path, PATH_MAX, "%s/%s", backup_root, constr_rel_path(child_watch_path));		

		int rc;
		// Delete directory recursively
        	if (current_event->mask & IN_ISDIR) {
			
			// Remove dir subtree from hashmap & inotify instance (specified by 1)
			// hm_delete_tree_wddir allows access/deletion via path
			hm_delete_tree_wddir(child_watch_path, ininst_fd, 1);   
                	rc = remove_backup_tree(child_backup_path);
        	}

        	// Delete file
        	else
                	rc = unlink(child_backup_path);
	
		// Log removal error	
		if (rc != 0) 
			syslog(LOG_WARNING, "Unable to remove file/dir");

		// Remove the IN_MOVED_FROM event from the hashmap
		hm_delete_cookie_event(current_event);
	}
}


// SIGTERM handler for graceful shutdown
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
	
	// Opens connection to system log --> "dirsyncd" in red is prepended to all syslog messages
	openlog("\033[31mdirsyncd\033[0m", LOG_PID, LOG_DAEMON);

	// Initialize inotify instance w/ nonblocking fd for event reads
	int ininst_fd = inotify_init1(IN_NONBLOCK);
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
	
	// Recursively scan and track all subdirectories in watch_root
	// Builds the inotify watch tree
	if (watch_tree(watch_root, ininst_fd) == DSYNC_ERROR) {
		closelog();
		close(ininst_fd);
		free_blacklist();
		free(watch_root);
		free(backup_root);
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
			break;
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
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;

			syslog(LOG_ERR, "Error while reading inotify events: %m");
			return_status = -1;
			break;
		}
		

		// Handle events read from kernel inotify queue
		int i = 0;
		while (i < length) {
			struct inotify_event* event = (struct inotify_event*)(ebuf + i);
		
			// Skip event if the wd is invalid / outdated (aside from IN_Q_OVERFLOW)
			struct wddir* watched_dir = hm_find_wddir(event->wd);
			if (!watched_dir && !(event->mask & IN_Q_OVERFLOW)) {
				i += sizeof(struct inotify_event) + event->len;
				continue;	
			}


			// Ignore the entry if it matches a blacklisted pattern
			// Leading periods MUST be explicitly put in the blacklisted pattern
			int skip_event = 0;
			if (event->len > 0) {
				for (int j = 0; j < blacklist_counter; j++)
					if (fnmatch(blacklist[j], event->name, FNM_PERIOD) == 0) {
						i += sizeof(struct inotify_event) + event->len;
						skip_event = 1;
						break;
					}
			}
				
			// File or Dir name matches blacklisted pattern	
			if (skip_event)
				continue;

			// ========================================================
        		// Process events on valid entries
        		// ========================================================
	
			// Folder is created (Ignore file creation)
			if (event->mask & IN_CREATE && event->mask & IN_ISDIR) {
				backup_and_watch_new_dir(ininst_fd, watched_dir, event);
			}

			// File is closed after a write (Also triggered right after creation)
			else if (event->mask & IN_CLOSE_WRITE) {
				char file_path[PATH_MAX], backup_file_path[PATH_MAX];
				snprintf(file_path, 	   PATH_MAX, "%s/%s", watched_dir->path, event->name);
				snprintf(backup_file_path, PATH_MAX, "%s/%s", backup_root, constr_rel_path(file_path));				
				if (safe_copy(backup_file_path, file_path) != 0)
					syslog(LOG_WARNING, "Unable to backup file: %s", file_path);
			}

			// ONLY delete if file --> skip on directory
			else if (event->mask & IN_DELETE && !(event->mask & IN_ISDIR)) {
				char file_path[PATH_MAX], backup_file_path[PATH_MAX];
				snprintf(file_path, 	   PATH_MAX, "%s/%s", watched_dir->path, event->name);
                                snprintf(backup_file_path, PATH_MAX, "%s/%s", backup_root, constr_rel_path(file_path)); 				
				if (unlink(backup_file_path) != 0)
					syslog(LOG_WARNING, "Error unlinking file: %m");
			}

			// Directory is deleted
			// Linux behavior guarantees all subcontents are deleted (Files via IN_DELETE subdirs via IN_DELETE_SELF)
			else if (event->mask & IN_DELETE_SELF) {
				char backup_dir_path[PATH_MAX];
				snprintf(backup_dir_path, PATH_MAX, "%s/%s", backup_root, constr_rel_path(watched_dir->path));

				// Remove from hashmap
				// Do not call inotify_rm_watch() as kernel has done so for us (specified by 0 below)
				hm_delete_wddir(watched_dir, ininst_fd, 0);
				
				if (rmdir(backup_dir_path) != 0)
					syslog(LOG_WARNING, "Error removing dir: %m");
			}

			// 'Move from' event
			// File is either moved out of watched directory or renamed (move from triggers before move to)
			else if (event->mask & IN_MOVED_FROM) {
				// Set poll timeout only if successfully added to hashmap
				if (hm_add_cookie_event(event->cookie, event->wd, event->mask, event->name) == 0)
					timeout_ms = 50;
			}

			// 'Move to' event
			// File/folder is either moved into watched directory or renamed (always triggered AFTER a move from event)
			else if (event->mask & IN_MOVED_TO) {
				
				// Hashmap entry corresponding to a IN_MOVED_FROM event is found 
				// Rename occured (allows cross directory moves)
				struct cookie_event* mvf_event;
				if ((mvf_event = hm_find_cookie_event(event->cookie))) {
					struct wddir* mvf_dir = hm_find_wddir(mvf_event->wd);
					if (!mvf_dir) {
						syslog(LOG_WARNING, "Stale source wd for IN_MOVE_TO rename; dropping event");
						hm_delete_cookie_event(mvf_event);
						i += sizeof(struct inotify_event) + event->len;
						continue;
					}

					// 1.) Rename the backup file/folder
					char old_watch_path[PATH_MAX], 	new_watch_path[PATH_MAX];
					char old_backup_path[PATH_MAX], new_backup_path[PATH_MAX];
					snprintf(old_watch_path,  PATH_MAX, "%s/%s", mvf_dir->path, mvf_event->name);	
					snprintf(new_watch_path,  PATH_MAX, "%s/%s", watched_dir->path, event->name);
					snprintf(old_backup_path, PATH_MAX, "%s/%s", backup_root, constr_rel_path(old_watch_path));
					snprintf(new_backup_path, PATH_MAX, "%s/%s", backup_root, constr_rel_path(new_watch_path));
					
					if (rename(old_backup_path, new_backup_path) != 0)
						syslog(LOG_WARNING, "Failure renaming (%s) in backup path; de-sync immenent: %m", old_backup_path);	
					
					// 2.) Remove the entry from the IN_MOVED_FROM cache (hashmap)
					hm_delete_cookie_event(mvf_event);

					// 3.) Handle hashmap stale paths for renamed directories
					if (event->mask & IN_ISDIR) {
						
						// Nuke the old hashmap state and rebuild w/ updated paths 
						// Unadds and readds inotify watch descriptors
						hm_delete_tree_wddir(old_watch_path, ininst_fd, 1);
						if (watch_tree(new_watch_path, ininst_fd) == DSYNC_ERROR)
							syslog(LOG_ERR, "Error scanning new 'IN_MOVED_TO' directory");
					}
				}
			
				// File/Dir was moved from an unwatched directory
				// Treat as creation --> backup contents
				else {
					char new_watch_path[PATH_MAX], new_backup_path[PATH_MAX];
					snprintf(new_watch_path,  PATH_MAX, "%s/%s", watched_dir->path, event->name);
					snprintf(new_backup_path, PATH_MAX, "%s/%s", backup_root, constr_rel_path(new_watch_path));

					// Entry is a dir --> add entire tree to inotify watch 
					if (event->mask & IN_ISDIR) {
						if (watch_tree(new_watch_path, ininst_fd) != 0)
							syslog(LOG_WARNING, "Warning: (%s) may not be fully synced with (%s)", new_backup_path, new_watch_path);
						backup_tree(new_backup_path, new_watch_path);
					}	

					// Entry is a file
					else
						if (safe_copy(new_backup_path, new_watch_path) != 0)
							syslog(LOG_WARNING, "Unable to backup file: %s", new_watch_path);
				}	
			}

			/* Handles root watch-directory renames / moves
			 * CRITICAL EVENT: nuke the wddir hashmap and inotify watch instances
			  	* Exit process and force the user to update the config file with new watch path 
			  	* Restart daemon to resume proper behavior
			*/
			else if (event->mask & IN_MOVE_SELF) {
				// Confirms the root was moved/renamed --> Nuke hashmap
				if (strcmp(watched_dir->path, watch_root) == 0) {
					syslog(LOG_CRIT, "Root watch path altered: UPDATE CONF. & RESTART");
	
					// Breaks out of event read loop --> then fails outer while loop eval
					shutdown_requested = 1;
					break;	
				}
			}
	
			// Kernel overflowed with events
			else if (event->mask & IN_Q_OVERFLOW) {
				
				// Step 1: Drain event queue (read until empty / EAGAIN / EWOULDNOTBLOCK)
				while (read(ininst_fd, ebuf, EVENT_BATCH_COUNT * event_size) > 0) {
					// Deliberately drain all events
				}

				// Step 2: Clear current inotify tracking tree
				// Also clear wddir and cookie_event hashmaps
				hm_delete_all_wddir(ininst_fd, 1);
				hm_delete_all_cookie_event();
				
				// Step 3: Rebuild inotify tracking tree
				if (watch_tree(watch_root, ininst_fd) == DSYNC_ERROR)
					syslog(LOG_ERR, "Error rescanning root watch directory after IN_Q_OVERFLOW event");

				// Exit the event queue read loop --> Go back to polling ininst_fd
				syslog(LOG_WARNING, "Inotify event queue overflow. Watch tree reset");
				break;
			}

			// Watched directory was unmounted --> remove from hashmap
			else if (event->mask & IN_UNMOUNT) {
				syslog(LOG_NOTICE, "Dir unmounted: %s", watched_dir->path);
				
				// Do not call inotify_rm_watch(): '0'  --> wd was already removed by kernel
				hm_delete_wddir(watched_dir, ininst_fd, 0);
			}

			// Ignore IN_IGNORED --> Respective behavior/cleanup is handled by other events	
		
			i += sizeof(struct inotify_event) + event->len;
		}	
	}

	
	hm_delete_all_wddir(ininst_fd, 1);	// Frees all wddir_hm entries & removes all wd from kernel's inotify instance
	hm_delete_all_cookie_event();		// Frees all cookie_event entries
	close(ininst_fd);			// Closes inotify instance

	free_blacklist();
	free(watch_root);
	free(backup_root);

	// Closes connection to system log
	closelog();
	return return_status;	
}

