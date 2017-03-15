/* See LICENSE file for copyright and license details. */

#include <X11/X.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <assert.h>
#include <errno.h>
#include <fontconfig/fontconfig.h>
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
#include "config.h"

// Macros
#define MIN(a, b)           ((a) > (b) ? (b) : (a))
#define MAX(a, b)           ((a) < (b) ? (b) : (a))
#define LEN(a)              (sizeof(a) / sizeof(*(a)))
#define BETWEEN(x, a, b)    ((a) <= (x) && (x) <= (b))
#define IS_CONTROL(c)       ((c) < 0x20 || (c) == 0x7f)
#define IS_CONTINUATION(c)  ((c) >> 6 == 2)
#define IS_DELIM(c)         (strchr(" <>'`\"(){}", (c)) != NULL)
#define LIMIT(x, a, b)      ((x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define SWAP(a, b)          do { __typeof(a) _swap = (a); (a) = (b); (b) = _swap; } while (0)
#define TLINE(y)            (term.hist[term.alt ? HIST_SIZE + ((y) + term.scroll) % 64 : ((y) + term.scroll) % HIST_SIZE])
#define die(...)            do { fprintf(stderr, __VA_ARGS__); exit(1); } while (0)

#define TIMEDIFF(t1, t2)    ((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_nsec - t2.tv_nsec) / 1000000)
#define AFTER(a, b)         ((a).y > (b).y || ((a).y == (b).y && (a).x > (b).x))

#define Glyph Glyph_

typedef enum { false, true } bool;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

enum { SNAP_NONE, SNAP_WORD, SNAP_LINE };

typedef enum {
	ATTR_BOLD       = 1 << 0,
	ATTR_FAINT      = 1 << 1,
	ATTR_ITALIC     = 1 << 2,
	ATTR_UNDERLINE  = 1 << 3,
	ATTR_BLINK      = 1 << 4,
	ATTR_BAR        = 1 << 5,
	ATTR_REVERSE    = 1 << 6,
	ATTR_INVISIBLE  = 1 << 7,
	ATTR_STRUCK     = 1 << 8,
} glyph_attribute;

typedef enum {
	MOUSE_NONE,
	MOUSE_BUTTON = 1000,
	MOUSE_MOTION = 1003,
} mouse_mode;

typedef struct {
	u8 u[4];   // raw UTF-8 bytes
	u16 mode;  // attribute bitmask
	u8 fg;     // foreground color
	u8 bg;     // background color
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

static struct {
	bool alt;
	int snap;
	Point ob; // original coordinates of the beginning of the selection
	Point oe; // original coordinates of the end of the selection
	Point nb; // normalized coordinates of the beginning of the selection
	Point ne; // normalized coordinates of the end of the selection
} sel;

static struct {
	char buf[BUFSIZ];
	char *c;
	char *last;
	int fd;
	int: 32;
} tty;

// Terminal state
static struct {
	int row;                               // row count
	int col;                               // column count
	Glyph hist[HIST_SIZE + 64][LINE_SIZE]; // history buffer
	TCursor c;                             // cursor
	TCursor saved_c[2];                    // saved cursors
	int scroll;                            // current scroll position
	int top;                               // top scroll limit
	int bot;                               // bottom scroll limit
	mouse_mode mouse;                      // terminal mode flags
	int lines;
	int cursor;                            // cursor style
	bool alt, hide, focus, charset, bracket_paste;
} term;

static char **opt_cmd = (char*[]) { SHELL, NULL };

// Drawing Context
static struct {
	Display *dpy;
	Window win;
	XftFont *font[4];
	Drawable buf;
	GC gc;
	XftDraw *draw;
	int w, h;     // window width and height
	int ch, cw;   // character width and height
	bool visible;
	bool focused;
} xw;

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

static Point ev2point(XButtonEvent *e)
{
	int x = (e->x - BORDERPX) / xw.cw;
	int y = (e->y - BORDERPX) / xw.ch;
	LIMIT(x, 0, term.col - 1);
	LIMIT(y, 0, term.row - 1);
	return (Point) { x, y + term.scroll };
}

static void selsnap(int *x, Glyph *line, int direction)
{
	if (sel.snap == SNAP_WORD) {
		while (BETWEEN(*x, 0, term.col - 1) && !IS_DELIM(line[*x].u[0]))
			*x += direction;
		*x -= direction;
	} else if (sel.snap >= SNAP_LINE) {
		*x = (direction < 0) ? 0 : term.col - 1;
	}
}

static void selnormalize(void)
{
	bool swapped = AFTER(sel.ob, sel.oe);
	sel.nb = swapped ? sel.oe : sel.ob;
	sel.ne = swapped ? sel.ob : sel.oe;
	selsnap(&sel.nb.x, TLINE(sel.nb.y - term.scroll), -1);
	selsnap(&sel.ne.x, TLINE(sel.ne.y - term.scroll), +1);
}

static bool selected(int x, int y)
{
	return BETWEEN(y, sel.nb.y, sel.ne.y)
		&& (y != sel.nb.y || x >= sel.nb.x)
		&& (y != sel.ne.y || x <= sel.ne.x);
}

static void load_fonts(char *fontstr)
{
	if (!FcInit())
		die("Could not init fontconfig.\n");

	FcPattern *pattern = FcNameParse((FcChar8 *) fontstr);
	FcResult result;

	for (int i = 0; i < 4; ++i) {
		FcPatternDel(pattern, FC_SLANT);
		FcPatternDel(pattern, FC_WEIGHT);
		FcPatternAddInteger(pattern, FC_SLANT, (i & 2) ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
		FcPatternAddInteger(pattern, FC_WEIGHT, (i & 1) ? FC_WEIGHT_BOLD : FC_WEIGHT_NORMAL);
		FcPattern *match = XftFontMatch(xw.dpy, DefaultScreen(xw.dpy), pattern, &result);
		xw.font[i] = XftFontOpenPattern(xw.dpy, match);
		FcPatternDestroy(match);
	}

	xw.cw = xw.font[0]->max_advance_width;
	xw.ch = xw.font[0]->height;
	FcPatternDestroy(pattern);
}

static void create_window(void)
{
	if (!(xw.dpy = XOpenDisplay(NULL)))
		die("Can't open display\n");

	// Set geometry to some arbitrary values while we wait for the resize event
	xw.w = term.col * xw.cw + 2 * BORDERPX;
	xw.h = term.row * xw.ch + 2 * BORDERPX;

	// Events
	Visual *visual = XDefaultVisual(xw.dpy, DefaultScreen(xw.dpy));
	XSetWindowAttributes attrs = {
		.background_pixel = colors->pixel,
		.event_mask = FocusChangeMask | VisibilityChangeMask | StructureNotifyMask
			| KeyPressMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
	};

	Window parent = XRootWindow(xw.dpy, DefaultScreen(xw.dpy));
	xw.win = XCreateWindow(xw.dpy, parent, 0, 0, xw.w, xw.h, 0,
			CopyFromParent, InputOutput, visual,
			CWBackPixel | CWEventMask, &attrs);

	// Graphic context
	XGCValues gcvalues = { .graphics_exposures = False };
	xw.gc = XCreateGC(xw.dpy, parent, GCGraphicsExposures, &gcvalues);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h, DefaultDepth(xw.dpy, DefaultScreen(xw.dpy)));
	xw.draw = XftDrawCreate(xw.dpy, xw.buf, visual, CopyFromParent);
	XDefineCursor(xw.dpy, xw.win, XCreateFontCursor(xw.dpy, XC_xterm));

	// Various
	XSetLocaleModifiers("");
	XMapWindow(xw.dpy, xw.win);
	XStoreName(xw.dpy, xw.win, "st");
}

static void draw_text(Glyph glyph, u8 *text, int len, int x, int y)
{
	bool bold = (glyph.mode & ATTR_BOLD) != 0;
	bool italic = (glyph.mode & (ATTR_ITALIC | ATTR_BLINK)) != 0;
	XftFont *font = xw.font[bold + 2 * italic];
	const XftColor *fg = &colors[glyph.fg && glyph.fg != glyph.bg ? glyph.fg : DEFAULTFG];
	const XftColor *bg = &colors[glyph.bg];
	int baseline = y + font->ascent;
	int width = xw.cw * len;

	if (glyph.mode & ATTR_REVERSE)
		SWAP(fg, bg);

	if (glyph.mode & ATTR_INVISIBLE)
		fg = bg;

	// Draw the background, then the text, then decorations
	XftDrawRect(xw.draw, bg, x, y, width, xw.ch);
	XftDrawStringUtf8(xw.draw, fg, font, x, baseline, text, len);

	if (glyph.mode & ATTR_UNDERLINE)
		XftDrawRect(xw.draw, fg, x, baseline + 1, xw.cw, 1);

	if (glyph.mode & ATTR_STRUCK)
		XftDrawRect(xw.draw, fg, x, (2 * baseline + y) / 3, width, 1);

	if (glyph.mode & ATTR_BAR)
		XftDrawRect(xw.draw, fg, x, y, 2, xw.ch);
}

// Draws the glyph at the given terminal coordinates.
static void draw_glyph(int x, int y)
{
	static u8 buf[4 * LINE_SIZE];
	static int len;
	static Glyph prev;
	static int draw_x, draw_y;

	Glyph glyph = TLINE(y)[x];

	// Handle selection and cursor
	if (sel.alt == term.alt && selected(x, y + term.scroll))
		glyph.mode ^= ATTR_REVERSE;

	int cursor = term.hide || term.scroll != term.lines ? 0 :
		xw.focused && term.cursor < 3 ? ATTR_REVERSE :
		term.cursor < 5 ? ATTR_UNDERLINE : ATTR_BAR;

	if (x == term.c.x && y == term.c.y)
		glyph.mode ^= cursor;

	bool diff = glyph.fg != prev.fg || glyph.bg != prev.bg || glyph.mode != prev.mode;

	if (x == 0 || diff) {
		draw_text(prev, buf, len, draw_x, draw_y);
		len = 0;
		draw_x = BORDERPX + x * xw.cw;
		draw_y = BORDERPX + y * xw.ch;
		prev = glyph;
	}

	buf[len++] = MAX(glyph.u[0], ' ');
	for (int i = 1; i < 4 && IS_CONTINUATION(glyph.u[i]); ++i)
		buf[len++] = glyph.u[i];
}

// Redraws all glyphs on our buffer, then flushes it to the window.
static void draw(void)
{
	XftDrawRect(xw.draw, colors, 0, 0, xw.w, xw.h);

	for (int y = 0; y < term.row; ++y)
		for (int x = 0; x < term.col; ++x)
			draw_glyph(x, y);
	draw_glyph(0, 0);

	XCopyArea(xw.dpy, xw.buf, xw.win, xw.gc, 0, 0, xw.w, xw.h, 0, 0);
}

static void ttywrite(const char *buf, size_t n)
{
	while (n > 0) {
		ssize_t result = write(tty.fd, buf, n);
		if (result < 0)
			die("write error on tty: %s\n", strerror(errno));
		n -= result;
		buf += result;
	}
}

static void tclearregion(int x1, int y1, int x2, int y2)
{
	LIMIT(x1, 0, term.col - 1);
	LIMIT(x2, 0, term.col - 1);
	LIMIT(y1, 0, term.row - 1);
	LIMIT(y2, 0, term.row - 1);
	assert(y1 < y2 || (y1 == y2 && x1 <= x2));

	if (sel.nb.y <= y2 && sel.ne.y >= y1)
		sel.ne.y = -1;

	for (int y = y1; y <= y2; y++) {
		int xstart = y == y1 ? x1 : 0;
		int xend = y == y2 ? x2 : term.col - 1;
		memset(TLINE(y) + xstart, 0, (xend - xstart + 1) * sizeof(Glyph));
	}
}

static void move_region(int start, int end, int diff) {
	int step = diff < 0 ? -1 : 1;
	if (diff < 0)
		SWAP(start, end);
	int last = end - diff + step;
	for (int y = start; y != last; y += step)
		memcpy(TLINE(y), TLINE(y + diff), sizeof(TLINE(y)));
	tclearregion(0, MIN(last, end), term.col - 1, MAX(last, end));
}

static void tscroll(int n)
{
	if (!term.alt) {
		term.scroll += n;
		LIMIT(term.scroll, MAX(0, term.lines - HIST_SIZE + term.row), term.lines);
	}
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

static void mousereport(XButtonEvent *e)
{
	static int ox, oy;

	Point point = ev2point(e);
	int x = point.x, y = point.y - term.scroll;
	int button = e->button;
	char buf[40];

	if (x > 222 || y > 222)
		return;

	if (e->type == MotionNotify) {
		if (term.mouse != MOUSE_MOTION || (x == ox && y == oy))
			return;
	} else {
		button = e->type == ButtonRelease ? 3 : button - Button1;
	}

	ox = x;
	oy = y;

	button += ((e->state & ShiftMask  ) ? 4  : 0)
		+ ((e->state & Mod4Mask   ) ? 8  : 0)
		+ ((e->state & ControlMask) ? 16 : 0);

	int len = snprintf(buf, sizeof(buf), "\033[M%c%c%c", 32 + button, 33 + x, 33 + y);
	ttywrite(buf, len);
}

static char* kmap(KeySym k, u32 state)
{
	for (Key *kp = key; kp < key + LEN(key); kp++)
		if (kp->k == k && !(kp->mask & ~state))
			return kp->s;
	return "";
}

static void tmoveto(int x, int y)
{
	term.c.x = LIMIT(x, 0, term.col - 1);
	term.c.y = LIMIT(y, 0, term.row - 1);
}

static void tswapscreen(void)
{
	static int scroll_save = 0;

	term.saved_c[term.alt] = term.c;
	term.alt = !term.alt;
	SWAP(scroll_save, term.lines);
	term.scroll = term.lines;
	term.c = term.saved_c[term.alt];
}

static void tsetscroll(int top, int bot)
{
	term.top = top;
	term.bot = bot;
}

static void tnewline(bool first_col)
{
	if (term.c.y == term.bot) {
		if (term.top) {
			move_region(term.top, term.bot, 1);
		} else {
			++term.lines;
			++term.scroll;
			tclearregion(0, term.bot, term.col - 1, term.row - 1);
		}
	} else {
		++term.c.y;
	}
	if (first_col)
		term.c.x = 0;
}

static void resize(int width, int height)
{
	int col = (width - 2 * BORDERPX) / xw.cw;
	int row = (height - 2 * BORDERPX) / xw.ch;

	// Update terminal info
	term.col = col;
	term.row = row;
	tsetscroll(0, row - 1);
	tmoveto(term.c.x, term.c.y);

	// Update X window data
	xw.w = width;
	xw.h = height;
	XFreePixmap(xw.dpy, xw.buf);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h, DefaultDepth(xw.dpy, DefaultScreen(xw.dpy)));
	XftDrawChange(xw.draw, xw.buf);
	XftDrawRect(xw.draw, colors, 0, 0, xw.w, xw.h);

	// Send our size to the tty driver so that applications can query it
	struct winsize w = { (u16) term.row, (u16) term.col, 0, 0 };
	if (ioctl(tty.fd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

static void kpress(XEvent *e)
{
	XKeyEvent *ev = &e->xkey;
	char buf[8];
	KeySym ksym;
	int len = XLookupString(ev, buf, LEN(buf) - 1, &ksym, NULL);

	// Clear the selection
	if (BETWEEN(term.c.y + term.scroll, sel.nb.y, sel.ne.y))
		sel.ne.y = -1;

	// Internal shortcuts
	if (ksym == XK_Insert && (ev->state & ShiftMask))
		xsel("xsel -po", false);
	else if (ksym == XK_Prior && (ev->state & ShiftMask))
		tscroll(4 - term.row);
	else if (ksym == XK_Next && (ev->state & ShiftMask))
		tscroll(term.row - 4);
	else if (ksym == XK_C && !((ControlMask | ShiftMask) & ~ev->state))
		xsel("xsel -bi", true);
	else if (ksym == XK_V && !((ControlMask | ShiftMask) & ~ev->state))
		xsel("xsel -bo", false);
	else if (len && *buf != '\b' && *buf != '\x7F') {
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

	if (term.mouse == MOUSE_MOTION && !(ev->state & ShiftMask)) {
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
			++sel.snap;
			selnormalize();
		} else {
			sel.ob = sel.oe = point;
			sel.ne.y = -1;
			sel.snap = SNAP_NONE;
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
		xsel("xsel -po", false);
	else if (ev->button == Button1 || ev->button == Button3)
		xsel("xsel -pi", true);
}

static void selclear(__attribute__((unused)) XEvent *e)
{
	sel.ne.y = -1;
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

static char ttyread(void)
{
	if (tty.c >= tty.last) {
		tty.c = tty.buf;
		tty.last = tty.buf + read(tty.fd, tty.buf, BUFSIZ);
		if (tty.last < tty.buf)
			exit(0);
	}

	return *tty.c++;
}

static void ttynew(void)
{
	int slave;
	struct winsize w = { (unsigned short) term.row, (unsigned short) term.col, 0, 0 };

	// seems to work fine on linux, openbsd and freebsd
	if (openpty(&tty.fd, &slave, NULL, NULL, &w) < 0)
		die("openpty failed: %s\n", strerror(errno));

	switch (fork()) {
	case -1:
		die("fork failed\n");
	case 0:
		setsid(); // create a new process group
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);
		if (ioctl(slave, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
		close(slave);
		close(tty.fd);
		execvp(opt_cmd[0], opt_cmd);
		die("exec failed: %s\n", strerror(errno));
	default:
		close(slave);
		signal(SIGCHLD, SIG_IGN);
	}
}

static int tsetattr(int *attr)
{
	switch (*attr) {
	case 0:
		memset(&term.c.attr, 0, sizeof(term.c.attr));
		return 1;
	case 1 ... 9:
		term.c.attr.mode |= 1 << (*attr - 1);
		return 1;
	case 21 ... 29:
		term.c.attr.mode &= ~(1 << (*attr - 21));
		return 1;
	case 30 ... 37:
		term.c.attr.fg = (u8) (*attr - 30);
		return 1;
	case 38:
		term.c.attr.fg = (u8) attr[2];
		return 3;
	case 39:
		term.c.attr.fg = 0;
		return 1;
	case 40 ... 47:
		term.c.attr.bg = (u8) (*attr - 40);
		return 1;
	case 48:
		term.c.attr.bg = (u8) attr[2];
		return 3;
	case 49:
		term.c.attr.bg = 0;
		return 1;
	case 90 ... 97:
		term.c.attr.fg = (u8) (*attr - 90 + 8);
		return 1;
	case 100 ... 107:
		term.c.attr.bg = (u8) (*attr - 100 + 8);
		return 1;
	default:
		return 1;
	}
}

static void tsetmode(bool set, int arg)
{
	switch (arg) {
	case 1: // DECCKM -- Cursor key (ignored)
	case 4: // IRM -- Insert Mode (TODO)
	case 7: // DECAWM -- Auto wrap (ignored)
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
	case MOUSE_BUTTON:
	case MOUSE_MOTION:
		term.mouse = set * arg;
		break;
	case 1004: // Report focus events
		term.focus = set;
		break;
	case 2004: // Bracketed paste mode
		term.bracket_paste = set;
		break;
	}
}

static void csihandle()
{
	int arg[16] = { 0 };
	u32 nargs = 1;
	char command = 0;

	csi:
	switch (command = ttyread()) {
	case '0' ... '9':
		arg[nargs - 1] = 10 * arg[nargs - 1] + command - '0';
		goto csi;
	case ';':
		++nargs;
		goto csi;
	case ' ':
	case '?':
	case '!':
		goto csi;
	case '\030':
		break;
	case 'A': // CUU -- Cursor <n> Up
		tmoveto(term.c.x, term.c.y - MAX(*arg, 1));
		break;
	case 'B': // CUD -- Cursor <n> Down
	case 'e': // VPR -- Cursor <n> Down
		tmoveto(term.c.x, term.c.y + MAX(*arg, 1));
		break;
	case 'C': // CUF -- Cursor <n> Forward
	case 'a': // HPR -- Cursor <n> Forward
		tmoveto(term.c.x + MAX(*arg, 1), term.c.y);
		break;
	case 'D': // CUB -- Cursor <n> Backward
		tmoveto(term.c.x - MAX(*arg, 1), term.c.y);
		break;
	case 'E': // CNL -- Cursor <n> Down and first col
		tmoveto(0, term.c.y + MAX(*arg, 1));
		break;
	case 'F': // CPL -- Cursor <n> Up and first col
		tmoveto(0, term.c.y + MAX(*arg, 1));
		break;
	case 'G': // CHA -- Move to <col>
	case '`': // HPA -- Move to <col>
		tmoveto(*arg - 1, term.c.y);
		break;
	case 'H': // CUP -- Move to <row> <col>
	case 'f': // HVP -- Move to <row> <col>
		tmoveto(arg[1] - 1, arg[0] - 1);
		break;
	case 'I': // CHT -- Cursor Forward Tabulation <n> tab stops
		tmoveto((term.c.x & ~7) + (MAX(*arg, 1) << 3), term.c.y);
		break;
	case 'J': // ED -- Clear screen
	case 'K': // EL -- Clear line
		tclearregion(
			*arg ? 0 : term.c.x,
			*arg && command == 'J' ? 0 : term.c.y,
			*arg == 1 ? term.c.x : term.col - 1,
			*arg == 1 || command == 'K' ? term.c.y : term.row - 1);
		break;
	case 'L': // IL -- Insert <n> blank lines
	case 'M': // DL -- Delete <n> lines
		if (!BETWEEN(term.c.y, term.top, term.bot))
			break;
		LIMIT(*arg, 1, term.bot);
		move_region(term.c.y, term.bot, command == 'L' ? -*arg : *arg);
		term.c.x = 0;
		break;
	case 'P': // DCH -- Delete <n> char
	case '@': // ICH -- Insert <n> blank char
		LIMIT(*arg, 1, term.col - term.c.x);
		int dst = term.c.x + (command == '@' ? *arg : 0);
		int src = term.c.x + (command == '@' ? 0 : *arg);
		int size = term.col - (term.c.x + *arg);
		int del = (command == '@' ? term.c.x : term.col - *arg);
		Glyph *line = TLINE(term.c.y);
		memmove(line + dst, line + src, size * sizeof(Glyph));
		tclearregion(del, term.c.y, del + *arg - 1, term.c.y);
		break;
	case 'S': // SU -- Scroll <n> line up
		move_region(term.top, term.bot, MAX(*arg, 1));
		break;
	case 'T': // SD -- Scroll <n> line down
		move_region(term.top, term.bot, -MAX(*arg, 1));
		break;
	case 'X': // ECH -- Erase <n> char
		LIMIT(*arg, 1, term.col - term.c.x);
		tclearregion(term.c.x, term.c.y, term.c.x + *arg - 1, term.c.y);
		break;
	case 'Z': // CBT -- Cursor Backward Tabulation <n> tab stops
		tmoveto((term.c.x & ~7) - (MAX(*arg, 1) << 3), term.c.y);
		break;
	case 'c': // DA -- Device Attributes
		if (*arg == 0)
			ttywrite(VTIDEN, sizeof(VTIDEN) - 1);
		break;
	case 'd': // VPA -- Move to <row>
		tmoveto(term.c.x, *arg - 1);
		break;
	case 'h': // SM -- Set terminal mode
	case 'l': // RM -- Reset Mode
		for (u32 i = 0; i < nargs; ++i)
			tsetmode(command == 'h', arg[i]);
		break;
	case 'm': // SGR -- Terminal attribute
		for (u32 i = 0; i < nargs; i += tsetattr(arg + i));
		break;
	case 'n': // DSR – Device Status Report (cursor position)
		if (*arg != 6)
			break;
		char buf[40];
		int len = snprintf(buf, sizeof(buf),"\033[%i;%iR", term.c.y + 1, term.c.x + 1);
		ttywrite(buf, len);
		break;
	case 'p': // DECSTR -- Soft terminal reset
		memset(&term.c, 0, sizeof(term.c));
		term.alt = term.hide = term.focus = term.charset = term.bracket_paste = false;
		break;
	case 'q': // DECSCUSR -- Set Cursor Style
		if (*arg <= 6)
			term.cursor = *arg;
		break;
	case 'r': // DECSTBM -- Set Scrolling Region
		arg[0] = arg[0] ? arg[0] : 1;
		arg[1] = arg[1] ? arg[1] : term.row;
		if (arg[0] >= arg[1] || arg[1] > term.row)
			break;
		tsetscroll(arg[0] - 1, arg[1] - 1);
		tmoveto(0, 0);
		break;
	case 's': // DECSC -- Save cursor position
		term.saved_c[term.alt] = term.c;
		break;
	case 'u': // DECRC -- Restore cursor position
		term.c = term.saved_c[term.alt];
		break;
	}
}

static void eschandle(u8 ascii)
{
	switch (ascii) {
	case '[':
		csihandle();
		break;
	case 'P': // DCS -- Device Control String
	case '_': // APC -- Application Program Command
	case '^': // PM -- Privacy Message
	case ']': // OSC -- Operating System Command
		while (!IS_CONTROL(ttyread()));
		break;
	case 'n': // LS2 -- Locking shift 2
	case 'o': // LS3 -- Locking shift 3
	case '\\': // ST -- String Terminator
		break;
	case '(': // GZD4 -- Set primary charset G0
	case ')': // G1D4 -- Set secondary charset G1
	case '*': // G2D4 -- Set tertiary charset G2
	case '+': // G3D4 -- Set quaternary charset G3
		ttyread();
		break;
	case '=': // DECKPAM -- Application keypad (ignored)
	case '>': // DECKPNM -- Normal keypad (ignored)
		break;
	case 'D': // IND -- Linefeed
	case 'E': // NEL -- Next line
		tnewline(ascii == 'E');
		break;
	case 'M': // RI -- Reverse index
		if (term.c.y <= term.top)
			move_region(term.top, term.bot, -1);
		else
			--term.c.y;
		break;
	case 'Z': // DECID -- Identify Terminal
		ttywrite(VTIDEN, sizeof(VTIDEN) - 1);
		break;
	case 'c': // RIS -- Reset to inital state
		memset(&term, 0, sizeof(term));
		resize(xw.w, xw.h);
		break;
	case '7': // DECSC -- Save Cursor
		term.saved_c[term.alt] = term.c;
		break;
	case '8': // DECRC -- Restore Cursor
		term.c = term.saved_c[term.alt];
		break;
	default:
		break;
	}
}

static void tcontrolcode(u8 ascii)
{
	switch (ascii) {
	case '\t':
		tmoveto((term.c.x & ~7) + 8, term.c.y);
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
	case '\033': // ESC
		eschandle(ttyread());
		break;
	case '\016': // LS1 -- Locking shift 1)
	case '\017': // LS0 -- Locking shift 0)
		term.charset = ascii == '\016';
		break;
	}
}

static void tputc(u8 u)
{
	if (IS_CONTROL(u)) {
		tcontrolcode(u);
		return;
	}

	static u8 *p;
	if (IS_CONTINUATION(u) && (int) p & 3) {
		*p++ = u;
		return;
	}

	Glyph *glyph = &TLINE(term.c.y)[term.c.x];
	*glyph = term.c.attr;
	p = glyph->u;
	*p++ = u;

	static const char* box_drawing = "┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│";
	if (term.charset && BETWEEN(u, 'k', 'y'))
		memcpy(glyph->u, box_drawing + (u - 'k') * 3, 3);

	if (term.c.x + 1 < term.col)
		tmoveto(term.c.x + 1, term.c.y);
	else
		tnewline(true);
}

static void __attribute__((noreturn)) run(void)
{
	fd_set read_fds;
	int xfd = XConnectionNumber(xw.dpy);
	int nfd = MAX(xfd, tty.fd) + 1;
	const struct timespec timeout = { 0, 1000000000 / FPS };
	bool dirty = true;
	struct timespec now, last = { 0, 0 };

	for (;;) {
		FD_ZERO(&read_fds);
		FD_SET(tty.fd, &read_fds);
		FD_SET(xfd, &read_fds);

		int result = pselect(nfd, &read_fds, 0, 0, &timeout, 0);
		if (result < 0)
			die("select failed: %s\n", strerror(errno));
		dirty |= result > 0;

		if (FD_ISSET(tty.fd, &read_fds)) {
			term.scroll = term.lines;
			tputc(ttyread());
			while (tty.c < tty.last)
				tputc(*tty.c++);
		}

		XEvent ev;
		while (XPending(xw.dpy)) {
			XNextEvent(xw.dpy, &ev);
			if (handler[ev.type])
				(handler[ev.type])(&ev);
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (dirty && xw.visible && TIMEDIFF(now, last) > 1000 / FPS) {
			draw();
			dirty = false;
			last = now;
		}
	}
}

int main(int argc, char *argv[])
{
	if (argc > 1)
		opt_cmd = argv + 1;

	term.row = 24;
	term.col = 80;
	setlocale(LC_CTYPE, "");
	create_window();
	load_fonts(FONTNAME);
	ttynew();
	run();
}
