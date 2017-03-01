/* See LICENSE file for copyright and license details. */

#define histsize 2048

/*
 * appearance
 *
 * font: see http://freedesktop.org/software/fontconfig/fontconfig-user.html
 */
static char font[] = "Hack-12:antialias=true:autohint=true";
static int borderpx = 2;

/*
 * What program is execed by st depends of these precedence rules:
 * 1: program passed with -e
 * 2: utmp option
 * 3: SHELL environment variable
 * 4: value of shell in /etc/passwd
 * 5: value of shell in config.h
 */
static char shell[] = "/bin/zsh";
static char *utmp = NULL;
static char stty_args[] = "stty raw pass8 nl -echo -iexten -cstopb 38400";

/* identification sequence returned in DA and DECID */
static char vtiden[] = "\033[?6c";

/* Kerning / character bounding-box multipliers */
static float cwscale = 1.0;
static float chscale = 1.0;

/*
 * word delimiter string
 *
 */
static char worddelimiters[] = " <>'`\"(){}";

/* selection timeouts (in milliseconds) */
static unsigned int doubleclicktimeout = 300;
static unsigned int tripleclicktimeout = 600;

/* frames per second st should at maximum draw to the screen */
static unsigned int xfps = 60;
static unsigned int actionfps = 30;

/*
 * thickness of underline and bar cursors
 */
static unsigned int cursorthickness = 2;

/*
 * bell volume. It must be a value between -100 and 100. Use 0 for disabling
 * it
 */
static int bellvolume = 0;

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
 * Default shape of cursor
 * 2:Underline")
 * 4: Underline ("_")
 * 6: Bar ("|")
 * 7: Snowman ("☃")
 */
static unsigned int cursorshape = 2;

/*
 * Default colour and shape of the mouse cursor
 */
static unsigned int mouseshape = XC_xterm;
static unsigned int mousefg = 7;
static unsigned int mousebg = 0;

/* Internal keyboard shortcuts. */
static Shortcut shortcuts[] = {
	/* mask                  keysym          function    */
	{ ShiftMask,             XK_Insert,      selpaste    },
	{ ShiftMask,             XK_Page_Up,     scrollup    },
	{ ShiftMask,             XK_Page_Down,   scrolldown  },
	{ ControlMask|ShiftMask, XK_C,           clipcopy    },
	{ ControlMask|ShiftMask, XK_V,           clippaste   },
};

/*
 * State bits to ignore when matching key or button events.  By default,
 * numlock (Mod2Mask) and keyboard layout (XK_SWITCH_MOD) are ignored.
 */
static uint ignoremod = Mod2Mask|XK_SWITCH_MOD;

/*
 * Override mouse-select while mask is active (when MODE_MOUSE is set).
 * Note that if you want to use ShiftMask with selmasks, set this to an other
 * modifier, set to 0 to not use it.
 */
static uint forceselmod = ShiftMask;

/*
 * This is the huge key array which defines all compatibility to the Linux
 * world. Please decide about changes wisely.
 */
static Key key[] = {
	/* keysym           mask            string      appcursor */
	{ XK_Up,            ControlMask,    "\033[1;5A",    0 },
	{ XK_Up,            XK_ANY_MOD,     "\033[A",      -1 },
	{ XK_Up,            XK_ANY_MOD,     "\033OA",      +1 },
	{ XK_Down,          ControlMask,    "\033[1;5B",    0 },
	{ XK_Down,          XK_ANY_MOD,     "\033[B",      -1 },
	{ XK_Down,          XK_ANY_MOD,     "\033OB",      +1 },
	{ XK_Right,         ControlMask,    "\033[1;5C",    0 },
	{ XK_Right,         XK_ANY_MOD,     "\033[C",      -1 },
	{ XK_Right,         XK_ANY_MOD,     "\033OC",      +1 },
	{ XK_Left,          ControlMask,    "\033[1;5D",    0 },
	{ XK_Left,          XK_ANY_MOD,     "\033[D",      -1 },
	{ XK_Left,          XK_ANY_MOD,     "\033OD",      +1 },
	{ XK_ISO_Left_Tab,  ShiftMask,      "\033[Z",       0 },
	{ XK_Return,        XK_ANY_MOD,     "\r",           0 },
	{ XK_BackSpace,     XK_NO_MOD,      "\x7F",         0 },
	{ XK_BackSpace,     ControlMask,    "\027",         0 },
	{ XK_Home,          XK_ANY_MOD,     "\033[H",      -1 },
	{ XK_Home,          XK_ANY_MOD,     "\033[1~",     +1 },
	{ XK_Insert,        XK_ANY_MOD,     "\033[2~",      0 },
	{ XK_Delete,        ControlMask,    "\033[3;5~",    0 },
	{ XK_Delete,        ShiftMask,      "\033[3;2~",    0 },
	{ XK_Delete,        XK_ANY_MOD,     "\033[3~",      0 },
	{ XK_End,           XK_ANY_MOD,     "\033[F",      -1 },
	{ XK_End,           XK_ANY_MOD,     "\033[4~",     +1 },
	{ XK_Prior,         XK_ANY_MOD,     "\033[5~",      0 },
	{ XK_Next,          XK_ANY_MOD,     "\033[6~",      0 },
};

/*
 * Printable characters in ASCII, used to estimate the advance width
 * of single wide characters.
 */
static char ascii_printable[] =
	" !\"#$%&'()*+,-./0123456789:;<=>?"
	"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
	"`abcdefghijklmnopqrstuvwxyz{|}~";

