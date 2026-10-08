#include "keeper.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

/* Walk up the sysfs tree from `start` until a directory with idVendor/
 * idProduct is found (tty device links point at the interface or lower). */
static int read_usbid(const char *start, uint16_t *vid, uint16_t *pid)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", start);

    for (int i = 0; i < 6; i++) {
        char file[PATH_MAX];
        struct stat st;
        size_t plen = strlen(path);
        const char *names[2] = { "/idVendor", "/idProduct" };
        unsigned vals[2];
        int ok = 1;
        for (int k = 0; k < 2 && ok; k++) {
            if (plen + strlen(names[k]) + 1 > sizeof(file)) {
                return -1;
            }
            snprintf(file, sizeof(file), "%s%s", path, names[k]);
            if (!(stat(file, &st) == 0 && S_ISREG(st.st_mode))) {
                ok = 0;
                break;
            }
            char buf[32];
            FILE *fp = fopen(file, "r");
            if (fp == NULL || fgets(buf, sizeof(buf), fp) == NULL) {
                if (fp) {
                    fclose(fp);
                }
                return -1;
            }
            fclose(fp);
            vals[k] = (unsigned)strtoul(buf, NULL, 16);
        }
        if (ok) {
            *vid = (uint16_t)vals[0];
            *pid = (uint16_t)vals[1];
            return 0;
        }
        char *slash = strrchr(path, '/');
        if (slash == NULL || slash == path) {
            return -1;
        }
        *slash = '\0';
    }
    return -1;
}

int tty_scan_espressif(tty_port_t *out, int max)
{
    DIR *d = opendir("/sys/class/tty");
    if (d == NULL) {
        return 0;
    }
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "ttyACM", 6) != 0 &&
            strncmp(e->d_name, "ttyUSB", 6) != 0) {
            continue;
        }
        char linkpath[512];
        snprintf(linkpath, sizeof(linkpath), "/sys/class/tty/%s/device",
                 e->d_name);
        char real[PATH_MAX];
        if (realpath(linkpath, real) == NULL) {
            continue;
        }
        uint16_t vid, pid;
        if (read_usbid(real, &vid, &pid) != 0 || vid != ESPRESSIF_VID) {
            continue;
        }
        if (count < max) {
            snprintf(out[count].dev, sizeof(out[count].dev), "/dev/%s",
                     e->d_name);
            out[count].vid = vid;
            out[count].pid = pid;
        }
        count++;
    }
    closedir(d);
    return count;
}

int tty_open(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    /* One keeper process at a time: interleaved "@commands" from two
     * processes would corrupt the dongle's command stream. */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        errno = EBUSY;
        return -1;
    }
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &tio);
    }
    /* Deassert DTR/RTS: some ESP32 boards reset when they are asserted.
     * USB CDC adapters accept these ioctls; other tty kinds may not. */
    int flags = TIOCM_DTR | TIOCM_RTS;
    ioctl(fd, TIOCMBIC, &flags);
    return fd;
}
