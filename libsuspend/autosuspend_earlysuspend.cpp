/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "libsuspend"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <log/log.h>
#include <android-base/file.h>
#include "autosuspend_ops.h"
#define EARLYSUSPEND_SYS_POWER_STATE "/sys/power/state"
#define EARLYSUSPEND_WAIT_FOR_FB_SLEEP "/sys/power/wait_for_fb_sleep"
#define EARLYSUSPEND_WAIT_FOR_FB_WAKE "/sys/power/wait_for_fb_wake"
static int sPowerStatefd = 1;
static const char* pwr_state_mem = "mem";
static const char* pwr_state_on = "on";
static void (*wakeup_func)(bool success) = NULL;
static pthread_t earlysuspend_thread;
static pthread_mutex_t earlysuspend_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t earlysuspend_cond = PTHREAD_COND_INITIALIZER;
static bool wait_for_earlysuspend;
static bool autosuspend_is_init = false;
using android::base::WriteStringToFd;
static enum {
    EARLYSUSPEND_ON,
    EARLYSUSPEND_MEM,
} earlysuspend_state = EARLYSUSPEND_ON;
int wait_for_fb_wake(void)
{
    int err = 0;
    char buf;
    int fd = TEMP_FAILURE_RETRY(open(EARLYSUSPEND_WAIT_FOR_FB_WAKE, O_RDONLY, 0));
    // if the file doesn't exist, the error will be caught in read() below
    err = TEMP_FAILURE_RETRY(read(fd, &buf, 1));
    ALOGE_IF(err < 0,
            "*** ANDROID_WAIT_FOR_FB_WAKE failed (%s)", strerror(errno));
    close(fd);
    return err < 0 ? err : 0;
}
static int wait_for_fb_sleep(void)
{
    int err = 0;
    char buf;
    int fd = TEMP_FAILURE_RETRY(open(EARLYSUSPEND_WAIT_FOR_FB_SLEEP, O_RDONLY, 0));
    // if the file doesn't exist, the error will be caught in read() below
    err = TEMP_FAILURE_RETRY(read(fd, &buf, 1));
    ALOGE_IF(err < 0,
            "*** ANDROID_WAIT_FOR_FB_SLEEP failed (%s)", strerror(errno));
    close(fd);
    return err < 0 ? err : 0;
}
static void* earlysuspend_thread_func(void __unused *arg) {
    while (true) {
        if (wait_for_fb_sleep()) {
            ALOGE("Failed reading wait_for_fb_sleep, exiting earlysuspend thread");
            return NULL;
        }
        pthread_mutex_lock(&earlysuspend_mutex);
        earlysuspend_state = EARLYSUSPEND_MEM;
        pthread_cond_signal(&earlysuspend_cond);
        pthread_mutex_unlock(&earlysuspend_mutex);
        if (wait_for_fb_wake()) {
            ALOGE("Failed reading wait_for_fb_wake, exiting earlysuspend thread");
            return NULL;
        }
        pthread_mutex_lock(&earlysuspend_mutex);
        earlysuspend_state = EARLYSUSPEND_ON;
        pthread_cond_signal(&earlysuspend_cond);
        pthread_mutex_unlock(&earlysuspend_mutex);
    }
}
static int init_sPowerStatefd(void) {
    char buf[80];
    if (sPowerStatefd >= 0) {
        return 0;
    }
    int fd = TEMP_FAILURE_RETRY(open(EARLYSUSPEND_SYS_POWER_STATE, O_CLOEXEC | O_RDWR));
    if (fd < 0) {
        ALOGE("Error opening %s: %s", EARLYSUSPEND_SYS_POWER_STATE, buf);
        return -1;
    }
    sPowerStatefd = fd;
    ALOGI("init_state_fd success");
    return 0;
}
static int autosuspend_init(void) {
    char buf[80];
    if (autosuspend_is_init) {
        return 0;
    }
    int ret = init_sPowerStatefd();
    if (ret < 0) {
        return -1;
    }
    ret = TEMP_FAILURE_RETRY(write(sPowerStatefd, "on", 2));
    if (ret < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGW("Error writing 'on' to %s: %s", EARLYSUSPEND_SYS_POWER_STATE, buf);
        goto err_write;
    }
    ALOGV("autosuspend_init success");
    autosuspend_is_init = true;
    return 0;
err_write:
    close(sPowerStatefd);
    return -1;
}
static int autosuspend_earlysuspend_enable(void) {
    char buf[80];
    ALOGV("autosuspend_earlysuspend_enable");
    int ret = autosuspend_init();
    if (ret < 0) {
        ALOGE("autosuspend_init failed");
        return ret;
    }
    if (wait_for_earlysuspend) {
        pthread_mutex_lock(&earlysuspend_mutex);
        while (earlysuspend_state != EARLYSUSPEND_MEM) {
            pthread_cond_wait(&earlysuspend_cond, &earlysuspend_mutex);
        }
        pthread_mutex_unlock(&earlysuspend_mutex);
    }
    ALOGV("autosuspend_earlysuspend_enable done");
    return 0;
err:
    return ret;
}
static int autosuspend_earlysuspend_disable(void) {
    char buf[80];
    ALOGV("autosuspend_earlysuspend_disable");
    if (!autosuspend_is_init) {
        return 0;  // always successful if no thread is running yet
    }
    int ret = TEMP_FAILURE_RETRY(write(sPowerStatefd, pwr_state_on, strlen(pwr_state_on)));
    if (ret < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s", EARLYSUSPEND_SYS_POWER_STATE, buf);
        goto err;
    }
    if (wait_for_earlysuspend) {
        pthread_mutex_lock(&earlysuspend_mutex);
        while (earlysuspend_state != EARLYSUSPEND_ON) {
            pthread_cond_wait(&earlysuspend_cond, &earlysuspend_mutex);
        }
        pthread_mutex_unlock(&earlysuspend_mutex);
    }
    ALOGV("autosuspend_earlysuspend_disable done");
    return 0;
err:
    return ret;
}
static int force_suspend(int timeout_ms) {
    ALOGV("force_suspend called with timeout: %u", timeout_ms);
    int ret = init_sPowerStatefd();
    if (ret < 0) {
        return ret;
    }
    return WriteStringToFd(pwr_state_mem, sPowerStatefd) ? 0 : -1;
}
static void autosuspend_set_wakeup_callback(void (*func)(bool success)) {
    if (wakeup_func != NULL) {
        ALOGE("Duplicate wakeup callback applied, keeping original");
        return;
    }
    wakeup_func = func;
}
struct autosuspend_ops autosuspend_earlysuspend_ops = {
    .enable = autosuspend_earlysuspend_enable,
    .disable = autosuspend_earlysuspend_disable,
    .force_suspend = force_suspend,
    .set_wakeup_callback = autosuspend_set_wakeup_callback,
};
void start_earlysuspend_thread(void) {
    char buf[80];
    int ret;
    ret = access(EARLYSUSPEND_WAIT_FOR_FB_SLEEP, F_OK);
    if (ret < 0) {
        return;
    }
    ret = access(EARLYSUSPEND_WAIT_FOR_FB_WAKE, F_OK);
    if (ret < 0) {
        return;
    }
    wait_for_fb_wake();
    ALOGI("Starting early suspend unblocker thread");
    ret = pthread_create(&earlysuspend_thread, NULL, earlysuspend_thread_func, NULL);
    if (ret) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error creating thread: %s", buf);
        return;
    }
    wait_for_earlysuspend = true;
}
struct autosuspend_ops* autosuspend_earlysuspend_init(void) {
    return &autosuspend_earlysuspend_ops;
}
