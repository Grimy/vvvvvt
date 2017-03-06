/* See LICENSE file for copyright and license details. */

#define histsize 2048

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
#define FPS 30

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

/*
 * Default colour and shape of the mouse cursor
 */
static unsigned int mouseshape = XC_xterm;
static unsigned int mousefg = 7;
static unsigned int mousebg = 0;

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
 * State bits to ignore when matching key or button events.  By default,
 * numlock (Mod2Mask) and keyboard layout (XK_SWITCH_MOD) are ignored.
 */
static u32 ignoremod = Mod2Mask|XK_SWITCH_MOD;

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
	/* keysym           mask          app  string */
	{ XK_Up,            ControlMask,   0,  "\033[1;5A" },
	{ XK_Up,            XK_ANY_MOD,   -1,  "\033[A"    },
	{ XK_Up,            XK_ANY_MOD,   +1,  "\033OA"    },
	{ XK_Down,          ControlMask,   0,  "\033[1;5B" },
	{ XK_Down,          XK_ANY_MOD,   -1,  "\033[B"    },
	{ XK_Down,          XK_ANY_MOD,   +1,  "\033OB"    },
	{ XK_Right,         ControlMask,   0,  "\033[1;5C" },
	{ XK_Right,         XK_ANY_MOD,   -1,  "\033[C"    },
	{ XK_Right,         XK_ANY_MOD,   +1,  "\033OC"    },
	{ XK_Left,          ControlMask,   0,  "\033[1;5D" },
	{ XK_Left,          XK_ANY_MOD,   -1,  "\033[D"    },
	{ XK_Left,          XK_ANY_MOD,   +1,  "\033OD"    },
	{ XK_Escape,        XK_ANY_MOD,    0,  "\033"      },
	{ XK_ISO_Left_Tab,  ShiftMask,     0,  "\033[Z"    },
	{ XK_ISO_Left_Tab,  XK_ANY_MOD,    0,  "\t"        },
	{ XK_Return,        XK_ANY_MOD,    0,  "\r"        },
	{ XK_KP_Enter,      XK_ANY_MOD,    0,  "\r"        },
	{ XK_BackSpace,     XK_NO_MOD,     0,  "\x7F"      },
	{ XK_BackSpace,     ControlMask,   0,  "\027"      },
	{ XK_Home,          XK_ANY_MOD,   -1,  "\033[H"    },
	{ XK_Home,          XK_ANY_MOD,   +1,  "\033[1~"   },
	{ XK_Insert,        XK_ANY_MOD,    0,  "\033[2~"   },
	{ XK_Delete,        ControlMask,   0,  "\033[3;5~" },
	{ XK_Delete,        ShiftMask,     0,  "\033[3;2~" },
	{ XK_Delete,        XK_ANY_MOD,    0,  "\033[3~"   },
	{ XK_End,           XK_ANY_MOD,   -1,  "\033[F"    },
	{ XK_End,           XK_ANY_MOD,   +1,  "\033[4~"   },
	{ XK_Prior,         XK_ANY_MOD,    0,  "\033[5~"   },
	{ XK_Next,          XK_ANY_MOD,    0,  "\033[6~"   },
};

/*
 * Printable characters in ASCII, used to estimate the advance width
 * of single wide characters.
 */
static char ascii_printable[] =
	" !\"#$%&'()*+,-./0123456789:;<=>?"
	"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
	"`abcdefghijklmnopqrstuvwxyz{|}~";

