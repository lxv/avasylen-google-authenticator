// PAM module for two-factor authentication.
//
// Copyright 2010 Google Inc.
// Author: Markus Gutschke
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifdef linux
// We much rather prefer to use setfsuid(), but this function is unfortunately
// not available on all systems.
#include <sys/fsuid.h>
#define HAS_SETFSUID
#endif

#ifndef PAM_EXTERN
#define PAM_EXTERN
#endif

#if !defined(LOG_AUTHPRIV) && defined(LOG_AUTH)
#define LOG_AUTHPRIV LOG_AUTH
#endif

#define PAM_SM_AUTH
#include <security/pam_appl.h>
#include <security/pam_modules.h>

#include "base32.h"
#include "hmac.h"
#include "sha1.h"

#define MODULE_NAME "pam_google_authenticator"
#define SECRET      "~/.google_authenticator"

typedef struct Params {
  const char *secret_filename_spec;
  enum { NULLERR=0, NULLOK, SECRETNOTFOUND } nullok;
  int        noskewadj;
  int        echocode;
  int        fixed_uid;
  uid_t      uid;
  enum { PROMPT = 0, TRY_FIRST_PASS, USE_FIRST_PASS } pass_mode;
  int        forward_pass;
} Params;

static char oom;

#if defined(DEMO) || defined(TESTING)
static char error_msg[128];

const char *get_error_msg(void) __attribute__((visibility("default")));
const char *get_error_msg(void) {
  return error_msg;
}
#endif

static void log_message(int priority, pam_handle_t *pamh,
                        const char *format, ...) {
  char *service = NULL;
  if (pamh)
    pam_get_item(pamh, PAM_SERVICE, (void *)&service);
  if (!service)
    service = "";

  char logname[80];
  snprintf(logname, sizeof(logname), "%s(" MODULE_NAME ")", service);

  va_list args;
  va_start(args, format);
#if !defined(DEMO) && !defined(TESTING)
  openlog(logname, LOG_CONS | LOG_PID, LOG_AUTHPRIV);
  vsyslog(priority, format, args);
  closelog();
#else
  if (!*error_msg) {
    vsnprintf(error_msg, sizeof(error_msg), format, args);
  }
#endif

  va_end(args);

  if (priority == LOG_EMERG) {
    // Something really bad happened. There is no way we can proceed safely.
    _exit(1);
  }
}

static int converse(pam_handle_t *pamh, int nargs,
                    const struct pam_message **message,
                    struct pam_response **response) {
  struct pam_conv *conv;
  int retval = pam_get_item(pamh, PAM_CONV, (void *)&conv);
  if (retval != PAM_SUCCESS) {
    return retval;
  }
  return conv->conv(nargs, message, response, conv->appdata_ptr);
}

static const char *get_user_name(pam_handle_t *pamh) {
  // Obtain the user's name
  const char *username;
  if (pam_get_item(pamh, PAM_USER, (void *)&username) != PAM_SUCCESS ||
      !username || !*username) {
    log_message(LOG_ERR, pamh,
                "No user name available when checking verification code");
    return NULL;
  }
  return username;
}

static char *get_secret_filename(pam_handle_t *pamh, const Params *params,
                                 const char *username, int *uid) {
  // Check whether the administrator decided to override the default location
  // for the secret file.
  const char *spec = params->secret_filename_spec
    ? params->secret_filename_spec : SECRET;

  // Obtain the user's id and home directory
  struct passwd pwbuf, *pw = NULL;
  char *buf = NULL;
  char *secret_filename = NULL;
  if (!params->fixed_uid) {
    #ifdef _SC_GETPW_R_SIZE_MAX
    int len = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (len <= 0) {
      len = 4096;
    }
    #else
    int len = 4096;
    #endif
    buf = malloc(len);
    *uid = -1;
    if (buf == NULL ||
        getpwnam_r(username, &pwbuf, buf, len, &pw) ||
        !pw ||
        !pw->pw_dir ||
        *pw->pw_dir != '/') {
    err:
      log_message(LOG_ERR, pamh, "Failed to compute location of secret file");
      free(buf);
      free(secret_filename);
      return NULL;
    }
  }

  // Expand filename specification to an actual filename.
  if ((secret_filename = strdup(spec)) == NULL) {
    goto err;
  }
  int allow_tilde = 1;
  for (int offset = 0; secret_filename[offset];) {
    char *cur = secret_filename + offset;
    char *var = NULL;
    size_t var_len = 0;
    const char *subst = NULL;
    if (allow_tilde && *cur == '~') {
      var_len = 1;
      if (!pw) {
        goto err;
      }
      subst = pw->pw_dir;
      var = cur;
    } else if (secret_filename[offset] == '$') {
      if (!memcmp(cur, "${HOME}", 7)) {
        var_len = 7;
        if (!pw) {
          goto err;
        }
        subst = pw->pw_dir;
        var = cur;
      } else if (!memcmp(cur, "${USER}", 7)) {
        var_len = 7;
        subst = username;
        var = cur;
      }
    }
    if (var) {
      size_t subst_len = strlen(subst);
      char *resized = realloc(secret_filename,
                              strlen(secret_filename) + subst_len);
      if (!resized) {
        goto err;
      }
      var += resized - secret_filename;
      secret_filename = resized;
      memmove(var + subst_len, var + var_len, strlen(var + var_len) + 1);
      memmove(var, subst, subst_len);
      offset = var + subst_len - resized;
      allow_tilde = 0;
    } else {
      allow_tilde = *cur == '/';
      ++offset;
    }
  }

  *uid = params->fixed_uid ? params->uid : pw->pw_uid;
  free(buf);
  return secret_filename;
}

static int setuser(int uid) {
#ifdef HAS_SETFSUID
  // The semantics for setfsuid() are a little unusual. On success, the
  // previous user id is returned. On failure, the current user id is returned.
  int old_uid = setfsuid(uid);
  if (uid != setfsuid(uid)) {
    setfsuid(old_uid);
    return -1;
  }
#else
  int old_uid = geteuid();
  if (old_uid != uid && seteuid(uid)) {
    return -1;
  }
#endif
  return old_uid;
}

static int setgroup(int gid) {
#ifdef HAS_SETFSUID
  // The semantics of setfsgid() are a little unusual. On success, the
  // previous group id is returned. On failure, the current groupd id is
  // returned.
  int old_gid = setfsgid(gid);
  if (gid != setfsgid(gid)) {
    setfsgid(old_gid);
    return -1;
  }
#else
  int old_gid = getegid();
  if (old_gid != gid && setegid(gid)) {
    return -1;
  }
#endif
  return old_gid;
}

static int drop_privileges(pam_handle_t *pamh, const char *username, int uid,
                           int *old_uid, int *old_gid) {
  // Try to become the new user. This might be necessary for NFS mounted home
  // directories.

  // First, look up the user's default group
  #ifdef _SC_GETPW_R_SIZE_MAX
  int len = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (len <= 0) {
    len = 4096;
  }
  #else
  int len = 4096;
  #endif
  char *buf = malloc(len);
  if (!buf) {
    log_message(LOG_ERR, pamh, "Out of memory");
    return -1;
  }
  struct passwd pwbuf, *pw;
  if (getpwuid_r(uid, &pwbuf, buf, len, &pw) || !pw) {
    log_message(LOG_ERR, pamh, "Cannot look up user id %d", uid);
    free(buf);
    return -1;
  }
  gid_t gid = pw->pw_gid;
  free(buf);

  int gid_o = setgroup(gid);
  int uid_o = setuser(uid);
  if (uid_o < 0) {
    if (gid_o >= 0) {
      if (setgroup(gid_o) < 0 || setgroup(gid_o) != gid_o) {
        // Inform the caller that we were unsuccessful in resetting the group.
        *old_gid = gid_o;
      }
    }
    log_message(LOG_ERR, pamh, "Failed to change user id to \"%s\"",
                username);
    return -1;
  }
  if (gid_o < 0 && (gid_o = setgroup(gid)) < 0) {
    // In most typical use cases, the PAM module will end up being called
    // while uid=0. This allows the module to change to an arbitrary group
    // prior to changing the uid. But there are many ways that PAM modules
    // can be invoked and in some scenarios this might not work. So, we also
    // try changing the group _after_ changing the uid. It might just work.
    if (setuser(uid_o) < 0 || setuser(uid_o) != uid_o) {
      // Inform the caller that we were unsuccessful in resetting the uid.
      *old_uid = uid_o;
    }
    log_message(LOG_ERR, pamh,
                "Failed to change group id for user \"%s\" to %d", username,
                (int)gid);
    return -1;
  }

  *old_uid = uid_o;
  *old_gid = gid_o;
  return 0;
}

static int open_secret_file(pam_handle_t *pamh, const char *secret_filename,
                            struct Params *params, const char *username,
                            int uid, off_t *size, time_t *mtime) {
  // Try to open "~/.google_authenticator"
  *size = 0;
  *mtime = 0;
  int fd = open(secret_filename, O_RDONLY);
  struct stat sb;
  if (fd < 0 ||
      fstat(fd, &sb) < 0) {
    if (params->nullok != NULLERR && errno == ENOENT) {
      // The user doesn't have a state file, but the admininistrator said
      // that this is OK. We still return an error from open_secret_file(),
      // but we remember that this was the result of a missing state file.
      params->nullok = SECRETNOTFOUND;
    } else {
      log_message(LOG_ERR, pamh, "Failed to read \"%s\"", secret_filename);
    }
 error:
    if (fd >= 0) {
      close(fd);
    }
    return -1;
  }

  // Check permissions on "~/.google_authenticator"
  if ((sb.st_mode & 03577) != 0400 ||
      !S_ISREG(sb.st_mode) ||
      sb.st_uid != (uid_t)uid) {
    char buf[80];
    if (params->fixed_uid) {
      sprintf(buf, "user id %d", params->uid);
      username = buf;
    }
    log_message(LOG_ERR, pamh,
                "Secret file \"%s\" must only be accessible by %s",
                secret_filename, username);
    goto error;
  }

  // Sanity check for file length
  if (sb.st_size < 1 || sb.st_size > 64*1024) {
    log_message(LOG_ERR, pamh,
                "Invalid file size for \"%s\"", secret_filename);
    goto error;
  }

  *size = sb.st_size;
  *mtime = sb.st_mtime;
  return fd;
}

static char *read_file_contents(pam_handle_t *pamh,
                                const char *secret_filename, int *fd,
                                off_t filesize) {
  // Read file contents
  char *buf = malloc(filesize + 1);
  if (!buf ||
      read(*fd, buf, filesize) != filesize) {
    close(*fd);
    *fd = -1;
    log_message(LOG_ERR, pamh, "Could not read \"%s\"", secret_filename);
 error:
    if (buf) {
      memset(buf, 0, filesize);
      free(buf);
    }
    return NULL;
  }
  close(*fd);
  *fd = -1;

  // The rest of the code assumes that there are no NUL bytes in the file.
  if (memchr(buf, 0, filesize)) {
    log_message(LOG_ERR, pamh, "Invalid file contents in \"%s\"",
                secret_filename);
    goto error;
  }

  // Terminate the buffer with a NUL byte.
  buf[filesize] = '\000';

  return buf;
}

static int is_totp(const char *buf) {
  return !!strstr(buf, "\" TOTP_AUTH");
}

static int write_file_contents(pam_handle_t *pamh, const char *secret_filename,
                               off_t old_size, time_t old_mtime,
                               const char *buf) {
  // Safely overwrite the old secret file.
  char *tmp_filename = malloc(strlen(secret_filename) + 2);
  if (tmp_filename == NULL) {
 removal_failure:
    log_message(LOG_ERR, pamh, "Failed to update secret file \"%s\"",
                secret_filename);
    return -1;
  }

  strcat(strcpy(tmp_filename, secret_filename), "~");
  int fd = open(tmp_filename,
                O_WRONLY|O_CREAT|O_NOFOLLOW|O_TRUNC|O_EXCL, 0400);
  if (fd < 0) {
    free(tmp_filename);
    goto removal_failure;
  }

  // Make sure the secret file is still the same. This prevents attackers
  // from opening a lot of pending sessions and then reusing the same
  // scratch code multiple times.
  struct stat sb;
  if (stat(secret_filename, &sb) != 0 ||
      sb.st_size != old_size ||
      sb.st_mtime != old_mtime) {
    log_message(LOG_ERR, pamh,
                "Secret file \"%s\" changed while trying to use "
                "scratch code\n", secret_filename);
    unlink(tmp_filename);
    free(tmp_filename);
    close(fd);
    return -1;
  }

  // Write the new file contents
  if (write(fd, buf, strlen(buf)) != (ssize_t)strlen(buf) ||
      rename(tmp_filename, secret_filename) != 0) {
    unlink(tmp_filename);
    free(tmp_filename);
    close(fd);
    goto removal_failure;
  }

  free(tmp_filename);
  close(fd);

  return 0;
}

static uint8_t *get_shared_secret(pam_handle_t *pamh,
                                  const char *secret_filename,
                                  const char *buf, int *secretLen) {
  // Decode secret key
  int base32Len = strcspn(buf, "\n");
  *secretLen = (base32Len*5 + 7)/8;
  uint8_t *secret = malloc(base32Len + 1);
  if (secret == NULL) {
    *secretLen = 0;
    return NULL;
  }
  memcpy(secret, buf, base32Len);
  secret[base32Len] = '\000';
  if ((*secretLen = base32_decode(secret, secret, base32Len)) < 1) {
    log_message(LOG_ERR, pamh,
                "Could not find a valid BASE32 encoded secret in \"%s\"",
                secret_filename);
    memset(secret, 0, base32Len);
    free(secret);
    return NULL;
  }
  memset(secret + *secretLen, 0, base32Len + 1 - *secretLen);
  return secret;
}

#ifdef TESTING
static time_t current_time;
void set_time(time_t t) __attribute__((visibility("default")));
void set_time(time_t t) {
  current_time = t;
}

static time_t get_time(void) {
  return current_time;
}
#else
static time_t get_time(void) {
  return time(NULL);
}
#endif

static int get_timestamp(void) {
  return get_time()/30;
}

static int comparator(const void *a, const void *b) {
  return *(unsigned int *)a - *(unsigned int *)b;
}

static char *get_cfg_value(pam_handle_t *pamh, const char *key,
                           const char *buf) {
  size_t key_len = strlen(key);
  for (const char *line = buf; *line; ) {
    const char *ptr;
    if (line[0] == '"' && line[1] == ' ' && !memcmp(line+2, key, key_len) &&
        (!*(ptr = line+2+key_len) || *ptr == ' ' || *ptr == '\t' ||
         *ptr == '\r' || *ptr == '\n')) {
      ptr += strspn(ptr, " \t");
      size_t val_len = strcspn(ptr, "\r\n");
      char *val = malloc(val_len + 1);
      if (!val) {
        log_message(LOG_ERR, pamh, "Out of memory");
        return &oom;
      } else {
        memcpy(val, ptr, val_len);
        val[val_len] = '\000';
        return val;
      }
    } else {
      line += strcspn(line, "\r\n");
      line += strspn(line, "\r\n");
    }
  }
  return NULL;
}

static int set_cfg_value(pam_handle_t *pamh, const char *key, const char *val,
                         char **buf) {
  size_t key_len = strlen(key);
  char *start = NULL;
  char *stop = NULL;

  // Find an existing line, if any.
  for (char *line = *buf; *line; ) {
    char *ptr;
    if (line[0] == '"' && line[1] == ' ' && !memcmp(line+2, key, key_len) &&
        (!*(ptr = line+2+key_len) || *ptr == ' ' || *ptr == '\t' ||
         *ptr == '\r' || *ptr == '\n')) {
      start = line;
      stop  = start + strcspn(start, "\r\n");
      stop += strspn(stop, "\r\n");
      break;
    } else {
      line += strcspn(line, "\r\n");
      line += strspn(line, "\r\n");
    }
  }

  // If no existing line, insert immediately after the first line.
  if (!start) {
    start  = *buf + strcspn(*buf, "\r\n");
    start += strspn(start, "\r\n");
    stop   = start;
  }

  // Replace [start..stop] with the new contents.
  size_t val_len = strlen(val);
  size_t total_len = key_len + val_len + 4;
  if (total_len <= stop - start) {
    // We are decreasing out space requirements. Shrink the buffer and pad with
    // NUL characters.
    size_t tail_len = strlen(stop);
    memmove(start + total_len, stop, tail_len + 1);
    memset(start + total_len + tail_len, 0, stop - start - total_len + 1);
  } else {
    // Must resize existing buffer. We cannot call realloc(), as it could
    // leave parts of the buffer content in unused parts of the heap.
    size_t buf_len = strlen(*buf);
    size_t tail_len = buf_len - (stop - *buf);
    char *resized = malloc(buf_len - (stop - start) + total_len + 1);
    if (!resized) {
      log_message(LOG_ERR, pamh, "Out of memory");
      return -1;
    }
    memcpy(resized, *buf, start - *buf);
    memcpy(resized + (start - *buf) + total_len, stop, tail_len + 1);
    memset(*buf, 0, buf_len);
    free(*buf);
    start = start - *buf + resized;
    *buf = resized;
  }

  // Fill in new contents.
  start[0] = '"';
  start[1] = ' ';
  memcpy(start + 2, key, key_len);
  start[2+key_len] = ' ';
  memcpy(start+3+key_len, val, val_len);
  start[3+key_len+val_len] = '\n';

  // Check if there are any other occurrences of "value". If so, delete them.
  for (char *line = start + 4 + key_len + val_len; *line; ) {
    char *ptr;
    if (line[0] == '"' && line[1] == ' ' && !memcmp(line+2, key, key_len) &&
        (!*(ptr = line+2+key_len) || *ptr == ' ' || *ptr == '\t' ||
         *ptr == '\r' || *ptr == '\n')) {
      start = line;
      stop = start + strcspn(start, "\r\n");
      stop += strspn(stop, "\r\n");
      size_t tail_len = strlen(stop);
      memmove(start, stop, tail_len + 1);
      memset(start + tail_len, 0, stop - start);
      line = start;
    } else {
      line += strcspn(line, "\r\n");
      line += strspn(line, "\r\n");
    }
  }

  return 0;
}

static long get_hotp_counter(pam_handle_t *pamh, const char *buf) {
  const char *counter_str = get_cfg_value(pamh, "HOTP_COUNTER", buf);
  if (counter_str == &oom) {
    // Out of memory. This is a fatal error
    return -1;
  }

  long counter = 0;
  if (counter_str) {
    counter = strtol(counter_str, NULL, 10);
  }
  free((void *)counter_str);

  return counter;
}

static int rate_limit(pam_handle_t *pamh, const char *secret_filename,
                      int *updated, char **buf) {
  const char *value = get_cfg_value(pamh, "RATE_LIMIT", *buf);
  if (!value) {
    // Rate limiting is not enabled for this account
    return 0;
  } else if (value == &oom) {
    // Out of memory. This is a fatal error.
    return -1;
  }

  // Parse both the maximum number of login attempts and the time interval
  // that we are looking at.
  const char *endptr = value, *ptr;
  int attempts, interval;
  errno = 0;
  if (((attempts = (int)strtoul(ptr = endptr, (char **)&endptr, 10)) < 1) ||
      ptr == endptr ||
      attempts > 100 ||
      errno ||
      (*endptr != ' ' && *endptr != '\t') ||
      ((interval = (int)strtoul(ptr = endptr, (char **)&endptr, 10)) < 1) ||
      ptr == endptr ||
      interval > 3600 ||
      errno) {
    free((void *)value);
    log_message(LOG_ERR, pamh, "Invalid RATE_LIMIT option. Check \"%s\".",
                secret_filename);
    return -1;
  }

  // Parse the time stamps of all previous login attempts.
  unsigned int now = get_time();
  unsigned int *timestamps = malloc(sizeof(int));
  if (!timestamps) {
  oom:
    free((void *)value);
    log_message(LOG_ERR, pamh, "Out of memory");
    return -1;
  }
  timestamps[0] = now;
  int num_timestamps = 1;
  while (*endptr && *endptr != '\r' && *endptr != '\n') {
    unsigned int timestamp;
    errno = 0;
    if ((*endptr != ' ' && *endptr != '\t') ||
        ((timestamp = (int)strtoul(ptr = endptr, (char **)&endptr, 10)),
         errno) ||
        ptr == endptr) {
      free((void *)value);
      free(timestamps);
      log_message(LOG_ERR, pamh, "Invalid list of timestamps in RATE_LIMIT. "
                  "Check \"%s\".", secret_filename);
      return -1;
    }
    num_timestamps++;
    unsigned int *tmp = (unsigned int *)realloc(timestamps,
                                                sizeof(int) * num_timestamps);
    if (!tmp) {
      free(timestamps);
      goto oom;
    }
    timestamps = tmp;
    timestamps[num_timestamps-1] = timestamp;
  }
  free((void *)value);
  value = NULL;

  // Sort time stamps, then prune all entries outside of the current time
  // interval.
  qsort(timestamps, num_timestamps, sizeof(int), comparator);
  int start = 0, stop = -1;
  for (int i = 0; i < num_timestamps; ++i) {
    if (timestamps[i] < now - interval) {
      start = i+1;
    } else if (timestamps[i] > now) {
      break;
    }
    stop = i;
  }

  // Error out, if there are too many login attempts.
  int exceeded = 0;
  if (stop - start + 1 > attempts) {
    exceeded = 1;
    start = stop - attempts + 1;
  }

  // Construct new list of timestamps within the current time interval.
  char *list = malloc(25 * (2 + (stop - start + 1)) + 4);
  if (!list) {
    free(timestamps);
    goto oom;
  }
  sprintf(list, "%d %d", attempts, interval);
  char *prnt = strchr(list, '\000');
  for (int i = start; i <= stop; ++i) {
    prnt += sprintf(prnt, " %u", timestamps[i]);
  }
  free(timestamps);

  // Try to update RATE_LIMIT line.
  if (set_cfg_value(pamh, "RATE_LIMIT", list, buf) < 0) {
    free(list);
    return -1;
  }
  free(list);

  // Mark the state file as changed.
  *updated = 1;

  // If necessary, notify the user of the rate limiting that is in effect.
  if (exceeded) {
    log_message(LOG_ERR, pamh,
                "Too many concurrent login attempts. Please try again.");
    return -1;
  }

  return 0;
}

static char *get_first_pass(pam_handle_t *pamh) {
  const void *password = NULL;
  if (pam_get_item(pamh, PAM_AUTHTOK, &password) == PAM_SUCCESS &&
      password) {
    return strdup((const char *)password);
  }
  return NULL;
}

static char *request_pass(pam_handle_t *pamh, int echocode,
                          const char *prompt) {
  // Query user for verification code
  const struct pam_message msg = { .msg_style = echocode,
                                   .msg       = prompt };
  const struct pam_message *msgs = &msg;
  struct pam_response *resp = NULL;
  int retval = converse(pamh, 1, &msgs, &resp);
  char *ret = NULL;
  if (retval != PAM_SUCCESS || resp == NULL || resp->resp == NULL ||
      *resp->resp == '\000') {
    log_message(LOG_ERR, pamh, "Did not receive verification code from user");
    if (retval == PAM_SUCCESS && resp && resp->resp) {
      ret = resp->resp;
    }
  } else {
    ret = resp->resp;
  }

  // Deallocate temporary storage
  if (resp) {
    if (!ret) {
      free(resp->resp);
    }
    free(resp);
  }

  return ret;
}

/* Checks for possible use of scratch codes. Returns -1 on error, 0 on success,
 * and 1, if no scratch code had been entered, and subsequent tests should be
 * applied.
 */
static int check_scratch_codes(pam_handle_t *pamh, const char *secret_filename,
                               int *updated, char *buf, int code) {
  // Skip the first line. It contains the shared secret.
  char *ptr = buf + strcspn(buf, "\n");

  // Check if this is one of the scratch codes
  char *endptr = NULL;
  for (;;) {
    // Skip newlines and blank lines
    while (*ptr == '\r' || *ptr == '\n') {
      ptr++;
    }

    // Skip any lines starting with double-quotes. They contain option fields
    if (*ptr == '"') {
      ptr += strcspn(ptr, "\n");
      continue;
    }

    // Try to interpret the line as a scratch code
    errno = 0;
    int scratchcode = (int)strtoul(ptr, &endptr, 10);

    // Sanity check that we read a valid scratch code. Scratchcodes are all
    // numeric eight-digit codes. There must not be any other information on
    // that line.
    if (errno ||
        ptr == endptr ||
        (*endptr != '\r' && *endptr != '\n' && *endptr) ||
        scratchcode  <  10*1000*1000 ||
        scratchcode >= 100*1000*1000) {
      break;
    }

    // Check if the code matches
    if (scratchcode == code) {
      // Remove scratch code after using it
      while (*endptr == '\n' || *endptr == '\r') {
        ++endptr;
      }
      memmove(ptr, endptr, strlen(endptr) + 1);
      memset(strrchr(ptr, '\000'), 0, endptr - ptr + 1);

      // Mark the state file as changed
      *updated = 1;

      // Successfully removed scratch code. Allow user to log in.
      return 0;
    }
    ptr = endptr;
  }

  // No scratch code has been used. Continue checking other types of codes.
  return 1;
}

static int window_size(pam_handle_t *pamh, const char *secret_filename,
                       const char *buf) {
  const char *value = get_cfg_value(pamh, "WINDOW_SIZE", buf);
  if (!value) {
    // Default window size is 3. This gives us one 30s window before and
    // after the current one.
    return 3;
  } else if (value == &oom) {
    // Out of memory. This is a fatal error.
    return 0;
  }

  char *endptr;
  errno = 0;
  int window = (int)strtoul(value, &endptr, 10);
  if (errno || !*value || value == endptr ||
      (*endptr && *endptr != ' ' && *endptr != '\t' &&
       *endptr != '\n' && *endptr != '\r') ||
      window < 1 || window > 100) {
    free((void *)value);
    log_message(LOG_ERR, pamh, "Invalid WINDOW_SIZE option in \"%s\"",
                secret_filename);
    return 0;
  }
  free((void *)value);
  return window;
}

/* If the DISALLOW_REUSE option has been set, record timestamps have been
 * used to log in successfully and disallow their reuse.
 *
 * Returns -1 on error, and 0 on success.
 */
static int invalidate_timebased_code(int tm, pam_handle_t *pamh,
                                     const char *secret_filename,
                                     int *updated, char **buf) {
  char *disallow = get_cfg_value(pamh, "DISALLOW_REUSE", *buf);
  if (!disallow) {
    // Reuse of tokens is not explicitly disallowed. Allow the login request
    // to proceed.
    return 0;
  } else if (disallow == &oom) {
    // Out of memory. This is a fatal error.
    return -1;
  }

  // Allow the user to customize the window size parameter.
  int window = window_size(pamh, secret_filename, *buf);
  if (!window) {
    // The user configured a non-standard window size, but there was some
    // error with the value of this parameter.
    free((void *)disallow);
    return -1;
  }

  // The DISALLOW_REUSE option is followed by all known timestamps that are
  // currently unavailable for login.
  for (char *ptr = disallow; *ptr;) {
    // Skip white-space, if any
    ptr += strspn(ptr, " \t\r\n");
    if (!*ptr) {
      break;
    }

    // Parse timestamp value.
    char *endptr;
    errno = 0;
    int blocked = (int)strtoul(ptr, &endptr, 10);

    // Treat syntactically invalid options as an error
    if (errno ||
        ptr == endptr ||
        (*endptr != ' ' && *endptr != '\t' &&
         *endptr != '\r' && *endptr != '\n' && *endptr)) {
      free((void *)disallow);
      return -1;
    }

    if (tm == blocked) {
      // The code is currently blocked from use. Disallow login.
      free((void *)disallow);
      log_message(LOG_ERR, pamh,
                  "Trying to reuse a previously used time-based code. "
                  "Retry again in 30 seconds. "
                  "Warning! This might mean, you are currently subject to a "
                  "man-in-the-middle attack.");
      return -1;
    }

    // If the blocked code is outside of the possible window of timestamps,
    // remove it from the file.
    if (blocked - tm >= window || tm - blocked >= window) {
      endptr += strspn(endptr, " \t");
      memmove(ptr, endptr, strlen(endptr) + 1);
    } else {
      ptr = endptr;
    }
  }

  // Add the current timestamp to the list of disallowed timestamps.
  char *resized = realloc(disallow, strlen(disallow) + 40);
  if (!resized) {
    free((void *)disallow);
    log_message(LOG_ERR, pamh,
                "Failed to allocate memory when updating \"%s\"",
                secret_filename);
    return -1;
  }
  disallow = resized;
  sprintf(strrchr(disallow, '\000'), " %d" + !*disallow, tm);
  if (set_cfg_value(pamh, "DISALLOW_REUSE", disallow, buf) < 0) {
    free((void *)disallow);
    return -1;
  }
  free((void *)disallow);

  // Mark the state file as changed
  *updated = 1;

  // Allow access.
  return 0;
}

/* Given an input value, this function computes the hash code that forms the
 * expected authentication token.
 */
#ifdef TESTING
int compute_code(const uint8_t *secret, int secretLen, unsigned long value)
  __attribute__((visibility("default")));
#else
static
#endif
int compute_code(const uint8_t *secret, int secretLen, unsigned long value) {
  uint8_t val[8];
  for (int i = 8; i--; value >>= 8) {
    val[i] = value;
  }
  uint8_t hash[SHA1_DIGEST_LENGTH];
  hmac_sha1(secret, secretLen, val, 8, hash, SHA1_DIGEST_LENGTH);
  memset(val, 0, sizeof(val));
  int offset = hash[SHA1_DIGEST_LENGTH - 1] & 0xF;
  unsigned int truncatedHash = 0;
  for (int i = 0; i < 4; ++i) {
    truncatedHash <<= 8;
    truncatedHash  |= hash[offset + i];
  }
  memset(hash, 0, sizeof(hash));
  truncatedHash &= 0x7FFFFFFF;
  truncatedHash %= 1000000;
  return truncatedHash;
}

/* If a user repeated attempts to log in with the same time skew, remember
 * this skew factor for future login attempts.
 */
static int check_time_skew(pam_handle_t *pamh, const char *secret_filename,
                           int *updated, char **buf, int skew, int tm) {
  int rc = -1;

  // Parse current RESETTING_TIME_SKEW line, if any.
  char *resetting = get_cfg_value(pamh, "RESETTING_TIME_SKEW", *buf);
  if (resetting == &oom) {
    // Out of memory. This is a fatal error.
    return -1;
  }

  // If the user can produce a sequence of three consecutive codes that fall
  // within a day of the current time. And if he can enter these codes in
  // quick succession, then we allow the time skew to be reset.
  // N.B. the number "3" was picked so that it would not trigger the rate
  // limiting limit if set up with default parameters.
  unsigned int tms[3];
  int skews[sizeof(tms)/sizeof(int)];

  int num_entries = 0;
  if (resetting) {
    char *ptr = resetting;

    // Read the three most recent pairs of time stamps and skew values into
    // our arrays.
    while (*ptr && *ptr != '\r' && *ptr != '\n') {
      char *endptr;
      errno = 0;
      unsigned int i = (int)strtoul(ptr, &endptr, 10);
      if (errno || ptr == endptr || (*endptr != '+' && *endptr != '-')) {
        break;
      }
      ptr = endptr;
      int j = (int)strtoul(ptr + 1, &endptr, 10);
      if (errno ||
          ptr == endptr ||
          (*endptr != ' ' && *endptr != '\t' &&
           *endptr != '\r' && *endptr != '\n' && *endptr)) {
        break;
      }
      if (*ptr == '-') {
        j = -j;
      }
      if (num_entries == sizeof(tms)/sizeof(int)) {
        memmove(tms, tms+1, sizeof(tms)-sizeof(int));
        memmove(skews, skews+1, sizeof(skews)-sizeof(int));
      } else {
        ++num_entries;
      }
      tms[num_entries-1]   = i;
      skews[num_entries-1] = j;
      ptr = endptr;
    }

    // If the user entered an identical code, assume they are just getting
    // desperate. This doesn't actually provide us with any useful data,
    // though. Don't change any state and hope the user keeps trying a few
    // more times.
    if (num_entries &&
        tm + skew == tms[num_entries-1] + skews[num_entries-1]) {
      free((void *)resetting);
      return -1;
    }
  }
  free((void *)resetting);

  // Append new timestamp entry
  if (num_entries == sizeof(tms)/sizeof(int)) {
    memmove(tms, tms+1, sizeof(tms)-sizeof(int));
    memmove(skews, skews+1, sizeof(skews)-sizeof(int));
  } else {
    ++num_entries;
  }
  tms[num_entries-1]   = tm;
  skews[num_entries-1] = skew;

  // Check if we have the required amount of valid entries.
  if (num_entries == sizeof(tms)/sizeof(int)) {
    unsigned int last_tm = tms[0];
    int last_skew = skews[0];
    int avg_skew = last_skew;
    for (int i = 1; i < sizeof(tms)/sizeof(int); ++i) {
      // Check that we have a consecutive sequence of timestamps with no big
      // gaps in between. Also check that the time skew stays constant. Allow
      // a minor amount of fuzziness on all parameters.
      if (tms[i] <= last_tm || tms[i] > last_tm+2 ||
          last_skew - skew < -1 || last_skew - skew > 1) {
        goto keep_trying;
      }
      last_tm   = tms[i];
      last_skew = skews[i];
      avg_skew += last_skew;
    }
    avg_skew /= (int)(sizeof(tms)/sizeof(int));

    // The user entered the required number of valid codes in quick
    // succession. Establish a new valid time skew for all future login
    // attempts.
    char time_skew[40];
    sprintf(time_skew, "%d", avg_skew);
    if (set_cfg_value(pamh, "TIME_SKEW", time_skew, buf) < 0) {
      return -1;
    }
    rc = 0;
  keep_trying:;
  }

  // Set the new RESETTING_TIME_SKEW line, while the user is still trying
  // to reset the time skew.
  char reset[80 * (sizeof(tms)/sizeof(int))];
  *reset = '\000';
  if (rc) {
    for (int i = 0; i < num_entries; ++i) {
      sprintf(strrchr(reset, '\000'), " %d%+d" + !*reset, tms[i], skews[i]);
    }
  }
  if (set_cfg_value(pamh, "RESETTING_TIME_SKEW", reset, buf) < 0) {
    return -1;
  }

  // Mark the state file as changed
  *updated = 1;

  return rc;
}

/* Checks for time based verification code. Returns -1 on error, 0 on success,
 * and 1, if no time based code had been entered, and subsequent tests should
 * be applied.
 */
static int check_timebased_code(pam_handle_t *pamh, const char*secret_filename,
                                int *updated, char **buf, const uint8_t*secret,
                                int secretLen, int code, Params *params) {
  if (!is_totp(*buf)) {
    // The secret file does not actually contain information for a time-based
    // code. Return to caller and see if any other authentication methods
    // apply.
    return 1;
  }

  if (code < 0 || code >= 1000000) {
    // All time based verification codes are no longer than six digits.
    return 1;
  }

  // Compute verification codes and compare them with user input
  const int tm = get_timestamp();
  const char *skew_str = get_cfg_value(pamh, "TIME_SKEW", *buf);
  if (skew_str == &oom) {
    // Out of memory. This is a fatal error
    return -1;
  }

  int skew = 0;
  if (skew_str) {
    skew = (int)strtol(skew_str, NULL, 10);
  }
  free((void *)skew_str);

  int window = window_size(pamh, secret_filename, *buf);
  if (!window) {
    return -1;
  }
  for (int i = -((window-1)/2); i <= window/2; ++i) {
    unsigned int hash = compute_code(secret, secretLen, tm + skew + i);
    if (hash == (unsigned int)code) {
      return invalidate_timebased_code(tm + skew + i, pamh, secret_filename,
                                       updated, buf);
    }
  }

  if (!params->noskewadj) {
    // The most common failure mode is for the clocks to be insufficiently
    // synchronized. We can detect this and store a skew value for future
    // use.
    skew = 1000000;
    for (int i = 0; i < 25*60; ++i) {
      unsigned int hash = compute_code(secret, secretLen, tm - i);
      if (hash == (unsigned int)code && skew == 1000000) {
        // Don't short-circuit out of the loop as the obvious difference in
        // computation time could be a signal that is valuable to an attacker.
        skew = -i;
      }
      hash = compute_code(secret, secretLen, tm + i);
      if (hash == (unsigned int)code && skew == 1000000) {
        skew = i;
      }
    }
    if (skew != 1000000) {
      return check_time_skew(pamh, secret_filename, updated, buf, skew, tm);
    }
  }

  return 1;
}

/* Checks for counter based verification code. Returns -1 on error, 0 on
 * success, and 1, if no counter based code had been entered, and subsequent
 * tests should be applied.
 */
static int check_counterbased_code(pam_handle_t *pamh,
                                   const char*secret_filename, int *updated,
                                   char **buf, const uint8_t*secret,
                                   int secretLen, int code, Params *params,
                                   long hotp_counter,
                                   int *must_advance_counter) {
  if (hotp_counter < 1) {
    // The secret file did not actually contain information for a counter-based
    // code. Return to caller and see if any other authentication methods
    // apply.
    return 1;
  }

  if (code < 0 || code >= 1000000) {
    // All counter based verification codes are no longer than six digits.
    return 1;
  }

  // Compute [window_size] verification codes and compare them with user input.
  // Future codes are allowed in case the user computed but did not use a code.
  int window = window_size(pamh, secret_filename, *buf);
  if (!window) {
    return -1;
  }
  for (int i = 0; i < window; ++i) {
    unsigned int hash = compute_code(secret, secretLen, hotp_counter + i);
    if (hash == (unsigned int)code) {
      char counter_str[40];
      sprintf(counter_str, "%ld", hotp_counter + i + 1);
      if (set_cfg_value(pamh, "HOTP_COUNTER", counter_str, buf) < 0) {
        return -1;
      }
      *updated = 1;
      *must_advance_counter = 0;
      return 0;
    }
  }

  *must_advance_counter = 1;
  return 1;
}

static int parse_user(pam_handle_t *pamh, const char *name, uid_t *uid) {
  char *endptr;
  errno = 0;
  long l = strtol(name, &endptr, 10);
  if (!errno && endptr != name && l >= 0 && l <= INT_MAX) {
    *uid = (uid_t)l;
    return 0;
  }
  #ifdef _SC_GETPW_R_SIZE_MAX
  int len   = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (len <= 0) {
    len = 4096;
  }
  #else
  int len   = 4096;
  #endif
  char *buf = malloc(len);
  if (!buf) {
    log_message(LOG_ERR, pamh, "Out of memory");
    return -1;
  }
  struct passwd pwbuf, *pw;
  if (getpwnam_r(name, &pwbuf, buf, len, &pw) || !pw) {
    free(buf);
    log_message(LOG_ERR, pamh, "Failed to look up user \"%s\"", name);
    return -1;
  }
  *uid = pw->pw_uid;
  free(buf);
  return 0;
}

static int parse_args(pam_handle_t *pamh, int argc, const char **argv,
                      Params *params) {
  params->echocode = PAM_PROMPT_ECHO_OFF;
  for (int i = 0; i < argc; ++i) {
    if (!memcmp(argv[i], "secret=", 7)) {
      free((void *)params->secret_filename_spec);
      params->secret_filename_spec = argv[i] + 7;
    } else if (!memcmp(argv[i], "user=", 5)) {
      uid_t uid;
      if (parse_user(pamh, argv[i] + 5, &uid) < 0) {
        return -1;
      }
      params->fixed_uid = 1;
      params->uid = uid;
    } else if (!strcmp(argv[i], "try_first_pass")) {
      params->pass_mode = TRY_FIRST_PASS;
    } else if (!strcmp(argv[i], "use_first_pass")) {
      params->pass_mode = USE_FIRST_PASS;
    } else if (!strcmp(argv[i], "forward_pass")) {
      params->forward_pass = 1;
    } else if (!strcmp(argv[i], "noskewadj")) {
      params->noskewadj = 1;
    } else if (!strcmp(argv[i], "nullok")) {
      params->nullok = NULLOK;
    } else if (!strcmp(argv[i], "echo-verification-code") ||
               !strcmp(argv[i], "echo_verification_code")) {
      params->echocode = PAM_PROMPT_ECHO_ON;
    } else {
      log_message(LOG_ERR, pamh, "Unrecognized option \"%s\"", argv[i]);
      return -1;
    }
  }
  return 0;
}

static int google_authenticator(pam_handle_t *pamh, int flags,
                                int argc, const char **argv) {
  int        rc = PAM_AUTH_ERR;
  const char *username;
  char       *secret_filename = NULL;
  int        uid = -1, old_uid = -1, old_gid = -1, fd = -1;
  off_t      filesize = 0;
  time_t     mtime = 0;
  char       *buf = NULL;
  uint8_t    *secret = NULL;
  int        secretLen = 0;

#if defined(DEMO) || defined(TESTING)
  *error_msg = '\000';
#endif

  // Handle optional arguments that configure our PAM module
  Params params = { 0 };
  if (parse_args(pamh, argc, argv, &params) < 0) {
    return rc;
  }

  // Read and process status file, then ask the user for the verification code.
  int early_updated = 0, updated = 0;
  if ((username = get_user_name(pamh)) &&
      (secret_filename = get_secret_filename(pamh, &params, username, &uid)) &&
      !drop_privileges(pamh, username, uid, &old_uid, &old_gid) &&
      (fd = open_secret_file(pamh, secret_filename, &params, username, uid,
                             &filesize, &mtime)) >= 0 &&
      (buf = read_file_contents(pamh, secret_filename, &fd, filesize)) &&
      (secret = get_shared_secret(pamh, secret_filename, buf, &secretLen)) &&
       rate_limit(pamh, secret_filename, &early_updated, &buf) >= 0) {
    long hotp_counter = get_hotp_counter(pamh, buf);
    int must_advance_counter = 0;
    char *pw = NULL, *saved_pw = NULL;
    for (int mode = 0; mode < 4; ++mode) {
      // In the case of TRY_FIRST_PASS, we don't actually know whether we
      // get the verification code from the system password or from prompting
      // the user. We need to attempt both.
      // This only works correctly, if all failed attempts leave the global
      // state unchanged.
      if (updated || pw) {
        // Oops. There is something wrong with the internal logic of our
        // code. This error should never trigger. The unittest checks for
        // this.
        if (pw) {
          memset(pw, 0, strlen(pw));
          free(pw);
          pw = NULL;
        }
        rc = PAM_AUTH_ERR;
        break;
      }
      switch (mode) {
      case 0: // Extract possible verification code
      case 1: // Extract possible scratch code
        if (params.pass_mode == USE_FIRST_PASS ||
            params.pass_mode == TRY_FIRST_PASS) {
          pw = get_first_pass(pamh);
        }
        break;
      default:
        if (mode != 2 && // Prompt for pw and possible verification code
            mode != 3) { // Prompt for pw and possible scratch code
          rc = PAM_AUTH_ERR;
          continue;
        }
        if (params.pass_mode == PROMPT ||
            params.pass_mode == TRY_FIRST_PASS) {
          if (!saved_pw) {
            // If forwarding the password to the next stacked PAM module,
            // we cannot tell the difference between an eight digit scratch
            // code or a two digit password immediately followed by a six
            // digit verification code. We have to loop and try both
            // options.
            saved_pw = request_pass(pamh, params.echocode,
                                    params.forward_pass ?
                                    "Password & verification code: " :
                                    "Verification code: ");
          }
          if (saved_pw) {
            pw = strdup(saved_pw);
          }
        }
        break;
      }
      if (!pw) {
        continue;
      }

      // We are often dealing with a combined password and verification
      // code. Separate them now.
      int pw_len = strlen(pw);
      int expected_len = mode & 1 ? 8 : 6;
      char ch;
      if (pw_len < expected_len ||
          // Verification are six digits starting with '0'..'9',
          // scratch codes are eight digits starting with '1'..'9'
          (ch = pw[pw_len - expected_len]) > '9' ||
          ch < (expected_len == 8 ? '1' : '0')) {
      invalid:
        memset(pw, 0, pw_len);
        free(pw);
        pw = NULL;
        continue;
      }
      char *endptr;
      errno = 0;
      long l = strtol(pw + pw_len - expected_len, &endptr, 10);
      if (errno || l < 0 || *endptr) {
        goto invalid;
      }
      int code = (int)l;
      memset(pw + pw_len - expected_len, 0, expected_len);

      if ((mode == 2 || mode == 3) && !params.forward_pass) {
        // We are explicitly configured so that we don't try to share
        // the password with any other stacked PAM module. We must
        // therefore verify that the user entered just the verification
        // code, but no password.
        if (*pw) {
          goto invalid;
        }
      }

      // Check all possible types of verification codes.
      switch (check_scratch_codes(pamh, secret_filename, &updated, buf, code)){
      case 1:
        if (hotp_counter > 0) {
          switch (check_counterbased_code(pamh, secret_filename, &updated,
                                          &buf, secret, secretLen, code,
                                          &params, hotp_counter,
                                          &must_advance_counter)) {
          case 0:
            rc = PAM_SUCCESS;
            break;
          case 1:
            goto invalid;
          default:
            break;
          }
        } else {
          switch (check_timebased_code(pamh, secret_filename, &updated, &buf,
                                       secret, secretLen, code, &params)) {
          case 0:
            rc = PAM_SUCCESS;
            break;
          case 1:
            goto invalid;
          default:
            break;
          }
        }
        break;
      case 0:
        rc = PAM_SUCCESS;
        break;
      default:
        break;
      }

      break;
    }

    // Update the system password, if we were asked to forward
    // the system password. We already removed the verification
    // code from the end of the password.
    if (rc == PAM_SUCCESS && params.forward_pass) {
      if (!pw || pam_set_item(pamh, PAM_AUTHTOK, pw) != PAM_SUCCESS) {
        rc = PAM_AUTH_ERR;
      }
    }

    // Clear out password and deallocate memory
    if (pw) {
      memset(pw, 0, strlen(pw));
      free(pw);
    }
    if (saved_pw) {
      memset(saved_pw, 0, strlen(saved_pw));
      free(saved_pw);
    }

    // If an hotp login attempt has been made, the counter must always be
    // advanced by at least one.
    if (must_advance_counter) {
      char counter_str[40];
      sprintf(counter_str, "%ld", hotp_counter + 1);
      if (set_cfg_value(pamh, "HOTP_COUNTER", counter_str, &buf) < 0) {
        rc = PAM_AUTH_ERR;
      }
      updated = 1;
    }

    // If nothing matched, display an error message
    if (rc != PAM_SUCCESS) {
      log_message(LOG_ERR, pamh, "Invalid verification code");
    }
  }

  // If the user has not created a state file with a shared secret, and if
  // the administrator set the "nullok" option, this PAM module completes
  // successfully, without ever prompting the user.
  if (params.nullok == SECRETNOTFOUND) {
    rc = PAM_SUCCESS;
  }

  // Persist the new state.
  if (early_updated || updated) {
    if (write_file_contents(pamh, secret_filename, filesize,
                            mtime, buf) < 0) {
      // Could not persist new state. Deny access.
      rc = PAM_AUTH_ERR;
    }
  }
  if (fd >= 0) {
    close(fd);
  }
  if (old_gid >= 0) {
    if (setgroup(old_gid) >= 0 && setgroup(old_gid) == old_gid) {
      old_gid = -1;
    }
  }
  if (old_uid >= 0) {
    if (setuser(old_uid) < 0 || setuser(old_uid) != old_uid) {
      log_message(LOG_EMERG, pamh, "We switched users from %d to %d, "
                  "but can't switch back", old_uid, uid);
    }
  }
  free(secret_filename);

  // Clean up
  if (buf) {
    memset(buf, 0, strlen(buf));
    free(buf);
  }
  if (secret) {
    memset(secret, 0, secretLen);
    free(secret);
  }
  return rc;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags,
                                   int argc, const char **argv)
  __attribute__((visibility("default")));
PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags,
                                   int argc, const char **argv) {
  return google_authenticator(pamh, flags, argc, argv);
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc,
                                     const char **argv)
  __attribute__((visibility("default")));
PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc,
                                     const char **argv) {
  return PAM_SUCCESS;
}

#ifdef PAM_STATIC
struct pam_module _pam_listfile_modstruct = {
  MODULE_NAME,
  pam_sm_authenticate,
  pam_sm_setcred,
  NULL,
  NULL,
  NULL,
  NULL
};
#endif
