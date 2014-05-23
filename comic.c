#include "arg.h"

#include <archive_entry.h>
#include <archive.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
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

char *argv0;

static void die(const char *errstr, ...);
static void jpegerrorexit (j_common_ptr ci);
static void decodejpeg(void *buf, size_t size, Node *nodeout);
static XImage *createimage(Node *node, int target_w, int target_h);
static Window createwindow(Display *dpy, int screen, int x, int y, int w, int h);
static struct archive * openarchive(const char *filename);

static int moveoffset(int offset);

void seek(const Arg *arg);
void quit(const Arg *arg);

static void loadnext(void);
static void setup(void);
static void usage(void);
static void run(void);
static void cleanupnode(Node *node);
static void cleanup(void);
static void render(void);
static void xsettitle(Window w, const char *str);

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
    decodebuf = malloc (3 * (width * height));

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

void
loadnext(void) {
    char *data;
    size_t size, read;

    struct Node *node;
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

        printf("%s\n", archive_entry_pathname(entry));
        if(archive_entry_filetype(entry) & AE_IFDIR) {
            ++AR(node).idx;
            return;
        }

        size = archive_entry_size(entry);
        if(size > IMAGE_SIZE_LIMIT)
            die("Image size too large");

        data = malloc(size);
        if((read = archive_read_data(a, data, size)) != size)
            die("Failed to read whole %d != %d, %s, %s, %s\n", read, size,
                archive_entry_pathname(entry),
                archive_error_string(a), strerror(errno));

        Node *imagenode = malloc(sizeof(Node));
        *imagenode = (Node){
            .type = Image,
            .name = strdup(archive_entry_pathname(entry)),
            .parent = c.curnode };

        decodejpeg(data, size, imagenode);
        free(data);

        AR(node).entry = entry;
        ++AR(node).idx;
        c.curnode = imagenode;
        break;
    case FileList:
        //TODO:
        die("Not implemented yet: loadnext, filelist");
    }
}

XImage *
createimage(Node *node, int target_w, int target_h) {
    unsigned char *buf = IMG(node).imagebuf;
    int w = IMG(node).width, h = IMG(node).height;
    XImage *img = NULL;
    int i = 0, out_idx = 0;
    int x, y, sample_x, sample_y;
    uint32_t *image_buf;

    image_buf = malloc (sizeof(uint32_t) * target_w* target_h);
    // Nearest sampling
    for(y = 0; y < target_h; y++) {
        sample_y = y * h / target_h;
        for(x = 0; x < target_w; x++) {
            sample_x = x * w / target_w;
            i = (sample_y * w + sample_x) * 3;

            image_buf[out_idx] = (buf[i] << 16) | (buf[i+1] << 8) | (buf[i+2]);
            ++out_idx;
        }
    }

    img = XCreateImage (dpy,
        CopyFromParent, DefaultDepth(dpy, screen),
        ZPixmap, 0,
        (char *) image_buf,
        target_w, target_h,
        32, 0
    );

    XInitImage (img);
    img->byte_order = LSBFirst;
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
render(void) {
    if(c.img)
        XDestroyImage(c.img);

    if(c.curnode->type != Image)
        die("BUG: curdnode->type != Image on render()");

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
    char title[TITLE_LENGTH_LIMIT];
    snprintf(title, TITLE_LENGTH_LIMIT, "%s", c.node->name);
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
        printf("w:%d, h:%d\n", c.screenWidth, c.screenHeight);
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
        break;
    case FileList:
        die("Not implemented: cleanupnode, filelist");
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
        if(idx == AR(c.node).count)
            return 0;

        if(offset >= 0) {
            advance = MIN(offset, AR(node).count - idx);
            while(--advance > 0) {
                ++idx;
                if(archive_read_next_header(a, &entry) != ARCHIVE_OK)
                    die("Failed to seek archive");
            }
        } else {
            archive_read_free(a);
            a = openarchive(node->name);

            advance = AR(c.node).idx + offset;
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
        //TODO:
        die("Not implemented yet: seek filelist");
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

	if(XmbTextListToTextProperty(dpy, (char **)&str, 1, XCompoundTextStyle,
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
    if(archive_read_open_filename(a, c.node->name, ARCHIVE_BLOCK_SIZE) != ARCHIVE_OK)
        die("Failed to open archive: %s\n", archive_error_string(a));

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

    struct archive *a;
    struct archive_entry *entry;
    a = openarchive(c.node->name);
    while(archive_read_next_header(a, &entry) == ARCHIVE_OK)
        ++AR(c.node).count;

    archive_read_free(a);

    AR(c.node).a = openarchive(c.node->name);
    AR(c.node).idx = 0;

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
        break;
    } ARGEND;

    if(argc == 0)
        usage();

    c.node = malloc(sizeof(Node));
    *c.node = (Node){
        .parent = NULL,
        .type = Archive,
        .name = argv[0],
    };

    setup();
    run();
    cleanup();

    return EXIT_SUCCESS;
}
