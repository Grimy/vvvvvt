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
#define LEN(a)              (sizeof(a) / sizeof(*(a)))
#define MIN(a, b)           ((a) > (b) ? (b) : (a))
#define MAX(a, b)           ((a) < (b) ? (b) : (a))
#define BETWEEN(x, a, b)    ((a) <= (x) && (x) <= (b))
#define IS_CONTROL(c)       ((c) < 0x20 || (c) == 0x7f)
#define UTF_LEN(c)          ((c) < 0x80 ? 1 : (c) < 0xE0 ? 2 : (c) < 0xF0 ? 3 : 4)
#define LIMIT(x, a, b)      ((x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define SWAP(a, b)          do { __typeof(a) _swap = (a); (a) = (b); (b) = _swap; } while (0)
#define SLINE(y)            (term.hist[term.alt ? HIST_SIZE + (y) % 64 : (y) % HIST_SIZE])
#define TLINE(y)            (SLINE((y) + term.scroll))
#define die(...)            do { fprintf(stderr, __VA_ARGS__); exit(1); } while (0)
#define pty_printf(...)     dprintf(pty.fd, __VA_ARGS__)

#define IS_DELIM(c)         (strchr(" <>'`\"(){}", *(c)) != NULL)
#define TIMEDIFF(t1, t2)    ((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_nsec - t2.tv_nsec) / 1000000)
#define AFTER(a, b)         ((a).y > (b).y || ((a).y == (b).y && (a).x > (b).x))

#define Glyph Glyph_

typedef enum { false, true } bool;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

enum { SNAP_NONE, SNAP_WORD, SNAP_LINE };

enum {
	ATTR_BOLD       = 1 << 0,
	ATTR_FAINT      = 1 << 1,
	ATTR_ITALIC     = 1 << 2,
	ATTR_UNDERLINE  = 1 << 3,
	ATTR_BLINK      = 1 << 4,
	ATTR_BAR        = 1 << 5,
	ATTR_REVERSE    = 1 << 6,
	ATTR_INVISIBLE  = 1 << 7,
	ATTR_STRUCK     = 1 << 8,
};

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
} pty;

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
	int lines;
	int cursor;                            // cursor style
	int charset;
	bool report_buttons, report_motion;
	bool alt, hide, focus, line_drawing;   // terminal mode flags
} term;

static char **opt_cmd = (char*[]) { SHELL, NULL };

#define selclear() (sel.ne.y = -1)

static void clear_region(int x1, int y1, int x2, int y2)
{
	LIMIT(x1, 0, term.col - 1);
	LIMIT(x2, 0, term.col - 1);
	LIMIT(y1, 0, term.row - 1);
	LIMIT(y2, 0, term.row - 1);
	assert(y1 < y2 || (y1 == y2 && x1 <= x2));

	if (sel.nb.y <= y2 && sel.ne.y >= y1)
		selclear();

	for (int y = y1; y <= y2; y++) {
		int xstart = y == y1 ? x1 : 0;
		int xend = y == y2 ? x2 : term.col - 1;
		memset(TLINE(y) + xstart, 0, (xend - xstart + 1) * sizeof(Glyph));
	}
}

static void move_region(int start, int diff) {
	int end = term.bot;
	int step = diff < 0 ? -1 : 1;
	if (diff < 0)
		SWAP(start, end);
	int last = end - diff + step;
	for (int y = start; y != last; y += step)
		memcpy(TLINE(y), TLINE(y + diff), sizeof(TLINE(y)));
	clear_region(0, MIN(last, end), term.col - 1, MAX(last, end));
}

static void move_chars(bool forward, int diff) {
	int dst = term.c.x + (forward ? diff : 0);
	int src = term.c.x + (forward ? 0 : diff);
	int size = term.col - (term.c.x + diff);
	int del = (forward ? term.c.x : term.col - diff);
	Glyph *line = TLINE(term.c.y);
	memmove(line + dst, line + src, size * sizeof(Glyph));
	clear_region(del, term.c.y, del + diff - 1, term.c.y);
}

static void move_to(int x, int y)
{
	term.c.x = LIMIT(x, 0, term.col - 1);
	term.c.y = LIMIT(y, 0, term.row - 1);
}

static void swap_screen(void)
{
	static int scroll_save = 0;

	selclear();
	term.saved_c[term.alt] = term.c;
	term.alt = !term.alt;
	SWAP(scroll_save, term.lines);
	term.scroll = term.lines;
	term.c = term.saved_c[term.alt];
}

static void newline()
{
	if (term.c.y != term.bot) {
		++term.c.y;
	} else if (term.top) {
		move_region(term.top, 1);
	} else {
		++term.lines;
		++term.scroll;
		clear_region(0, term.bot, term.col - 1, term.row - 1);
	}
}

// Drawing Context
static struct {
	Display *dpy;
	Window win;
	XftFont *font[4];
	Drawable pixmap;
	GC gc;
	XftDraw *draw;
	int width, height;
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
	{ XK_BackSpace,     0,            "\177"      },
	{ XK_Home,          0,            "\033[1~"   },
	{ XK_Insert,        0,            "\033[2~"   },
	{ XK_Delete,        ControlMask,  "\033[3;5~" },
	{ XK_Delete,        0,            "\033[3~"   },
	{ XK_End,           0,            "\033[4~"   },
	{ XK_Prior,         0,            "\033[5~"   },
	{ XK_Next,          0,            "\033[6~"   },
};

static void scroll(int n)
{
	if (!term.alt) {
		term.scroll += n;
		LIMIT(term.scroll, MAX(0, term.lines - HIST_SIZE + term.row), term.lines);
	}
}

static Point ev2point(XButtonEvent *e)
{
	int x = (e->x - BORDERPX) / xw.cw;
	int y = (e->y - BORDERPX) / xw.ch;
	LIMIT(x, 0, term.col - 1);
	LIMIT(y, 0, term.row - 1);
	return (Point) { x, y + term.scroll };
}

static void selnormalize(void)
{
	bool swapped = AFTER(sel.ob, sel.oe);
	sel.nb = swapped ? sel.oe : sel.ob;
	sel.ne = swapped ? sel.ob : sel.oe;
	if (sel.snap >= SNAP_LINE) {
		sel.nb.x = 0;
		sel.ne.x = term.col - 1;
	} else if (sel.snap == SNAP_WORD) {
		while (sel.nb.x > 0 && !IS_DELIM(SLINE(sel.nb.y)[sel.nb.x - 1].u))
			--sel.nb.x;
		while (!IS_DELIM(SLINE(sel.ne.y)[sel.ne.x + 1].u))
			++sel.ne.x;
	}
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
	xw.width = term.col * xw.cw + 2 * BORDERPX;
	xw.height = term.row * xw.ch + 2 * BORDERPX;

	// Events
	Visual *visual = DefaultVisual(xw.dpy, DefaultScreen(xw.dpy));
	XSetWindowAttributes attrs = {
		.event_mask = FocusChangeMask | VisibilityChangeMask | StructureNotifyMask
			| KeyPressMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
	};

	Window parent = XRootWindow(xw.dpy, DefaultScreen(xw.dpy));
	xw.win = XCreateWindow(xw.dpy, parent, 0, 0, xw.width, xw.height, 0,
			CopyFromParent, InputOutput, visual, CWEventMask, &attrs);

	// Graphic context
	XGCValues gcvalues = { .graphics_exposures = False };
	xw.gc = XCreateGC(xw.dpy, parent, GCGraphicsExposures, &gcvalues);
	xw.pixmap = XCreatePixmap(xw.dpy, xw.win, xw.width, xw.height, 24);
	xw.draw = XftDrawCreate(xw.dpy, xw.pixmap, visual, CopyFromParent);
	XDefineCursor(xw.dpy, xw.win, XCreateFontCursor(xw.dpy, XC_xterm));

	// Various
	XMapWindow(xw.dpy, xw.win);
	XStoreName(xw.dpy, xw.win, "st");
}

static void draw_text(Glyph glyph, u8 *text, int len, int x, int y)
{
	bool bold = (glyph.mode & ATTR_BOLD) != 0;
	bool italic = (glyph.mode & (ATTR_ITALIC | ATTR_BLINK)) != 0;
	XftFont *font = xw.font[bold + 2 * italic];
	const XftColor *fg = &colors[glyph.fg ? glyph.fg : DEFAULTFG];
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
	if (selected(x, y + term.scroll))
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

	int utf_len = UTF_LEN(*glyph.u);
	if (glyph.u[utf_len - 1]) {
		memcpy(buf + len, glyph.u, utf_len);
		len += utf_len;
	} else if (utf_len == 1) {
		buf[len++] = ' ';
	} else {
		memcpy(buf + len, "⁇", 3);
		len += 3;
	}
}

// Redraws all glyphs on our buffer, then flushes it to the window.
static void draw(void)
{
	XftDrawRect(xw.draw, colors, 0, 0, xw.width, xw.height);

	for (int y = 0; y < term.row; ++y)
		for (int x = 0; x < term.col; ++x)
			draw_glyph(x, y);
	draw_glyph(0, 0);

	XCopyArea(xw.dpy, xw.pixmap, xw.win, xw.gc, 0, 0, xw.width, xw.height, 0, 0);
}

// append every set & selected glyph to the selection
static void getsel(FILE* pipe)
{
	for (int y = sel.nb.y; y <= sel.ne.y; y++) {
		Glyph *line = SLINE(y);
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

	if (copy) {
		getsel(pipe);
	} else {
		char sel_buf[BUFSIZ] = { 0 };
		fread(sel_buf, 1, BUFSIZ, pipe);
		pty_printf("%s", sel_buf);
	}

	fclose(pipe);
}

static void mousereport(XButtonEvent *e)
{
	static int ox, oy;

	Point point = ev2point(e);
	int x = point.x, y = point.y - term.scroll;
	int button = e->type == ButtonRelease ? 3 : e->button - Button1;

	if (x > 222 || y > 222)
		return;

	if (e->type == MotionNotify && x == ox && y == oy)
		return;

	ox = x;
	oy = y;

	button += ((e->state & ShiftMask  ) ? 4  : 0)
		+ ((e->state & Mod4Mask   ) ? 8  : 0)
		+ ((e->state & ControlMask) ? 16 : 0);

	pty_printf("\033[M%c%c%c", 32 + button, 33 + x, 33 + y);
}

static char* kmap(KeySym k, u32 state)
{
	for (Key *kp = key; kp < key + LEN(key); kp++)
		if (kp->k == k && !(kp->mask & ~state))
			return kp->s;
	return "";
}

static void resize(int width, int height)
{
	int col = (width - 2 * BORDERPX) / xw.cw;
	int row = (height - 2 * BORDERPX) / xw.ch;

	// Update terminal info
	term.col = col;
	term.row = row;
	term.top = 0;
	term.bot = row - 1;
	move_to(term.c.x, term.c.y);

	// Update X window data
	xw.width = width;
	xw.height = height;
	XFreePixmap(xw.dpy, xw.pixmap);
	xw.pixmap = XCreatePixmap(xw.dpy, xw.win, width, height, 24);
	XftDrawChange(xw.draw, xw.pixmap);
	XftDrawRect(xw.draw, colors, 0, 0, width, height);

	// Send our size to the pty driver so that applications can query it
	struct winsize w = { (u16) term.row, (u16) term.col, 0, 0 };
	if (ioctl(pty.fd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

static void kpress(XEvent *e)
{
	XKeyEvent *ev = &e->xkey;
	char buf[8] = { 0 };
	KeySym ksym;
	int len = XLookupString(ev, buf, LEN(buf) - 1, &ksym, NULL);

	if (BETWEEN(term.c.y + term.scroll, sel.nb.y, sel.ne.y))
		selclear();

	// Internal shortcuts
	if (ksym == XK_Insert && (ev->state & ShiftMask))
		xsel("xsel -po", false);
	else if (ksym == XK_Prior && (ev->state & ShiftMask))
		scroll(4 - term.row);
	else if (ksym == XK_Next && (ev->state & ShiftMask))
		scroll(term.row - 4);
	else if (ksym == XK_C && !((ControlMask | ShiftMask) & ~ev->state))
		xsel("xsel -bi", true);
	else if (ksym == XK_V && !((ControlMask | ShiftMask) & ~ev->state))
		xsel("xsel -bo", false);
	else if (len && *buf != '\b' && *buf != '\177')
		pty_printf("%s", buf);
	else
		pty_printf("%s", kmap(ksym, ev->state));
}

static void configure_notify(XEvent *e)
{
	XConfigureEvent *ev = &e->xconfigure;

	if (ev->width != xw.width || ev->height != xw.height)
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
		pty_printf("\033[%c", xw.focused ? 'I' : 'O');
}

static void bmotion(XEvent *e)
{
	XButtonEvent *ev = &e->xbutton;

	if (term.report_motion && !(ev->state & ShiftMask)) {
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

	if (term.report_buttons && !(ev->state & ShiftMask))
		mousereport(ev);

	switch (ev->button) {
	case Button1:
		if (sel.ob.x == point.x && sel.ob.y == point.y) {
			++sel.snap;
			selnormalize();
		} else {
			selclear();
			sel.ob = sel.oe = point;
			sel.snap = SNAP_NONE;
		}
		break;
	case Button3:
		sel.snap = SNAP_LINE;
		sel.oe = ev2point(ev);
		selnormalize();
		break;
	case Button4:
		scroll(-5);
		break;
	case Button5:
		scroll(5);
		break;
	}
}

static void brelease(XEvent *e)
{
	XButtonEvent *ev = &e->xbutton;

	if (term.report_buttons && !(ev->state & ShiftMask))
		mousereport(ev);
	else if (ev->button == Button2)
		xsel("xsel -po", false);
	else if (ev->button == Button1 || ev->button == Button3)
		xsel("xsel -pi", true);
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
};

static char pty_getchar(void)
{
	if (pty.c >= pty.last) {
		pty.c = pty.buf;
		pty.last = pty.buf + read(pty.fd, pty.buf, BUFSIZ);
		if (pty.last < pty.buf)
			exit(0);
	}

	return *pty.c++;
}

static void pty_new(void)
{
	switch (forkpty(&pty.fd, NULL, NULL, &(struct winsize) { 24, 80, 0, 0 })) {
	case -1:
		die("forkpty failed\n");
	case 0:
		execvp(opt_cmd[0], opt_cmd);
		die("exec failed: %s\n", strerror(errno));
	default:
		signal(SIGCHLD, SIG_IGN);
	}
}

static int set_attr(int *attr)
{
	u8 *color = &term.c.attr.fg;
	if (BETWEEN(*attr, 40, 49) || BETWEEN(*attr, 100, 107)) {
		color = &term.c.attr.bg;
		*attr -= 10;
	}

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
		*color = (u8) (*attr - 30);
		return 1;
	case 38:
		*color = (u8) attr[2];
		return 3;
	case 39:
		*color = 0;
		return 1;
	case 90 ... 97:
		*color = (u8) (*attr - 90 + 8);
		return 1;
	default:
		return 1;
	}
}

static void set_mode(bool set, int mode)
{
	switch (mode) {
	case 25:   // Show cursor
		term.hide = !set;
		break;
	case 47:
	case 1047:
	case 1048:
	case 1049: // Swap screen & set/restore cursor
		if (set ^ term.alt)
			swap_screen();
		if (set)
			clear_region(0, 0, term.col - 1, term.row - 1);
		break;
	case 1000: // Report mouse buttons
	case 1003: // Report mouse motion
		term.report_buttons = set;
		term.report_motion = mode == 1003 && set;
		break;
	case 1004: // Report focus events
		term.focus = set;
		break;
	}
}

static void handle_csi()
{
	int arg[16] = { 0 };
	u32 nargs = 0;
	char command = 0;

	csi:
	switch (command = pty_getchar()) {
	case '0' ... '9':
		if (arg[nargs] < 10000)
			arg[nargs] = 10 * arg[nargs] + command - '0';
		goto csi;
	case ';':
		nargs = MIN(nargs + 1, LEN(arg) - 1);
		goto csi;
	case ' ' ... '/': // Leading intermediate bytes
	case '<' ... '?': // Private mode characters
		goto csi;
	case 'A': // CUU -- Cursor <n> Up
		move_to(term.c.x, term.c.y - MAX(*arg, 1));
		break;
	case 'B': // CUD -- Cursor <n> Down
	case 'e': // VPR -- Cursor <n> Down
		move_to(term.c.x, term.c.y + MAX(*arg, 1));
		break;
	case 'C': // CUF -- Cursor <n> Forward
	case 'a': // HPR -- Cursor <n> Forward
		move_to(term.c.x + MAX(*arg, 1), term.c.y);
		break;
	case 'D': // CUB -- Cursor <n> Backward
		move_to(term.c.x - MAX(*arg, 1), term.c.y);
		break;
	case 'E': // CNL -- Cursor <n> Down and first col
		move_to(0, term.c.y + MAX(*arg, 1));
		break;
	case 'F': // CPL -- Cursor <n> Up and first col
		move_to(0, term.c.y + MAX(*arg, 1));
		break;
	case 'G': // CHA -- Move to <col>
	case '`': // HPA -- Move to <col>
		move_to(*arg - 1, term.c.y);
		break;
	case 'H': // CUP -- Move to <row> <col>
	case 'f': // HVP -- Move to <row> <col>
		move_to(arg[1] - 1, arg[0] - 1);
		break;
	case 'I': // CHT -- Cursor Forward Tabulation <n> tab stops
		move_to((term.c.x & ~7) + (MAX(*arg, 1) << 3), term.c.y);
		break;
	case 'J': // ED -- Clear screen
	case 'K': // EL -- Clear line
		clear_region(
			*arg ? 0 : term.c.x,
			*arg && command == 'J' ? 0 : term.c.y,
			*arg == 1 ? term.c.x : term.col - 1,
			*arg == 1 || command == 'K' ? term.c.y : term.row - 1);
		break;
	case 'L': // IL -- Insert <n> blank lines
	case 'M': // DL -- Delete <n> lines
		if (BETWEEN(term.c.y, term.top, term.bot)) {
			LIMIT(*arg, 1, term.bot - term.c.y + 1);
			move_region(term.c.y, command == 'L' ? -*arg : *arg);
			term.c.x = 0;
		}
		break;
	case 'P': // DCH -- Delete <n> char
	case '@': // ICH -- Insert <n> blank char
		LIMIT(*arg, 1, term.col - term.c.x);
		move_chars(command == '@', *arg);
		break;
	case 'S': // SU -- Scroll <n> line up
	case 'T': // SD -- Scroll <n> line down
		LIMIT(*arg, 1, term.bot - term.top + 1);
		move_region(term.top, command == 'T' ? -*arg : *arg);
		break;
	case 'X': // ECH -- Erase <n> char
		LIMIT(*arg, 1, term.col - term.c.x);
		clear_region(term.c.x, term.c.y, term.c.x + *arg - 1, term.c.y);
		break;
	case 'Z': // CBT -- Cursor Backward Tabulation <n> tab stops
		move_to((term.c.x & ~7) - (MAX(*arg, 1) << 3), term.c.y);
		break;
	case 'c': // DA -- Device Attributes
		if (*arg == 0)
			pty_printf("%s", VTIDEN);
		break;
	case 'd': // VPA -- Move to <row>
		move_to(term.c.x, *arg - 1);
		break;
	case 'h': // SM -- Set terminal mode
	case 'l': // RM -- Reset Mode
		for (u32 i = 0; i <= nargs; ++i)
			set_mode(command == 'h', arg[i]);
		break;
	case 'm': // SGR -- Terminal attribute
		for (u32 i = 0; i <= nargs; i += set_attr(arg + i));
		break;
	case 'n': // DSR – Device Status Report (cursor position)
		if (*arg == 6)
			pty_printf("\033[%i;%iR", term.c.y + 1, term.c.x + 1);
		break;
	case 'p': // DECSTR -- Soft terminal reset
		memset(&term.c, 0, sizeof(term.c));
		term.alt = term.hide = term.focus = term.charset = false;
		term.top = 0;
		term.bot = term.row - 1;
		break;
	case 'q': // DECSCUSR -- Set Cursor Style
		if (*arg <= 6)
			term.cursor = *arg;
		break;
	case 'r': // DECSTBM -- Set Scrolling Region
		arg[0] = arg[0] ? arg[0] : 1;
		arg[1] = arg[1] && arg[1] < term.row ? arg[1] : term.row;
		if (arg[0] < arg[1]) {
			term.top = arg[0] - 1;
			term.bot = arg[1] - 1;
			move_to(0, 0);
		}
		break;
	case 's': // DECSC -- Save cursor position
		term.saved_c[term.alt] = term.c;
		break;
	case 'u': // DECRC -- Restore cursor position
		term.c = term.saved_c[term.alt];
		break;
	}
}

static void handle_esc(u8 ascii)
{
	switch (ascii) {
	case '[': // CSI -- Control Sequence Introducer
		handle_csi();
		break;
	case ']': // OSC -- Operating System Command
		while (!IS_CONTROL(pty_getchar()));
		break;
	case ')': // G1D4 -- Set secondary charset G1
		term.charset = pty_getchar();
		break;
	case 'E': // NEL -- Next line
		newline();
		term.c.x = 0;
		break;
	case 'M': // RI -- Reverse index
		if (term.c.y <= term.top)
			move_region(term.top, -1);
		else
			--term.c.y;
		break;
	case 'c': // RIS -- Reset to inital state
		memset(&term, 0, sizeof(term));
		resize(xw.width, xw.height);
		break;
	case '7': // DECSC -- Save Cursor
		term.saved_c[term.alt] = term.c;
		break;
	case '8': // DECRC -- Restore Cursor
		term.c = term.saved_c[term.alt];
		break;
	}
}

static void tputc(u8 u)
{
	static const char* line_drawing = "┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│";
	static long i;
	static long utf_len;
	static Glyph *glyph;

	switch (u) {
	case '\b':
		move_to(term.c.x - 1, term.c.y);
		return;
	case '\t':
		move_to((term.c.x & ~7) + 8, term.c.y);
		return;
	case '\n' ... '\f':
		newline();
		return;
	case '\r':
		term.c.x = 0;
		return;
	case '\016': // LS1 -- Locking shift 1
	case '\017': // LS0 -- Locking shift 0
		term.line_drawing = term.charset == '0' && u == '\016';
		return;
	case '\033': // ESC
		handle_esc(pty_getchar());
		return;
	case 128 ... 191:
		if (i < utf_len) {
			glyph->u[i++] = u;
			return;
		}
		// FALLTHROUGH
	case ' ' ... '~':
	case 192 ... 255:
		glyph = &TLINE(term.c.y)[term.c.x];
		*glyph = term.c.attr;
		glyph->u[0] = u;
		utf_len = UTF_LEN(u);
		i = 1;

		if (term.line_drawing && BETWEEN(u, 'j', 'x'))
			memcpy(glyph->u, line_drawing + (u - 'j') * 3, 3);

		++term.c.x;
		if (term.c.x == term.col) {
			newline();
			term.c.x = 0;
		}
	}
}

static void __attribute__((noreturn)) run(void)
{
	fd_set read_fds;
	int xfd = XConnectionNumber(xw.dpy);
	int nfd = MAX(xfd, pty.fd) + 1;
	const struct timespec timeout = { 0, 1000000000 / FPS };
	struct timespec now, last = { 0, 0 };
	bool dirty = true;

	for (;;) {
		FD_ZERO(&read_fds);
		FD_SET(pty.fd, &read_fds);
		FD_SET(xfd, &read_fds);

		int result = pselect(nfd, &read_fds, 0, 0, &timeout, 0);
		if (result < 0)
			die("select failed: %s\n", strerror(errno));
		dirty |= result > 0;

		if (FD_ISSET(pty.fd, &read_fds)) {
			term.scroll = term.lines;
			tputc(pty_getchar());
			while (pty.c < pty.last)
				tputc(*pty.c++);
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
	create_window();
	setlocale(LC_CTYPE, "");
	load_fonts(FONTNAME);
	pty_new();
	run();
}
