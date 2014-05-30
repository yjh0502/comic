#include "arg.h"

#include <archive_entry.h>
#include <archive.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>

#include <jpeglib.h>
#include <jerror.h>

/* macros */
#define CLEANMASK(mask)         (mask & ~(LockMask) & (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define LENGTH(X)               (sizeof X / sizeof X[0])

typedef union {
    int i;
} Arg;

typedef struct {
	unsigned int mask;
	unsigned int button;
	void (*func)(const Arg *arg);
	const Arg arg;
} Button;

typedef struct {
    unsigned int mod;
    KeySym keysym;
    void (*func)(const Arg *);
    const Arg arg;
} Key;

enum {
    Image,
    FileList,
    Archive,
} Type;

typedef struct Node Node;
struct Node {
    int type;
    char *name;
    Node *parent;

    union {
        struct {
            unsigned char *imagebuf;
            int width, height;
        } image;
        struct {
            int idx, count;
            struct archive *a;
            struct archive_entry *entry;
        } archive;
        struct {
            int idx, count;
            char * const * filenames;
        } filelist;
    } u;
};

#define AR(node) ((node)->u.archive)
#define IMG(node) ((node)->u.image)
#define FL(node) ((node)->u.filelist)

typedef struct Client Client;
struct Client {
    Node *node, *curnode;

    XImage *img;
    int image_width, image_height;
    int screenWidth, screenHeight;
};

static Client c;
static Display *dpy;
static Window win;
static GC gc;

static int screen;
static Bool running = True;
static char *wmname = "comic";

char *argv0;

static char * readfile(const char *filename, size_t *size);
static int moveoffset(int offset);
static struct archive * openarchive(const char *filename);
static void cleanupnode(Node *node);
static void cleanup(void);
static void decodejpeg(void *buf, size_t size, Node *nodeout);
static void die(const char *errstr, ...);
static void gentitle(Node *node, char *buf, size_t size, size_t *outsize);
static void jpegerrorexit (j_common_ptr ci);
static void loadnext(void);
static void render(void);
static void run(void);
static void setup(void);
static void usage(void);
static void xsettitle(Window w, const char *str);
static Window createwindow(Display *dpy, int screen, int x, int y, int w, int h);
static XImage *createimage(Node *node, int target_w, int target_h);
void quit(const Arg *arg);
void seek(const Arg *arg);
static void buttonpress(XEvent *e);
static void configurenotify(XEvent *e);
static void expose(XEvent *e);
static void keypress(XEvent *e);

/* configuration, allows nested code to access above variables */
#include "config.h"

static void (*handler[LASTEvent]) (XEvent *) = {
    [ButtonPress] = buttonpress,
    [ConfigureNotify] = configurenotify,
    [Expose] = expose,
    [KeyPress] = keypress,
};

void
die(const char *errstr, ...) {
    va_list ap;

    va_start(ap, errstr);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

void
jpegerrorexit (j_common_ptr cinfo) {
    cinfo->err->output_message (cinfo);
    die("Error on jpeg\n");
}

/*This returns an array for a 24 bit image.*/
void
decodejpeg (void *buf, size_t size, Node *nodeout) {
    register JSAMPARRAY lineBuf;
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr err_mgr;
    int width, height, bytesPerPix;
    unsigned char *decodebuf;

    cinfo.err = jpeg_std_error (&err_mgr);
    err_mgr.error_exit = jpegerrorexit;

    jpeg_create_decompress (&cinfo);
    jpeg_mem_src (&cinfo, buf, size);
    jpeg_read_header (&cinfo, 1);
    cinfo.do_fancy_upsampling = 0;
    cinfo.do_block_smoothing = 0;
    jpeg_start_decompress (&cinfo);

    width = cinfo.output_width;
    height = cinfo.output_height;
    bytesPerPix = cinfo.output_components;

    lineBuf = cinfo.mem->alloc_sarray ((j_common_ptr) &cinfo, JPOOL_IMAGE, (width * bytesPerPix), 1);
    decodebuf = malloc(3 * (width * height) + 1);

    if (NULL == buf)
        die("Failed to allocate memory on JPEG decoding");

    int x, y;
    unsigned char *base = decodebuf;
    if (3 == bytesPerPix) {
        int lineOffset = (width * 3);
        for (y = 0; y < cinfo.output_height; ++y) {
            jpeg_read_scanlines (&cinfo, lineBuf, 1);
            memcpy(base, *lineBuf, lineOffset);
            base += lineOffset;
        }
    } else if (1 == bytesPerPix) {
        unsigned int col;
        for (y = 0; y < cinfo.output_height; ++y) {
            jpeg_read_scanlines (&cinfo, lineBuf, 1);

            for (x = 0; x < width; ++x) {
                col = lineBuf[0][x];

                memset(base, col, 3);
                base += 3;
            }
        }
    } else {
        die("The number of color channels is %d."
            "This program only handles 1 or 3\n", bytesPerPix);
    }
    jpeg_finish_decompress (&cinfo);
    jpeg_destroy_decompress (&cinfo);

    IMG(nodeout).imagebuf = decodebuf;
    IMG(nodeout).height = height;
    IMG(nodeout).width = width;
}

char *
readfile(const char *filename, size_t *size) {
    int fd;
    char *buf;
    struct stat stat;

    fd = open(filename, O_RDONLY);
    fstat(fd, &stat);

    buf = malloc(stat.st_size);
    if((*size = read(fd, buf, stat.st_size)) != stat.st_size)
        die("Failed to read whole: %lu != %lu", *size, stat.st_size);

    return buf;
}

Node *
imagenode(const char *name, char *data, size_t size, Node * parent) {
    //TODO: proper error handling
    Node *newnode = malloc(sizeof(Node));
    *newnode = (Node){
        .type = Image,
        .name = strdup(name),
        .parent = parent };

    decodejpeg(data, size, newnode);
    free(data);
    return newnode;
}

void
loadnext(void) {
    char *filename, *data;
    int count;
    size_t size, read;

    struct Node *node, *newnode;
    struct archive *a;
    struct archive_entry *entry;

    node = c.curnode;
    switch(node->type) {
    case Image:
        moveoffset(1);
        return;

    case Archive:
        a = AR(node).a;
        if(archive_read_next_header(a, &entry) != ARCHIVE_OK)
            die("Failed to read archive: %s\n", archive_error_string(a));

        if(archive_entry_filetype(entry) & AE_IFDIR) {
            die("!!!!");
            ++AR(node).idx;
            return;
        }

        size = archive_entry_size(entry);
        if(size > IMAGE_SIZE_LIMIT)
            die("Image size too large");

        data = malloc(size);
        if((read = archive_read_data(a, data, size)) != size)
            die("Failed to read whole %ld != %ld, %s, %s, %s\n", read, size,
                archive_entry_pathname(entry),
                archive_error_string(a), strerror(errno));

        AR(node).entry = entry;
        ++AR(node).idx;
        c.curnode = imagenode(archive_entry_pathname(entry),
            data, size, node);
        break;

    case FileList:
        filename = FL(node).filenames[FL(node).idx];

        //TODO: should detect filetype
        count = 0;
        a = openarchive(filename);
        if(a) {
            while(archive_read_next_header(a, &entry) == ARCHIVE_OK) {
                archive_read_data_skip(a);
                ++count;
            }
            archive_read_free(a);

            newnode = malloc(sizeof(Node));
            *newnode = (Node){
                .type = Archive,
                .name = strdup(filename),
                .parent = node,
                .u = { .archive = {
                    .a = openarchive(filename),
                    .idx = 0,
                    .count = count,
                }}};

            c.curnode = newnode;
            loadnext();
        } else {
            // Cannot open given file as an archive, try to open as a image.
            data = readfile(filename, &size);
            if(!data) {
                printf("Failed to read file: %s", filename);
                moveoffset(1);
                loadnext();
                return;
            }

            c.curnode = imagenode(filename, data, size, node);
        }
        break;
    }
}

XImage *
createimage(Node *node, int target_w, int target_h) {
    unsigned char *buf = IMG(node).imagebuf;
    int w = IMG(node).width, h = IMG(node).height;
    XImage *img = NULL;
    int out_idx = 0;
    int x, y, sample_x, sample_y;
    uint32_t *imagebuf;

    imagebuf = malloc(sizeof(uint32_t) * target_w * target_h);
    // Nearest sampling
    for(y = 0; y < target_h; y++) {
        sample_y = y * h / target_h * w * 3;
        for(x = 0; x < target_w; x++) {
            sample_x = x * w / target_w * 3;
            imagebuf[out_idx] = (*(uint32_t*)(buf + sample_y + sample_x)) << 8;
            ++out_idx;
        }
    }

    img = XCreateImage (dpy,
        CopyFromParent, DefaultDepth(dpy, screen),
        ZPixmap, 0,
        (char *) imagebuf,
        target_w, target_h,
        32, 0
    );

    XInitImage (img);
    img->byte_order = MSBFirst;
    img->bitmap_bit_order = MSBFirst;

    return img;
}

Window
createwindow(Display *dpy, int screen, int x, int y, int w, int h) {
    Window win;
    XSetWindowAttributes wa;

    wa.border_pixel = BlackPixel (dpy, screen);
    wa.background_pixel = BlackPixel (dpy, screen);
    wa.override_redirect = 0;

    win = XCreateWindow (dpy, DefaultRootWindow (dpy),
        x, y,
        w, h,
        0, DefaultDepth(dpy, screen),
        InputOutput, CopyFromParent,
        CWBackPixel | CWBorderPixel, &wa
    );

    return win;
}

void
gentitle(Node *node, char *buf, size_t size, size_t *outsize) {
    if(!node) {
        *outsize = 0;
        return;
    }

    gentitle(node->parent, buf, size, outsize);
    buf += *outsize;
    size -= *outsize;
    switch(node->type) {
    case Image:
        *outsize += snprintf(buf, size, "%s", node->name);
        break;
    case Archive:
        *outsize += snprintf(buf, size, "%s [%d/%d] | ", node->name, AR(node).idx + 1, AR(node).count);
        break;
    case FileList:
        *outsize += snprintf(buf, size, "%s [%d/%d] | ", node->name, FL(node).idx + 1, FL(node).count);
        break;
    }
}

void
render(void) {
    if(c.img) {
        XDestroyImage(c.img);
        c.img = NULL;
    }

    if(c.curnode->type != Image)
        die("BUG: curnode->type != Image on render()");

    int width, height;
    width = IMG(c.curnode).width;
    height = IMG(c.curnode).height;

    if(!width || !height)
        return;

    double ratio = (double)width / height;
    double screenRatio = (double)c.screenWidth / c.screenHeight;

    if(ratio > screenRatio) {
        c.image_width = c.screenWidth;
        c.image_height = height * c.screenWidth / width;
    } else {
        c.image_width = width * c.screenHeight / height;
        c.image_height = c.screenHeight;
    }

    c.img = createimage(c.curnode, c.image_width, c.image_height);
    if (!c.img)
        die("Failed to create image\n");

    XClearArea(dpy, win, 0, 0, 0, 0, False);
    XPutImage (dpy, win, gc, c.img, 0, 0,
        (c.screenWidth - c.image_width) / 2,
        (c.screenHeight - c.image_height) / 2,
        c.image_width, c.image_height);
    XFlush (dpy);

    //TODO: recursive title generation
    size_t writelen;
    char title[TITLE_LENGTH_LIMIT];
    gentitle(c.curnode, title, TITLE_LENGTH_LIMIT, &writelen);
    xsettitle(win, title);
}

void
buttonpress(XEvent *e) {
    int i;
	XButtonPressedEvent *ev = &e->xbutton;

	for(i = 0; i < LENGTH(buttons); i++)
		if(buttons[i].func && buttons[i].button == ev->button
		&& CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state))
			buttons[i].func(&buttons[i].arg);
}

void
configurenotify(XEvent *e) {
    XConfigureEvent xce = e->xconfigure;

    if(c.screenWidth != xce.width || c.screenHeight != xce.height) {
        c.screenWidth = xce.width;
        c.screenHeight = xce.height;
        render();
    }
}

void
expose(XEvent *e) {
    XExposeEvent *ev = &e->xexpose;

    if(ev->count == 0) {
        render();
    }
}

void
run(void) {
    XEvent ev;
    /* main event loop */
    XSync(dpy, False);
    while(running && !XNextEvent(dpy, &ev))
        if(handler[ev.type])
            handler[ev.type](&ev); /* call handler */
}

void
cleanupnode(Node *node) {
    switch(node->type) {
    case Image:
        free(IMG(node).imagebuf);
        free(node->name);
        break;
    case Archive:
        archive_read_free(AR(node).a);
        free(node->name);
        break;
    case FileList:
        break;
    }

    free(node);
    return;
}

void
cleanup(void) {
    XDestroyImage(c.img);

    Node *node;
    while(c.curnode) {
        node = c.curnode->parent;
        cleanupnode(c.curnode);
        c.curnode = node;
    }

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
}

void
quit(const Arg *arg) {
    running = False;
}

int
moveoffset(int offset) {
    Node *node;
    int idx, advance = 0;
    struct archive *a;
    struct archive_entry *entry = NULL;

    node = c.curnode;
    switch(node->type) {
    case Image:
        c.curnode = c.curnode->parent;
        cleanupnode(node);
        return moveoffset(offset);

    case Archive:
        a = AR(node).a;
        idx = AR(node).idx;
        if(idx == AR(node).count) {
            c.curnode = c.curnode->parent;
            cleanupnode(node);
            return moveoffset(offset);
        }

        if(offset >= 0) {
            advance = MIN(offset, AR(node).count - idx);
            while(--advance > 0) {
                ++idx;
                if(archive_read_next_header(a, &entry) != ARCHIVE_OK)
                    die("Failed to seek archive");
            }
        } else {
            archive_read_free(a);
            // should not fail
            a = openarchive(node->name);

            advance = idx + offset;
            idx = 0;
            while(--advance > 0) {
                if(archive_read_next_header(a, &entry) != ARCHIVE_OK)
                    die("Failed to seek archive");
                ++idx;
            }
            AR(node).a = a;
        }

        AR(node).idx = idx;
        AR(node).entry = entry;
        break;

    case FileList:
        // BUG: At the end of the file, Same file opened twice.
        // It does not be a problem if file is an image, but
        // if the last file is an archive, same file is opened twice
        // and seek to position 0
        FL(node).idx = MIN(MAX(FL(node).idx + offset, 0), FL(node).count - 1);
        break;
    }

    loadnext();
    render();
    return advance;
    return 0;
}

void
seek(const Arg *arg) {
    moveoffset(arg->i);
}

void
keypress(XEvent *e) {
    unsigned int i;
    KeySym keysym;
    XKeyEvent *ev;

    ev = &e->xkey;
    keysym = XKeycodeToKeysym(dpy, (KeyCode)ev->keycode, 0);
    for(i = 0; i < LENGTH(keys); i++)
        if(keysym == keys[i].keysym
        && CLEANMASK(keys[i].mod) == CLEANMASK(ev->state)
        && keys[i].func)
            keys[i].func(&(keys[i].arg));
}

void
xsettitle(Window w, const char *str) {
	XTextProperty xtp;

	if(Xutf8TextListToTextProperty(dpy, (char **)&str, 1, XUTF8StringStyle,
				&xtp) == Success) {
		XSetTextProperty(dpy, w, &xtp, XA_WM_NAME);
		XFree(xtp.value);
	}
}

struct archive *
openarchive(const char *filename) {
    struct archive *a;
    a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if(archive_read_open_filename(a, filename, ARCHIVE_BLOCK_SIZE) != ARCHIVE_OK)
        return NULL;

    return a;
}

void
setup(void) {
    dpy = XOpenDisplay(NULL);
    screen = DefaultScreen(dpy);
    win = createwindow (dpy, screen, 0, 0, 800, 600);
    gc = XCreateGC (dpy, win, 0, NULL);

    if(DefaultDepth(dpy, screen) < 24)
        die("This program does not support displays with a depth less than 24\n");

    XMapRaised(dpy, win);
    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask | KeyPressMask | ButtonPressMask);

    c.curnode = c.node;
    loadnext();
}

void
usage(void) {
	fputs("usage: comic [filename]\n", stderr);
	exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[]) {
    /* command line args */
    ARGBEGIN {
    default:
	case 'n':
		wmname = EARGF(usage());
        break;
    } ARGEND;

    if(argc == 0)
        usage();

    c.node = malloc(sizeof(Node));
    *c.node = (Node){
        .type = FileList,
        .name = wmname,
        .parent = NULL,
        .u = { .filelist = { .idx = 0, .count = argc, .filenames = argv } }
    };

    setup();
    run();
    cleanup();

    return EXIT_SUCCESS;
}
