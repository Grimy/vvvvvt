// vvvvvt - varicolored vernacular vivacious verisimilar virtual terminal
// See LICENSE file for copyright and license details.

#include <X11/X.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <locale.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
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
#define LINE(y)             (SLINE((y) + term.scroll))
#define UTF_LEN(c)          ((c) < 0xC0 ? 1 : (c) < 0xE0 ? 2 : (c) < 0xF0 ? 3 : utf_len[c & 0x0F])
#define clear_selection()   (sel.end = sel.start)

#define die(message)        do { perror(message); clean_exit(w.disp); } while (0)

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
	ATTR_DIRTY      = 1 << 9,
};

typedef struct {
	u8 u[4];   // raw UTF-8 bytes
	u16 attr;  // bitmask of ATTR_* flags
	u8 fg;     // foreground color
	u8 bg;     // background color
} Rune;

typedef struct {
	int x;
	int y;
} Point;

// State affected by save cursor / restore cursor
static struct {
	Rune rune; // current char attributes
	int x, y;  // cursor position
} cursor, saved_cursors[2];

static struct {
	int snap;    // snapping mode
	Point mark;  // coordinates of the point clicked to start the selection
	Point start; // coordinates of the beginning of the selection (inclusive)
	Point end;   // coordinates of the end of the selection (exclusive)
} sel;

static struct {
	char buf[BUFSIZ];  // input buffer
	char *c;           // current reading position (points inside `buf`)
	char *end;         // one past the last valid char (points inside `buf`)
	int fd;            // file descriptor of the master pty
	int rows;          // number of lines in a screen
	int cols;          // number of characters in a line
	int: 32;
} pty;

// Terminal state
static struct {
	Rune hist[HIST_SIZE][LINE_SIZE]; // history ring buffer
	int scroll;                      // scroll position (index inside `hist`)
	int lines;                       // last line printed (index inside `hist`)
	int top;                         // top scroll limit
	int bot;                         // bottom scroll limit
	int cursor_style;                // appearance of the cursor
	u8 charsets[4];                  // designated character sets
	int charset;                     // invoked character set (index inside `charsets`)
	bool alt;                        // use the alternate screen buffer?
	bool hide;                       // hide the cursor?
	bool reverse_video;              // use a dark background?
	bool report_buttons;             // report clicks/scrolls to the application?
	bool report_motion;              // report mouse motions to the application?
	bool report_focus;               // report focus in/out events to the application?
	bool bracketed_paste;            // send escape sequences before/after each paste?
	bool app_keys;                   // send different escape sequences for arrow keys?
	bool meta_sends_escape;          // send an ESC char when a key is pressed with meta held?
} term;

// Drawing context
static struct {
	Display *disp;
	XftFont *font[4];
	XftDraw *draw;
	XftColor colors[256];
	Window parent;
	Window win;
	int screen;
	bool dirty;
	int font_height, font_width;
	int border;
	bool focused;
} w;

// Is the character at row `y`, column `x` currently selected?
static bool selected(int x, int y)
{
	return BETWEEN(y, sel.start.y, sel.end.y)
		&& (y != sel.start.y || x >= sel.start.x)
		&& (y != sel.end.y || x < sel.end.x);
}

// Erase all characters between lines `start` and `end` (exclusive)
static void erase_lines(int start, int end)
{
	if (sel.start.y < end && sel.end.y >= start)
		clear_selection();
	for (int y = start; y < end; y++)
		memset(LINE(y), 0, sizeof(LINE(y)));
}

// Erase characters between columns `start` and `end` in the current line
static void erase_chars(int start, int end)
{
	Rune *line = LINE(cursor.y);
	for (int x = start; x < end; ++x)
		line[x] = (Rune) { "", 0, 0, cursor.rune.bg };
}

// Move lines between `start` and `end` by `diff` rows down
static void move_lines(int start, int end, int diff)
{
	if (sel.start.y >= start && sel.end.y < end) {
		sel.start.y -= diff;
		sel.end.y -= diff;
	}

	int step = diff < 0 ? -1 : 1;
	if (diff < 0)
		SWAP(start, end);
	int last = end - diff + step;
	for (int y = start; y != last; y += step)
		memcpy(LINE(y), LINE(y + diff), sizeof(LINE(y)));
	erase_lines(MIN(last, end), MAX(last, end) + 1);
}

// Move characters between columns `start` and `end` of the current line by `diff`
static void move_chars(int start, int end, int diff)
{
	Rune *line = LINE(cursor.y);

	int step = diff < 0 ? -1 : 1;
	if (diff < 0)
		SWAP(start, end);
	int last = end - diff + step;
	for (int x = start; x != last; x += step)
		line[x] = line[x + diff];
	erase_chars(MIN(last, end), MAX(last, end) + 1);
}

// Set the cursor position
static void move_to(int x, int y)
{
	cursor.x = LIMIT(x, 0, pty.cols - 1);
	cursor.y = LIMIT(y, 0, pty.rows - 1);
}

// Scroll the viewport `n` lines down (n < 0: scroll up)
static void scroll(int n)
{
	int min_scroll = MAX(0, term.lines - HIST_SIZE + 2 * pty.rows);
	LIMIT(n, min_scroll - term.scroll, term.lines - term.scroll);
	term.scroll += n;
	sel.mark.y -= n;
	sel.start.y -= n;
	sel.end.y -= n;
}

// Move the cursor to the next line, scrolling if necessary
static void newline()
{
	if (cursor.y != term.bot) {
		move_to(cursor.x, cursor.y + 1);
	} else if (term.top || term.alt) {
		move_lines(term.top, term.bot, 1);
	} else {
		++term.lines;
		scroll(1);
		move_lines(term.bot, pty.rows - 1, -1);
		erase_lines(2 * pty.rows - 1, 2 * pty.rows);
	}
}

// Get the text coordinates corresponding to the given pixel coordinates
static Point pixel2cell(int px, int py)
{
	int x = (px - w.border) / w.font_width;
	int y = (py - w.border) / w.font_height;
	return (Point) { MAX(x, 0), MAX(y, 0) };
}

// Copy the selected text to the primary selection (or the clipboard, if `clipboard` is set)
static void copy(bool clipboard)
{
	// If the selection is empty, leave the clipboard as-is rather than emptying it
	if (!POINT_GT(sel.end, sel.start))
		return;

	FILE* pipe = popen(clipboard ? "xsel -bi" : "xsel -i", "w");

	for (int y = sel.start.y; y <= sel.end.y; y++) {
		Rune *line = LINE(y);
		int xstart = y == sel.start.y ? sel.start.x : 0;
		int xend = y == sel.end.y ? sel.end.x : pty.cols;

		for (int x = xstart; x < xend; ++x)
			fprintf(pipe, "%.4s", line[x].u);
		if (xend == pty.cols && !line[pty.cols - 1].u[0])
			fprintf(pipe, "\n");
	}

	pclose(pipe);
}

// Print the primary selection (or the clipboard, if `clipboard` is set) to the terminal
static void paste(bool clipboard)
{
	FILE* pipe = popen(clipboard ? "xsel -bo" : "xsel -o", "r");

	if (term.bracketed_paste)
		dprintf(pty.fd, "\033[200~");

	char sel_buf[BUFSIZ] = "";
	fread(sel_buf, 1, BUFSIZ, pipe);
	for (char *p = sel_buf; (p = strchr(p, '\n')); *p++ = '\r');
	dprintf(pty.fd, "%s", sel_buf);

	if (term.bracketed_paste)
		dprintf(pty.fd, "\033[201~");

	pclose(pipe);
}

// Set the selection’s point (last position selected)
static void sel_set_point(Point point)
{
	// `point` can be before `mark` (if the user drags the mouse up/left),
	// but `end` should always be after `start`
	bool swapped = POINT_GT(sel.mark, point);
	sel.start = swapped ? point : sel.mark;
	sel.end = swapped ? sel.mark : point;

	if (sel.snap == SNAP_LINE) {
		sel.start.x = 0;
		sel.end.x = pty.cols;
	} else if (sel.snap == SNAP_WORD) {
		while (sel.start.x > 0 && !IS_DELIM(LINE(sel.start.y)[sel.start.x - 1].u))
			--sel.start.x;
		while (!IS_DELIM(LINE(sel.end.y)[sel.end.x].u))
			++sel.end.x;
	}
}

// Keep Valgrind from complaining
static int __attribute__((noreturn)) clean_exit(Display *disp)
{
	for (int i = 0; i < 4; ++i)
		XftFontClose(disp, w.font[i]);
	XCloseDisplay(disp);
	exit(0);
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

// Should `word` be treated as true when used as an X resource?
static bool is_true(const char* word)
{
	return !strcasecmp(word, "true") || !strcasecmp(word, "yes") || !strcasecmp(word, "on");
}

// Get the value of the resource `resource`, defaulting to `fallback`
static const char* get_resource(const char* resource, const char* fallback)
{
	char* result = XGetDefault(w.disp, "vvvvvt", resource);
	return result ? result : fallback;
}

// Read the X resources used for configuration and take action accordingly
static void load_resources() {
	// Fonts
	const char *face_name = get_resource("faceName", "mono");
	const char *style[] = { "", "bold", "italic", "bold italic" };
	char font_name[128];
	for (int i = 0; i < 4; ++i) {
		sprintf(font_name, "%s:style=%s", face_name, style[i]);
		if (w.font[i])
			XftFontClose(w.disp, w.font[i]);
		w.font[i] = XftFontOpenName(w.disp, w.screen, font_name);
	}

	// Colors
	Colormap colormap = DefaultColormap(w.disp, w.screen);
	char color_name[16] = "color";
	char def[16] = "";
	for (u16 i = 0; i < 256; ++i) {
		sprintf(color_name + 5, "%d", i);
		sprintf(def, "#%02x%02x%02x", default_color(i, 2), default_color(i, 1), default_color(i, 0));
		XColor *color = (XColor*) &w.colors[i];
		XAllocNamedColor(w.disp, colormap, get_resource(color_name, def), color, color);
		w.colors[i].color.alpha = 0xffff;
	}
	XSetWindowBackground(w.disp, w.parent, w.colors[4].pixel);

	// Others
	double scale_height = atof(get_resource("scaleHeight", "1"));
	w.font_height = (int) ((w.font[0]->height + 1) * scale_height + .999);
	w.font_width = w.font[0]->max_advance_width;
	w.border = atoi(get_resource("internalBorder", "2"));
	term.meta_sends_escape = is_true(get_resource("metaSendsEscape", ""));
	term.reverse_video = is_true(get_resource("reverseVideo", "on"));

	u32 width = pty.cols * w.font_width;
	u32 height = pty.rows * w.font_height;
	XMoveWindow(w.disp, w.win, w.border, w.border);
	XResizeWindow(w.disp, w.win, width, height);
	XResizeWindow(w.disp, w.parent, width + 2 * w.border, height + 2 * w.border);
}

// Connect to the X server and set up our windows (gritty X11 stuff)
static void x_init(void)
{
	if (!(w.disp = XOpenDisplay(0))) {
		fprintf(stderr, "Failed to open display %s\n", getenv("DISPLAY"));
		exit(1);
	}

	w.screen = XDefaultScreen(w.disp);
	setlocale(LC_CTYPE, ""); // required to parse keypresses correctly
	XSetLocaleModifiers(""); // Xlib leaks memory if we don’t call this

	Window root = XRootWindow(w.disp, DefaultScreen(w.disp));
	XSelectInput(w.disp, root, PropertyChangeMask);

	w.parent = XCreateSimpleWindow(w.disp, root, 0, 0, 640, 480, 0, None, None);
	XDefineCursor(w.disp, w.parent, XCreateFontCursor(w.disp, XC_xterm));
	XSelectInput(w.disp, w.parent, ExposureMask | FocusChangeMask | StructureNotifyMask
			| KeyPressMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask);
	XStoreName(w.disp, w.parent, "vvvvvt");
	XMapWindow(w.disp, w.parent);

	w.win = XCreateSimpleWindow(w.disp, w.parent, 0, 0, 1, 1, 0, None, None);
	XChangeWindowAttributes(w.disp, w.win, CWBitGravity, &(XSetWindowAttributes) { .bit_gravity = NorthWestGravity });
	XMapWindow(w.disp, w.win);
	w.draw = XftDrawCreate(w.disp, w.win, DefaultVisual(w.disp, DefaultScreen(w.disp)), None);

	load_resources();
	XSetIOErrorHandler(clean_exit);
}

static void draw_text(Rune rune, u8 *text, int num_chars, int num_bytes, Point pos)
{
	int x = pos.x * w.font_width;
	int y = pos.y * w.font_height;
	XRectangle r = { 0, 0, (short) (num_chars * w.font_width), (short) w.font_height };
	bool bold = (rune.attr & ATTR_BOLD) != 0;
	bool italic = (rune.attr & (ATTR_ITALIC | ATTR_BLINK)) != 0;
	XftFont *font = w.font[bold + 2 * italic];
	XftColor fg = w.colors[rune.fg || !term.reverse_video ? rune.fg : 15];
	XftColor bg = w.colors[rune.bg ||  term.reverse_video ? rune.bg : 15];
	int baseline = y + font->ascent;

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
	XftDrawSetClipRectangles(w.draw, x, y, &r, 1);
	XftDrawRect(w.draw, &bg, x, y, r.width, r.height);
	XftDrawStringUtf8(w.draw, &fg, font, x, baseline, text, num_bytes);

	if (rune.attr & ATTR_UNDERLINE)
		XftDrawRect(w.draw, &fg, x, baseline + 1, r.width, 1);

	if (rune.attr & ATTR_STRUCK)
		XftDrawRect(w.draw, &fg, x, (2 * baseline + y) / 3, r.width, 1);

	if (rune.attr & ATTR_BAR)
		XftDrawRect(w.draw, &fg, x, y, 2, w.font_height);
}

// Draws the rune at the given terminal coordinates.
static void draw_rune(Point pos)
{
	static u8 buf[4 * LINE_SIZE];
	static int len;
	static Rune prev;
	static Point prev_pos;

	Rune rune = LINE(pos.y)[pos.x];
	Rune *old_rune = &SLINE(pos.y + term.lines + pty.rows)[pos.x];

	// Handle selection and cursor
	if (pos.x != pty.cols && selected(pos.x, pos.y))
		rune.attr ^= ATTR_REVERSE;

	if (!term.hide && term.scroll == term.lines && POINT_EQ(pos, cursor)) {
		rune.attr ^= w.focused && term.cursor_style < 3 ? ATTR_REVERSE :
			term.cursor_style < 5 ? ATTR_UNDERLINE : ATTR_BAR;
	}

	if (w.dirty || memcmp(&rune, old_rune, sizeof(Rune))) {
		*old_rune = rune;
		rune.attr |= ATTR_DIRTY;
	}

	bool diff = rune.fg != prev.fg || rune.bg != prev.bg || rune.attr != prev.attr;

	if ((pos.x == pty.cols || diff) && (prev.attr & ATTR_DIRTY)) {
		draw_text(prev, buf, pos.x - prev_pos.x, len, prev_pos);
	}

	if (pos.x == 0 || diff) {
		len = 0;
		prev = rune;
		prev_pos = pos;
	}

	// Pick an appropriate rendition: NUL becomes space, invalid UTF-8 becomes ⁇
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

// Update the display
static void draw(void)
{
	static int old_lines;
	if (term.lines != old_lines) {
		XCopyArea(w.disp, w.win, w.win, XDefaultGC(w.disp, w.screen),
			0, (term.lines - old_lines) * w.font_height,
			pty.cols * w.font_width, 9999, 0, 0);
		old_lines = term.lines;
	}
	for (Point pos = { 0, 0 }; pos.y < pty.rows; ++pos.y)
		for (pos.x = 0; pos.x <= pty.cols; ++pos.x)
			draw_rune(pos);
	XFlush(w.disp);
	w.dirty = false;
}

// Print the escape sequence for special key `c`, with modifiers `state`
static void special_key(u8 c, int state)
{
	if (state && c < 'A')
		dprintf(pty.fd, "\033[%d;%d~", c, state + 1);
	else if (state)
		dprintf(pty.fd, "\033[1;%d%c", state + 1, c);
	else if (c < 'A')
		dprintf(pty.fd, "\033[%d~", c);
	else
		dprintf(pty.fd, "\033%c%c", term.app_keys ? 'O' : '[', c);
}

// Handle keyboard shortcuts, or print the pressed key to the pty
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

	if (selected(cursor.x, cursor.y))
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

// Recompute the number of text rows/columns
static void on_resize(XConfigureEvent *e)
{
	Point old_size = { pty.cols, pty.rows };
	Point new_size = pixel2cell(e->width - w.border, e->height - w.border);

	if (POINT_EQ(old_size, new_size))
		return;

	// Update terminal info
	pty.cols = LIMIT(new_size.x, 1, LINE_SIZE - 1);
	pty.rows = LIMIT(new_size.y, 1, HIST_SIZE / 2);
	term.top = 0;
	term.bot = pty.rows - 1;
	move_to(cursor.x, cursor.y);

	XResizeWindow(w.disp, w.win, pty.cols * w.font_width, pty.rows * w.font_height);

	// Send our size to the pty driver so that applications can query it
	struct winsize size = { (u16) pty.rows, (u16) pty.cols, 0, 0 };
	if (ioctl(pty.fd, TIOCSWINSZ, &size) < 0)
		perror("Couldn't set window size");
}

// Load the new X resources if they changed
static void on_property_change(XPropertyEvent *e)
{
	static XrmDatabase xrm;

	if (e->atom != XInternAtom(w.disp, "RESOURCE_MANAGER", false))
		return;

	union { Atom atom; int i; unsigned long ul; } ignored;
	unsigned char *xprop;
	XGetWindowProperty(w.disp, e->window, e->atom, 0, 65536, 0, AnyPropertyType,
			&ignored.atom, &ignored.i, &ignored.ul, &ignored.ul, &xprop);

	if (xrm)
		XrmDestroyDatabase(XrmGetDatabase(w.disp));
	xrm = XrmGetStringDatabase((char*) xprop);
	XrmSetDatabase(w.disp, xrm);
	XFree(xprop);

	load_resources();
	w.dirty = true;
}

// Handle selection, middle-click paste, and scrolling with the wheel
static void on_mouse(XButtonEvent *e)
{
	static Point prev;
	int button = e->type == ButtonRelease ? 4 : e->button + (e->button >= Button4 ? 61 : 0);
	Point pos = pixel2cell(e->x, e->y);

	if ((e->state & Mod4Mask) || (!button && POINT_EQ(pos, prev)))
		return;
	prev = pos;

	if (term.report_buttons && !(e->state & ShiftMask)) {
		if ((button || term.report_motion) && pos.x <= 222 && pos.y <= 222)
			dprintf(pty.fd, "\033[M%c%c%c", 31 + button, 33 + pos.x, 33 + pos.y);
		return;
	}

	switch (button) {
	case 0:  // Cursor movement
		if (e->state & (Button1Mask | Button3Mask))
			sel_set_point(pos);
		break;
	case 1:  // Left click
		sel.snap = POINT_EQ(pos, sel.mark) * sel.snap + 1 & 3;
		sel.mark = pos;
		sel_set_point(pos);
		break;
	case 2:  // Middle click
		paste(false);
		break;
	case 3:  // Right click
		sel.snap = SNAP_LINE;
		sel_set_point(pos);
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

// Delegate to the appropriate event handler, depending on the event’s type
static void dispatch_event(XEvent * e)
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
	case PropertyNotify:
		on_property_change((XPropertyEvent*) e);
		break;
	case Expose:
		w.dirty = true;
		break;
	case FocusIn:
	case FocusOut:
		w.focused = e->type == FocusIn;
		if (term.report_focus)
			dprintf(pty.fd, "\033[%c", w.focused ? 'I' : 'O');
		break;
	}
}

// Fork and initialize the pty
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

// Read one character from the pty, blocking if necessary
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

static void reset()
{
	memset(&cursor.rune, 0, sizeof(cursor.rune));
	memset(&saved_cursors, 0, sizeof(saved_cursors));
	term.top = 0;
	term.bot = pty.rows - 1;
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
	case 30:
		*color = 232;
		return 1;
	case 31 ... 37:
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
	case 1:    // DECCKM — Application cursor keys
		term.app_keys = set;
		break;
	case 5:    // DECSCNM — Reverse video
		term.reverse_video = set;
		w.dirty = true;
		break;
	case 25:   // DECTCEM — Show cursor
		term.hide = !set;
		break;
	case 47:
	case 1049: // Alternate screen buffer
		clear_selection();
		term.lines += (set - term.alt) * pty.rows;
		term.scroll = term.lines;
		if (set)
			erase_lines(0, pty.rows);
		else
			cursor = saved_cursors[0];
		saved_cursors[term.alt] = cursor;
		term.alt = set;
		break;
	case 1000: // Report mouse buttons
	case 1003: // Report mouse motion
		term.report_buttons = set;
		term.report_motion = mode == 1003 && set;
		break;
	case 1004: // Report focus events
		term.report_focus = set;
		break;
	case 1036: // Send ESC when Meta modifies a key
		term.meta_sends_escape = set;
		break;
	case 2004: // Send special sequences before/after each paste
		term.bracketed_paste = set;
		break;
	}
}

static void handle_csi()
{
	int arg[16] = { 0 };
	u32 nargs = 0;
	char command = 0;

next_csi_byte:
	switch (command = pty_getchar()) {
	case '0' ... '9':
		if (arg[nargs] < 10000)
			arg[nargs] = 10 * arg[nargs] + command - '0';
		goto next_csi_byte;
	case ';':
		nargs = MIN(nargs + 1, LEN(arg) - 1);
		goto next_csi_byte;
	case ' ' ... '/': // Intermediate bytes
	case '<' ... '?': // Private parameter bytes
		goto next_csi_byte;
	case 'A': // CUU — Cursor <n> up
		move_to(cursor.x, cursor.y - MAX(*arg, 1));
		break;
	case 'B': // CUD — Cursor <n> down
	case 'e': // VPR — Cursor <n> down
		move_to(cursor.x, cursor.y + MAX(*arg, 1));
		break;
	case 'C': // CUF — Cursor <n> forward
	case 'a': // HPR — Cursor <n> forward
		move_to(cursor.x + MAX(*arg, 1), cursor.y);
		break;
	case 'D': // CUB — Cursor <n> backward
		LIMIT(cursor.x, 0, pty.cols - 1);
		move_to(cursor.x - MAX(*arg, 1), cursor.y);
		break;
	case 'E': // CNL — Cursor <n> down and first col
		move_to(0, cursor.y + MAX(*arg, 1));
		break;
	case 'F': // CPL — Cursor <n> up and first col
		move_to(0, cursor.y - MAX(*arg, 1));
		break;
	case 'G': // CHA — Move to <col>
	case '`': // HPA — Move to <col>
		move_to(*arg - 1, cursor.y);
		break;
	case 'H': // CUP — Move to <row> <col>
	case 'f': // HVP — Move to <row> <col>
		move_to(arg[1] - 1, arg[0] - 1);
		break;
	case 'I': // CHT — Cursor forward <n> tabulation stops
		move_to(((cursor.x >> 3) + MAX(*arg, 1)) << 3, cursor.y);
		break;
	case 'J': // ED — Erase display
		erase_lines(*arg ? 0 : cursor.y + 1, *arg == 1 ? cursor.y : pty.rows);
		// FALLTHROUGH
	case 'K': // EL — Erase line
		erase_chars(*arg ? 0 : cursor.x, *arg == 1 ? cursor.x + 1 : pty.cols);
		break;
	case 'L': // IL — Insert <n> blank lines
	case 'M': // DL — Delete <n> lines
		if (BETWEEN(cursor.y, term.top, term.bot)) {
			LIMIT(*arg, 1, term.bot - cursor.y + 1);
			move_lines(cursor.y, term.bot, command == 'L' ? -*arg : *arg);
			cursor.x = 0;
		}
		break;
	case '@': // ICH — Insert <n> blank chars
	case 'P': // DCH — Delete <n> chars
		LIMIT(*arg, 1, pty.cols - cursor.x);
		LIMIT(cursor.x, 0, pty.cols - 1);
		move_chars(cursor.x, pty.cols, command == '@' ? -*arg : *arg);
		break;
	case 'S': // SU — Scroll <n> lines up
	case 'T': // SD — Scroll <n> lines down
		LIMIT(*arg, 1, term.bot - term.top + 1);
		move_lines(term.top, term.bot, command == 'T' ? -*arg : *arg);
		break;
	case 'X': // ECH — Erase <n> chars
		LIMIT(*arg, 1, pty.cols - cursor.x);
		erase_chars(cursor.x, cursor.x + *arg);
		break;
	case 'Z': // CBT — Cursor backward <n> tabulation stops
		move_to(((cursor.x >> 3) - MAX(*arg, 1)) << 3, cursor.y);
		break;
	case 'c': // DA — Device Attributes
		if (*arg == 0)
			dprintf(pty.fd, "%s", "\033[?64;15;22c");
		break;
	case 'd': // VPA — Move to <row>
		move_to(cursor.x, *arg - 1);
		break;
	case 'h': // SM — Set Mode
	case 'l': // RM — Reset Mode
		for (u32 i = 0; i <= nargs; ++i)
			set_mode(command == 'h', arg[i]);
		break;
	case 'm': // SGR — Select Graphic Rendition
		for (u32 i = 0; i <= nargs; i += set_attr(arg + i));
		break;
	case 'n': // DSR – Device Status Report (cursor position)
		if (*arg == 6)
			dprintf(pty.fd, "\033[%i;%iR", cursor.y + 1, cursor.x + 1);
		break;
	case 'p': // DECSTR — Soft terminal reset
		reset();
		break;
	case 'q': // DECSCUSR — Set Cursor Style
		if (*arg <= 6)
			term.cursor_style = *arg;
		break;
	case 'r': // DECSTBM — Set Scrolling Region
		arg[0] = arg[0] ? arg[0] : 1;
		arg[1] = arg[1] && arg[1] < pty.rows ? arg[1] : pty.rows;
		if (arg[0] < arg[1]) {
			term.top = arg[0] - 1;
			term.bot = arg[1] - 1;
			move_to(0, 0);
		}
		break;
	case 's': // DECSC — Save cursor position
		saved_cursors[term.alt] = cursor;
		break;
	case 'u': // DECRC — Restore cursor position
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
		term.charsets[second_byte - '('] = final_byte;
		break;
	case '7': // DECSC — Save Cursor
		saved_cursors[term.alt] = cursor;
		break;
	case '8': // DECRC — Restore Cursor
		cursor = saved_cursors[term.alt];
		break;
	case 'E': // NEL — Next line
		newline();
		cursor.x = 0;
		break;
	case 'M': // RI — Reverse index
		if (cursor.y <= term.top)
			move_lines(term.top, term.bot, -1);
		else
			--cursor.y;
		break;
	case '[': // CSI — Control Sequence Introducer
		handle_csi();
		break;
	case ']': // OSC — Operating System Command
		for (u8 c = 0; c != '\a' && (c != '\033' || pty_getchar() != '\\'); c = pty_getchar());
		break;
	case 'c': // RIS — Reset to inital state
		memset(&term, 0, sizeof(term));
		move_to(0, 0);
		reset();
		break;
	case 'n':
	case 'o':
		term.charset = second_byte - 'n' + 2;
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
	case '\016': // LS1 — Locking shift 1
	case '\017': // LS0 — Locking shift 0
		term.charset = u == '\016';
		return;
	case '\033': // ESC
		handle_esc(pty_getchar());
		return;
	case ' ' ... '~':
	case 128 ... 255:
		if (cursor.x == pty.cols) {
			newline();
			cursor.x = 0;
		}

		cursor.rune.u[0] = u;
		Rune *rune = &LINE(cursor.y)[cursor.x];
		*rune = cursor.rune;
		++cursor.x;

		for (long i = 1, len = UTF_LEN(u); i < len; ++i) {
			u = pty_getchar();
			if (!BETWEEN(u, 128, 191))
				goto invalid_utf8;
			rune->u[i & 3] = u;
		}

		if (term.charsets[term.charset] == '0' && BETWEEN(u, 'j', 'x'))
			memcpy(rune, &"┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│"[(u - 'j') * 3], 3);
	}
}

static void run(fd_set read_fds)
{
	static struct timeval timeout;

	if (select(pty.fd + 1, &read_fds, 0, 0, &timeout) < 0)
		die("select failed");

	if (FD_ISSET(pty.fd, &read_fds)) {
		scroll(term.lines - term.scroll);
		pty_putchar(pty_getchar());
		while (pty.c < pty.end)
			pty_putchar(*pty.c++);
	}

	XEvent e;
	while (XPending(w.disp)) {
		XNextEvent(w.disp, &e);
		dispatch_event(&e);
	}

	timeout.tv_usec = MIN(timeout.tv_usec - 60, 16680);
	if (timeout.tv_usec <= 0) {
		draw();
		timeout.tv_usec = 999999;
	}
}

int main(int argc, char *argv[])
{
	pty.rows = 24;
	pty.cols = 80;
	x_init();
	pty_new(argc > 1 ? argv + 1 : (char*[]) { getenv("SHELL"), NULL });

	fd_set read_fds;
	FD_ZERO(&read_fds);
	FD_SET(XConnectionNumber(w.disp), &read_fds);
	FD_SET(pty.fd, &read_fds);

	for (;;)
		run(read_fds);
}
