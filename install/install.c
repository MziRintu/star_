#define CFCOMMON
//#define LOG_FP
#include "common.h"
#include <libtar.h>
#include <lzma.h>
#include <pthread.h>
#include <zlib.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <notify.h>
#include <stdarg.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <fts.h>
#include <CoreFoundation/CoreFoundation.h>

extern void do_copy(char *, char *, ssize_t (*)(int, const void *, size_t));
extern void init();
extern void finish();
extern void register_application(CFStringRef app);
static int written_bytes;
static bool is_ipad;

static const char *freeze;
static int freeze_len;
static void (*set_progress)(float);

static void wrote_bytes(ssize_t bytes) {
    written_bytes += bytes;
    set_progress(written_bytes / 15759544.0);
}

ssize_t my_write(int fd, const void *buf, size_t len) {
    ssize_t ret = write(fd, buf, len);
    if(ret > 0) wrote_bytes(ret);
    return ret;
}


static inline void remove_files(char *path) {
    char *argv[2];
    argv[0] = path;
    argv[1] = NULL;
    FTS *fts = fts_open(argv, FTS_NOCHDIR | FTS_PHYSICAL, NULL);
    FTSENT *ent;
    while(ent = fts_read(fts)) {
        switch(ent->fts_info) {
        case FTS_F:
        case FTS_SL:
        case FTS_SLNONE:
        case FTS_NSOK:
        case FTS_DEFAULT:
            //I("Unlinking %s", ent->fts_accpath);
            //TRY2(rf_unlink, ent->fts_accpath, unlink(ent->fts_accpath));
            if(unlink(ent->fts_accpath))
                I("Unlink %s failed", ent->fts_accpath);
            break;
            
            case FTS_DP:
                if(rmdir(ent->fts_accpath))
                    I("Rmdir %s failed", ent->fts_accpath);
                break;
            
            case FTS_NS:
            case FTS_ERR: // I'm getting errno=0
                return;
                //AST2(fts_err, path, 0);
                //break;
            }
        }

}

static inline void qcopy(const char *a, const char *b) {
    int fd1 = open(a, O_RDONLY);
    AST2(qcopy_from, a, fd1);
    struct stat st;
    fstat(fd1, &st);
    int fd2 = open(b, O_WRONLY | O_CREAT, st.st_mode);
    AST2(qcopy_to, b, fd2);
    fchmod(fd2, st.st_mode);
    fchown(fd2, 0, 0);
    char *buf = malloc(st.st_size);
    read(fd1, buf, st.st_size);
    my_write(fd2, buf, st.st_size);
    free(buf);
    close(fd1);
    close(fd2);
}

static inline void copy_files(const char *from, const char *to, bool copy) {
    DIR *dir = opendir(from);
    char *a = malloc(1025 + strlen(from));
    char *b = malloc(1025 + strlen(to));
    I("copy_files %s -> %s", from, to);
    struct dirent *ent;
    while(ent = readdir(dir)) {
        if(ent->d_type == DT_REG) {
            sprintf(a, "%s/%s", from, ent->d_name);
            sprintf(b, "%s/%s", to, ent->d_name);
            //printf("%s %s -> %s\n", copy ? "Copy" : "Move", a, b);
            if(copy) {
                qcopy(a, b);
            } else {
                TRY(rename, rename(a, b));
            }
        }
    }
    free(a);
    free(b);
}

static int qlaunchctl(char *what, char *who) {
    char *args[] = {
        "/bin/launchctl",
        what,
        who,
    NULL };
    pid_t pid;
    int stat;
    posix_spawn(&pid, args[0], NULL, NULL, args, NULL);
    waitpid(pid, &stat, 0);
    return stat;
}

unsigned int config_vnode_patch;

static void lol_mkdir() {
    // This is a REALLY nasty hack but the HFS thing is causing corruption
    // It patches the mkdir function to ask for NOCROSSMOUNT
    // does the mkdir real quick, then patches it back

    int fd = open("/dev/kmem", O_RDWR);
    AST(lol_kmem_fd, fd > 0);

    int flags;
    AST(lol_read_flags, 4 == pread(fd, &flags, 4, (off_t) config_vnode_patch));
    I("lol_mkdir: addy = %x flags = %x", config_vnode_patch, flags);

    int flags2 = flags | 0x100;
    AST(lol_write_flags_1, 4 == pwrite(fd, &flags2, 4, (off_t) config_vnode_patch));

    // I don't want to leave the kernel in this state no matter what.
    int ret, fail = 0; 

    fail |= ret = mkdir("/private/var", 0755);
    I("mkdir 1: %d", ret);
    fail |= ret = mkdir("/private/var/db", 0755);
    I("mkdir 2: %d", ret);
    fail |= ret = mkdir("/private/var/db/.launchd_use_gmalloc", 0755);
    I("mkdir 3: %d", ret);
   
    // Restore original flags
    AST(lol_write_flags_2, 4 == pwrite(fd, &flags, 4, (off_t) config_vnode_patch));
}

static void qstat(const char *path) {
    struct stat st;
    if(lstat(path, &st)) {
        I("Could not lstat %s: %s\n", path, strerror(errno));
    } else {
        I("%s: size %d uid %d gid %d mode %04o flags %d\n", path, (int) st.st_size, (int) st.st_uid, (int) st.st_gid, (int) st.st_mode, (int) st.st_flags);
        I("again, mode is %d", (int) st.st_mode);
        I("access is %d", R_OK | W_OK | F_OK | X_OK);
    }
}

struct lzmactx {
    lzma_stream strm;
    uint8_t buf[BUFSIZ];
    uint8_t in_buf[BUFSIZ];
    char *read_buf;
    int read_len;
};

int lzmaopen(const char *path, int oflag, int foo) {
    struct lzmactx *ctx = malloc(sizeof(struct lzmactx));
    ctx->strm = (lzma_stream) LZMA_STREAM_INIT;
    lzma_ret ret;
    TRY(stream_decoder, lzma_stream_decoder(&ctx->strm, 64*1024*1024, 0));

    ctx->strm.avail_in = freeze_len;
    ctx->strm.next_in = freeze; 
    ctx->strm.next_out = ctx->buf;
    ctx->strm.avail_out = BUFSIZ;
    ctx->read_buf = ctx->buf;
    ctx->read_len = 0;

    return (int) ctx;
}

int lzmaclose(int fd) {
    return 0;
}

ssize_t lzmaread(int fd, void *buf_, size_t len) {
    struct lzmactx *ctx = (void *) fd;
    char *buf = buf_;
    while(len > 0) {
        if(ctx->read_len > 0) {
            size_t bytes_to_read = len < ctx->read_len ? len : ctx->read_len;
            memcpy(buf, ctx->read_buf, bytes_to_read);
            buf += bytes_to_read;
            ctx->read_buf += bytes_to_read;
            ctx->read_len -= bytes_to_read;
            len -= bytes_to_read;
            continue;                
        }

        AST(still_has_bytes, ctx->strm.avail_in != 0);

        if(ctx->strm.avail_out <= 128) {
            ctx->strm.next_out = ctx->buf;
            ctx->strm.avail_out = BUFSIZ;
            ctx->read_buf = ctx->buf;
        }

        size_t old_avail = ctx->strm.avail_out;

        if(lzma_code(&ctx->strm, LZMA_RUN)) break;
        ctx->read_len = old_avail - ctx->strm.avail_out;
    }

    ssize_t br = buf - (char *) buf_;
    wrote_bytes(br);
    return br;
}

tartype_t xztype = { (openfunc_t) lzmaopen, (closefunc_t) lzmaclose, (readfunc_t) lzmaread, (writefunc_t) NULL };

static void add_app(CFMutableDictionaryRef mi_cache, char *app) {
    char *info_plist; asprintf(&info_plist, "%s/Info.plist", app);
    if(access(info_plist, R_OK)) return;
    CFStringRef app_ = CFStringCreateWithCString(NULL, app, kCFStringEncodingASCII);
    
    CFDataRef data = cr(info_plist);

    CFMutableDictionaryRef plist = (void*) CFPropertyListCreateFromXMLData(NULL, data, kCFPropertyListMutableContainersAndLeaves, NULL);
    CFDictionarySetValue(plist, CFSTR("ApplicationType"), CFSTR("System"));
    CFDictionarySetValue(plist, CFSTR("Path"), app_);
    CFMutableDictionaryRef system = (void*) CFDictionaryGetValue(mi_cache, CFSTR("System"));
    CFDictionarySetValue(system, CFDictionaryGetValue(plist, CFSTR("CFBundleIdentifier")), plist);
   
    if(1) {//is_ipad) {
        register_application(app_);
    }

    free(info_plist);
    CFRelease(app_);
    CFRelease(plist);
    CFRelease(data);
}


static void extract() {
    CFDataRef mi_cache_data = cr("/var/mobile/Library/Caches/com.apple.mobile.installation.plist");
    CFMutableDictionaryRef mi_cache = (void*) CFPropertyListCreateFromXMLData(NULL, mi_cache_data, kCFPropertyListMutableContainersAndLeaves, NULL);

    TAR *tar;
    char *current_app = NULL;
    char *fn = "<buf>";
    // TAR_VERBOSE
    if(tar_open(&tar, fn, &xztype, O_RDONLY, 0, TAR_GNU)) {
        E("could not open %s: %s", fn, strerror(errno));
        exit(3);
    }
    while(!th_read(tar)) {
        char *pathname = th_get_pathname(tar);
        char *full; asprintf(&full, "/%s", pathname);
        tar_extract_file(tar, full);
        if(strstr(full, "LaunchDaemons/") && strstr(full, ".plist")) {
            I("loading it");
            qlaunchctl("load", full); 
        }

        if(current_app && memcmp(current_app, full, strlen(current_app))) {
            I("done with %s (%s), adding it", current_app, full);
            add_app(mi_cache, current_app);
            free(current_app);
            current_app = NULL;
        }

        int len = strlen(full);
        if(len > 4 && (!memcmp(full + len - 4, ".app\0", 5) || !memcmp(full + len - 5, ".app/\0", 6))) {
            current_app = strdup(full);
            I("current_app = %s", current_app);
        }
        free(full);
    }

    if(current_app) {
        I("done with %s, adding it", current_app);
        add_app(mi_cache, current_app);
        free(current_app);
        current_app = NULL;
    }

    tar_close(tar);
    CFDataRef mi_cache_outdata = CFPropertyListCreateXMLData(NULL, mi_cache);
    I("out");
    //I("outdata is %s", CFDataGetBytePtr(mi_cache_outdata));
    I("data");
    cw("/var/mobile/Library/Caches/com.apple.mobile.installation.plist", mi_cache_outdata);

    CFRelease(mi_cache);
    CFRelease(mi_cache_data);
    CFRelease(mi_cache_outdata);
}

static void qmount() {
    char x[16];
    char *args[] = {
        "/sbin/mount",
        "-u", // ?? this doesn't seem to be necessary with blackra1n 
        "-o", "rw,suid,dev", x,
    NULL };
    pid_t pid, pid2;

    strcpy(x, "/");
    posix_spawn(&pid, args[0], NULL, NULL, args, NULL);
    //strcpy(x, "/private/var");
    //posix_spawn(&pid2, args[0], NULL, NULL, args, NULL);
    int stat;
    waitpid(pid, &stat, 0);
    //waitpid(pid2, &stat, 0);
    //printf("mount %s %s with %d\n", x, WIFEXITED(stat) ? "exited" : "terminated", WIFEXITED(stat) ? WEXITSTATUS(stat) : WTERMSIG(stat));
}
    
extern int mount_it(char **error);

static void remount() {
    I("remount...");
    qmount();
    I(".");
    { // fstab
        FILE *fp = fopen("/etc/fstab", "r+b");
        AST(open_fstab, fp);
        fseek(fp, 0, SEEK_END);
        size_t len = ftell(fp);
        char *buf = malloc(len+1);
        fseek(fp, 0, 0);
        fread(buf, len, 1, fp);
        buf[len] = 0;

        I("Old fstab was %s", buf);

        char *s = strstr(buf, "hfs ro");
        if(s) {
            s[5] = 'w';
        }

        s = strstr(buf, ",nosuid,nodev");
        if(s) {
            memset(s, ' ', strlen(",nosuid,nodev"));
        }
        
        I("My new fstab: %s", buf);

        fseek(fp, 0, 0);
        fwrite(buf, len, 1, fp);
        fclose(fp);
        free(buf);
    }
}

static void do_stash(const char *from, const char *to) {
    struct stat st;
    bool noexist = lstat(from, &st) && errno == ENOENT;
    if(noexist) {
        I("do_stash: mkdir %s", to);
        TRY(stash2_mkdir, mkdir(to, 0755));
        TRY(stash2_symlink, symlink(to, from));
    } else {
        char *from2 = NULL;
        asprintf(&from2, "%s.old", from);
        I("do_stash: copy %s -> %s", from, to);
        char *from_ = strdup(from);
        char *to_ = strdup(to);
        TIME(do_copy(from_, to_, my_write));
        free(from_);
        free(to_);
        TRY(stash_rename, rename(from, from2));
        TRY(stash_symlink, symlink(to, from));
        TIME(remove_files(from2));
        free(from2);
    }
}

static void stash() {
    mkdir("/var/stash", 0755);
    do_stash("/Applications", "/var/stash/Applications");
    do_stash("/Library/Ringtones", "/var/stash/Ringtones");
    do_stash("/Library/Wallpaper", "/var/stash/Wallpaper");
    //do_stash("/System/Library/Fonts", "/var/stash/Fonts");
    //do_stash("/System/Library/TextInput", "/var/stash/TextInput");
    do_stash("/usr/include", "/var/stash/include");
    do_stash("/usr/lib/pam", "/var/stash/pam");
    do_stash("/usr/libexec", "/var/stash/libexec");
    do_stash("/usr/share", "/var/stash/share");
}

static void dok48() {
    const char *fn = "/System/Library/CoreServices/SpringBoard.app/K48AP.plist";
    is_ipad = !access(fn, R_OK);
    if(!is_ipad) return;
    I("K48AP.plist exists");
    CFDataRef data = cr(fn);
    CFMutableDictionaryRef plist = (void*) CFPropertyListCreateFromXMLData(NULL, data, kCFPropertyListMutableContainers, NULL); 
    CFRelease(data);
    CFDictionarySetValue((void*)CFDictionaryGetValue(plist, CFSTR("capabilities")), CFSTR("hide-non-default-apps"), kCFBooleanFalse);
    CFDataRef outdata = CFPropertyListCreateXMLData(NULL, plist);
    cw(fn, outdata);
    CFRelease(plist);
    CFRelease(outdata);
}

static void kill_installd() {
    killall("installd");
    notify_post("com.apple.mobile.application_installed");
    sleep(1);
    notify_post("com.apple.mobile.application_installed");
}

static void write_gmalloc(unsigned char *one, unsigned int one_len) {
    int fd = open("/usr/lib/libgmalloc.dylib", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    AST(gmalloc_fd, fd > 0);
    AST(gmalloc_write, write(fd, one, one_len) == one_len);
    close(fd);
}

void do_install(const char *freeze_, int freeze_len_, void (*set_progress_)(float), unsigned int config_vnode_patch_, unsigned char *one, unsigned int one_len) {
    set_progress = set_progress_;
    config_vnode_patch = config_vnode_patch_;
    freeze = freeze_;
    freeze_len = freeze_len_;

#if 0
    hex_dump((void *) freeze, 0x40);
    I("<- %p len=%d", freeze, freeze_len);
    int xfd = lzmaopen("", 0, 0);
    char buf[0x40];
    lzmaread(xfd, buf, 0x40);
    hex_dump(buf, 0x40);
    return;
#endif

    chdir("/");
    I("do_install");
    TIME(remount());
    //I("S1"); sleep(5);
    TIME(lol_mkdir()); 
    //TIME(stash());
    TIME(write_gmalloc(one, one_len));
    TIME(dok48());
    //I("S2"); sleep(5);
    TIME(extract());
    //I("S3"); sleep(5);
    I("extract out.");
    //I("S4"); sleep(5);
    TIME(kill_installd());
    //I("S5"); sleep(5);
    TIME(sync());
    I("written_bytes = %d", written_bytes);
}

