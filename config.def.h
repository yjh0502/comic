
#define MODKEY Mod1Mask

static Key keys[] = {
    /* modifier       key        function        argument */
    { 0,              XK_q,      quit,          { 0 } },
    { 0,              XK_n,      next,          { 0 } },
};

static Button buttons[] = {
    /*event mask      button          function        argument */
    { 0,              Button1,        next,           {0} },
};
