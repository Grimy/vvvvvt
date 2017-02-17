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

/* alt screens */
static int allowaltscreen = 1;

/* frames per second st should at maximum draw to the screen */
static unsigned int xfps = 60;
static unsigned int actionfps = 30;

/*
 * blinking timeout (set to 0 to disable blinking) for the terminal blinking
 * attribute.
 */
static unsigned int blinktimeout = 387;

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
static char termname[] = "st-256color";

/*
 * spaces per tab
 *
 * When you are changing this value, don't forget to adapt the »it« value in
 * the st.info and appropriately install the st.info in the environment where
 * you use this st version.
 *
 *	it#$tabspaces,
 *
 * Secondly make sure your kernel is not expanding tabs. When running `stty
 * -a` »tab0« should appear. You can tell the terminal to not expand tabs by
 *  running following command:
 *
 *	stty tabs
 */
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
	"#2b262c",
	"#8d949d",
	"#e77308",
	"#6fe40f",
	"#fac10f",
	"#9acfff",
	"#9481fe",
	"#11e1d1",
	"#f5f3f5",

	/* more colors can be added after 255 to use with DefaultXX */
	[255] = 0,
	"#cccccc",
	"#555555",
};


/*
 * Default colors (colorname index)
 * foreground, background, cursor, reverse cursor
 */
static unsigned int defaultfg = 15;
static unsigned int defaultbg = 0;
static unsigned int defaultcs = 256;
static unsigned int defaultrcs = 257;

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

/*
 * Colors used, when the specific fg == defaultfg. So in reverse mode this
 * will reverse too. Another logic would only make the simple feature too
 * complex.
 */
static unsigned int defaultitalic = 11;
static unsigned int defaultunderline = 7;

/* Internal keyboard shortcuts. */

static Shortcut shortcuts[] = {
	/* mask                  keysym          function        argument */
	{ XK_ANY_MOD,            XK_Break,       sendbreak,      {.i =  0} },
	{ ControlMask|ShiftMask, XK_Prior,       xzoom,          {.f = +1} },
	{ ControlMask|ShiftMask, XK_Next,        xzoom,          {.f = -1} },
	{ ControlMask|ShiftMask, XK_Home,        xzoomreset,     {.f =  0} },
	{ ShiftMask,             XK_Insert,      selpaste,       {.i =  0} },
	{ XK_ANY_MOD,            XK_Page_Up,     kscrollup,      {.i = -2} },
	{ XK_ANY_MOD,            XK_Page_Down,   kscrolldown,    {.i = -2} },
	{ ControlMask|ShiftMask, XK_C,           clipcopy,       {.i =  0} },
	{ ControlMask|ShiftMask, XK_V,           clippaste,      {.i =  0} },
};

/*
 * Special keys (change & recompile st.info accordingly)
 *
 * Mask value:
 * * Use XK_ANY_MOD to match the key no matter modifiers state
 * * Use XK_NO_MOD to match the key alone (no modifiers)
 * appkey value:
 * * 0: no value
 * * > 0: keypad application mode enabled
 * *   = 2: term.numlock = 1
 * * < 0: keypad application mode disabled
 * appcursor value:
 * * 0: no value
 * * > 0: cursor application mode enabled
 * * < 0: cursor application mode disabled
 * crlf value
 * * 0: no value
 * * > 0: crlf mode is enabled
 * * < 0: crlf mode is disabled
 *
 * Be careful with the order of the definitions because st searches in
 * this table sequentially, so any XK_ANY_MOD must be in the last
 * position for a key.
 */

/*
 * If you want keys other than the X11 function keys (0xFD00 - 0xFFFF)
 * to be mapped below, add them to this array.
 */
static KeySym mappedkeys[] = { -1 };

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
	/* keysym           mask            string      appkey appcursor crlf */
	{ XK_Up,            ShiftMask,      "\033[1;2A",     0,    0,    0},
	{ XK_Up,            ControlMask,    "\033[1;5A",     0,    0,    0},
	{ XK_Up,            XK_ANY_MOD,     "\033[A",        0,   -1,    0},
	{ XK_Up,            XK_ANY_MOD,     "\033OA",        0,   +1,    0},
	{ XK_Down,          ShiftMask,      "\033[1;2B",     0,    0,    0},
	{ XK_Down,          ControlMask,    "\033[1;5B",     0,    0,    0},
	{ XK_Down,          XK_ANY_MOD,     "\033[B",        0,   -1,    0},
	{ XK_Down,          XK_ANY_MOD,     "\033OB",        0,   +1,    0},
	{ XK_Left,          ShiftMask,      "\033[1;2D",     0,    0,    0},
	{ XK_Left,          ControlMask,    "\033[1;5D",     0,    0,    0},
	{ XK_Left,          XK_ANY_MOD,     "\033[D",        0,   -1,    0},
	{ XK_Left,          XK_ANY_MOD,     "\033OD",        0,   +1,    0},
	{ XK_Right,         ShiftMask,      "\033[1;2C",     0,    0,    0},
	{ XK_Right,         ControlMask,    "\033[1;5C",     0,    0,    0},
	{ XK_Right,         XK_ANY_MOD,     "\033[C",        0,   -1,    0},
	{ XK_Right,         XK_ANY_MOD,     "\033OC",        0,   +1,    0},
	{ XK_ISO_Left_Tab,  ShiftMask,      "\033[Z",        0,    0,    0},
	{ XK_Return,        XK_ANY_MOD,     "\r",            0,    0,   -1},
	{ XK_Return,        XK_ANY_MOD,     "\r\n",          0,    0,   +1},
	{ XK_Insert,        XK_ANY_MOD,     "\033[2~",       0,    0,    0},
	{ XK_Delete,        ControlMask,    "\033[3;5~",     0,    0,    0},
	{ XK_Delete,        ShiftMask,      "\033[3;2~",     0,    0,    0},
	{ XK_Delete,        XK_ANY_MOD,     "\033[3~",       0,    0,    0},
	{ XK_BackSpace,     XK_NO_MOD,      "\b",            0,    0,    0},
	{ XK_BackSpace,     ControlMask,    "\027",          0,    0,    0},
	{ XK_Home,          XK_ANY_MOD,     "\033[H",        0,   -1,    0},
	{ XK_End,           XK_ANY_MOD,     "\033[F",        0,    0,    0},
	{ XK_Prior,         ControlMask,    "\033[5;5~",     0,    0,    0},
	{ XK_Prior,         ShiftMask,      "\033[5;2~",     0,    0,    0},
	{ XK_Prior,         XK_ANY_MOD,     "\033[5~",       0,    0,    0},
	{ XK_Next,          ControlMask,    "\033[6;5~",     0,    0,    0},
	{ XK_Next,          ShiftMask,      "\033[6;2~",     0,    0,    0},
	{ XK_Next,          XK_ANY_MOD,     "\033[6~",       0,    0,    0},
};

/*
 * Selection types' masks.
 * Use the same masks as usual.
 * Button1Mask is always unset, to make masks match between ButtonPress.
 * ButtonRelease and MotionNotify.
 * If no match is found, regular selection is used.
 */
static uint selmasks[] = {
	[SEL_RECTANGULAR] = Mod1Mask,
};

/*
 * Printable characters in ASCII, used to estimate the advance width
 * of single wide characters.
 */
static char ascii_printable[] =
	" !\"#$%&'()*+,-./0123456789:;<=>?"
	"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
	"`abcdefghijklmnopqrstuvwxyz{|}~";

