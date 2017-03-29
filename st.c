/* See LICENSE file for copyright and license details. */

#include <X11/X.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <assert.h>
#include <errno.h>
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

// Config
#define LINE_SIZE 256
#define HIST_SIZE 2048
#define FPS 60
#define DEFAULTFG 15

// Macros
#define LEN(a)              (sizeof(a) / sizeof(*(a)))
#define MIN(a, b)           ((a) > (b) ? (b) : (a))
#define MAX(a, b)           ((a) < (b) ? (b) : (a))
#define BETWEEN(x, a, b)    ((a) <= (x) && (x) <= (b))
#define UTF_LEN(c)          ((c) < 0x80 ? 1 : (c) < 0xE0 ? 2 : (c) < 0xF0 ? 3 : 4)
#define LIMIT(x, a, b)      ((x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define SWAP(a, b)          do { __typeof(a) _swap = (a); (a) = (b); (b) = _swap; } while (0)
#define SLINE(y)            (term.hist[(y) % HIST_SIZE])
#define TLINE(y)            (SLINE((y) + term.scroll))
#define die(...)            do { fprintf(stderr, __VA_ARGS__); exit(1); } while (0)

#define IS_DELIM(c)         (strchr(" <>'`\"(){}", *(c)))
#define TIMEDIFF(t1, t2)    ((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_nsec - t2.tv_nsec) / 1000000)
#define AFTER(a, b)         ((a).y > (b).y || ((a).y == (b).y && (a).x >= (b).x))

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
	u16 mode;  // bitmask of ATTR_* flags
	u8 fg;     // foreground color
	u8 bg;     // background color
} Glyph;

typedef struct {
	Glyph attr; // current char attributes
	int x;
	int y;
} TCursor;

typedef struct {
	int x;
	int y;
} Point;

static struct {
	int snap; // snapping mode
	Point ob; // original coordinates of the beginning of the selection
	Point oe; // original coordinates of the end of the selection
	Point nb; // normalized coordinates of the beginning of the selection
	Point ne; // normalized coordinates of the end of the selection
} sel;

static struct {
	char buf[BUFSIZ];  // input buffer
	char *c;           // current reading position (points inside buf)
	char *end;         // one past the last valid char in buf
	int fd;            // file descriptor of the master pty
	int: 32;
	int rows;          // number of lines in a screen
	int cols;          // number of characters in a line
} pty;

// Terminal state
static struct {
	Glyph hist[HIST_SIZE][LINE_SIZE];     // history ring buffer
	int scroll;                           // hist index of the scroll position
	int lines;                            // hist index of the last line
	TCursor c;                            // cursor
	TCursor saved_c[2];                   // saved cursors
	int top;                              // top scroll limit
	int bot;                              // bottom scroll limit
	int cursor_style;
	u8 charset[4];
	bool report_buttons, report_motion, report_focus;
	bool alt, hide, appcursor, line_drawing;
} term;

// Drawing Context
static struct {
	Display *dpy;
	Window win;
	XftFont *font[4];
	Pixmap pixmap;
	GC gc;
	XftDraw *draw;
	char *font_name;
	int width, height;
	int font_height, font_width;
	int border;
	bool visible: 16;
	bool focused: 16;
} xw;

static XftColor colors[256];

#define selclear() (sel.ne.y = -1)

static void clear_region(int x1, int y1, int x2, int y2)
{
	if (sel.nb.y <= y2 && sel.ne.y >= y1)
		selclear();

	for (int y = y1; y <= y2; y++) {
		int xstart = y == y1 ? x1 : 0;
		int xend = y == y2 ? x2 : pty.cols - 1;
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
	clear_region(0, MIN(last, end), LINE_SIZE - 1, MAX(last, end));
}

static void move_chars(bool forward, int diff) {
	int dst = term.c.x + (forward ? diff : 0);
	int src = term.c.x + (forward ? 0 : diff);
	int size = pty.cols - (term.c.x + diff);
	int del = (forward ? term.c.x : pty.cols - diff);
	Glyph *line = TLINE(term.c.y);
	memmove(line + dst, line + src, size * sizeof(Glyph));
	clear_region(del, term.c.y, del + diff - 1, term.c.y);
}

static void move_to(int x, int y)
{
	term.c.x = LIMIT(x, 0, pty.cols - 1);
	term.c.y = LIMIT(y, 0, pty.rows - 1);
}

static void swap_screen(void)
{
	selclear();
	term.saved_c[term.alt] = term.c;
	term.alt = !term.alt;
	term.c = term.saved_c[term.alt];
	term.lines += term.alt ? pty.rows : -pty.rows;
	term.scroll = term.lines;
}

static void newline()
{
	if (term.c.y != term.bot) {
		move_to(term.c.x, term.c.y + 1);
	} else if (term.top || term.alt) {
		move_region(term.top, term.bot, 1);
	} else {
		++term.lines;
		++term.scroll;
		move_region(term.bot, pty.rows - 1, -1);
	}
}

static void scroll(int n)
{
	if (!term.alt) {
		term.scroll += n;
		LIMIT(term.scroll, MAX(0, term.lines - HIST_SIZE + pty.rows), term.lines);
	}
}

static Point pixel2cell(int px, int py)
{
	int x = (px - xw.border) / xw.font_width;
	int y = (py - xw.border) / xw.font_height;
	return (Point) { x, y + term.scroll };
}

static void selnormalize(void)
{
	bool swapped = AFTER(sel.ob, sel.oe);
	sel.nb = swapped ? sel.oe : sel.ob;
	sel.ne = swapped ? sel.ob : sel.oe;
	if (sel.snap >= SNAP_LINE) {
		sel.nb.x = 0;
		sel.ne.x = pty.cols - 1;
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

static int __attribute__((noreturn)) clean_exit(Display *dpy)
{
	for (int i = 0; i < 4; ++i)
		XftFontClose(dpy, xw.font[i]);
	XCloseDisplay(dpy);
	exit(0);
}

static void create_window(void)
{
	if (!(xw.dpy = XOpenDisplay(0)))
		die("Failed to open display\n");

	// Load fonts
	char *face_name = XGetDefault(xw.dpy, "st", "faceName");
	if (!face_name)
		face_name = "mono";

	char font_name[128];
	char *style[] = { "", ":style=bold", ":style=italic", ":style=bold italic" };
	for (int i = 0; i < 4; ++i) {
		strcpy(font_name, face_name);
		strcat(font_name, style[i]);
		xw.font[i] = XftFontOpenName(xw.dpy, DefaultScreen(xw.dpy), font_name);
	}

	xw.font_height = xw.font[0]->height + 1;
	xw.font_width = xw.font[0]->max_advance_width;

	char *scale_height = XGetDefault(xw.dpy, "st", "scaleHeight");
	if (scale_height)
		xw.font_height = (int) (xw.font_height * atof(scale_height) + .999);

	char *border = XGetDefault(xw.dpy, "st", "borderWidth");
	xw.border = border ? atoi(border) : 2;

	// Events
	XSetIOErrorHandler(clean_exit);
	XSetWindowAttributes attrs;
	attrs.event_mask = FocusChangeMask | VisibilityChangeMask | StructureNotifyMask;
	attrs.event_mask |= KeyPressMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask;

	// Create and map the window
	Window parent = XRootWindow(xw.dpy, DefaultScreen(xw.dpy));
	xw.win = XCreateSimpleWindow(xw.dpy, parent, 0, 0, 1, 1, 0, CopyFromParent, CopyFromParent);
	XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &attrs);
	XMapWindow(xw.dpy, xw.win);
	XStoreName(xw.dpy, xw.win, "st");

	// Graphic context
	XGCValues gcvalues = { .graphics_exposures = False };
	xw.gc = XCreateGC(xw.dpy, parent, GCGraphicsExposures, &gcvalues);
	XDefineCursor(xw.dpy, xw.win, XCreateFontCursor(xw.dpy, XC_xterm));
}

static void draw_text(Glyph glyph, u8 *text, int len, int x, int y)
{
	bool bold = (glyph.mode & ATTR_BOLD) != 0;
	bool italic = (glyph.mode & (ATTR_ITALIC | ATTR_BLINK)) != 0;
	XftFont *font = xw.font[bold + 2 * italic];
	XftColor fg = colors[glyph.fg ? glyph.fg : DEFAULTFG];
	XftColor bg = colors[glyph.bg];
	int baseline = y + font->ascent;
	int width = len * xw.font_width;

	if (glyph.mode & ATTR_INVISIBLE) {
		fg = bg;
	} else if (glyph.mode & ATTR_FAINT) {
		fg.color.red /= 2;
		fg.color.green /= 2;
		fg.color.blue /= 2;
	}

	if (glyph.mode & ATTR_REVERSE)
		SWAP(fg, bg);

	// Draw the background, then the text, then decorations
	XftDrawRect(xw.draw, &bg, x, y, width, xw.font_height);
	XftDrawStringUtf8(xw.draw, &fg, font, x, baseline, text, len);

	if (glyph.mode & ATTR_UNDERLINE)
		XftDrawRect(xw.draw, &fg, x, baseline + 1, width, 1);

	if (glyph.mode & ATTR_STRUCK)
		XftDrawRect(xw.draw, &fg, x, (2 * baseline + y) / 3, width, 1);

	if (glyph.mode & ATTR_BAR)
		XftDrawRect(xw.draw, &fg, x, y, 2, xw.font_height);
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
		xw.focused && term.cursor_style < 3 ? ATTR_REVERSE :
		term.cursor_style < 5 ? ATTR_UNDERLINE : ATTR_BAR;

	if (x == term.c.x && y == term.c.y)
		glyph.mode ^= cursor;

	bool diff = glyph.fg != prev.fg || glyph.bg != prev.bg || glyph.mode != prev.mode;

	if (x == 0 || diff) {
		draw_text(prev, buf, len, draw_x, draw_y);
		len = 0;
		draw_x = xw.border + x * xw.font_width;
		draw_y = xw.border + y * xw.font_height;
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

	for (int y = 0; y < pty.rows; ++y)
		for (int x = 0; x < pty.cols; ++x)
			draw_glyph(x, y);
	draw_glyph(0, 0);

	XCopyArea(xw.dpy, xw.pixmap, xw.win, xw.gc, 0, 0, xw.width, xw.height, 0, 0);
}

// Write every set and selected byte to the pipe
static void getsel(FILE* pipe)
{
	for (int y = sel.nb.y; y <= sel.ne.y; y++) {
		Glyph *line = SLINE(y);
		int x1 = y == sel.nb.y ? sel.nb.x : 0;
		int x2 = y == sel.ne.y ? sel.ne.x : pty.cols - 1;
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
		dprintf(pty.fd, "%s", sel_buf);
	}

	pclose(pipe);
}

static void mousereport(XButtonEvent *e)
{
	static Point prev;
	Point pos = pixel2cell(e->x, e->y);
	pos.y -= term.scroll;

	if (pos.x > 255 || pos.y > 255)
		return;

	if (e->type == MotionNotify && pos.x == prev.x && pos.y == prev.y)
		return;

	prev = pos;

	int button = e->type == ButtonRelease ? 3 : e->button + (e->button >= Button4 ? 60 : -1);
	dprintf(pty.fd, "\033[M%c%c%c", 32 + button, 33 + pos.x, 33 + pos.y);
}

static void special_key(u8 c, int state)
{
	if (state)
		dprintf(pty.fd, "\033[%d;%d%c", c < '@' ? c : 1, state + 1, c < '@' ? '~' : c);
	else if (c < '@')
		dprintf(pty.fd, "\033[%d~", c);
	else
		dprintf(pty.fd, "\033%c%c", term.appcursor ? 'O' : '[', c);
}

static void kpress(XEvent *e)
{
	static const u8 codes[] = {
		'H', 'D', 'A', 'C', 'B', 5, 6, 'F', [19] = 2, [175] = 3,     // cursor keys
		[69] = 'H', 'D', 'A', 'C', 'B', 5, 6, 'F', 'E', 2, 3,        // numpad
		[110] = 'P', 'Q', 'R', 'S', 15, 17, 18, 19, 20, 21, 23, 24,  // function keys
	};

	XKeyEvent *ev = &e->xkey;
	char buf[8] = "";
	KeySym keysym;
	XLookupString(ev, buf, LEN(buf) - 1, &keysym, NULL);

	if (BETWEEN(term.c.y + term.scroll, sel.nb.y, sel.ne.y))
		selclear();

	bool shift = (ev->state & ShiftMask) != 0;
	bool ctrl = (ev->state & ControlMask) != 0;
	bool alt = (ev->state & Mod1Mask) != 0;

	if (shift && keysym == XK_Insert)
		xsel("xsel -po", false);
	else if (shift && keysym == XK_Prior)
		scroll(4 - pty.rows);
	else if (shift && keysym == XK_Next)
		scroll(pty.rows - 4);
	else if (ctrl && shift && keysym == XK_C)
		xsel("xsel -bi", true);
	else if (ctrl && shift && keysym == XK_V)
		xsel("xsel -bo", false);
	else if (keysym == XK_ISO_Left_Tab)
		dprintf(pty.fd, "%s", "\033[Z");
	else if (keysym == XK_BackSpace)
		dprintf(pty.fd, "%c", ctrl ? 027 : 0177);
	else if (BETWEEN(keysym, 0xff50, 0xffff) && codes[keysym - 0xff50])
		special_key(codes[keysym - 0xff50], 4 * ctrl + 2 * alt + shift);
	else if (*buf)
		dprintf(pty.fd, "%s%s", alt ? "\033" : "", buf);
}

static void resize(XEvent *e)
{
	XConfigureEvent *ev = &e->xconfigure;

	if (ev->width == xw.width && ev->height == xw.height)
		return;

	// Update terminal info
	Point pty_size = pixel2cell(ev->width, ev->height);
	pty_size.y -= term.scroll;
	pty.cols = MIN((u16) pty_size.x, LINE_SIZE - 1);
	pty.rows = MIN((u16) pty_size.y, HIST_SIZE);
	term.top = 0;
	term.bot = pty.rows - 1;
	move_to(term.c.x, term.c.y);

	// Update X window data
	if (xw.pixmap)
		XFreePixmap(xw.dpy, xw.pixmap);
	if (xw.draw)
		XftDrawDestroy(xw.draw);

	Visual *visual = DefaultVisual(xw.dpy, DefaultScreen(xw.dpy));
	xw.width = ev->width;
	xw.height = ev->height;
	xw.pixmap = XCreatePixmap(xw.dpy, xw.win, xw.width, xw.height, 24);
	xw.draw = XftDrawCreate(xw.dpy, xw.pixmap, visual, CopyFromParent);

	// Send our size to the pty driver so that applications can query it
	struct winsize w = { (u16) pty.rows, (u16) pty.cols, 0, 0 };
	if (ioctl(pty.fd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

static void visibility(XEvent *e)
{
	XVisibilityEvent *ev = &e->xvisibility;
	xw.visible = ev->state != VisibilityFullyObscured;
}

static void focus(XEvent *ev)
{
	xw.focused = ev->type == FocusIn;
	if (term.report_focus)
		dprintf(pty.fd, "\033[%c", xw.focused ? 'I' : 'O');
}

static void bmotion(XEvent *e)
{
	XButtonEvent *ev = &e->xbutton;

	if (term.report_motion && !(ev->state & ShiftMask)) {
		mousereport(ev);
	} else if (ev->state & (Button1Mask | Button3Mask)) {
		sel.oe = pixel2cell(ev->x, ev->y);
		selnormalize();
	}
}

static void bpress(XEvent *e)
{
	XButtonEvent *ev = &e->xbutton;
	Point point = pixel2cell(ev->x, ev->y);

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
		sel.oe = point;
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
	[ConfigureNotify] = resize,
	[VisibilityNotify] = visibility,
	[FocusIn] = focus,
	[FocusOut] = focus,
	[MotionNotify] = bmotion,
	[ButtonPress] = bpress,
	[ButtonRelease] = brelease,
};

static char pty_getchar(void)
{
	if (pty.c >= pty.end) {
		pty.c = pty.buf;
		long result = read(pty.fd, pty.buf, BUFSIZ);
		if (result < 0)
			clean_exit(xw.dpy);
		pty.end = pty.buf + result;
	}

	return *pty.c++;
}

static void pty_new(char* cmd[])
{
	switch (forkpty(&pty.fd, 0, 0, 0)) {
	case -1:
		die("forkpty failed\n");
	case 0:
		execvp(cmd[0], cmd);
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
	case 1:    // Application cursor keys
		term.appcursor = set;
		break;
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
			clear_region(0, 0, pty.cols - 1, pty.rows - 1);
		break;
	case 1000: // Report mouse buttons
	case 1003: // Report mouse motion
		term.report_buttons = set;
		term.report_motion = mode == 1003 && set;
		break;
	case 1004: // Report focus events
		term.report_focus = set;
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
	case ' ' ... '/': // Intermediate bytes
	case '<' ... '?': // Private parameter bytes
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
	case 'I': // CHT -- Cursor forward <n> tabulation stops
		move_to(((term.c.x >> 3) + MAX(*arg, 1)) << 3, term.c.y);
		break;
	case 'J': // ED -- Clear screen
	case 'K': // EL -- Clear line
		clear_region(
			*arg ? 0 : term.c.x,
			*arg && command == 'J' ? 0 : term.c.y,
			*arg == 1 ? term.c.x : pty.cols - 1,
			*arg == 1 || command == 'K' ? term.c.y : pty.rows - 1);
		break;
	case 'L': // IL -- Insert <n> blank lines
	case 'M': // DL -- Delete <n> lines
		if (BETWEEN(term.c.y, term.top, term.bot)) {
			LIMIT(*arg, 1, term.bot - term.c.y + 1);
			move_region(term.c.y, term.bot, command == 'L' ? -*arg : *arg);
			term.c.x = 0;
		}
		break;
	case 'P': // DCH -- Delete <n> char
	case '@': // ICH -- Insert <n> blank char
		LIMIT(*arg, 1, pty.cols - term.c.x);
		move_chars(command == '@', *arg);
		break;
	case 'S': // SU -- Scroll <n> line up
	case 'T': // SD -- Scroll <n> line down
		LIMIT(*arg, 1, term.bot - term.top + 1);
		move_region(term.top, term.bot, command == 'T' ? -*arg : *arg);
		break;
	case 'X': // ECH -- Erase <n> char
		LIMIT(*arg, 1, pty.cols - term.c.x);
		clear_region(term.c.x, term.c.y, term.c.x + *arg - 1, term.c.y);
		break;
	case 'Z': // CBT -- Cursor backward <n> tabulation stops
		move_to(((term.c.x >> 3) - MAX(*arg, 1)) << 3, term.c.y);
		break;
	case 'c': // DA -- Device Attributes
		if (*arg == 0)
			dprintf(pty.fd, "%s", "\033[?64;15;22c");
		break;
	case 'd': // VPA -- Move to <row>
		move_to(term.c.x, *arg - 1);
		break;
	case 'h': // SM -- Set Mode
	case 'l': // RM -- Reset Mode
		for (u32 i = 0; i <= nargs; ++i)
			set_mode(command == 'h', arg[i]);
		break;
	case 'm': // SGR -- Select Graphic Rendition
		for (u32 i = 0; i <= nargs; i += set_attr(arg + i));
		break;
	case 'n': // DSR – Device Status Report (cursor position)
		if (*arg == 6)
			dprintf(pty.fd, "\033[%i;%iR", term.c.y + 1, term.c.x + 1);
		break;
	case 'p': // DECSTR -- Soft terminal reset
		memset(&term.top, 0, sizeof(term) - ((char*) &term.top - (char*) &term));
		term.bot = pty.rows - 1;
		break;
	case 'q': // DECSCUSR -- Set Cursor Style
		if (*arg <= 6)
			term.cursor_style = *arg;
		break;
	case 'r': // DECSTBM -- Set Scrolling Region
		arg[0] = arg[0] ? arg[0] : 1;
		arg[1] = arg[1] && arg[1] < pty.rows ? arg[1] : pty.rows;
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

static void handle_esc(u8 second_byte)
{
	u8 final_byte = ' ';

	switch (second_byte) {
	case ' ' ... '/':
		while (BETWEEN(final_byte, ' ', '/'))
			final_byte = pty_getchar();
		if (BETWEEN(second_byte, '(', '+'))
			term.charset[second_byte - '('] = final_byte;
		break;
	case '7': // DECSC -- Save Cursor
		term.saved_c[term.alt] = term.c;
		break;
	case '8': // DECRC -- Restore Cursor
		term.c = term.saved_c[term.alt];
		break;
	case 'E': // NEL -- Next line
		newline();
		term.c.x = 0;
		break;
	case 'M': // RI -- Reverse index
		if (term.c.y <= term.top)
			move_region(term.top, term.bot, -1);
		else
			--term.c.y;
		break;
	case '[': // CSI -- Control Sequence Introducer
		handle_csi();
		break;
	case ']': // OSC -- Operating System Command
		for (u8 c = 0; c != '\a'; c = pty_getchar())
			if (c == '\033' && pty_getchar())
				break;
		break;
	case 'c': // RIS -- Reset to inital state
		memset(&term, 0, sizeof(term));
		term.bot = pty.rows - 1;
		break;
	case 'n':
	case 'o':
		term.line_drawing = term.charset[second_byte - 'n' + 2] == '0';
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
		move_to(((term.c.x >> 3) + 1) << 3, term.c.y);
		return;
	case '\n' ... '\f':
		newline();
		return;
	case '\r':
		term.c.x = 0;
		return;
	case '\016': // LS1 -- Locking shift 1
	case '\017': // LS0 -- Locking shift 0
		term.line_drawing = term.charset[u == '\016'] == '0';
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
		if (term.c.x == pty.cols) {
			newline();
			term.c.x = 0;
		}

		glyph = &TLINE(term.c.y)[term.c.x];
		*glyph = term.c.attr;
		glyph->u[0] = u;
		utf_len = UTF_LEN(u);
		i = 1;

		if (term.line_drawing && BETWEEN(u, 'j', 'x'))
			memcpy(glyph->u, line_drawing + (u - 'j') * 3, 3);

		++term.c.x;
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

		if (FD_ISSET(pty.fd, &read_fds) && pty.rows) {
			term.scroll = term.lines;
			tputc(pty_getchar());
			while (pty.c < pty.end)
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

static u16 default_color(u16 i, int rgb)
{
	u16 theme[] = {
		0000, 0610, 0151, 0540, 0037, 0606, 0066, 0333,
		0222, 0730, 0471, 0750, 0427, 0727, 0057, 0777,
	};

	if (i < 16) // 0 ... 15: 16 system colors
		return 3 + 36 * ((theme[i] >> 3 * rgb) & 7);

	if (i >= 232) // 232 ... 255: 24 grayscale colors
		return 10 * (i - 232) + (u16[]) { 15, 5, 5 } [rgb];

	// 16 ... 231: 6x6x6 color cube
	i = (i - 16) / (u16[]) { 1, 6, 36 } [rgb] % 6;
	return 17 * (u16[]) { 0, 6, 8, 11, 13, 15 } [i];
}

static void load_colors() {
	Colormap colormap = DefaultColormap(xw.dpy, DefaultScreen(xw.dpy));
	char color_name[9] = "color";
	char *value;

	for (u16 i = 0; i < 256; ++i) {
		sprintf(color_name + 5, "%d", i);
		XColor *color = (XColor*) &colors[i];

		if ((value = XGetDefault(xw.dpy, "st", color_name))) {
			XLookupColor(xw.dpy, colormap, value, color, color);
		} else {
			color->red   = default_color(i, 2) * 257;
			color->green = default_color(i, 1) * 257;
			color->blue  = default_color(i, 0) * 257;
		}

		colors[i].color.alpha = 0xffff;
	}
}

int main(int argc, char *argv[])
{
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers(""); // Xlib leaks memory if we don’t call this
	create_window();
	load_colors();
	pty_new(argc > 1 ? argv + 1 : (char*[]) { getenv("SHELL"), NULL });
	run();
}
