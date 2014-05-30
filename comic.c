#include "arg.h"

#include <archive_entry.h>
#include <archive.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
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

enum {
    Left = 1,
    Right = 2,
    Any = 3,
} Side;

typedef struct {
    unsigned int mask;
    unsigned int button;
    int side;
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
    Page,
    Archive,
    FileList,
} Type;

typedef struct vec2 vec2;
struct vec2 { int x, y; };
static double vec2_ratio(vec2 v) { return (double)v.x / v.y; }
static vec2 vec2_scale(vec2 v, double r) { v.x *= r; v.y *= r; return v; }
static vec2 vec2_add(vec2 v1, vec2 vec2) { v1.x += vec2.x; v1.y += vec2.y; return v1; }

typedef struct Node Node;
struct Node {
    int type;
    char *name;
    Node *parent;

    union {
        struct {
            unsigned char *imagebuf;
            vec2 size;
        } image;
        struct {
            int count;
            Node **images;
        } page;
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
#define PG(node) ((node)->u.page)
#define FL(node) ((node)->u.filelist)

static Display *dpy;
static Window win;
static int screen;
static GC gc;

Node *node, *curnode;
static Bool running = True;
static char *wmname = "comic";
static int imageperpage = 1;
vec2 viewsize;

char *argv0;

static char *readfile(const char *filename, size_t *size);
static int moveoffset(int offset);
static Node *imagenode(Node * parent, const char *name, char *data, size_t size);
static Node *pagenode(Node * parent, Node **images, int count);
static struct archive * openarchive(const char *filename);
static void cleanupnode(Node *node);
static void cleanup(void);
static void decodejpeg(void *buf, size_t size, Node *nodeout);
static void die(const char *errstr, ...);
static char *gentitle(Node *node);
static void jpegerrorexit (j_common_ptr ci);
static void loadnext(void);
static void render(void);
static void run(void);
static void setup(void);
static void usage(void);
static void xsettitle(Window w, const char *str);
static Window createwindow(Display *dpy, int screen, int x, int y, int w, int h);
static XImage *createimage(Node *node, vec2 size);
static void quit(const Arg *arg);
static void seek(const Arg *arg);
static void seekabs(const Arg *arg);
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
    die("Error on jpeg");
}

/*This returns an array for a 24 bit image.*/
void
decodejpeg (void *buf, size_t size, Node *nodeout) {
    JSAMPARRAY linebuf;
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr err_mgr;
    vec2 imgsize;
    int x = 0, y, bytesperpixel, lineoffset;
    unsigned char *decodebuf, *base;

    cinfo.err = jpeg_std_error (&err_mgr);
    err_mgr.error_exit = jpegerrorexit;

    jpeg_create_decompress (&cinfo);
    jpeg_mem_src (&cinfo, buf, size);
    jpeg_read_header (&cinfo, 1);
    jpeg_start_decompress (&cinfo);

    imgsize = (vec2){.x = cinfo.output_width, .y = cinfo.output_height};
    bytesperpixel = cinfo.output_components;

    linebuf = cinfo.mem->alloc_sarray ((j_common_ptr) &cinfo, JPOOL_IMAGE, (imgsize.x * bytesperpixel), 1);
    if(!(decodebuf = malloc(3 * (imgsize.x * imgsize.y) + 1)))
        die("Failed to allocate memory on JPEG decoding");

    base = decodebuf;
    lineoffset = (imgsize.x * 3);
    for (y = 0; y < imgsize.y; ++y) {
        jpeg_read_scanlines (&cinfo, linebuf, 1);
        if (3 == bytesperpixel) {
            memcpy(base, *linebuf, lineoffset);
            base += lineoffset;
        } else if (1 == bytesperpixel) {
            for (x = 0; x < imgsize.x; ++x) {
                memset(base, linebuf[0][x], 3);
                base += 3;
            }
        } else {
            die("The number of color channels is %d."
                "This program only handles 1 or 3\n", bytesperpixel);
        }
    }

    jpeg_finish_decompress (&cinfo);
    jpeg_destroy_decompress (&cinfo);

    IMG(nodeout).imagebuf = decodebuf;
    IMG(nodeout).size = imgsize;
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
pagenode(Node * parent, Node **images, int count) {
    char *name= strdup("page"), *out;
    int i;
    for(i = 0; i < count; i++) {
        asprintf(&out, "%s,%s", name, images[i]->name);
        free(name);
        name = out;
    }
    Node *newnode =malloc(sizeof(Node));
    *newnode = (Node) {
        .parent = parent, .type = Page, .name = name,
        .u = { .page = {
               .count = count,
               .images = images,
        }}};
    return newnode;
}

Node *
imagenode(Node * parent, const char *name, char *data, size_t size) {
    //TODO: proper error handling
    Node *newnode = malloc(sizeof(Node));
    *newnode = (Node){ .type = Image,
                       .name = strdup(name),
                       .parent = parent };
    decodejpeg(data, size, newnode);
    free(data);
    return newnode;
}

void
loadnext(void) {
    char *filename, *data;
    int i, count, ret;
    size_t size, read;

    struct Node *node, *newnode;
    struct archive *a;
    struct archive_entry *entry;

    node = curnode;
    switch(node->type) {
    case Image:
    case Page:
        moveoffset(imageperpage);
        return;

    case Archive:
        a = AR(node).a;

        Node **images = malloc(sizeof(Node *) * imageperpage);
        for(i = 0; i < imageperpage; i++) {
            if((ret = archive_read_next_header(a, &entry)) == ARCHIVE_EOF) {
                break;
            } else if(ret != ARCHIVE_OK) {
                die("Failed to read archive: %s\n", archive_error_string(a));
            }

            if(archive_entry_filetype(entry) & AE_IFDIR) {
                ++AR(node).idx;
                continue;
            }

            if((size = archive_entry_size(entry)) > IMAGE_SIZE_LIMIT)
                die("Image size too large");

            data = malloc(size);
            if((read = archive_read_data(a, data, size)) != size)
                die("Failed to read whole %ld != %ld, %s, %s, %s\n", read, size,
                    archive_entry_pathname(entry),
                    archive_error_string(a), strerror(errno));

            AR(node).entry = entry;
            ++AR(node).idx;
            images[i] = imagenode(node, archive_entry_pathname(entry), data, size);
        }
        curnode = pagenode(node, images, i);;
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

            curnode = newnode;
            loadnext();
        } else {
            // Cannot open given file as an archive, try to open as a image.
            Node **images = malloc(sizeof(Node *) * imageperpage);
            for(i = 0; i < imageperpage; i++) {
                if(FL(node).idx == FL(node).count)
                    break;

                filename = FL(node).filenames[FL(node).idx];
                if(!(data = readfile(filename, &size)))
                    die("failed to read file: %s", filename);

                images[i] = imagenode(node, filename, data, size);
                ++FL(node).idx;
            }
            newnode = pagenode(node, images, i);
            curnode = newnode;
        }
        break;
    }
}

XImage *
createimage(Node *node, vec2 size) {
    unsigned char *buf = IMG(node).imagebuf;
    int w = IMG(node).size.x, h = IMG(node).size.y;
    XImage *img = NULL;
    int out_idx = 0;
    int x, y, sample_x, sample_y;
    uint32_t *imagebuf;

    imagebuf = malloc(sizeof(uint32_t) * size.x * size.y);
    // Nearest sampling
    for(y = 0; y < size.y; y++) {
        sample_y = y * h / size.y * w * 3;
        for(x = 0; x < size.x; x++) {
            sample_x = x * w / size.x * 3;
            imagebuf[out_idx] = (*(uint32_t*)(buf + sample_y + sample_x)) << 8;
            ++out_idx;
        }
    }

    img = XCreateImage (dpy,
        CopyFromParent, DefaultDepth(dpy, screen),
        ZPixmap, 0,
        (char *) imagebuf,
        size.x, size.y,
        32, 0
    );

    XInitImage (img);
    img->byte_order = MSBFirst;
    img->bitmap_bit_order = MSBFirst;

    return img;
}

Window
createwindow(Display *dpy, int screen, int x, int y, int w, int h) {
    XSetWindowAttributes wa;
    wa.border_pixel = BlackPixel(dpy, screen);
    wa.background_pixel = BlackPixel(dpy, screen);
    wa.override_redirect = 0;

    return XCreateWindow(dpy, DefaultRootWindow (dpy),
        x, y, w, h,
        0, DefaultDepth(dpy, screen),
        InputOutput, CopyFromParent,
        CWBackPixel | CWBorderPixel, &wa);
}

char *
gentitle(Node *node) {
    if(!node)
        return strdup("");

    char *title = NULL, *parenttitle;
    parenttitle = gentitle(node->parent);
    if(node->type == Image || node->type == Page)
        asprintf(&title, "%s %s", parenttitle, node->name);
    else if(node->type == Archive)
        asprintf(&title, "%s %s [%d/%d] |", parenttitle, node->name, AR(node).idx + 1, AR(node).count);
    else if(node->type == FileList)
        asprintf(&title, "%s %s [%d/%d] |", parenttitle, node->name, FL(node).idx + 1, FL(node).count);
    free(parenttitle);
    return title;
}

void
render(void) {
    if(curnode->type != Page)
        die("BUG: curnode->type != Image on render()");

    int i;
    Node *imgnode;
    XImage *img;
    vec2 anchor, imgsize, size = (vec2){.x=0, .y=0};

    for(i = 0; i < PG(curnode).count; i++) {
        imgnode = PG(curnode).images[i];
        size.x += IMG(imgnode).size.x;
        size.y = MAX(IMG(imgnode).size.y, size.y);
    }

    double resizeratio;
    if(vec2_ratio(size) > vec2_ratio(viewsize))
        resizeratio = (double)viewsize.x / size.x;
    else
        resizeratio = (double)viewsize.y / size.y;

    XClearArea(dpy, win, 0, 0, 0, 0, False);
    anchor = vec2_scale(vec2_add(viewsize, vec2_scale(size, -resizeratio)), .5f);
    for(i = 0; i < curnode->u.page.count; i++) {
        imgnode = curnode->u.page.images[i];
        imgsize = vec2_scale(IMG(imgnode).size, resizeratio);
        if (!(img = createimage(imgnode, imgsize)))
            die("Failed to create image\n");

        XPutImage (dpy, win, gc, img, 0, 0, anchor.x, anchor.y, imgsize.x, imgsize.y);
        anchor.x += imgsize.x;
        XDestroyImage(img);
    }
    XFlush (dpy);

    char *title = gentitle(curnode);
    xsettitle(win, title);
    free(title);
}

void
buttonpress(XEvent *e) {
    int i;
    XButtonPressedEvent *ev = &e->xbutton;
    int side = (ev->x > viewsize.x / 2) + 1;

    for(i = 0; i < LENGTH(buttons); i++)
        if(buttons[i].func && buttons[i].button == ev->button
                && CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state)
                && side & buttons[i].side)
            buttons[i].func(&buttons[i].arg);
}

void
configurenotify(XEvent *e) {
    XConfigureEvent xce = e->xconfigure;
    viewsize = (vec2){.x = xce.width, .y = xce.height};
    render();
}

void
expose(XEvent *e) {
    if((&e->xexpose)->count)
        render();
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
    int i;
    if(node->type == Image)
        free(IMG(node).imagebuf);
    else if(node->type == Archive)
        archive_read_free(AR(node).a);
    else if(node->type == Page) {
        for(i = 0; i < PG(node).count; i++)
            cleanupnode(PG(node).images[i]);
        free(PG(node).images);
    }

    free(node->name);
    free(node);
    return;
}

void
cleanup(void) {
    Node *node;
    while(curnode) {
        node = curnode->parent;
        cleanupnode(curnode);
        curnode = node;
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

    node = curnode;
    switch(node->type) {
    case Image:
        die("Image on moveoffset()");
    case Page:
        curnode = curnode->parent;
        cleanupnode(node);
        return moveoffset(offset);

    case Archive:
        a = AR(node).a;
        idx = AR(node).idx;
        if(idx == AR(node).count) {
            curnode = curnode->parent;
            cleanupnode(node);
            return moveoffset(offset);
        }

        offset -= imageperpage;
        if(offset >= 0) {
            advance = MIN(offset, AR(node).count - idx);
            while(advance > 0) {
                if(archive_read_next_header(a, &entry) != ARCHIVE_OK)
                    die("Failed to seek archive");
                --advance; ++idx;
            }
        } else {
            archive_read_free(a);
            // should not fail
            a = openarchive(node->name);

            advance = idx + offset;
            idx = 0;
            while(advance > 0) {
                if(archive_read_next_header(a, &entry) != ARCHIVE_OK)
                    die("Failed to seek archive");
                --advance; ++idx;
            }
            AR(node).a = a; } AR(node).idx = idx;
        AR(node).entry = entry;
        break;

    case FileList:
        // BUG: At the end of the file, same file opened twice.
        // It does not be a problem if file is an image, but
        // if the last file is an archive, same file is opened twice
        // and seek to position 0
        FL(node).idx = MAX(MIN(FL(node).idx + offset - imageperpage, FL(node).count - 1), 0);
        break;
    }

    loadnext();
    render();
    return advance;
    return 0;
}

void
seek(const Arg *arg) {
    moveoffset(arg->i * imageperpage);
}

void
seekabs(const Arg *arg) {
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

    loadnext();
}

void
usage(void) {
    fputs("usage: comic [-d] [filename]\n", stderr);
    exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[]) {
    /* command line args */
    ARGBEGIN {
    default:
    case 'd':
        imageperpage = 2;
        break;
    case 'n':
        wmname = EARGF(usage());
        break;
    } ARGEND;

    if(argc == 0)
        usage();

    node = malloc(sizeof(Node));
    *node = (Node){
        .type = FileList,
        .name = strdup(wmname),
        .parent = NULL,
        .u = { .filelist = { .idx = 0, .count = argc, .filenames = argv } }
    };
    curnode = node;

    setup();
    run();
    cleanup();

    return EXIT_SUCCESS;
}
