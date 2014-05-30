#define ARCHIVE_BLOCK_SIZE  1024 * 16
#define IMAGE_SIZE_LIMIT    1000 * 1000 * 10
#define TITLE_LENGTH_LIMIT  1024

#define MODKEY Mod1Mask
static Key keys[] = {
    /* modifier       key        function        argument */
    { 0,              XK_q,      quit,          { 0 } },
    { 0,              XK_p,      seek,          {.i = -1 } },
    { 0,              XK_n,      seek,          {.i = 1 } },
    { 0,              XK_b,      seek,          {.i = -10 } },
    { 0,              XK_f,      seek,          {.i = 10 } },
    { 0,              XK_comma,  seekabs,       {.i = -1 } },
    { 0,              XK_period, seekabs,       {.i = 1 } },
};

static Button buttons[] = {
    /*event mask      button          function        argument */
    { 0,              Button1,        seek,           {.i = 1 } },
};
