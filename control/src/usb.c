#include "keeper.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define USB_SYSFS "/sys/bus/usb/devices"

int usb_list_rockchip(unsigned *pids, int max)
{
    DIR *d = opendir(USB_SYSFS);
    if (d == NULL) {
        return 0;
    }
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strchr(e->d_name, ':') != NULL) {
            continue; // "<bus>-<dev>:<iface>" entries are interfaces
        }
        char path[512];
        char buf[32];
        snprintf(path, sizeof(path), USB_SYSFS "/%s/idVendor", e->d_name);
        FILE *fp = fopen(path, "r");
        if (fp == NULL || fgets(buf, sizeof(buf), fp) == NULL) {
            if (fp != NULL) {
                fclose(fp);
            }
            continue;
        }
        fclose(fp);
        if ((unsigned)strtoul(buf, NULL, 16) != USB_VID_ROCKCHIP) {
            continue;
        }
        snprintf(path, sizeof(path), USB_SYSFS "/%s/idProduct", e->d_name);
        fp = fopen(path, "r");
        if (fp == NULL || fgets(buf, sizeof(buf), fp) == NULL) {
            if (fp != NULL) {
                fclose(fp);
            }
            continue;
        }
        fclose(fp);
        unsigned pid = (unsigned)strtoul(buf, NULL, 16);
        int dup = 0;
        for (int i = 0; i < count; i++) {
            if (pids[i] == pid) {
                dup = 1;
                break;
            }
        }
        if (!dup && count < max) {
            pids[count++] = pid;
        }
    }
    closedir(d);
    return count;
}

static void set_deadline(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static int past_deadline(const struct timespec *ts)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > ts->tv_sec ||
           (now.tv_sec == ts->tv_sec && now.tv_nsec >= ts->tv_nsec);
}

int usb_wait_rockchip(const unsigned *want, int nwant, int timeout_ms,
                      unsigned *found_pid)
{
    unsigned seen[16];
    struct timespec dl;
    set_deadline(&dl, timeout_ms);
    for (;;) {
        int n = usb_list_rockchip(seen, 16);
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < nwant; j++) {
                if (seen[i] == want[j]) {
                    *found_pid = seen[i];
                    return 1;
                }
            }
        }
        if (past_deadline(&dl)) {
            return 0;
        }
        usleep(200 * 1000);
    }
}
