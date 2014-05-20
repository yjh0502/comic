#include "arg.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

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
#define INTERSECT(x,y,w,h,m)    (MAX(0, MIN((x)+(w),(m)->wx+(m)->ww) - MAX((x),(m)->wx)) \
                               * MAX(0, MIN((y)+(h),(m)->wy+(m)->wh) - MAX((y),(m)->wy)))
#define ISVISIBLE(C)            ((C->tags & C->mon->tagset[C->mon->seltags]))
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define MOUSEMASK               (BUTTONMASK|PointerMotionMask)
#define WIDTH(X)                ((X)->w + 2 * (X)->bw)
#define HEIGHT(X)               ((X)->h + 2 * (X)->bw)
#define TAGMASK                 ((1 << LENGTH(tags)) - 1)
#define TEXTW(X)                (textnw(X, strlen(X)) + dc.font.height)




typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct Client Client;
struct Client {
	const char *filename;
	struct archive *a;
	struct archive_entry *entry;

    XImage *img;
    int imageWidth;
    int imageHeight;

};

static Client c;
static Display *dpy;
static Window win;
static GC gc;

static int screen;
static Bool running = True;

char *argv0;

static void die(const char *errstr, ...);

static int get_byte_order (void);
static void jpeg_error_exit (j_common_ptr cinfo);

unsigned char * list(char *filename, int *widthPtr, int *heightPtr);
static unsigned char * decode_jpeg(void *buf, size_t size, int *widthPtr, int *heightPtr);
static XImage *create_image_from_buffer(unsigned char *buf, int width, int height);

static Window create_window(Display *dpy, int screen, int x, int y, int width, int height);

static int64_t curusec(void);

static void quit(const Arg *arg);
static void next(const Arg *arg);

static void keypress(XEvent *e);
static void loadnext(void);
static void setup(void);
static void run(void);
static void cleanup(void);
static void render(void);
static void expose(XEvent *e);

/* configuration, allows nested code to access above variables */
#include "config.h"

static void (*handler[LASTEvent]) (XEvent *) = {
	/*
	[ButtonPress] = buttonpress,
	[ClientMessage] = clientmessage,
	[ConfigureRequest] = configurerequest,
	[ConfigureNotify] = configurenotify,
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


static void
die(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

int64_t
curusec(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000 * 1000) + tv.tv_usec;
}

int
get_byte_order(void) {
    union {
        char c[sizeof(short)];
        short s;
    } order;

    order.s = 1;
    if ((1 == order.c[0])) {
        return LSBFirst;
    } else {
        return MSBFirst;
    }
}

void
jpeg_error_exit (j_common_ptr cinfo) {
    cinfo->err->output_message (cinfo);
    exit (EXIT_FAILURE);
}


/*This returns an array for a 24 bit image.*/
static unsigned char *
decode_jpeg (void *buf, size_t size, int *widthPtr, int *heightPtr) {
    register JSAMPARRAY lineBuf;
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr err_mgr;
    int bytesPerPix;
    unsigned char *retBuf;

    cinfo.err = jpeg_std_error (&err_mgr);
    err_mgr.error_exit = jpeg_error_exit;

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


	printf("bytesPerPix: %d\n", bytesPerPix);
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
        fprintf (stderr, "Error: the number of color channels is %d.  This program only handles 1 or 3\n", bytesPerPix);
        return NULL;
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

	uint64_t start, end;
	
	if(res != ARCHIVE_OK)
		die("Failed to read archive: %s", archive_error_string(c.a));

	const char * name = archive_entry_pathname(c.entry);
	size_t size = archive_entry_size(c.entry);
	printf("%s %lu\n", name, size);

	char * data = malloc(size);
	size_t read = archive_read_data(c.a, data, size);
	if(read != size)
		die("Failed to read whole: %lu != %lu", read, size);

	u_char *buf;
	start = curusec();
	buf = decode_jpeg(data, size, &c.imageWidth, &c.imageHeight);
	printf("decode_jpeg() %d us\n", curusec() - start);
    if (!buf)
        die("Unable to decode JPEG");
	free(data);

	start = curusec();

	if(c.img)
		XDestroyImage(c.img);

    c.img = create_image_from_buffer(buf, c.imageWidth, c.imageHeight);
	printf("create_image_from_buffer() %d us\n", curusec() - start);
    if (!c.img)
		die("Failed to create image");

    free (buf);
}

static XImage *
create_image_from_buffer (unsigned char *buf, int width, int height) {
    int depth;
    XImage *img = NULL;
    Visual *vis;
    double rRatio;
    double gRatio;
    double bRatio;
    int outIndex = 0;
    int i;
    int numBufBytes = (3 * (width * height));

    depth = DefaultDepth (dpy, screen);
    vis = DefaultVisual (dpy, screen);

    rRatio = vis->red_mask / 255.0;
    gRatio = vis->green_mask / 255.0;
    bRatio = vis->blue_mask / 255.0;


    if (depth >= 24) {
        size_t numNewBufBytes = (4 * (width * height));
        u_int32_t *newBuf = malloc (numNewBufBytes);

        for (i = 0; i < numBufBytes; i += 3) {
            unsigned int r, g, b;
            r = (buf[i] << 16);
            g = (buf[i+1] << 8);
            b = (buf[i+2]);

            newBuf[outIndex] = r | g | b;
            ++outIndex;
		}

        img = XCreateImage (dpy,
            CopyFromParent, depth,
            ZPixmap, 0,
            (char *) newBuf,
            width, height,
            32, 0
        );

    } else if (depth >= 15) {
        size_t numNewBufBytes = (2 * (width * height));
        u_int16_t *newBuf = malloc (numNewBufBytes);

        for (i = 0; i < numBufBytes; ++i) {
            unsigned int r, g, b;

            r = (buf[i] * rRatio);
            ++i;
            g = (buf[i] * gRatio);
            ++i;
            b = (buf[i] * bRatio);

            r &= vis->red_mask;
            g &= vis->green_mask;
            b &= vis->blue_mask;

            newBuf[outIndex] = r | g | b;
            ++outIndex;
        }

        img = XCreateImage (dpy,
            CopyFromParent, depth,
            ZPixmap, 0,
            (char *) newBuf,
            width, height,
            16, 0
        );
    } else {
        fprintf (stderr, "This program does not support dpyplays with a depth less than 15.");
        return NULL;
    }

    XInitImage (img);
    /*Set the client's byte order, so that XPutImage knows what to do with the data.*/
    /*The default in a new X image is the server's format, which may not be what we want.*/
    if ((LSBFirst == get_byte_order ())) {
        img->byte_order = LSBFirst;
    } else {
        img->byte_order = MSBFirst;
    }

    /*The bitmap_bit_order doesn't matter with ZPixmap images.*/
    img->bitmap_bit_order = MSBFirst;

    return img;
}

static Window
create_window (Display *dpy, int screen, int x, int y, int width, int height) {
    Window win;
    unsigned long windowMask;
    XSetWindowAttributes winAttrib;

    windowMask = CWBackPixel | CWBorderPixel;
    winAttrib.border_pixel = BlackPixel (dpy, screen);
    winAttrib.background_pixel = BlackPixel (dpy, screen);
    winAttrib.override_redirect = 0;

    win = XCreateWindow (dpy, DefaultRootWindow (dpy),
        x, y,
        width, height,
        0, DefaultDepth(dpy, screen),
        InputOutput, CopyFromParent,
        windowMask, &winAttrib
    );

    return win;
}

void
render(void) {
	XPutImage (dpy, win, gc, c.img, 0, 0, 0, 0, c.imageWidth, c.imageHeight);
	XFlush (dpy);
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
	while(running && !XNextEvent(dpy, &ev)) {
		printf("event:%d\n", ev.type);
		if(handler[ev.type])
			handler[ev.type](&ev); /* call handler */
	}
}

void
cleanup(void) {
	archive_read_free(c.a);
}

void
quit(const Arg *arg) {
	running = False;
}

void
next(const Arg *arg) {
	uint64_t start, end;
	start = curusec();
	loadnext();
	printf("loadnext() %dus\n", curusec() - start);

	start = curusec();
	render();
	printf("render() %dus\n", curusec() - start);
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
setup(void) {
    dpy = XOpenDisplay(NULL);
	screen = DefaultScreen(dpy);
    win = create_window (dpy, screen, 0, 0, 10, 10);
    gc = XCreateGC (dpy, win, 0, NULL);

	c.a = archive_read_new();
	archive_read_support_filter_all(c.a);
	archive_read_support_format_all(c.a);
	if(archive_read_open_filename(c.a, c.filename, 10240) != ARCHIVE_OK)
		die("Failed to open archive: %s", archive_error_string(c.a));

    XMapRaised (dpy, win);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask);
}

int
main(int argc, char *argv[]) {
	/* command line args */
	ARGBEGIN {
	default:
		break;
	} ARGEND;

	if(argc == 0)
		die("No filename specified");
	c.filename = argv[0];
	
	setup();
	// Populate first image
	loadnext();
	run();
	cleanup();
    
    return EXIT_SUCCESS;
}
