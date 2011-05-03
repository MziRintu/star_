#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <notify.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "inject.h"
#include <common/common.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CFNetwork/CFNetwork.h>
#include <libgen.h>
#include <sys/stat.h>
#include <libproc.h>
#include <dlfcn.h>

static const float download_share = 0.65;

// todo: what is up with release_surface panics
// and SB freezing or hanging on black
// blinking alertview of doom

//#define TINY

#ifdef TINY
static void do_nothing_with(CFTypeRef r) {}
#define CFRelease do_nothing_with
#define NSLog(...)
#else
extern void NSLog(CFStringRef fmt, ...);
#endif

extern char dylib[];
extern unsigned int dylib_len;

static void update_state(const char *state, CFStringRef err);
static void run_install();

static double progress = 0.0;
static bool paused;
static size_t downloaded_bytes;

static struct request {
    CFStringRef url;
    const char *output;
    CFStringRef content_type;
    union {
        struct {};
        struct {
            CFReadStreamRef read_stream;
            int out_fd;
            size_t content_length;
            bool finished;
            bool got_headers;
        };
    };
} requests[] = {
    {CFSTR("http://a.qoid.us/starstuff_%s_%s.tar.xz"), "/tmp/starstuff.tar.xz", CFSTR("application/x-tar"), {}},
    {CFSTR("http://test.saurik.com/dhowett/Cydia-4.1b1-Srk.txz"), "/tmp/freeze.tar.xz", CFSTR("text/plain"), {}},
    {CFSTR("http://a.qoid.us/install.dylib"), "/tmp/install.dylib", CFSTR("text/plain"), {}},
}, *const requests_end = requests + sizeof(requests)/sizeof(*requests);

__attribute__((noreturn))
static void leave() {
    // maybe remove temp?
    exit(0);
}

static void did_download(size_t bytes) {
    downloaded_bytes += bytes;

    size_t total = 0;
    for(struct request *r = requests; r < requests_end; r++) {
        if(!r->content_length) {
            goto nevermind; // stay at 0
        }
        total += r->content_length;
    }
    progress = download_share * ((double) downloaded_bytes / total);

    nevermind:

    update_state("DOWNLOADING_ICON_LABEL", NULL);
}

static void pause_it(CFStringRef err) {
    if(err) NSLog(CFSTR("err: %@"), err);
    paused = true;
    for(struct request *r = requests; r < requests_end; r++) {
        if(r->read_stream) {
            CFReadStreamClose(r->read_stream);
            CFRelease(r->read_stream);
            r->read_stream = NULL;
        }
    }
    update_state("PAUSED_ICON_LABEL", err);
}

// handle an error or completion
static void handle_error(struct request *r, CFStringRef err) {
    close(r->out_fd);
    r->out_fd = 0;
    if(err) {
        pause_it(err);
    } else {
        rename(basename((char *) r->output), r->output);
        r->finished = true;
        CFReadStreamClose(r->read_stream);
        CFRelease(r->read_stream);
        r->read_stream = NULL;

        for(struct request *r = requests; r < requests_end; r++) {
            if(!r->finished) return;
        }
        // We are all finished.
        run_install();
    }
}

static void request_callback(CFReadStreamRef stream, CFStreamEventType event_type, void *info) {
    struct request *r = info;

    switch(event_type) {
    case kCFStreamEventErrorOccurred:
        {
        CFErrorRef error = _assert(CFReadStreamCopyError(stream));
        CFStringRef description = CFErrorCopyDescription(error);
        /*if(CFErrorGetDomain(error) == kCFErrorDomainCFNetwork) {

        }*/
        handle_error(r, description);
        CFRelease(error);
        }
        break;
    case kCFStreamEventEndEncountered:
        if((size_t) lseek(r->out_fd, 0, SEEK_CUR) != r->content_length) {
            handle_error(r, CFSTR("Truncated"));
        } else {
            handle_error(r, NULL);
        }
        break;
    case kCFStreamEventHasBytesAvailable:
        if(!r->got_headers) {
            r->got_headers = true;

            // we need to record the content-length
            // also, if the server ignored a range request, we might be at the wrong file position, so we have to check
            CFHTTPMessageRef response = (void *) CFReadStreamCopyProperty(stream, kCFStreamPropertyHTTPResponseHeader);
            if(!response) break;
            CFIndex code = CFHTTPMessageGetResponseStatusCode(response);
            NSLog(CFSTR("[%@] status %d"), r->url, (int) code);
            if(code == 206) {
                // partial content
                off_t off = 0;
                CFStringRef range = CFHTTPMessageCopyHeaderFieldValue(response, CFSTR("Content-Range"));
                if(range) {
                    if(kCFCompareEqualTo == CFStringCompareWithOptions(range, CFSTR("bytes "), CFRangeMake(0, 6), kCFCompareCaseInsensitive)) {
                        CFRange r = CFStringFind(range, CFSTR("-"), 0);
                        if(r.length != 0 && r.location > 6) {
                            CFStringRef sub = CFStringCreateWithSubstring(NULL, range, CFRangeMake(6, r.location - 6));
                            off = (off_t) CFStringGetIntValue(sub);
                            CFRelease(sub);
                        }
                    }
                    CFRelease(range);
                }
                if(off != lseek(r->out_fd, off, SEEK_SET)) {
                    handle_error(r, CFSTR("Server fails (206)")); 
                    break;
                }
            } else if(code != 200) {
                handle_error(r, CFStringCreateWithFormat(NULL, NULL, CFSTR("HTTP response code %d"), (int) code));
                break;
            }
            CFStringRef content_type = CFHTTPMessageCopyHeaderFieldValue(response, CFSTR("Content-Type"));
            if(!content_type || kCFCompareEqualTo != CFStringCompare(content_type, r->content_type, kCFCompareCaseInsensitive)) {
                NSLog(CFSTR("got %@, expected %@"), content_type, r->content_type);
                
                handle_error(r, CFStringCreateWithFormat(NULL, NULL, CFSTR("Wrong Content-Type; are you on a fail Wi-Fi network?")));
            }

            CFStringRef cl = CFHTTPMessageCopyHeaderFieldValue(response, CFSTR("Content-Length"));
            if(!cl) {
                handle_error(r, CFSTR("Server fails (no length)"));
                break;
            } else {
                r->content_length = CFStringGetIntValue(cl);
                CFRelease(cl);
            }
        }

        // actually read
        size_t written = 0;
        while(CFReadStreamHasBytesAvailable(r->read_stream)) {
            static char compressed[8192], uncompressed[16384];
            CFIndex idx = CFReadStreamRead(r->read_stream, (void *) compressed, sizeof(compressed));
            if(idx == -1) {
                handle_error(r, CFSTR("Huh?"));
                goto end;
            }

            written += (size_t) idx;

            _assert((CFIndex) write(r->out_fd, compressed, idx) == idx);
        }
        end:
        did_download(written);
        break;
    }
}


static void init_requests() {
    paused = false;
    for(struct request *r = requests; r < requests_end; r++) {
        if(r->read_stream) continue;

        CFURLRef url = _assert(CFURLCreateWithString(NULL, r->url, NULL));
        CFHTTPMessageRef message = _assert(CFHTTPMessageCreateRequest(NULL, CFSTR("GET"), url, kCFHTTPVersion1_1));
        CFRelease(url);

        if(r->out_fd && r->content_length) {
            CFStringRef range = CFStringCreateWithFormat(NULL, NULL, CFSTR("bytes=%d-"), (int) lseek(r->out_fd, 0, SEEK_CUR));
            NSLog(CFSTR("[%@] sending range %@"), r->url, range);
            CFHTTPMessageSetHeaderFieldValue(message, CFSTR("Range"), range);
            CFRelease(range);
        } else {
            r->out_fd = _assert(open(basename((char *) r->output), O_WRONLY | O_CREAT | O_TRUNC, 0644));
        }

        r->read_stream = _assert(CFReadStreamCreateForHTTPRequest(NULL, message));
        CFRelease(message);
        // creating the read stream should succeed, but there might be an error later
        r->got_headers = false;

        static CFStreamClientContext context;
        context.info = r;
        CFReadStreamSetClient(r->read_stream, kCFStreamEventHasBytesAvailable | kCFStreamEventErrorOccurred | kCFStreamEventEndEncountered, request_callback, &context);

        CFReadStreamScheduleWithRunLoop(r->read_stream, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFReadStreamOpen(r->read_stream);
        
    }
    update_state("DOWNLOADING_ICON_LABEL", NULL);
}

static void set_progress(float progress_) {
    progress = download_share + progress_ * (1.0 - download_share);
    update_state("INSTALLING_ICON_LABEL", NULL);
}

static void run_install() {
    signal(SIGUSR1, SIG_IGN);
    progress = 0.0;
    update_state("INSTALLING_ICON_LABEL", NULL);
    void *install = dlopen("/tmp/install.dylib", RTLD_LAZY);
    void (*do_install)(void (*set_progress)(float)) = dlsym(install, "do_install");
    do_install(set_progress);
    notify_post("locutus.installed");
    leave();
}

static pid_t find_springboard() {
    static int name[] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t length;
    _assert(!sysctl(&name[0], sizeof(name) / sizeof(*name), NULL, &length, NULL, 0));
    struct kinfo_proc *proc = malloc(length);
    _assert(!sysctl(&name[0], sizeof(name) / sizeof(*name), proc, &length, NULL, 0));
    for(size_t i = 0; i < length/sizeof(*proc); i++) {
        if(!strncmp(proc[i].kp_proc.p_comm, "SpringBoard", sizeof(proc[i].kp_proc.p_comm))) {
            pid_t result = proc[i].kp_proc.p_pid;
            free(proc);
            return result;
        }
    }
    _assert(false);
}

static void init_state() {
    notify_post("locutus.go-away");
    int fd = open("/tmp/locutus.state", O_RDWR | O_CREAT, 0666);
    _assert(fd >= 0);
    _assert_zero(flock(fd, LOCK_EX));
    if(0 == lseek(fd, 0, SEEK_END)) {
        // we created it
    } else {
        char buf[10];
        int pid;
        if(pread(fd, buf, sizeof(buf), 0) != -1 && (pid = atoi(buf))) {
            kill((pid_t) pid, SIGUSR1);
            char whatever[PROC_PIDTBSDINFO_SIZE];
            if(proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &whatever, sizeof(whatever))) {
                // it stayed alive (and is not a zombie) - so it's installing and we should give up
                exit(0);
            }
        }
    }
    flock(fd, LOCK_UN);
    close(fd);
}

static void update_state(const char *state, CFStringRef err) {
    int fd = open("/tmp/new.state", O_RDWR | O_CREAT, 0666);
    _assert(fd >= 0);
    _assert_zero(flock(fd, LOCK_EX));
    _assert_zero(ftruncate(fd, 0));
    FILE *fp = fdopen(fd, "w");
    _assert(fp);

    char errs[128];
    if(err) {
        CFStringGetCString(err, errs, sizeof(errs), kCFStringEncodingUTF8);
        CFRelease(err);
    } else {
        strcpy(errs, "ok");
    }

    fprintf(fp, "%d\t%s\t%f\t%s\t\n", (int) getpid(), state, progress, errs);

    fclose(fp);

    _assert_zero(rename("/tmp/new.state", "/tmp/locutus.state"));
    notify_post("locutus.updated-state");
}

int main(int argc, char **argv) {
    if(argc == 0) {
        // make me root and remove sandbox
        syscall(0);
    }

    NSLog(CFSTR("omg hax"));
    //return 0;
    mkdir("/tmp/locutus-temp", 0755); // might fail
    _assert_zero(chdir("/tmp/locutus-temp"));

    /*uint32_t one = 1;
    _assert_zero(sysctlbyname("security.mac.vnode_enforce", NULL, NULL, &one, sizeof(one)));*/

    init_state();

    int ignored;
    notify_register_dispatch("locutus.pause", &ignored, dispatch_get_main_queue(), ^(int token) {
        if(paused) {
            init_requests();
        } else {
            pause_it(NULL);
        }
    });
    notify_register_dispatch("locutus.cancel", &ignored, dispatch_get_main_queue(), ^(int token) {
        // maybe delete the temporary files?
        leave();
    });
        
    // dump our load
    const char *name = tempnam("/tmp/", "locutus.dylib-");
    printf("name = %s\n", name);
    int dylib_fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    _assert(dylib_fd >= 0);

    for(unsigned int i = 0; i < dylib_len - 32; i++) {
        if(!memcmp(dylib + i, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", 32)) {
            memcpy(dylib + i, name, 32);
        }
    }

    _assert(write(dylib_fd, dylib, dylib_len) == (ssize_t) dylib_len);
    close(dylib_fd);

    pid_t pid = find_springboard();
    printf("pid = %d\n", (int) pid);

    inject(pid, name);

    NSLog(CFSTR("OK"));

    char machine[32], osversion[32];
    size_t size = 32;
    _assert_zero(sysctlbyname("hw.machine", machine, &size, NULL, 0));
    size = 32;
    _assert_zero(sysctlbyname("kern.osversion", osversion, &size, NULL, 0));
    requests[0].url = CFStringCreateWithFormat(NULL, NULL, requests[0].url, machine, osversion);

    init_requests();
    CFRunLoopRun();

    return 0;
}
