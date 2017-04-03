// vvvvvt - varicolored vernacular vivacious verisimilar virtual terminal
// See LICENSE file for copyright and license details.

#include <X11/X.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
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
#include <strings.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

// Config
#define LINE_SIZE 256
#define HIST_SIZE 2048

// Macros
#define BETWEEN(x, a, b)    ((a) <= (x) && (x) <= (b))
#define LEN(a)              (sizeof(a) / sizeof(*(a)))
#define LIMIT(x, a, b)      ((x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define MAX(a, b)           ((a) < (b) ? (b) : (a))
#define MIN(a, b)           ((a) > (b) ? (b) : (a))
#define SWAP(a, b)          do { __typeof(a) _swap = (a); (a) = (b); (b) = _swap; } while (0)

#define IS_DELIM(c)         (strchr(" <>()[]{}'`\"", *(c)))
#define POINT_EQ(a, b)      ((a).x == (b).x && (a).y == (b).y)
#define POINT_GT(a, b)      ((a).y > (b).y || ((a).y == (b).y && (a).x > (b).x))
#define SLINE(y)            (term.hist[(y) % HIST_SIZE])
#define TLINE(y)            (SLINE((y) + term.scroll))
#define UTF_LEN(c)          ((c) < 0xC0 ? 1 : (c) < 0xE0 ? 2 : (c) < 0xF0 ? 3 : utf_len[c & 0x0F])
#define clear_selection()   (sel.ne.y = -1)
#define die(message)        exit(fprintf(stderr, message ": %s\n", errno ? strerror(errno) : ""))

typedef enum { false, true } bool;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

static u32 utf_len[16] = { 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 0, 0 };

// Selection snapping modes
enum { SNAP_WORD = 2, SNAP_LINE = 3 };

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
	u16 attr;  // bitmask of ATTR_* flags
	u8 fg;     // foreground color
	u8 bg;     // background color
} Rune;

static struct {
	Rune rune; // current char attributes
	int x;
	int y;
} cursor, saved_cursors[2];

typedef struct {
	int x;
	int y;
} Point;

static struct {
	int snap; // snapping mode
	Point ob; // coordinates of the point clicked to start the selection
	Point nb; // coordinates of the beginning of the selection (inclusive)
	Point ne; // coordinates of the end of the selection (exclusive)
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
	Rune hist[HIST_SIZE][LINE_SIZE];     // history ring buffer
	int scroll;                          // hist index of the scroll position
	int lines;                           // hist index of the last line
	int top;                             // top scroll limit
	int bot;                             // bottom scroll limit
	int cursor_style;
	u8 charset[4];
	bool report_buttons, report_motion, report_focus;
	bool alt, hide, appcursor, line_drawing;
	bool meta_sends_escape, reverse;
} term;

// Drawing Context
static struct {
	Display *disp;
	XftFont *font[4];
	XftDraw *draw;
	char *font_name;
	int width, height;
	int font_height, font_width;
	int border;
	bool visible: 16;
	bool focused: 16;
} w;

static XftColor colors[256];
static XrmDatabase xrm;

static void clear_region(int x1, int y1, int x2, int y2)
{
	if (sel.nb.y <= y2 && sel.ne.y >= y1)
		clear_selection();

	for (int y = y1; y <= y2; y++) {
		int xstart = y == y1 ? x1 : 0;
		int xend = y == y2 ? x2 : pty.cols - 1;
		memset(TLINE(y) + xstart, 0, (xend - xstart + 1) * sizeof(Rune));
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
	int dst = cursor.x + (forward ? diff : 0);
	int src = cursor.x + (forward ? 0 : diff);
	int size = pty.cols - (cursor.x + diff);
	int del = (forward ? cursor.x : pty.cols - diff);
	Rune *line = TLINE(cursor.y);
	memmove(line + dst, line + src, size * sizeof(Rune));
	clear_region(del, cursor.y, del + diff - 1, cursor.y);
}

static void move_to(int x, int y)
{
	cursor.x = LIMIT(x, 0, pty.cols - 1);
	cursor.y = LIMIT(y, 0, pty.rows - 1);
}

static void swap_screen(void)
{
	clear_selection();
	saved_cursors[term.alt] = cursor;
	term.alt = !term.alt;
	cursor = saved_cursors[term.alt];
	term.lines += term.alt ? pty.rows : -pty.rows;
	term.scroll = term.lines;
}

static void newline()
{
	if (cursor.y != term.bot) {
		move_to(cursor.x, cursor.y + 1);
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
	int x = (px - w.border) / w.font_width;
	int y = (py - w.border) / w.font_height;
	return (Point) { x, y };
}

static void copy(bool clipboard)
{
	// If the selection is empty, leave the clipboard as-is rather than emptying it
	if (!POINT_GT(sel.ne, sel.nb))
		return;

	FILE* pipe = popen(clipboard ? "xsel -bi" : "xsel -i", "w");

	for (int y = sel.nb.y; y <= sel.ne.y; y++) {
		Rune *line = SLINE(y);
		int x1 = y == sel.nb.y ? sel.nb.x : 0;
		int x2 = y == sel.ne.y ? sel.ne.x : pty.cols;

		for (int x = x1; x < x2; ++x)
			fprintf(pipe, "%.4s", line[x].u);
		if (x2 == pty.cols)
			fprintf(pipe, "\n");
	}

	pclose(pipe);
}

static void paste(bool clipboard)
{
	FILE* pipe = popen(clipboard ? "xsel -bo" : "xsel -o", "r");

	char sel_buf[BUFSIZ] = "";
	fread(sel_buf, 1, BUFSIZ, pipe);
	for (char *p = sel_buf; (p = strchr(p, '\n')); *p++ = '\r');
	dprintf(pty.fd, "%s", sel_buf);

	pclose(pipe);
}

static void selnormalize(Point oe)
{
	bool swapped = POINT_GT(sel.ob, oe);
	sel.nb = swapped ? oe : sel.ob;
	sel.ne = swapped ? sel.ob : oe;

	if (sel.snap == SNAP_LINE) {
		sel.nb.x = 0;
		sel.ne.x = pty.cols;
	} else if (sel.snap == SNAP_WORD) {
		while (sel.nb.x > 0 && !IS_DELIM(SLINE(sel.nb.y)[sel.nb.x - 1].u))
			--sel.nb.x;
		while (!IS_DELIM(SLINE(sel.ne.y)[sel.ne.x].u))
			++sel.ne.x;
	}
}

static bool selected(int x, int y)
{
	return BETWEEN(y, sel.nb.y, sel.ne.y)
		&& (y != sel.nb.y || x >= sel.nb.x)
		&& (y != sel.ne.y || x <  sel.ne.x);
}

static int __attribute__((noreturn)) clean_exit(Display *disp)
{
	for (int i = 0; i < 4; ++i)
		XftFontClose(disp, w.font[i]);
	XrmDestroyDatabase(xrm);
	XCloseDisplay(disp);
	exit(0);
}

static void create_window(void)
{
	if (!(w.disp = XOpenDisplay(0)))
		die("Failed to open display");

	// Events
	XSetIOErrorHandler(clean_exit);
	XSetWindowAttributes attrs;
	attrs.event_mask = FocusChangeMask | VisibilityChangeMask | StructureNotifyMask;
	attrs.event_mask |= KeyPressMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask;

	// Create and map the window
	Window parent = XRootWindow(w.disp, DefaultScreen(w.disp));
	Window win = XCreateSimpleWindow(w.disp, parent, 0, 0, 1, 1, 0, CopyFromParent, CopyFromParent);
	w.draw = XftDrawCreate(w.disp, win, DefaultVisual(w.disp, DefaultScreen(w.disp)), CopyFromParent);

	XChangeWindowAttributes(w.disp, win, CWEventMask, &attrs);
	XStoreName(w.disp, win, "vvvvvt");
	XDefineCursor(w.disp, win, XCreateFontCursor(w.disp, XC_xterm));
	XMapWindow(w.disp, win);
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
	return i ? 55 + 40 * i : 0;
}

static bool is_true(const char* word)
{
	return !strcasecmp(word, "true") || !strcasecmp(word, "yes") || !strcasecmp(word, "on");
}

static const char* get_resource(const char* resource, const char* fallback)
{
	char *type;
	XrmValue result;
	char name[32] = "vvvvvt.";
	strcat(name, resource);
	XrmGetResource(xrm, name, name, &type, &result);
	return result.addr ? result.addr : fallback;
}

static void load_resources() {
	if (xrm)
		XrmDestroyDatabase(xrm);
	xrm = XrmGetFileDatabase("/home/grimy/config/Xresources");

	// Fonts
	const char *face_name = get_resource("faceName", "mono");
	const char *style[] = { "", "bold", "italic", "bold italic" };
	char font_name[128];
	for (int i = 0; i < 4; ++i) {
		sprintf(font_name, "%s:style=%s", face_name, style[i]);
		if (w.font[i])
			XftFontClose(w.disp, w.font[i]);
		w.font[i] = XftFontOpenName(w.disp, DefaultScreen(w.disp), font_name);
	}

	// Colors
	Colormap colormap = DefaultColormap(w.disp, DefaultScreen(w.disp));
	char color_name[16] = "color";
	char def[16] = "";
	for (u16 i = 0; i < 256; ++i) {
		sprintf(color_name + 5, "%d", i);
		sprintf(def, "#%02x%02x%02x", default_color(i, 2), default_color(i, 1), default_color(i, 0));
		XColor *color = (XColor*) &colors[i];
		XLookupColor(w.disp, colormap, get_resource(color_name, def), color, color);
		colors[i].color.alpha = 0xffff;
	}

	// Others
	double scale_height = atof(get_resource("scaleHeight", "1"));
	w.font_height = (int) ((w.font[0]->height + 1) * scale_height + .999);
	w.font_width = w.font[0]->max_advance_width;
	w.border = atoi(get_resource("borderWidth", "2"));
	term.meta_sends_escape = is_true(get_resource("metaSendsEscape", ""));
	term.reverse = is_true(get_resource("reverseVideo", "on"));
}

static void draw_text(Rune rune, u8 *text, int len, int x, int y)
{
	bool bold = (rune.attr & ATTR_BOLD) != 0;
	bool italic = (rune.attr & (ATTR_ITALIC | ATTR_BLINK)) != 0;
	XftFont *font = w.font[bold + 2 * italic];
	XftColor fg = colors[rune.fg || !term.reverse ? rune.fg : 15];
	XftColor bg = colors[rune.bg ||  term.reverse ? rune.bg : 15];
	int baseline = y + font->ascent;
	int width = len * w.font_width;

	if (rune.attr & ATTR_INVISIBLE) {
		fg = bg;
	} else if (rune.attr & ATTR_FAINT) {
		fg.color.red /= 2;
		fg.color.green /= 2;
		fg.color.blue /= 2;
	}

	if (rune.attr & ATTR_REVERSE)
		SWAP(fg, bg);

	// Draw the background, then the text, then decorations
	XftDrawRect(w.draw, &bg, x, y, width, w.font[0]->height + 1);
	XftDrawStringUtf8(w.draw, &fg, font, x, baseline, text, len);

	if (rune.attr & ATTR_UNDERLINE)
		XftDrawRect(w.draw, &fg, x, baseline + 1, width, 1);

	if (rune.attr & ATTR_STRUCK)
		XftDrawRect(w.draw, &fg, x, (2 * baseline + y) / 3, width, 1);

	if (rune.attr & ATTR_BAR)
		XftDrawRect(w.draw, &fg, x, y, 2, w.font_height);
}

// Draws the rune at the given terminal coordinates.
static void draw_rune(int x, int y)
{
	static u8 buf[4 * LINE_SIZE];
	static int len;
	static Rune prev;
	static int draw_x, draw_y;

	Rune rune = TLINE(y)[x];

	// Handle selection and cursor
	if (selected(x, y + term.scroll))
		rune.attr ^= ATTR_REVERSE;

	int cursor_attr = term.hide || term.scroll != term.lines ? 0 :
		w.focused && term.cursor_style < 3 ? ATTR_REVERSE :
		term.cursor_style < 5 ? ATTR_UNDERLINE : ATTR_BAR;

	if (x == cursor.x && y == cursor.y)
		rune.attr ^= cursor_attr;

	bool diff = rune.fg != prev.fg || rune.bg != prev.bg || rune.attr != prev.attr;

	if (x == 0 || diff) {
		draw_text(prev, buf, len, draw_x, draw_y);
		len = 0;
		draw_x = w.border + x * w.font_width;
		draw_y = w.border + y * w.font_height;
		prev = rune;
	}

	if (*rune.u < 0x80) {
		buf[len++] = MAX(*rune.u, ' ');
	} else if (*rune.u >= 0xC0 && strnlen((char*) rune.u, 4) == UTF_LEN(*rune.u)) {
		memcpy(buf + len, rune.u, UTF_LEN(*rune.u));
		len += UTF_LEN(*rune.u);
	} else {
		memcpy(buf + len, "⁇", 3);
		len += 3;
	}
}

// Redraws all runes on our buffer, then flushes it to the window.
static void draw(void)
{
	for (int y = 0; y < pty.rows; ++y)
		for (int x = 0; x < pty.cols; ++x)
			draw_rune(x, y);
	draw_rune(0, 0);
	XFlush(w.disp);
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

static void on_keypress(XKeyEvent *e)
{
	static const u8 codes[] = {
		'H', 'D', 'A', 'C', 'B', 5, 6, 'F', [19] = 2, [175] = 3,     // cursor keys
		[69] = 'H', 'D', 'A', 'C', 'B', 5, 6, 'F', 'E', 2, 3,        // numpad
		[110] = 'P', 'Q', 'R', 'S', 15, 17, 18, 19, 20, 21, 23, 24,  // function keys
	};

	bool shift = (e->state & ShiftMask) != 0;
	bool ctrl = (e->state & ControlMask) != 0;
	bool meta = (e->state & Mod1Mask) != 0;

	char buf[8] = "";
	KeySym keysym;
	int len = XLookupString(e, buf, LEN(buf) - 1, &keysym, NULL);

	if (selected(cursor.x, cursor.y + term.scroll))
		clear_selection();

	if (meta && term.meta_sends_escape && len)
		dprintf(pty.fd, "%c", 033);

	if (shift && keysym == XK_Insert)
		paste(false);
	else if (shift && keysym == XK_Prior)
		scroll(4 - pty.rows);
	else if (shift && keysym == XK_Next)
		scroll(pty.rows - 4);
	else if (ctrl && shift && keysym == XK_C)
		copy(true);
	else if (ctrl && shift && keysym == XK_V)
		paste(true);
	else if (keysym == XK_ISO_Left_Tab)
		dprintf(pty.fd, "%s", "\033[Z");
	else if (ctrl && keysym == XK_question)
		dprintf(pty.fd, "%c", 127);
	else if (keysym == XK_BackSpace)
		dprintf(pty.fd, "%c", ctrl ? 027 : 0177);
	else if (BETWEEN(keysym, 0xff50, 0xffff) && codes[keysym - 0xff50])
		special_key(codes[keysym - 0xff50], 4 * ctrl + 2 * meta + shift);
	else if (len)
		write(pty.fd, buf, len);
}

static void on_resize(XConfigureEvent *e)
{
	if (e->width == w.width && e->height == w.height)
		return;

	// Update terminal info
	Point pty_size = pixel2cell(e->width, e->height);
	pty.cols = MIN((u16) pty_size.x, LINE_SIZE - 1);
	pty.rows = MIN((u16) pty_size.y, HIST_SIZE / 2);
	term.top = 0;
	term.bot = pty.rows - 1;
	move_to(cursor.x, cursor.y);

	// Update X window data
	w.width = e->width;
	w.height = e->height;

	// Send our size to the pty driver so that applications can query it
	struct winsize size = { (u16) pty.rows, (u16) pty.cols, 0, 0 };
	if (ioctl(pty.fd, TIOCSWINSZ, &size) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

static void on_mouse(XButtonEvent *e)
{
	static Point prev;
	int button = e->type == ButtonRelease ? 4 : e->button + (e->button >= Button4 ? 61 : 0);
	Point pos = pixel2cell(e->x, e->y);
	pos.y += term.scroll;

	if (!button && POINT_EQ(pos, prev))
		return;
	prev = pos;

	if (term.report_buttons && (button || term.report_motion) && e->state != ShiftMask) {
		if (pos.x <= 222 && pos.y <= 222)
			dprintf(pty.fd, "\033[M%c%c%c", 31 + button, 33 + pos.x, 33 + pos.y);
		return;
	}

	switch (button) {
	case 0:  // Cursor movement
		if (e->state & (Button1Mask | Button3Mask))
			selnormalize(pos);
		break;
	case 1:  // Left click
		sel.snap = POINT_EQ(pos, sel.ob) * sel.snap + 1 & 3;
		sel.ob = pos;
		selnormalize(pos);
		break;
	case 2:  // Middle click
		paste(false);
		break;
	case 3:  // Right click
		sel.snap = SNAP_LINE;
		selnormalize(pos);
		break;
	case 4:  // Any button released
		copy(false);
		break;
	case 65: // Scroll wheel up
		scroll(-5);
		break;
	case 66: // Scroll wheel down
		scroll(5);
		break;
	}
}

static void handle_xevent(XEvent * e)
{
	switch (e->type) {
	case KeyPress:
		on_keypress((XKeyEvent*) e);
		break;
	case ButtonPress:
	case ButtonRelease:
	case MotionNotify:
		on_mouse((XButtonEvent*) e);
		break;
	case ConfigureNotify:
		on_resize((XConfigureEvent*) e);
		break;
	case VisibilityNotify:
		w.visible = ((XVisibilityEvent*) e)->state != VisibilityFullyObscured;
		break;
	case FocusIn:
	case FocusOut:
		w.focused = e->type == FocusIn;
		if (term.report_focus)
			dprintf(pty.fd, "\033[%c", w.focused ? 'I' : 'O');
		break;
	}
}

static u8 pty_getchar(void)
{
	if (pty.c >= pty.end) {
		pty.c = pty.buf;
		long result = read(pty.fd, pty.buf, BUFSIZ);
		if (result < 0)
			clean_exit(w.disp);
		pty.end = pty.buf + result;
	}

	return *pty.c++;
}

static void pty_new(char* cmd[])
{
	switch (forkpty(&pty.fd, 0, 0, 0)) {
	case -1:
		die("forkpty failed");
	case 0:
		setenv("TERM", "xterm-256color", 1);
		execvp(cmd[0], cmd);
		die("exec failed");
	default:
		signal(SIGCHLD, SIG_IGN);
	}
}

static int set_attr(int *attr)
{
	u8 *color = &cursor.rune.fg;
	if (BETWEEN(*attr, 40, 49) || BETWEEN(*attr, 100, 107)) {
		color = &cursor.rune.bg;
		*attr -= 10;
	}

	switch (*attr) {
	case 0:
		memset(&cursor.rune, 0, sizeof(cursor.rune));
		return 1;
	case 1 ... 9:
		cursor.rune.attr |= 1 << (*attr - 1);
		return 1;
	case 21 ... 29:
		cursor.rune.attr &= ~(1 << (*attr - 21));
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
	case 1036: // DECSET -- meta sends escape
		term.meta_sends_escape = set;
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
		move_to(cursor.x, cursor.y - MAX(*arg, 1));
		break;
	case 'B': // CUD -- Cursor <n> Down
	case 'e': // VPR -- Cursor <n> Down
		move_to(cursor.x, cursor.y + MAX(*arg, 1));
		break;
	case 'C': // CUF -- Cursor <n> Forward
	case 'a': // HPR -- Cursor <n> Forward
		move_to(cursor.x + MAX(*arg, 1), cursor.y);
		break;
	case 'D': // CUB -- Cursor <n> Backward
		move_to(cursor.x - MAX(*arg, 1), cursor.y);
		break;
	case 'E': // CNL -- Cursor <n> Down and first col
		move_to(0, cursor.y + MAX(*arg, 1));
		break;
	case 'F': // CPL -- Cursor <n> Up and first col
		move_to(0, cursor.y + MAX(*arg, 1));
		break;
	case 'G': // CHA -- Move to <col>
	case '`': // HPA -- Move to <col>
		move_to(*arg - 1, cursor.y);
		break;
	case 'H': // CUP -- Move to <row> <col>
	case 'f': // HVP -- Move to <row> <col>
		move_to(arg[1] - 1, arg[0] - 1);
		break;
	case 'I': // CHT -- Cursor forward <n> tabulation stops
		move_to(((cursor.x >> 3) + MAX(*arg, 1)) << 3, cursor.y);
		break;
	case 'J': // ED -- Clear screen
	case 'K': // EL -- Clear line
		clear_region(
			*arg ? 0 : cursor.x,
			*arg && command == 'J' ? 0 : cursor.y,
			*arg == 1 ? cursor.x : pty.cols - 1,
			*arg == 1 || command == 'K' ? cursor.y : pty.rows - 1);
		break;
	case 'L': // IL -- Insert <n> blank lines
	case 'M': // DL -- Delete <n> lines
		if (BETWEEN(cursor.y, term.top, term.bot)) {
			LIMIT(*arg, 1, term.bot - cursor.y + 1);
			move_region(cursor.y, term.bot, command == 'L' ? -*arg : *arg);
			cursor.x = 0;
		}
		break;
	case 'P': // DCH -- Delete <n> char
	case '@': // ICH -- Insert <n> blank char
		LIMIT(*arg, 1, pty.cols - cursor.x);
		move_chars(command == '@', *arg);
		break;
	case 'S': // SU -- Scroll <n> line up
	case 'T': // SD -- Scroll <n> line down
		LIMIT(*arg, 1, term.bot - term.top + 1);
		move_region(term.top, term.bot, command == 'T' ? -*arg : *arg);
		break;
	case 'X': // ECH -- Erase <n> char
		LIMIT(*arg, 1, pty.cols - cursor.x);
		clear_region(cursor.x, cursor.y, cursor.x + *arg - 1, cursor.y);
		break;
	case 'Z': // CBT -- Cursor backward <n> tabulation stops
		move_to(((cursor.x >> 3) - MAX(*arg, 1)) << 3, cursor.y);
		break;
	case 'c': // DA -- Device Attributes
		if (*arg == 0)
			dprintf(pty.fd, "%s", "\033[?64;15;22c");
		break;
	case 'd': // VPA -- Move to <row>
		move_to(cursor.x, *arg - 1);
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
			dprintf(pty.fd, "\033[%i;%iR", cursor.y + 1, cursor.x + 1);
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
		saved_cursors[term.alt] = cursor;
		break;
	case 'u': // DECRC -- Restore cursor position
		cursor = saved_cursors[term.alt];
		break;
	}
}

//
static void handle_esc(u8 second_byte)
{
	u8 final_byte = second_byte;
	while (BETWEEN(final_byte, ' ', '/'))
		final_byte = pty_getchar();

	switch (second_byte) {
	case '(' ... '+':
		term.charset[second_byte - '('] = final_byte;
		break;
	case '7': // DECSC -- Save Cursor
		saved_cursors[term.alt] = cursor;
		break;
	case '8': // DECRC -- Restore Cursor
		cursor = saved_cursors[term.alt];
		break;
	case 'E': // NEL -- Next line
		newline();
		cursor.x = 0;
		break;
	case 'M': // RI -- Reverse index
		if (cursor.y <= term.top)
			move_region(term.top, term.bot, -1);
		else
			--cursor.y;
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

static void pty_putchar(u8 u)
{
invalid_utf8:
	switch (u) {
	case '\b':
		move_to(cursor.x - 1, cursor.y);
		return;
	case '\t':
		move_to(((cursor.x >> 3) + 1) << 3, cursor.y);
		return;
	case '\n' ... '\f':
		newline();
		return;
	case '\r':
		cursor.x = 0;
		return;
	case '\016': // LS1 -- Locking shift 1
	case '\017': // LS0 -- Locking shift 0
		term.line_drawing = term.charset[u == '\016'] == '0';
		return;
	case '\033': // ESC
		handle_esc(pty_getchar());
		return;
	case ' ' ... 255:
		if (cursor.x == pty.cols) {
			newline();
			cursor.x = 0;
		}

		cursor.rune.u[0] = u;
		Rune *rune = &TLINE(cursor.y)[cursor.x];
		*rune = cursor.rune;
		++cursor.x;

		for (long i = 1, len = UTF_LEN(u); i < len; ++i) {
			u = pty_getchar();
			if (!BETWEEN(u, 128, 191))
				goto invalid_utf8;
			rune->u[i & 3] = u;
		}

		if (term.line_drawing && BETWEEN(u, 'j', 'x'))
			memcpy(rune, &"┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│"[(u - 'j') * 3], 3);
	}
}

static void __attribute__((noreturn)) run(void)
{
	fd_set read_fds;

	int xfd = XConnectionNumber(w.disp);
	int ifd = inotify_init();
	int nfd = MAX(ifd, MAX(xfd, pty.fd)) + 1;

	char fname[256];
	strcpy(fname, getenv("XDG_CONFIG_HOME"));
	strcat(fname, "/Xresources");
	inotify_add_watch(ifd, fname, IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF);

	const struct timespec timeout = { 0, 20000000 }; // 20ms
	struct timespec now, last = { 0, 0 };
	bool dirty = true;

	for (;;) {
		FD_ZERO(&read_fds);
		FD_SET(pty.fd, &read_fds);
		FD_SET(ifd, &read_fds);
		FD_SET(xfd, &read_fds);

		int result = pselect(nfd, &read_fds, 0, 0, &timeout, 0);
		if (result < 0)
			die("select failed");
		dirty |= result > 0;

		if (FD_ISSET(pty.fd, &read_fds) && pty.rows) {
			term.scroll = term.lines;
			pty_putchar(pty_getchar());
			while (pty.c < pty.end)
				pty_putchar(*pty.c++);
		}

		if (FD_ISSET(ifd, &read_fds)) {
			u8 buf[BUFSIZ];
			read(ifd, buf, BUFSIZ);
			inotify_add_watch(ifd, fname, IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF);
			load_resources();
		}

		XEvent e;
		while (XPending(w.disp)) {
			XNextEvent(w.disp, &e);
			handle_xevent(&e);
		}

		clock_gettime(CLOCK_MONOTONIC, &now);

		if (now.tv_sec > last.tv_sec || (dirty && w.visible && now.tv_nsec > last.tv_nsec + 20000000)) {
			draw();
			dirty = false;
			last = now;
		}
	}
}

int main(int argc, char *argv[])
{
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers(""); // Xlib leaks memory if we don’t call this
	XrmInitialize();
	create_window();
	load_resources();
	pty_new(argc > 1 ? argv + 1 : (char*[]) { getenv("SHELL"), NULL });
	run();
}
