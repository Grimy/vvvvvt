/* See LICENSE file for copyright and license details. */

/* see http://freedesktop.org/software/fontconfig/fontconfig-user.html */
static char fontname[] = "Hack:antialias=true:autohint=true";
static char *shell[] = { "/bin/zsh", NULL };
static int borderpx = 2;

/* identification sequence returned in DA and DECID */
static char vtiden[] = "\033[?6c";

/* Kerning / character bounding-box modifiers */
static int cw_add = 0;
static int ch_add = 0;

/* word delimiter string */
static char worddelimiters[] = " <>'`\"(){}";

/* frames per second st should at maximum draw to the screen */
#define FPS 60

/* Bell volume. It must be a value between -100 and 100. */
static int bellvolume = 12;

/* default TERM value */
static char* term_name = "st-256color";
static char* term_class = "st-256color";

/* spaces per tab */
static unsigned int tabspaces = 8;

/* Terminal colors (16 first used in escape sequence) */
static const char *colorname[] = {
	"#15141b",
	"#ff5584",
	"#50a708",
	"#b88d08",
	"#0aa2b5",
	"#ff43c7",
	"#08a875",
	"#8d949d",
	"#2b262c",
	"#e77308",
	"#6fe40f",
	"#fac10f",
	"#9acfff",
	"#9481fe",
	"#11e1d1",
	"#f5f3f5",
};

/* Default colors (colorname index) */
#define defaultfg 15
#define defaultbg 0

/* Internal keyboard shortcuts. */
static Shortcut shortcuts[] = {
	/* mask                   keysym         function   */
	{ ShiftMask,              XK_Insert,     selpaste   },
	{ ShiftMask,              XK_Page_Up,    scrollup   },
	{ ShiftMask,              XK_Page_Down,  scrolldown },
	{ ControlMask|ShiftMask,  XK_C,          clipcopy   },
	{ ControlMask|ShiftMask,  XK_V,          clippaste  },
};

/*
 * Override mouse-select while mask is active (when MODE_MOUSE is set).
 * Note that if you want to use ShiftMask with selmasks, set this to an other
 * modifier, set to 0 to not use it.
 */
static u32 forceselmod = ShiftMask;

/*
 * This is the huge key array which defines all compatibility to the Linux
 * world. Please decide about changes wisely.
 */
static Key key[] = {
	/* keysym           mask          app   string */
	{ XK_Up,            ControlMask,    0,  "\033[1;5A" },
	{ XK_Up,            0,              1,  "\033[A"    },
	{ XK_Up,            0,              0,  "\033OA"    },
	{ XK_Down,          ControlMask,    0,  "\033[1;5B" },
	{ XK_Down,          0,              1,  "\033[B"    },
	{ XK_Down,          0,              0,  "\033OB"    },
	{ XK_Right,         ControlMask,    0,  "\033[1;5C" },
	{ XK_Right,         0,              1,  "\033[C"    },
	{ XK_Right,         0,              0,  "\033OC"    },
	{ XK_Left,          ControlMask,    0,  "\033[1;5D" },
	{ XK_Left,          0,              1,  "\033[D"    },
	{ XK_Left,          0,              0,  "\033OD"    },
	{ XK_ISO_Left_Tab,  0,              0,  "\033[Z"    },
	{ XK_BackSpace,     0,              0,  "\x7F"      },
	{ XK_BackSpace,     ControlMask,    0,  "\027"      },
	{ XK_Home,          0,              1,  "\033[H"    },
	{ XK_Home,          0,              0,  "\033[1~"   },
	{ XK_Insert,        0,              0,  "\033[2~"   },
	{ XK_Delete,        ControlMask,    0,  "\033[3;5~" },
	{ XK_Delete,        ShiftMask,      0,  "\033[3;2~" },
	{ XK_Delete,        0,              0,  "\033[3~"   },
	{ XK_End,           0,              1,  "\033[F"    },
	{ XK_End,           0,              0,  "\033[4~"   },
	{ XK_Prior,         0,              0,  "\033[5~"   },
	{ XK_Next,          0,              0,  "\033[6~"   },
};
