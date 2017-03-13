/* See LICENSE file for copyright and license details. */

#include <X11/X.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrender.h>
#include <assert.h>
#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <libgen.h>
#include <limits.h>
#include <locale.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "colors.c"

// Config
#define FONT_SIZE 12
#define LINE_SIZE 256
#define HIST_SIZE 2048
#define FPS 60
#define DEFAULTFG 15
#define bellvolume 12
#define vtiden "\033[?6c"
#define borderpx 2
#define fontname "Hack:antialias=true:autohint=true"
#define shell "/bin/zsh"

// macros
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define LEN(a)			(sizeof(a) / sizeof(a)[0])
#define BETWEEN(x, a, b)	((a) <= (x) && (x) <= (b))
#define ISCONTROL(c)		((c) < 0x20 || (c) == 0x7f)
#define ISDELIM(u)		(strchr(" <>'`\"(){}", u) != NULL)
#define LIMIT(x, a, b)		((x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define TIMEDIFF(t1, t2)	((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_nsec - t2.tv_nsec) / 1000000)
#define SWAP(a, b)		do { __typeof(a) _swap = (a); (a) = (b); (b) = _swap; } while (0)
#define TLINE(y)		(term.hist[term.alt ? HIST_SIZE + ((y) + term.scroll) % 64 : ((y) + term.scroll) % HIST_SIZE])
#define AFTER(a, b)		((a).y > (b).y || ((a).y == (b).y && (a).x > (b).x))
#define die(...)		do { fprintf(stderr, __VA_ARGS__); exit(1); } while (0)
#define ERRESC(...)		die("erresc: " __VA_ARGS__)
#define ATTRCMP(a, b)		((a).mode != (b).mode || (a).fg != (b).fg || (a).bg != (b).bg)
#define UTF_LEN(u)		((u) < 192 ? 1 : (u) < 224 ? 2 : u < 240 ? 3 : 4)

typedef enum {
	ATTR_BOLD       = 1 << 0,
	ATTR_ITALIC     = 1 << 1,
	ATTR_FAINT      = 1 << 2,
	ATTR_UNDERLINE  = 1 << 3,
	ATTR_REVERSE    = 1 << 4,
	ATTR_INVISIBLE  = 1 << 5,
	ATTR_STRUCK     = 1 << 6,
	ATTR_WRAP       = 1 << 7,
	ATTR_BAR        = 1 << 8,
	ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
} glyph_attribute;

typedef enum {
	CURSOR_SAVE,
	CURSOR_LOAD
} cursor_movement;

typedef enum {
	MOUSE_NONE,
	MOUSE_X10 = 9,
	MOUSE_BTN = 1000,
	MOUSE_MOTION = 1002,
	MOUSE_MANY = 1003,
	MOUSE_SGR = 1006,
} mouse_mode;

typedef enum {
	ESC_NONE,
	ESC_START,
	ESC_CSI,
	ESC_STR,
	ESC_CHARSET,
} escape_state;

typedef enum {
	SNAP_WORD = 1,
	SNAP_LINE = 2
} selection_snap;

typedef enum { false, true } bool;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef uint32_t Rune;
#define Glyph Glyph_

typedef XftColor Color;

typedef struct {
	u8 u[4];   // bytes
	u16 mode;  // attribute flags
	u8 fg;     // foreground
	u8 bg;     // background
} Glyph;

typedef struct {
	Glyph attr; // current char attributes
	int x;
	int y;
} TCursor;

typedef struct {
	KeySym k;
	u64 mask;
	char *s;
} Key;

typedef struct {
	int x;
	int y;
} Point;

typedef struct {
	u32 mod;
	u32: 32;
	KeySym keysym;
	void (*func)();
} Shortcut;

// ESC '[' [[ [<priv>] <arg> [;]] <mode> [<mode>]]
static struct {
	int arg[16];
	u32 nargs;
} csi;

// Graphical info
static struct {
	Display *dpy;
	Window win;
	Drawable buf;
	Atom wmdeletewin;
	XftDraw *draw;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	int w, h;     // window width and height
	int ch, cw;   // character width and height
	bool visible;
	bool focused;
	int cursor;   // cursor style
} xw;

static struct {
	bool alt;
	int snap;
	Point ob; // original coordinates of the beginning of the selection
	Point oe; // original coordinates of the end of the selection
	Point nb; // normalized coordinates of the beginning of the selection
	Point ne; // normalized coordinates of the end of the selection
} sel;

// Function definitions used by X event handlers
static void clipcopy(void);
static void clippaste(void);
static void selpaste(void);
static void scrolldown(void);
static void scrollup(void);

// Internal keyboard shortcuts.
static Shortcut shortcuts[] = {
	// mask                   keysym         function
	{ ShiftMask,              XK_Insert,     selpaste   },
	{ ShiftMask,              XK_Page_Up,    scrollup   },
	{ ShiftMask,              XK_Page_Down,  scrolldown },
	{ ControlMask|ShiftMask,  XK_C,          clipcopy   },
	{ ControlMask|ShiftMask,  XK_V,          clippaste  },
};

// Escape sequences emitted when pressing special keys.
static Key key[] = {
	// keysym           mask          string
	{ XK_Up,            ControlMask,  "\033[1;5A" },
	{ XK_Up,            0,            "\033OA"    },
	{ XK_Down,          ControlMask,  "\033[1;5B" },
	{ XK_Down,          0,            "\033OB"    },
	{ XK_Right,         ControlMask,  "\033[1;5C" },
	{ XK_Right,         0,            "\033OC"    },
	{ XK_Left,          ControlMask,  "\033[1;5D" },
	{ XK_Left,          0,            "\033OD"    },
	{ XK_ISO_Left_Tab,  0,            "\033[Z"    },
	{ XK_BackSpace,     ControlMask,  "\027"      },
	{ XK_BackSpace,     0,            "\x7F"      },
	{ XK_Home,          0,            "\033[1~"   },
	{ XK_Insert,        0,            "\033[2~"   },
	{ XK_Delete,        ControlMask,  "\033[3;5~" },
	{ XK_Delete,        0,            "\033[3~"   },
	{ XK_End,           0,            "\033[4~"   },
	{ XK_Prior,         0,            "\033[5~"   },
	{ XK_Next,          0,            "\033[6~"   },
};

// Terminal state
static struct {
	int row;                               // row count
	int col;                               // column count
	Glyph hist[HIST_SIZE + 64][LINE_SIZE]; // history buffer
	TCursor c;                             // cursor
	int scroll;                            // current scroll position
	int top;                               // top scroll limit
	int bot;                               // bottom scroll limit
	mouse_mode mouse;                      // terminal mode flags
	escape_state esc;                      // escape state flags
	int lines;
	bool alt, hide, focus, charset, wrap, bracket_paste;
} term;

// Drawing Context
static struct {
	Color col[256];
	XftFont *font, *bfont, *ifont, *ibfont;
	GC gc;
} dc;

static int ttyfd;
static char **opt_cmd = (char*[]) { shell, NULL };
static void (*handler[LASTEvent])(XEvent *);

static Point ev2point(XButtonEvent *e)
{
	int x = (e->x - borderpx) / xw.cw;
	int y = (e->y - borderpx) / xw.ch;
	LIMIT(x, 0, term.col-1);
	LIMIT(y, 0, term.row-1);
	return (Point) { x, y + term.scroll };
}

static void selsnap(int *x, int y, int direction)
{
	if (sel.snap == SNAP_WORD) {
		while (BETWEEN(*x, 0, term.col - 1) && !ISDELIM(TLINE(y)[*x].u[0]))
			*x += direction;
		*x -= direction;
	} else if (sel.snap == SNAP_LINE) {
		*x = (direction < 0) ? 0 : term.col - 1;
	}
}

static void selnormalize(void)
{
	bool swapped = AFTER(sel.ob, sel.oe);
	sel.nb = swapped ? sel.oe : sel.ob;
	sel.ne = swapped ? sel.ob : sel.oe;
	selsnap(&sel.nb.x, sel.nb.y - term.scroll, -1);
	selsnap(&sel.ne.x, sel.ne.y - term.scroll, +1);
}

static bool selected(int x, int y)
{
	return BETWEEN(y, sel.nb.y, sel.ne.y)
		&& (y != sel.nb.y || x >= sel.nb.x)
		&& (y != sel.ne.y || x <= sel.ne.x);
}

static void xloadfont(XftFont **f, FcPattern *pattern)
{
	FcPattern *match;
	FcResult result;
	XGlyphInfo extents;

	match = XftFontMatch(xw.dpy, xw.scr, pattern, &result);
	if (!match)
		die("st: can't open font\n");

	if (!(*f = XftFontOpenPattern(xw.dpy, match))) {
		FcPatternDestroy(match);
		die("st: can't open font\n");
	}

	XftTextExtentsUtf8(xw.dpy, *f, (const FcChar8 *) "Q", 1, &extents);

	// Setting character width and height.
	xw.cw = extents.xOff;
	xw.ch = (*f)->ascent + (*f)->descent;
}

static void xloadfonts(char *fontstr)
{
	FcPattern *pattern;

	if (fontstr[0] == '-')
		pattern = XftXlfdParse(fontstr, False, False);
	else
		pattern = FcNameParse((FcChar8 *)fontstr);

	if (!pattern)
		die("st: can't open font %s\n", fontstr);

	FcPatternAddDouble(pattern, FC_SIZE, FONT_SIZE);
	xloadfont(&dc.font, pattern);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	xloadfont(&dc.ifont, pattern);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	xloadfont(&dc.ibfont, pattern);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	xloadfont(&dc.bfont, pattern);

	FcPatternDestroy(pattern);
}

static void xinit(void)
{
	if (!(xw.dpy = XOpenDisplay(NULL)))
		die("Can't open display\n");
	xw.scr = XDefaultScreen(xw.dpy);
	xw.vis = XDefaultVisual(xw.dpy, xw.scr);

	// Fonts
	if (!FcInit())
		die("Could not init fontconfig.\n");
	xloadfonts(fontname);

	// Colors
	Colormap cmap = XDefaultColormap(xw.dpy, xw.scr);
	for (u32 i = 0; i < LEN(dc.col); i++)
		if (!XftColorAllocName(xw.dpy, xw.vis, cmap, colors[i], &dc.col[i]))
			die("Could not allocate color %d\n", i);

	// Set geometry to some arbitrary values while we wait for the resize event
	xw.w = term.col * xw.cw + 2 * borderpx;
	xw.h = term.row * xw.ch + 2 * borderpx;

	// Events
	xw.attrs.background_pixel = dc.col[0].pixel;
	xw.attrs.event_mask = FocusChangeMask | KeyPressMask
		| VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
	xw.attrs.colormap = cmap;

	Window parent = XRootWindow(xw.dpy, xw.scr);
	xw.win = XCreateWindow(xw.dpy, parent, 0, 0, xw.w, xw.h, 0,
			XDefaultDepth(xw.dpy, xw.scr), InputOutput, xw.vis,
			CWBackPixel | CWBorderPixel | CWBitGravity | CWEventMask | CWColormap,
			&xw.attrs);

	// Graphic context
	XGCValues gcvalues = { .graphics_exposures = False };
	dc.gc = XCreateGC(xw.dpy, parent, GCGraphicsExposures, &gcvalues);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h, DefaultDepth(xw.dpy, xw.scr));
	xw.draw = XftDrawCreate(xw.dpy, xw.buf, xw.vis, xw.attrs.colormap);
	XDefineCursor(xw.dpy, xw.win, XCreateFontCursor(xw.dpy, XC_xterm));

	// Various
	XSetLocaleModifiers("");
	XMapWindow(xw.dpy, xw.win);
	XStoreName(xw.dpy, xw.win, "st");
}

static void xdrawglyphfontspec(Glyph glyph, u8 *buf, int len, int x, int winy)
{
	Color *fg, *bg;

	// Determine font for glyph
	XftFont *font = (XftFont*[]) { dc.font, dc.bfont, dc.ifont, dc.ibfont } [glyph.mode & (ATTR_BOLD | ATTR_ITALIC)];

	int y = winy + font->ascent;
	int width = xw.cw * len;
	// XRectangle r = { 0, 0, (u16) width, (u16) xw.ch };

	fg = &dc.col[glyph.fg ? glyph.fg : DEFAULTFG];
	bg = &dc.col[glyph.bg];

	if (glyph.mode & ATTR_REVERSE)
		SWAP(fg, bg);

	if (glyph.mode & ATTR_INVISIBLE)
		fg = bg;

	// Render the background
	XftDrawRect(xw.draw, bg, x, winy, width, xw.ch);

	// Set the clip region because Xft is sometimes dirty
	// XftDrawSetClipRectangles(xw.draw, x, winy, &r, 1);

	// Render the glyphs
	XftDrawStringUtf8(xw.draw, fg, font, x, y, buf, len);

	// Render bars
	if (glyph.mode & ATTR_UNDERLINE)
		XftDrawRect(xw.draw, fg, x, y + 1, xw.cw, 1);

	if (glyph.mode & ATTR_STRUCK)
		XftDrawRect(xw.draw, fg, x, (y + 2 * winy) / 3, xw.cw, 1);

	if (glyph.mode & ATTR_BAR)
		XftDrawRect(xw.draw, fg, x + 2, winy, 2, xw.ch);

	// Reset clip to none
	// XftDrawSetClip(xw.draw, 0);
}

static void xdrawglyph(int x, int y)
{
	static u8 buf[4 * LINE_SIZE];
	static int len;
	static Glyph prev;
	static int old_x, old_y;

	Glyph glyph = TLINE(y)[x];

	// Draw selection and cursor
	int cursor = term.hide || term.scroll != term.lines ? 0 :
		xw.focused && xw.cursor < 3 ? ATTR_REVERSE :
		xw.cursor < 5 ? ATTR_UNDERLINE : ATTR_BAR;

	if (sel.alt == term.alt && selected(x, y + term.scroll))
		glyph.mode ^= ATTR_REVERSE;
	if (x == term.c.x && y == term.c.y)
		glyph.mode ^= cursor;

	if (ATTRCMP(glyph, prev) || x == 0) {
		short xp = (short) (borderpx + old_x * xw.cw);
		short yp = (short) (borderpx + old_y * xw.ch);
		xdrawglyphfontspec(prev, buf, len, xp, yp);
		len = 0;
		old_x = x;
		old_y = y;
		prev = glyph;
	}

	for (int i = 0; i < UTF_LEN(*glyph.u); ++i)
		buf[len++] = MAX(glyph.u[i], ' ');
}

static void draw(void)
{
	if (!xw.visible)
		return;

	XftDrawRect(xw.draw, dc.col, 0, 0, xw.w, xw.h);

	for (int y = 0; y < term.row; ++y)
		for (int x = 0; x < term.col; ++x)
			xdrawglyph(x, y);

	XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, 0, 0, xw.w, xw.h, 0, 0);
	XSetForeground(xw.dpy, dc.gc, dc.col[0].pixel);
}

static void ttywrite(const char *buf, size_t n)
{
	while (n > 0) {
		ssize_t result = write(ttyfd, buf, n);
		if (result < 0)
			die("write error on tty: %s\n", strerror(errno));
		n -= result;
		buf += result;
	}
}

static void tclearregion(int x1, int y1, int x2, int y2)
{
	if (x1 > x2)
		SWAP(x1, x2);
	if (y1 > y2)
		SWAP(y1, y2);

	LIMIT(x1, 0, term.col - 1);
	LIMIT(x2, 0, term.col - 1);
	LIMIT(y1, 0, term.row - 1);
	LIMIT(y2, 0, term.row - 1);

	if (sel.nb.y <= y2 && sel.ne.y >= y1)
		sel.ne.y = -1;

	for (int y = y1; y <= y2; y++)
		memset(TLINE(y) + x1, 0, (x2 - x1) * sizeof(Glyph));
}

static void tscroll(int n)
{
	term.scroll += n;
	LIMIT(term.scroll, 0, term.lines);
	if (term.alt)
		tclearregion(0, term.c.y, term.col - 1, term.c.y);
}

// append every set & selected glyph to the selection
static void getsel(FILE* pipe)
{
	for (int y = sel.nb.y; y <= sel.ne.y; y++) {
		Glyph *line = term.hist[y % HIST_SIZE];
		int x1 = sel.nb.y == y ? sel.nb.x : 0;
		int x2 = sel.ne.y == y ? sel.ne.x : term.col - 1;
		int x;

		for (x = x1; x <= x2 && *line[x].u; ++x)
			fprintf(pipe, "%.4s", line[x].u);
		if (x <= x2)
			fprintf(pipe, "\n");
	}
}

static void xsel(char* opts, bool copy)
{
	if (copy && !AFTER(sel.ne, sel.nb))
		return;

	FILE* pipe = popen(opts, copy ? "w" : "r");
	char sel_buf[512];

	if (copy)
		getsel(pipe);
	else
		ttywrite(sel_buf, fread(sel_buf, 1, 512, pipe));
	fclose(pipe);
}

static void selcopy()   { xsel("xsel -pi", true); }
static void clipcopy()  { xsel("xsel -bi", true); }
static void selpaste()  { xsel("xsel -po", false);  }
static void clippaste() { xsel("xsel -bo", false); }

static void mousereport(XButtonEvent *e)
{
	static int oldbutton = 3; // on startup: 3 = release
	static int ox, oy;

	Point point = ev2point(e);
	int x = point.x, y = point.y - term.scroll;
	int button = e->button, state = e->state;
	int len;
	char buf[40];

	/* from urxvt */
	if (e->type == MotionNotify) {
		if (x == ox && y == oy)
			return;
		if (term.mouse != MOUSE_MOTION && term.mouse != MOUSE_MANY)
			return;
		/* MOUSE_MOTION: no reporting if no button is pressed */
		if (term.mouse == MOUSE_MOTION && oldbutton == 3)
			return;

		button = oldbutton + 32;
		ox = x;
		oy = y;
	} else {
		if (term.mouse != MOUSE_SGR && e->type == ButtonRelease) {
			button = 3;
		} else {
			button -= Button1;
			if (button >= 3)
				button += 64 - 3;
		}
		if (e->type == ButtonPress) {
			oldbutton = button;
			ox = x;
			oy = y;
		} else if (e->type == ButtonRelease) {
			oldbutton = 3;
			/* MOUSE_X10: no button release reporting */
			if (term.mouse == MOUSE_X10 || button == 64 || button == 65)
				return;
		}
	}

	if (term.mouse != MOUSE_X10) {
		button += ((state & ShiftMask  ) ? 4  : 0)
			+ ((state & Mod4Mask   ) ? 8  : 0)
			+ ((state & ControlMask) ? 16 : 0);
	}

	if (term.mouse == MOUSE_SGR) {
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
				button, x + 1, y + 1,
				e->type == ButtonRelease ? 'm' : 'M');
	} else if (x < 223 && y < 223) {
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
				32 + button, 32 + x + 1, 32 + y + 1);
	} else {
		return;
	}

	ttywrite(buf, len);
}

static char* kmap(KeySym k, u32 state)
{
	for (Key *kp = key; kp < key + LEN(key); kp++)
		if (kp->k == k && !(kp->mask & ~state))
			return kp->s;
	return "";
}


static void tdeletechar(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x;
	src = term.c.x + n;
	size = term.col - src;
	line = TLINE(term.c.y);

	memmove(line + dst, line + src, size * sizeof(Glyph));
	tclearregion(term.col - n, term.c.y, term.col - 1, term.c.y);
}

static void tinsertblank(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x + n;
	src = term.c.x;
	size = term.col - dst;
	line = TLINE(term.c.y);

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(src, term.c.y, dst - 1, term.c.y);
}

static void tputtab(int n)
{
	term.c.x &= ~7;
	term.c.x += n << 3;
}

static void tcursor(int mode)
{
	static TCursor c[2];

	if (mode == CURSOR_SAVE)
		c[term.alt] = term.c;
	else if (mode == CURSOR_LOAD)
		term.c = c[term.alt];
}

static void tmoveto(int x, int y)
{
	term.c.x = LIMIT(x, 0, term.col - 1);
	term.c.y = LIMIT(y, 0, term.row - 1);
}

static void tswapscreen(void)
{
	static int scroll_save = 0;

	tcursor(CURSOR_SAVE);
	term.alt = !term.alt;
	SWAP(scroll_save, term.lines);
	term.scroll = term.lines;
	tcursor(CURSOR_LOAD);
}

static void tsetscroll(int a, int b)
{
	LIMIT(a, 0, term.row - 1);
	LIMIT(b, 0, term.row - 1);
	term.top = MIN(a, b);
	term.bot = MAX(a, b);
}

static void tnewline(bool first_col)
{
	if (term.c.y == term.bot) {
		++term.lines;
		tscroll(1);
		tclearregion(0, term.c.y, term.col - 1, term.c.y);
	} else {
		++term.c.y;
	}
	if (first_col)
		term.c.x = 0;
}

static void treset(bool hard_reset)
{
	term.c = (TCursor) {{ .fg = 0, .bg = 0 }, .x = 0, .y = 0 };
	tsetscroll(0, term.row - 1);
	term.alt = term.hide = term.focus = term.charset = term.bracket_paste = false;
	term.wrap = true;

	for (int i = 0; i < (hard_reset ? 2 : 0); i++) {
		tmoveto(0, 0);
		tswapscreen();
		tclearregion(0, 0, term.col - 1, term.row - 1);
	}
}

static void resize(int width, int height)
{
	int col = (width - 2 * borderpx) / xw.cw;
	int row = (height - 2 * borderpx) / xw.ch;

	// Update terminal info
	term.col = col;
	term.row = row;
	tsetscroll(0, row - 1);
	tmoveto(term.c.x, term.c.y);

	// Update X window data
	xw.w = width;
	xw.h = height;
	XFreePixmap(xw.dpy, xw.buf);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h, DefaultDepth(xw.dpy, xw.scr));
	XftDrawChange(xw.draw, xw.buf);
	XftDrawRect(xw.draw, dc.col, 0, 0, xw.w, xw.h);

	// Send our size to the tty driver so that applications can query it
	struct winsize w = { (u16) term.row, (u16) term.col, 0, 0 };
	if (ioctl(ttyfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

static void kpress(XEvent *e)
{
	XKeyEvent *ev = &e->xkey;
	char buf[8];
	KeySym ksym;
	int len = XLookupString(ev, buf, LEN(buf) - 1, &ksym, NULL);

	for (Shortcut *bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if (bp->keysym == ksym && !(bp->mod & ~ev->state)) {
			bp->func();
			return;
		}
	}

	if (len && *buf != '\b' && *buf != '\x7F') {
		ttywrite(buf, len);
	} else {
		char *customkey = kmap(ksym, ev->state);
		ttywrite(customkey, strlen(customkey));
	}
}

static void configure_notify(XEvent *e)
{
	XConfigureEvent *ev = &e->xconfigure;

	if (ev->width != xw.w || ev->height != xw.h)
		resize(ev->width, ev->height);
}

static void visibility(XEvent *e)
{
	XVisibilityEvent *ev = &e->xvisibility;
	xw.visible = ev->state != VisibilityFullyObscured;
}

static void focus(XEvent *ev)
{
	XFocusChangeEvent *e = &ev->xfocus;

	if (e->mode == NotifyGrab)
		return;

	xw.focused = ev->type == FocusIn;

	if (term.focus)
		ttywrite(xw.focused ? "\033[I" : "\033[O", 3);
}

static void bmotion(XEvent *e)
{
	XButtonEvent *ev = &e->xbutton;

	if (term.mouse && !(ev->state & ShiftMask)) {
		mousereport(ev);
	} else if (ev->state & (Button1Mask | Button3Mask)) {
		sel.oe = ev2point(ev);
		selnormalize();
	}
}

static void bpress(XEvent *e)
{
	XButtonEvent *ev = &e->xbutton;
	Point point = ev2point(ev);

	if (term.mouse && !(ev->state & ShiftMask))
		mousereport(ev);

	switch (ev->button) {
	case Button1:
		sel.alt = term.alt;
		if (sel.ob.x == point.x && sel.ob.y == point.y) {
			sel.snap = MIN(sel.snap + 1, SNAP_LINE);
			selnormalize();
		} else {
			sel.ob = sel.oe = point;
			sel.ne.y = -1;
			sel.snap = 0;
		}
		break;
	case Button3:
		sel.snap = SNAP_LINE;
		sel.oe = ev2point(ev);
		selnormalize();
		break;
	case Button4:
		tscroll(-5);
		break;
	case Button5:
		tscroll(5);
		break;
	}
}

static void brelease(XEvent *e)
{
	XButtonEvent *ev = &e->xbutton;

	if (term.mouse && !(ev->state & ShiftMask))
		mousereport(ev);
	else if (ev->button == Button2)
		selpaste();
	else if (ev->button == Button1 || ev->button == Button3)
		selcopy();
}

static void selclear(__attribute__((unused)) XEvent *e)
{
	sel.ne.y = -1;
}

static void ttynew(void)
{
	int slave;
	struct winsize w = { (unsigned short) term.row, (unsigned short) term.col, 0, 0 };

	// seems to work fine on linux, openbsd and freebsd
	if (openpty(&ttyfd, &slave, NULL, NULL, &w) < 0)
		die("openpty failed: %s\n", strerror(errno));

	switch (fork()) {
	case -1:
		die("fork failed\n");
	case 0:
		close(1);
		setsid(); // create a new process group
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);
		if (ioctl(slave, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
		close(slave);
		close(ttyfd);
		execvp(opt_cmd[0], opt_cmd);
		die("exec failed: %s\n", strerror(errno));
	default:
		close(slave);
		signal(SIGCHLD, SIG_IGN);
	}
}

static void tsetattr(int attr)
{
	switch (attr) {
	case 0:
		memset(&term.c.attr, 0, sizeof(term.c.attr));
		break;
	case 1:
		term.c.attr.mode |= ATTR_BOLD;
		break;
	case 2:
		term.c.attr.mode |= ATTR_FAINT;
		break;
	case 3:
	case 5:
		term.c.attr.mode |= ATTR_ITALIC;
		break;
	case 4:
		term.c.attr.mode |= ATTR_UNDERLINE;
		break;
	case 7:
		term.c.attr.mode |= ATTR_REVERSE;
		break;
	case 8:
		term.c.attr.mode |= ATTR_INVISIBLE;
		break;
	case 9:
		term.c.attr.mode |= ATTR_STRUCK;
		break;
	case 22:
		term.c.attr.mode &= ~ATTR_BOLD_FAINT;
		break;
	case 23:
	case 25:
		term.c.attr.mode &= ~ATTR_ITALIC;
		break;
	case 24:
		term.c.attr.mode &= ~ATTR_UNDERLINE;
		break;
	case 27:
		term.c.attr.mode &= ~ATTR_REVERSE;
		break;
	case 28:
		term.c.attr.mode &= ~ATTR_INVISIBLE;
		break;
	case 29:
		term.c.attr.mode &= ~ATTR_STRUCK;
		break;
	case 30 ... 37:
		term.c.attr.fg = (u8) (attr - 30);
		break;
	case 39:
		term.c.attr.fg = 0;
		break;
	case 40 ... 47:
		term.c.attr.bg = (u8) (attr - 40);
		break;
	case 49:
		term.c.attr.bg = 0;
		break;
	case 90 ... 97:
		term.c.attr.fg = (u8) (attr - 90 + 8);
		break;
	case 100 ... 107:
		term.c.attr.bg = (u8) (attr - 100 + 8);
		break;
	default:
		ERRESC("attr %d unknown\n", attr);
	}
}

static void tsetmode(bool set, int arg)
{
	switch (arg) {
	case 1: // DECCKM -- Cursor key (ignored)
		break;
	case 7: // DECAWM -- Auto wrap
		term.wrap = set;
		break;
	case 12: // SRM -- Send/receive (TODO)
		break;
	case 25: // DECTCEM -- Text Cursor Enable Mode
		term.hide = !set;
		break;
	case 47:
	case 1047:
	case 1048:
	case 1049: // Swap screen & set/restore cursor
		if (set ^ term.alt)
			tswapscreen();
		if (set)
			tclearregion(0, 0, term.col - 1, term.row - 1);
		break;
	case MOUSE_X10:
	case MOUSE_BTN:
	case MOUSE_MOTION:
	case MOUSE_MANY:
	case MOUSE_SGR:
		xw.attrs.event_mask &= ~PointerMotionMask;
		xw.attrs.event_mask |= set * PointerMotionMask;
		XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
		term.mouse = set * arg;
		break;
	case 1004: // Report focus events
		term.focus = set;
		break;
	case 2004: // Bracketed paste mode
		term.bracket_paste = set;
		break;
	case 4: // IRM -- Insert Mode (TODO)
	default:
		if (set)
			ERRESC("unknown set/reset mode %d\n", arg);
	}
}

static void csihandle(char command, int *arg, u32 nargs)
{
	term.esc = ESC_NONE;

	// Argument default values
	if (arg[0] == 0)
		arg[0] = !strchr("JKcm", command);
	if (arg[1] == 0)
		arg[1] = 1;

	switch (command) {
	case '@': // ICH -- Insert <n> blank char
		tinsertblank(arg[0]);
		break;
	case 'A': // CUU -- Cursor <n> Up
		tmoveto(term.c.x, term.c.y - arg[0]);
		break;
	case 'B': // CUD -- Cursor <n> Down
	case 'e': // VPR -- Cursor <n> Down
		tmoveto(term.c.x, term.c.y + arg[0]);
		break;
	case 'C': // CUF -- Cursor <n> Forward
	case 'a': // HPR -- Cursor <n> Forward
		tmoveto(term.c.x + arg[0], term.c.y);
		break;
	case 'D': // CUB -- Cursor <n> Backward
		tmoveto(term.c.x - arg[0], term.c.y);
		break;
	case 'E': // CNL -- Cursor <n> Down and first col
		tmoveto(0, term.c.y + arg[0]);
		break;
	case 'F': // CPL -- Cursor <n> Up and first col
		tmoveto(0, term.c.y - arg[0]);
		break;
	case 'G': // CHA -- Move to <col>
	case '`': // HPA -- Move to <col>
		tmoveto(arg[0] - 1, term.c.y);
		break;
	case 'H': // CUP -- Move to <row> <col>
	case 'f': // HVP -- Move to <row> <col>
		tmoveto(arg[1] - 1, arg[0] - 1);
		break;
	case 'I': // CHT -- Cursor Forward Tabulation <n> tab stops
		tputtab(arg[0]);
		break;
	case 'J': // ED -- Clear screen
		switch (arg[0]) {
		case 0: // below
			tclearregion(term.c.x, term.c.y, term.col - 1, term.c.y);
			tclearregion(0, term.c.y + 1, term.col - 1, term.row - 1);
			break;
		case 1: // above
			tclearregion(0, 0, term.col - 1, term.c.y - 1);
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2: // all
			tclearregion(0, 0, term.col - 1, term.row - 1);
			break;
		default:
			goto unknown;
		}
		break;
	case 'K': // EL -- Clear line
		tclearregion(arg[0] ? 0 : term.c.x, term.c.y,
			arg[0] == 1 ? term.c.x : term.col - 1, term.c.y);
		break;
	case 'L': // IL -- Insert <n> blank lines
		if (!BETWEEN(term.c.y, term.top, term.bot))
			break;
		for (int y = term.bot - arg[0]; y >= term.c.y; --y)
			memmove(TLINE(y + arg[0]), TLINE(y), sizeof(*term.hist));
		tclearregion(0, term.c.y, term.col - 1, term.c.y + arg[0] - 1);
		break;
	case 'M': // DL -- Delete <n> lines
		if (!BETWEEN(term.c.y, term.top, term.bot))
			break;
		for (int y = term.c.y; y + arg[0] <= term.bot; ++y)
			memmove(TLINE(y), TLINE(y + arg[0]), sizeof(*term.hist));
		tclearregion(0, term.bot - arg[0] + 1, term.col - 1, term.bot);
		break;
	case 'P': // DCH -- Delete <n> char
		tdeletechar(arg[0]);
		break;
	case 'S': // SU -- Scroll <n> line up
		tscroll(-arg[0]);
		break;
	case 'T': // SD -- Scroll <n> line down
		tscroll(arg[0]);
		break;
	case 'X': // ECH -- Erase <n> char
		tclearregion(term.c.x, term.c.y, term.c.x + arg[0] - 1, term.c.y);
		break;
	case 'Z': // CBT -- Cursor Backward Tabulation <n> tab stops
		tputtab(-arg[0]);
		break;
	case 'c': // DA -- Device Attributes
		if (arg[0] == 0)
			ttywrite(vtiden, sizeof(vtiden) - 1);
		break;
	case 'd': // VPA -- Move to <row>
		tmoveto(term.c.x, arg[0] - 1);
		break;
	case 'h': // SM -- Set terminal mode
	case 'l': // RM -- Reset Mode
		for (u32 i = 0; i < nargs; ++i)
			tsetmode(command == 'h', arg[i]);
		break;
	case 'm': // SGR -- Terminal attribute
		if (arg[0] == 38 && arg[1] == 5)
			term.c.attr.fg = (u8) (arg[2]);
		else if (arg[0] == 48 && arg[1] == 5)
			term.c.attr.bg = (u8) (arg[2]);
		else
			for (u32 i = 0; i < nargs; i++)
				tsetattr(arg[i]);
		break;
	case 'n': // DSR – Device Status Report (cursor position)
		if (arg[0] != 6)
			goto unknown;
		char buf[40];
		int len = snprintf(buf, sizeof(buf),"\033[%i;%iR", term.c.y + 1, term.c.x + 1);
		ttywrite(buf, len);
		break;
	case 'p': // DECSTR -- Soft terminal reset
		treset(false);
		break;
	case 'q': // DECSCUSR -- Set Cursor Style
		if (!BETWEEN(arg[0], 0, 6))
			goto unknown;
		xw.cursor = arg[0];
		break;
	case 'r': // DECSTBM -- Set Scrolling Region
		tsetscroll(arg[0] - 1, arg[1] - 1);
		tmoveto(0, 0);
		break;
	case 's': // DECSC -- Save cursor position
	case 'u': // DECRC -- Restore cursor position
		tcursor(command == 'u' ? CURSOR_LOAD : CURSOR_SAVE);
		break;
	default:
	unknown:
		ERRESC("unknown CSI %c\n", command);
	}
}

static escape_state eschandle(u8 ascii)
{
	switch (ascii) {
	case '[':
		memset(&csi, 0, sizeof(csi));
		return ESC_CSI;
	case 'P': // DCS -- Device Control String
	case '_': // APC -- Application Program Command
	case '^': // PM -- Privacy Message
	case ']': // OSC -- Operating System Command
		return ESC_STR;
	case 'n': // LS2 -- Locking shift 2
	case 'o': // LS3 -- Locking shift 3
	case '\\': // ST -- String Terminator
		return ESC_NONE;
	case '(': // GZD4 -- Set primary charset G0
	case ')': // G1D4 -- Set secondary charset G1
	case '*': // G2D4 -- Set tertiary charset G2
	case '+': // G3D4 -- Set quaternary charset G3
		return ESC_CHARSET;
	case '=': // DECKPAM -- Application keypad (ignored)
	case '>': // DECKPNM -- Normal keypad (ignored)
		return ESC_NONE;
	case 'D': // IND -- Linefeed
	case 'E': // NEL -- Next line
		tnewline(ascii == 'E');
		return ESC_NONE;
	case 'M': // RI -- Reverse index
		if (term.c.y == term.top)
			tscroll(-1);
		else
			--term.c.y;
		return ESC_NONE;
	case 'Z': // DECID -- Identify Terminal
		ttywrite(vtiden, sizeof(vtiden) - 1);
		return ESC_NONE;
	case 'c': // RIS -- Reset to inital state
		treset(true);
		return ESC_NONE;
	case '7': // DECSC -- Save Cursor
	case '8': // DECRC -- Restore Cursor
		tcursor(ascii == '8' ? CURSOR_LOAD : CURSOR_SAVE);
		return ESC_NONE;
	default:
		return ESC_NONE;
	}
}

static void tcontrolcode(u8 ascii)
{
	switch (ascii) {
	case '\t':
		tputtab(1);
		break;
	case '\b':
		tmoveto(term.c.x - 1, term.c.y);
		break;
	case '\r':
		tmoveto(0, term.c.y);
		break;
	case '\f':
	case '\v':
	case '\n':
		tnewline(false);
		break;
	case '\a':
		if (!xw.focused)
			XkbBell(xw.dpy, xw.win, bellvolume, (Atom) NULL);
		break;
	case '\033': // ESC
		term.esc = ESC_START;
		break;
	case '\016': // LS1 -- Locking shift 1)
	case '\017': // LS0 -- Locking shift 0)
		term.charset = ascii == '\016';
		break;
	case '\030': // CAN
		term.esc = ESC_NONE;
		break;
	}
}

static void tputc(u8 u)
{
	// Actions of control codes must be performed as soon they arrive
	// because they can be embedded inside a control sequence, and
	// they must not cause conflicts with sequences.
	if (ISCONTROL(u)) {
		if (term.esc == ESC_STR)
			term.esc = ESC_NONE;
		else
			tcontrolcode((char) u);
		return;
	}

	switch (term.esc) {
	case ESC_START:
		term.esc = eschandle((char) u);
		return;
	case ESC_CSI:
		if (BETWEEN(u, '0', '9'))
			csi.arg[csi.nargs] = 10 * csi.arg[csi.nargs] + u - '0';
		else if (BETWEEN(u, '@', '~'))
			csihandle((char) u, csi.arg, ++csi.nargs);
		else if (u == ';' && csi.nargs < LEN(csi.arg) - 1)
			++csi.nargs;
		return;
	case ESC_STR:
		return;
	case ESC_CHARSET:
		term.esc = ESC_NONE;
		return;
	}

	if (BETWEEN(term.c.y, sel.ob.y, sel.oe.y))
		sel.ne.y = -1;

	if (term.c.x >= term.col)
		tnewline(true);

	// TODO graphic chars
	// static int vt100_0[] = {
		// 0x256c, 0x2592, 0, 0, 0, 0, 0xb0, 0xb1,            // ` - g
		// 0, 0, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0,   // h - o
		// 0, 0x2500, 0, 0, 0x251c, 0x2524, 0x2534, 0x252c,   // p - w
		// 0x2502, 0x2264, 0x2265, 0x3c0, 0x2260, 0xa3, 0xb7, // x - ~
	// };

	// if (term.charset && BETWEEN(glyph->u[-1], '`', '~'))
		// glyph->u = vt100_0[glyph->u[0] - '`'];


	static int i;
	if (i == 0)
		TLINE(term.c.y)[term.c.x] = term.c.attr;
	TLINE(term.c.y)[term.c.x].u[i++] = u;
	u8 tmp = TLINE(term.c.y)[term.c.x].u[0];
	if (i < UTF_LEN(tmp))
		return;
	i = 0;

	if (term.c.x + 1 < term.col) {
		tmoveto(term.c.x + 1, term.c.y);
	} else {
		TLINE(term.c.y)[term.c.x].mode |= ATTR_WRAP;
		tnewline(true);
	}
}

static size_t ttyread(void)
{
	static char buf[BUFSIZ];

	ssize_t buf_len = read(ttyfd, buf, BUFSIZ);
	if (buf_len < 0)
		exit(0);

	// Reset scroll
	if (!term.alt)
		term.scroll = term.lines;

	for (char *ptr = buf; ptr < buf + buf_len; ++ptr)
		tputc(*ptr);

	return buf_len;
}

static void scrolldown(void) { tscroll(term.row - 2); }
static void scrollup(void) { tscroll(2 - term.row); }

static void __attribute__((noreturn)) run(void)
{
	fd_set read_fds;
	int xfd = XConnectionNumber(xw.dpy);
	int nfd = MAX(xfd, ttyfd) + 1;
	const struct timespec timeout = { 0, 1000000000 / FPS };
	struct timespec now, last = { 0, 0 };
	bool dirty = true;

	for (;;) {
		FD_ZERO(&read_fds);
		FD_SET(ttyfd, &read_fds);
		FD_SET(xfd, &read_fds);

		int result = pselect(nfd, &read_fds, 0, 0, &timeout, 0);

		if (result < 0)
			die("select failed: %s\n", strerror(errno));

		dirty |= result > 0;

		if (FD_ISSET(ttyfd, &read_fds))
			ttyread();

		XEvent ev;
		while (XPending(xw.dpy)) {
			XNextEvent(xw.dpy, &ev);
			if (handler[ev.type])
				(handler[ev.type])(&ev);
		}

		clock_gettime(CLOCK_MONOTONIC, &now);

		if (dirty && TIMEDIFF(now, last) > 1000 / FPS) {
			draw();
			XFlush(xw.dpy);
			dirty = false;
			last = now;
		}
	}
}

static void (*handler[LASTEvent])(XEvent *) = {
	[KeyPress] = kpress,
	[ConfigureNotify] = configure_notify,
	[VisibilityNotify] = visibility,
	[FocusIn] = focus,
	[FocusOut] = focus,
	[MotionNotify] = bmotion,
	[ButtonPress] = bpress,
	[ButtonRelease] = brelease,
	[SelectionClear] = selclear,
};

int main(int argc, char *argv[])
{
	if (argc > 1)
		opt_cmd = argv + 1;

	term.row = 24;
	term.col = 80;
	setlocale(LC_CTYPE, "");
	treset(true);
	xinit();
	ttynew();
	run();
}
