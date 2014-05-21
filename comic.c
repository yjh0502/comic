#include "arg.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/time.h>
#include <string.h>

#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>

#include <archive.h>
#include <archive_entry.h>
#include <jpeglib.h>
#include <jerror.h>

/* macros */
#define BUTTONMASK              (ButtonPressMask|ButtonReleaseMask)
#define CLEANMASK(mask)         (mask & ~(LockMask) & (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define MOUSEMASK               (BUTTONMASK|PointerMotionMask)




typedef union {
    int i;
    unsigned int ui;
    float f;
    const void *v;
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

typedef struct Client Client;
struct Client {
    const char *filename;

    int idx, count;
    struct archive *a;
    struct archive_entry *entry;

    int screenWidth, screenHeight;
    unsigned char *buf;
    int width, height;
    XImage *img;
    int image_width, image_height;

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
static unsigned char * decodejpeg(void *buf, size_t size, int *w, int *h);
static XImage *createimage(unsigned char *buf, int w, int h, int target_w, int target_h);
static Window createwindow(Display *dpy, int screen, int x, int y, int w, int h);
static struct archive * openarchive(const char *filename);

static void quit(const Arg *arg);
static void prev(const Arg *arg);
static void next(const Arg *arg);

static void loadnext(void);
static void setup(void);
static void usage(void);
static void run(void);
static void cleanup(void);
static void render(void);

static void buttonpress(XEvent *e);
static void configurenotify(XEvent *e);
static void expose(XEvent *e);
static void keypress(XEvent *e);

/* configuration, allows nested code to access above variables */
#include "config.h"

static void (*handler[LASTEvent]) (XEvent *) = {
    [ButtonPress] = buttonpress,
    /*
    [ClientMessage] = clientmessage,
    [ConfigureRequest] = configurerequest,
    */
    [ConfigureNotify] = configurenotify,
    /*
    [DestroyNotify] = destroynotify,
    [EnterNotify] = enternotify,
    */
    [Expose] = expose,
    [KeyPress] = keypress,
    /*
    [MappingNotify] = mappingnotify,
    [MapRequest] = maprequest,
    [MotionNotify] = motionnotify,
    [PropertyNotify] = propertynotify,
    [UnmapNotify] = unmapnotify
    */
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
unsigned char *
decodejpeg (void *buf, size_t size, int *widthPtr, int *heightPtr) {
    register JSAMPARRAY lineBuf;
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr err_mgr;
    int bytesPerPix;
    unsigned char *retBuf;

    cinfo.err = jpeg_std_error (&err_mgr);
    err_mgr.error_exit = jpegerrorexit;

    jpeg_create_decompress (&cinfo);
    jpeg_mem_src (&cinfo, buf, size);
    jpeg_read_header (&cinfo, 1);
    cinfo.do_fancy_upsampling = 0;
    cinfo.do_block_smoothing = 0;
    jpeg_start_decompress (&cinfo);

    *widthPtr = cinfo.output_width;
    *heightPtr = cinfo.output_height;
    bytesPerPix = cinfo.output_components;

    lineBuf = cinfo.mem->alloc_sarray ((j_common_ptr) &cinfo, JPOOL_IMAGE, (*widthPtr * bytesPerPix), 1);
    retBuf = malloc (3 * (*widthPtr * *heightPtr));

    if (NULL == retBuf) {
        perror (NULL);
        return NULL;
    }

    if (3 == bytesPerPix) {
        int lineOffset = (*widthPtr * 3);
        int y;
        unsigned char *base = retBuf;
        for (y = 0; y < cinfo.output_height; ++y) {
            jpeg_read_scanlines (&cinfo, lineBuf, 1);
            memcpy(base, *lineBuf, lineOffset);
            base += lineOffset;
        }
    } else if (1 == bytesPerPix) {
        unsigned int col;
        int width = *widthPtr;
        int x, y;

        unsigned char * base = retBuf;
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

    return retBuf;
}


void
loadnext(void) {
    int res = archive_read_next_header(c.a, &c.entry);
    if(res == ARCHIVE_EOF)
        return;

    if(res != ARCHIVE_OK)
        die("Failed to read archive: %s\n", archive_error_string(c.a));

    const char * name = archive_entry_pathname(c.entry);
    size_t size = archive_entry_size(c.entry);
    printf("%s %lu\n", name, size);

    char * data = malloc(size);
    size_t read = archive_read_data(c.a, data, size);
    if(read != size)
        die("Failed to read whole: %lu != %lu\n", read, size);

    free(c.buf);
    c.buf = decodejpeg(data, size, &c.width, &c.height);
    if (!c.buf)
        die("Unable to decode JPEG\n");
    free(data);

    ++c.idx;
}

XImage *
createimage(unsigned char *buf, int w, int h, int target_w, int target_h) {
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

    double ratio = (double)c.width / c.height;
    double screenRatio = (double)c.screenWidth / c.screenHeight;

    if(ratio > screenRatio) {
        c.image_width = c.screenWidth;
        c.image_height = c.height * c.screenWidth / c.width;
    } else {
        c.image_width = c.width * c.screenHeight / c.height;
        c.image_height = c.screenHeight;
    }

    c.img = createimage(c.buf, c.width, c.height,
        c.image_width, c.image_height);
    if (!c.img)
        die("Failed to create image\n");

    XClearArea(dpy, win, 0, 0, 0, 0, False);
    XPutImage (dpy, win, gc, c.img, 0, 0,
        (c.screenWidth - c.image_width) / 2,
        (c.screenHeight - c.image_height) / 2,
        c.image_width, c.image_height);
    XFlush (dpy);
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
cleanup(void) {
    XDestroyImage(c.img);
    free(c.buf);

    archive_read_free(c.a);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
}

void
quit(const Arg *arg) {
    running = False;
}

void
prev(const Arg *arg) {
    int advance;
    struct archive_entry *entry;

    archive_read_free(c.a);
    c.a = openarchive(c.filename);

    advance = c.idx - arg->i;
    c.idx = 0;
    while(--advance > 0) {
        if(archive_read_next_header(c.a, &entry) != ARCHIVE_OK)
            die("Failed to seek archive");
        ++c.idx;
    }

    loadnext();
    render();
}

void
next(const Arg *arg) {
    int advance;
    if(c.idx == c.count)
        return;

    advance = MIN(arg->i, c.count - c.idx);
    while(--advance > 0) {
        ++c.idx;
        if(archive_read_next_header(c.a, &c.entry) != ARCHIVE_OK)
            die("Failed to seek archive");
    }

    loadnext();
    render();
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

struct archive *
openarchive(const char *filename) {
    struct archive *a;
    a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if(archive_read_open_filename(a, c.filename, 10240) != ARCHIVE_OK)
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

    a = openarchive(c.filename);
    while(archive_read_next_header(a, &entry) == ARCHIVE_OK)
        ++c.count;
    archive_read_free(a);

    c.a = openarchive(c.filename);
    c.idx = 0;

    XMapRaised(dpy, win);
    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask | KeyPressMask | MOUSEMASK);

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
    c.filename = argv[0];

    setup();
    run();
    cleanup();

    return EXIT_SUCCESS;
}
