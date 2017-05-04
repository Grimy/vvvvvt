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
#define LINE(y)             (term.hist[((y) + term.scroll) % HIST_SIZE])

#define zeromem(x)          (memset(&(x), 0, sizeof(x)))

typedef enum { false, true } bool;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// Selection snapping modes
enum { SNAP_WORD = 2, SNAP_LINE = 3 };

enum {
	ATTR_BOLD       = 1 << 1,
	ATTR_FAINT      = 1 << 2,
	ATTR_ITALIC     = 1 << 3,
	ATTR_UNDERLINE  = 1 << 4,
	ATTR_BLINK      = 1 << 5,  // rendered as italic
	ATTR_BLINK_FAST = 1 << 6,  // not implemented
	ATTR_REVERSE    = 1 << 7,
	ATTR_INVISIBLE  = 1 << 8,
	ATTR_STRUCK     = 1 << 9,
	ATTR_BAR        = 1 << 10,
	ATTR_DIRTY      = 1 << 11,
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

// State affected by Save Cursor / Restore Cursor
static struct {
	Rune rune; // current char attributes
	int x, y;  // cursor position
} cursor, saved_cursors[2];

static struct {
	u64 snap;    // snapping mode
	Point mark;  // coordinates of the point clicked to start the selection
	Point start; // coordinates of the beginning of the selection (inclusive)
	Point end;   // coordinates of the end of the selection (exclusive)
	u64 hash;    // hash of the contents of the selection
} sel;

static struct {
	char buf[BUFSIZ]; // input buffer
	char *c;          // current reading position (points inside `buf`)
	char *end;        // one past the last valid char (points inside `buf`)
	int fd;           // file descriptor of the master pty
	int rows, cols;   // size of the pty (in characters)
	int: 32;
} pty;

static struct {
	Rune hist[HIST_SIZE][LINE_SIZE]; // history ring buffer
	int scroll;                      // scroll position (index inside `hist`)
	int lines;                       // last line printed (index inside `hist`)
	int top;                         // top scroll limit
	int bot;                         // bottom scroll limit
	int cursor_style;                // appearance of the cursor
	u8 charsets[4];                  // designated character sets (see ISO/IEC 2022)
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

// Number of bytes in an UTF-8 sequence starting with byte `c`
static u32 utf_len(u8 c)
{
	static u32 lookup[16] = { 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 0, 0 };
	return c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : lookup[c & 0x0F];
}

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
	for (int y = start; y < end; y++)
		zeromem(LINE(y));
}

// Erase characters between columns `start` and `end` in the current line
static void erase_chars(int start, int end)
{
	Rune *line = LINE(cursor.y);
	// TODO memset(line + start, cursor.rune.bg, (end - start) * sizeof(Rune));
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
	}
}

// Get the text coordinates corresponding to the given pixel coordinates
static Point pixel2cell(int px, int py)
{
	int x = (px - w.border) / w.font_width;
	int y = (py - w.border) / w.font_height;
	return (Point) { MAX(x, 0), MAX(y, 0) };
}

// “Increment” the point `p`, wrapping at lines’ end
static void next_point(Point *p)
{
	++p->x;
	if (p->x == pty.cols)
		*p = (Point) { 0, p->y + 1 };
}

// Copy the selected text to the primary selection (or the clipboard, if `clipboard` is set)
static void copy(bool clipboard)
{
	// If the selection is empty, leave the clipboard as-is rather than emptying it
	if (!POINT_GT(sel.end, sel.start))
		return;

	FILE* pipe = popen(clipboard ? "xsel -bi" : "xsel -i", "w");
	bool empty = false;

	for (Point p = sel.start; !POINT_EQ(p, sel.end); next_point(&p)) {
		u8 *text = LINE(p.y)[p.x].u;
		if (empty && *text)
			fputc(p.x ? ' ' : '\n', pipe);
		empty = !fprintf(pipe, "%.4s", text);
	}

	pclose(pipe);
}

// Print the primary selection (or the clipboard, if `clipboard` is set) to the terminal
static void paste(bool clipboard)
{
	if (term.bracketed_paste)
		printf("\033[200~");

	system(clipboard ? "xsel -bo | tr '\n' '\r'" : "xsel -o | tr '\n' '\r'");

	if (term.bracketed_paste)
		printf("\033[201~");
}

// Compute the djb2 hash of the contents of the selection
static u64 sel_get_hash()
{
	u64 hash = 5381;

	for (Point p = sel.start; !POINT_EQ(p, sel.end); next_point(&p))
		hash = (hash << 5) + hash + LINE(p.y)[p.x].u[0];

	return hash;
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
		while (sel.start.y > 0 && LINE(sel.start.y - 1)[pty.cols - 1].u[0])
			--sel.start.y;
		while (LINE(sel.end.y)[pty.cols - 1].u[0])
			++sel.end.y;
	} else if (sel.snap == SNAP_WORD) {
		while (sel.start.x > 0 && !IS_DELIM(LINE(sel.start.y)[sel.start.x - 1].u))
			--sel.start.x;
		while (!IS_DELIM(LINE(sel.end.y)[sel.end.x].u))
			++sel.end.x;
	}

	sel.hash = sel_get_hash();
}

// Keep Valgrind from complaining
static int __attribute__((noreturn)) clean_exit(Display *disp)
{
	for (int i = 0; i < 4; ++i)
		XftFontClose(disp, w.font[i]);
	XCloseDisplay(disp);
	exit(0);
}

// Exit after a failed syscall
static void __attribute__((noreturn)) die(const char* message)
{
	perror(message);
	clean_exit(w.disp);
}

// Recompute the number of text rows/columns from the given pixel dimensions
static void fix_pty_size(int width, int height)
{
	Point old_size = { pty.cols, pty.rows };
	Point new_size = pixel2cell(width - w.border, height - w.border);
	if (POINT_EQ(old_size, new_size))
		return;

	pty.cols = LIMIT(new_size.x, 1, LINE_SIZE - 1);
	pty.rows = LIMIT(new_size.y, 1, HIST_SIZE / 2);
	term.top = 0;
	term.bot = pty.rows - 1;
	move_to(cursor.x, cursor.y);

	// Send our size to the pty driver so that applications can query it
	struct winsize size = { (u16) pty.rows, (u16) pty.cols, 0, 0 };
	if (ioctl(pty.fd, TIOCSWINSZ, &size) < 0)
		perror("Couldn't set pty size");

	// Resize the inner window to align it with the character grid
	XResizeWindow(w.disp, w.win, pty.cols * w.font_width, pty.rows * w.font_height);
}

// Default value of component `rgb` (0=red, 1=green, 2=blue) of color `i`
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
static void load_resources()
{
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

	double scale_height = atof(get_resource("scaleHeight", "1"));
	w.font_height = (int) ((w.font[0]->height + 1) * scale_height + .999);
	w.font_width = w.font[0]->max_advance_width;

	// Colors
	Colormap colormap = DefaultColormap(w.disp, w.screen);
	char resource_name[16] = "color";
	char def[16] = "";
	XColor color;

	for (u16 i = 0; i < 256; ++i) {
		sprintf(resource_name + 5, "%d", i);
		sprintf(def, "#%02x%02x%02x", default_color(i, 2), default_color(i, 1), default_color(i, 0));
		XLookupColor(w.disp, colormap, get_resource(resource_name, def), &color, &color);
		w.colors[i].color = (XRenderColor) { color.red, color.green, color.blue, 0xffff };
	}

	XAllocNamedColor(w.disp, colormap, get_resource("borderColor", "#000"), &color, &color);
	XSetWindowBackground(w.disp, w.parent, color.pixel);
	XClearWindow(w.disp, w.parent);

	// Others
	w.border = atoi(get_resource("internalBorder", "2"));
	XMoveWindow(w.disp, w.win, w.border, w.border);
	term.meta_sends_escape = is_true(get_resource("metaSendsEscape", ""));
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

	w.parent = XCreateSimpleWindow(w.disp, root, 0, 0, 1, 1, 0, None, None);
	XDefineCursor(w.disp, w.parent, XCreateFontCursor(w.disp, XC_xterm));
	XSelectInput(w.disp, w.parent, ExposureMask | FocusChangeMask | StructureNotifyMask
			| KeyPressMask | PointerMotionMask | ButtonPressMask | ButtonReleaseMask);
	XStoreName(w.disp, w.parent, "vvvvvt");

	w.win = XCreateSimpleWindow(w.disp, w.parent, 0, 0, 1, 1, 0, None, None);
	XChangeWindowAttributes(w.disp, w.win, CWBitGravity, &(XSetWindowAttributes) { .bit_gravity = NorthWestGravity });
	w.draw = XftDrawCreate(w.disp, w.win, DefaultVisual(w.disp, DefaultScreen(w.disp)), None);

	load_resources();
	XResizeWindow(w.disp, w.parent, 80 * w.font_width + 2 * w.border, 24 * w.font_height + 2 * w.border);
	XSetIOErrorHandler(clean_exit);

	XMapWindow(w.disp, w.parent);
	XMapWindow(w.disp, w.win);
}

// Draw the given text on screen
static void draw_text(Rune rune, u8 *text, int num_chars, int num_bytes, Point pos)
{
	int x = pos.x * w.font_width;
	int y = pos.y * w.font_height;
	XRectangle r = { 0, 0, (short) (num_chars * w.font_width), (short) w.font_height };
	bool bold = (rune.attr & ATTR_BOLD) != 0;
	bool italic = (rune.attr & (ATTR_ITALIC | ATTR_BLINK)) != 0;
	XftFont *font = w.font[bold + 2 * italic];
	XftColor fg = w.colors[rune.fg];
	XftColor bg = w.colors[rune.bg];
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

// Check the cell at position `pos`, redraw it if necessary
static void draw_rune(Point pos, Rune *cached_rune)
{
	static u8 buf[4 * LINE_SIZE];
	static int len;
	static Rune prev;
	static Point prev_pos;

	Rune rune = LINE(pos.y)[pos.x];

	// Default colors
	u8 *defaulted = term.reverse_video ? &rune.bg : &rune.fg;
	if (*defaulted == 0)
		*defaulted = 15;

	// Add special attributes to render the selection and cursor
	if (pos.x != pty.cols && selected(pos.x, pos.y))
		rune.attr ^= ATTR_REVERSE;

	if (!term.hide && term.scroll == term.lines && POINT_EQ(pos, cursor)) {
		rune.attr ^= w.focused && term.cursor_style < 3 ? ATTR_REVERSE :
			term.cursor_style < 5 ? ATTR_UNDERLINE : ATTR_BAR;
	}

	// Mark the cell as dirty if it changed since last time
	if (w.dirty || memcmp(&rune, cached_rune, sizeof(Rune))) {
		*cached_rune = rune;
		rune.attr |= ATTR_DIRTY;
	}

	// For performance, we batch together stretches of runes with the same colors and attrs
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
	} else if (*rune.u >= 0xC0 && strnlen((char*) rune.u, 4) == utf_len(*rune.u)) {
		memcpy(buf + len, rune.u, utf_len(*rune.u));
		len += utf_len(*rune.u);
	} else {
		memcpy(buf + len, "⁇", 3);
		len += 3;
	}
}

// Update the display
static void draw(void)
{
	static int old_scroll;

	if (term.scroll != old_scroll) {
		int src  = MAX(term.scroll - old_scroll, 0);
		int dest = MAX(old_scroll - term.scroll, 0);
		int size = pty.rows - src - dest;

		XCopyArea(w.disp, w.win, w.win, XDefaultGC(w.disp, w.screen),
			0, w.font_height * src,
			w.font_width * pty.cols, w.font_height * size,
			0, w.font_height * dest);
	}

	// Clear the selection if something wrote over it
	if (sel_get_hash() != sel.hash)
		sel.end = sel.start;

	for (int y = 0; y < pty.rows; ++y) {
		Rune *cache_line = LINE(y + pty.rows * (1 + (pty.rows - y + term.lines - term.scroll) / pty.rows));

		// The three hardest things in CS are off-by-one errors and cache invalidation
		if (!BETWEEN(y + term.scroll, old_scroll, old_scroll + pty.rows - 1))
			memset(cache_line, 0, sizeof(Rune[LINE_SIZE]));

		for (int x = 0; x <= pty.cols; ++x)
			draw_rune((Point) { x, y }, &cache_line[x]);
	}

	XFlush(w.disp);
	w.dirty = false;
	old_scroll = term.scroll;
}

// Print the escape sequence for special key `c`, with modifiers `state`
static void special_key(u8 c, int state)
{
	if (state && c < 'A')
		printf("\033[%d;%d~", c, state + 1);
	else if (state)
		printf("\033[1;%d%c", state + 1, c);
	else if (c < 'A')
		printf("\033[%d~", c);
	else
		printf("\033%c%c", term.app_keys ? 'O' : '[', c);
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

	if (meta && term.meta_sends_escape && len)
		printf("%c", 033);

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
		printf("%s", "\033[Z");
	else if (ctrl && keysym == XK_question)
		printf("%c", 127);
	else if (keysym == XK_BackSpace)
		printf("%c", ctrl ? 027 : 0177);
	else if (BETWEEN(keysym, 0xff50, 0xffff) && codes[keysym - 0xff50])
		special_key(codes[keysym - 0xff50], 4 * ctrl + 2 * meta + shift);
	else if (len)
		write(1, buf, len);
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

	XWindowAttributes attrs;
	XGetWindowAttributes(w.disp, w.parent, &attrs);
	fix_pty_size(attrs.width, attrs.height);
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
			printf("\033[M%c%c%c", 31 + button, 33 + pos.x, 33 + pos.y);
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
		fix_pty_size(((XConfigureEvent*) e)->width, ((XConfigureEvent*) e)->height);
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
			printf("\033[%c", w.focused ? 'I' : 'O');
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
		dup2(pty.fd, 1);
		setbuf(stdout, NULL);
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

// Set the graphical attributes of future text based on the parameter `**p`
static void set_attr(int **p)
{
	int attr = *(*p)++;
	u8 *color = &cursor.rune.fg;
	if (BETWEEN(attr, 40, 49) || BETWEEN(attr, 100, 107)) {
		color = &cursor.rune.bg;
		attr -= 10;
	}

	switch (attr) {
	case 0:
		zeromem(cursor.rune);
		break;
	case 1 ... 9:
		cursor.rune.attr |= 1 << attr;
		break;
	case 21:
		cursor.rune.attr |= ATTR_UNDERLINE;
		break;
	case 22:
		cursor.rune.attr &= ~(ATTR_BOLD | ATTR_FAINT);
		break;
	case 23 ... 29:
		cursor.rune.attr &= ~(1 << (attr - 20));
		break;
	case 30:
		*color = 232;
		break;
	case 31 ... 37:
		*color = (u8) (attr - 30);
		break;
	case 38:
		attr = *(*p)++;
		if (attr == 2) {
			int r = (*(*p)++ - 35) / 40;
			int g = (*(*p)++ - 35) / 40;
			int b = (*(*p)++ - 35) / 40;
			*color = (u8) (16 + 36 * r + 6 * g + b);
		} else if (attr == 5) {
			*color = (u8) *(*p)++;
		}
		break;
	case 39:
		*color = 0;
		break;
	case 90 ... 97:
		*color = (u8) (attr - 90 + 8);
		break;
	}
}

// Set or reset the terminal mode identified by `mode`
static void set_mode(bool set, int mode)
{
	switch (mode) {
	case 1:    // DECCKM — Application cursor keys
		term.app_keys = set;
		break;
	case 5:    // DECSCNM — Reverse video
		term.reverse_video = set;
		break;
	case 25:   // DECTCEM — Show cursor
		term.hide = !set;
		break;
	case 47:
	case 1049: // Alternate screen buffer
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

// Parse and interpret a control sequence started by ESC [
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
	case ':':
	case ';':
		nargs = MIN(nargs + 1, LEN(arg) - 3);
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
	case '@': // ICH — Insert <n> spaces
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
			printf("%s", "\033[?64;15;22c");
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
		for (int *p = arg; p <= arg + nargs; set_attr(&p));
		break;
	case 'n': // DSR – Device Status Report (cursor position)
		if (*arg == 6)
			printf("\033[%i;%iR", cursor.y + 1, cursor.x + 1);
		break;
	case 'p': // DECSTR — Soft Terminal Reset
		zeromem(cursor.rune);
		zeromem(saved_cursors[term.alt]);
		term.top = 0;
		term.bot = pty.rows - 1;
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
	case 's': // DECSC — Save Cursor
		saved_cursors[term.alt] = cursor;
		break;
	case 'u': // DECRC — Restore Cursor
		cursor = saved_cursors[term.alt];
		break;
	}
}

// Interpret an escape sequence started by an ESC byte
static void handle_esc()
{
	u8 second_byte = pty_getchar(), final_byte = second_byte;
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
	case 'P': // DCS — Device Control String
	case 'X': // SOS — Start of String
	case ']': // OSC — Operating System Command
	case '^': // PM — Privacy Message
	case '_': // APC — Application Program Command
		while (final_byte != '\033' && final_byte != '\a')
			final_byte = pty_getchar();
		if (final_byte == '\033')
			handle_esc();
		break;
	case '[': // CSI — Control Sequence Introducer
		handle_csi();
		break;
	case 'c': // RIS — Reset to inital state
		zeromem(term);
		zeromem(cursor);
		zeromem(saved_cursors);
		term.bot = pty.rows - 1;
		break;
	case 'n': // Invoke the G2 character set
	case 'o': // Invoke the G3 character set
		term.charset = second_byte - 'n' + 2;
		break;
	}
}

// Handle input from the pty: interpret control characters, parse and save utf-8
static void handle_input(u8 u)
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
		handle_esc();
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

		for (long i = 1, len = utf_len(u); i < len; ++i) {
			u = pty_getchar();
			if (!BETWEEN(u, 128, 191))
				goto invalid_utf8;
			rune->u[i & 3] = u;
		}

		if (term.charsets[term.charset] == '0' && BETWEEN(u, 'j', 'x'))
			memcpy(rune, &"┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│"[(u - 'j') * 3], 3);
	}
}

// Main loop: listen for X events and pty input, and periodically redraw the screen
static void run(fd_set read_fds)
{
	static struct timeval timeout;

	if (select(pty.fd + 1, &read_fds, 0, 0, &timeout) < 0)
		die("select failed");

	if (FD_ISSET(pty.fd, &read_fds)) {
		scroll(term.lines - term.scroll);
		handle_input(pty_getchar());
		while (pty.c < pty.end)
			handle_input(*pty.c++);
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

// Parse arguents, initialize everything, call the main loop
int main(int argc, char *argv[])
{
	x_init();
	pty_new(argc > 1 ? argv + 1 : (char*[]) { getenv("SHELL"), NULL });

	fd_set read_fds;
	FD_ZERO(&read_fds);
	FD_SET(XConnectionNumber(w.disp), &read_fds);
	FD_SET(pty.fd, &read_fds);

	for (;;)
		run(read_fds);
}
