
#define MODKEY Mod1Mask

static Key keys[] = {
    /* modifier       key        function        argument */
    { 0,              XK_q,      quit,          { 0 } },
    { 0,              XK_p,      prev,          {.i = 1 } },
    { 0,              XK_n,      next,          {.i = 1 } },
    { 0,              XK_b,      prev,          {.i = 10 } },
    { 0,              XK_f,      next,          {.i = 10 } },
};

static Button buttons[] = {
    /*event mask      button          function        argument */
    { 0,              Button1,        next,           {0} },
};
