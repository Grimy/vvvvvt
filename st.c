/* See LICENSE for license details. */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pty.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/XKBlib.h>
#include <fontconfig/fontconfig.h>

#include "arg.h"

char *argv0;

#define Glyph Glyph_

/* Arbitrary sizes */
#define UTF_INVALID   0xFFFD
#define UTF_SIZ       4
#define ESC_BUF_SIZ   (128 * UTF_SIZ)
#define ESC_ARG_SIZ   16
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1 << 13)

/* macros */
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define LEN(a)			(sizeof(a) / sizeof(a)[0])
#define DEFAULT(a, b)		(a) = (a) ? (a) : (b)
#define BETWEEN(x, a, b)	((a) <= (x) && (x) <= (b))
#define ISCONTROL(c)		((c) < 0x20 || BETWEEN((c), 0x7f, 0x9f))
#define ISDELIM(u)		(strchr(worddelimiters, u) != NULL)
#define LIMIT(x, a, b)		((x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x))
#define ATTRCMP(a, b)		((a).mode != (b).mode || (a).fg != (b).fg || (a).bg != (b).bg)
#define IS_SET(flag)		((term.mode & (flag)) != 0)
#define TIMEDIFF(t1, t2)	((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_nsec - t2.tv_nsec) / 1000000)
#define MODBIT(x, set, bit)	((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))
#define SWAP(a, b)		do { __typeof(a) _swap = (a); (a) = (b); (b) = _swap; } while (0)
#define TLINE(y)		(term.hist[((y) + term.scroll) % histsize])

enum glyph_attribute {
	ATTR_BOLD       = 1 << 0,
	ATTR_FAINT      = 1 << 1,
	ATTR_ITALIC     = 1 << 2,
	ATTR_UNDERLINE  = 1 << 3,
	ATTR_REVERSE    = 1 << 4,
	ATTR_INVISIBLE  = 1 << 5,
	ATTR_STRUCK     = 1 << 6,
	ATTR_WRAP       = 1 << 7,
	ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
};

enum cursor_movement {
	CURSOR_SAVE,
	CURSOR_LOAD
};

enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_ALTSCREEN   = 1 << 1,
	MODE_MOUSEBTN    = 1 << 2,
	MODE_MOUSEMOTION = 1 << 3,
	MODE_HIDE        = 1 << 4,
	MODE_APPCURSOR   = 1 << 5,
	MODE_MOUSESGR    = 1 << 6,
	MODE_FOCUS       = 1 << 7,
	MODE_MOUSEX10    = 1 << 8,
	MODE_MOUSEMANY   = 1 << 9,
	MODE_BRCKTPASTE  = 1 << 10,
	MODE_ABSMOVE     = 1 << 11,
	MODE_ALTCHARSET  = 1 << 12,
	MODE_MOUSE       = MODE_MOUSEBTN|MODE_MOUSEMOTION|MODE_MOUSEX10\
	                  |MODE_MOUSEMANY,
};

enum escape_state {
	ESC_NONE,
	ESC_START,
	ESC_CSI,
	ESC_STR,   /* DCS, OSC, PM, APC */
};

enum window_state {
	WIN_VISIBLE = 1,
	WIN_FOCUSED = 2
};

enum selection_mode {
	SEL_IDLE = 0,
	SEL_EMPTY = 1,
	SEL_READY = 2
};

enum selection_type {
	SEL_REGULAR = 1,
	SEL_RECTANGULAR = 2
};

enum selection_snap {
	SNAP_WORD = 1,
	SNAP_LINE = 2
};

typedef enum { false, true } bool;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef uint32_t Rune;

typedef XftDraw *Draw;
typedef XftColor Color;

typedef struct {
	Rune u;         /* character code */
	uint16_t mode;  /* attribute flags */
	uint8_t fg;     /* foreground  */
	uint8_t bg;     /* background  */
} Glyph;

typedef struct {
	Glyph attr; /* current char attributes */
	int x;
	int y;
	int state;
} TCursor;

/* ESC '[' [[ [<priv>] <arg> [;]] <mode> [<mode>]] */
static struct {
	char buf[ESC_BUF_SIZ]; /* raw string */
	int len;               /* raw string length */
} csiescseq;

/* Purely graphic info */
static struct {
	Display *dpy;
	Colormap cmap;
	Window win;
	Drawable buf;
	Atom xembed, wmdeletewin, netwmname, netwmpid;
	Draw draw;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	int l, t;   /* left and top offset */
	int gm;     /* geometry mask */
	int w, h;   /* window width and height */
	int ch, cw; /* character width and height */
	int state;  /* focus, redraw, visible */
	int cursor; /* cursor style */
} xw;

typedef struct {
	KeySym k;
	u32 mask;
	int appcursor; /* application cursor */
	char *s;
} Key;

static struct {
	int mode;
	int type;
	int snap;
	int alt;
	/*
	 * Selection variables:
	 * nb – normalized coordinates of the beginning of the selection
	 * ne – normalized coordinates of the end of the selection
	 * ob – original coordinates of the beginning of the selection
	 * oe – original coordinates of the end of the selection
	 */
	struct {
		int x, y;
	} nb, ne, ob, oe;

	char *primary, *clipboard;
	Atom xtarget;
} sel;

typedef struct {
	u32 mod;
	u32: 32;
	KeySym keysym;
	void (*func)();
} Shortcut;

/* function definitions used in config.h */
static void clipcopy(void);
static void clippaste(void);
static void selpaste(void);
static void scrolldown(void);
static void scrollup(void);

/* Config.h for applying patches and the configuration. */
#include "config.h"

/* Internal representation of the screen */
static struct {
	int row;                             /* row count */
	int col;                             /* column count */
	Glyph hist[histsize][LINE_SIZE];     /* history buffer */
	XftGlyphFontSpec specbuf[LINE_SIZE]; /* font spec buffer used for rendering */
	TCursor c;                           /* cursor */
	int scroll;                          /* current scroll position */
	int top;                             /* top scroll limit */
	int bot;                             /* bottom scroll limit */
	int mode;                            /* terminal mode flags */
	int esc;                             /* escape state flags */
	int lines;
	int padding;
} term;

/* Drawing Context */
static struct {
	Color col[256];
	XftFont *font, *bfont, *ifont, *ibfont;
	GC gc;
} dc;

static void die(const char *, ...);
static void draw(void);
static void drawregion(int, int, int, int);
static void execsh(void);
static void sigchld(int);
static void run(void);

static void csihandle(void);
static enum escape_state eschandle(u8);

static void tclearregion(int, int, int, int);
static void tcursor(int);
static void tdeletechar(int);
static void tinsertblank(int);
static int tlinelen(int);
static void tmoveto(int, int);
static void tmoveato(int, int);
static void tnewline(bool);
static void tputtab(int);
static void tputc(Rune);
static void treset(void);
static void tscroll(int);
static void tsetattr(int *, int);
static void tsetchar(Glyph *, int, int);
static void tsetscroll(int, int);
static void tswapscreen(void);
static void tsetmode(int, int *, int);
static void tcontrolcode(u8);
static u8 tdefcolor(int *, int *, int);
static inline int match(u32, u32);
static void ttynew(void);
static size_t ttyread(void);
static void ttywrite(const char *, size_t);

static inline u16 sixd_to_16bit(int);
static int xmakeglyphfontspecs(XftGlyphFontSpec *, const Glyph *, int, int, int);
static void xdrawglyphfontspecs(const XftGlyphFontSpec *, Glyph, int, int, int);
static void xdrawglyph(Glyph, int, int);
static void xhints(void);
static void xclear(int, int, int, int);
static void xdrawcursor(void);
static void xinit(void);
static void xloadcolors(void);
static void xloadfont(XftFont **, FcPattern *);
static void xloadfonts(char *);
static void xsettitle(char *);
static void xsetpointermotion(int);
static void xsetsel(char *, Time);

static void visibility(XEvent *);
static void unmap(XEvent *);
static char *kmap(KeySym, u32);
static void kpress(XEvent *);
static void resize(int, int);
static void configure_notify(XEvent *);
static void focus(XEvent *);
static void brelease(XEvent *);
static void bpress(XEvent *);
static void bmotion(XEvent *);
static void propnotify(XEvent *);
static void selnotify(XEvent *);
static void selclear(XEvent *);
static void selrequest(XEvent *);

static void selinit(void);
static void selnormalize(void);
static inline bool selected(int, int);
static char *getsel(void);
static void selsnap(int *, int *, int);
static int x2col(int);
static int y2row(int);
static void mousereport(XEvent *);

static size_t utf8encode(Rune, char *);
static char utf8encodebyte(Rune, size_t);
static size_t utf8validate(Rune *, size_t);

static void usage(void);

static void (*handler[LASTEvent])(XEvent *) = {
	[KeyPress] = kpress,
	[ConfigureNotify] = configure_notify,
	[VisibilityNotify] = visibility,
	[UnmapNotify] = unmap,
	[FocusIn] = focus,
	[FocusOut] = focus,
	[MotionNotify] = bmotion,
	[ButtonPress] = bpress,
	[ButtonRelease] = brelease,
	[SelectionClear] = selclear,
	[SelectionNotify] = selnotify,
/*
 * PropertyNotify is only turned on when there is some INCR transfer happening
 * for the selection retrieval.
 */
	[PropertyNotify] = propnotify,
	[SelectionRequest] = selrequest,
};

/* Globals */
static int cmdfd;
static pid_t pid;
static int iofd = 1;
static char **opt_cmd  = shell;
static char *opt_embed = NULL;
static char *opt_title = "st";
static int oldbutton   = 3; /* button event on startup: 3 = release */

static u8 utfbyte[UTF_SIZ + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static u8 utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static Rune utfmin[UTF_SIZ + 1] = {       0,    0,  0x80,  0x800,  0x10000};
static Rune utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

/* Font Ring Cache */
enum {
	FRC_NORMAL,
	FRC_ITALIC,
	FRC_BOLD,
	FRC_ITALICBOLD
};

size_t utf8encode(Rune u, char *c)
{
	size_t len, i;

	len = utf8validate(&u, 0);
	if (len > UTF_SIZ)
		return 0;

	for (i = len - 1; i != 0; --i) {
		c[i] = utf8encodebyte(u, 0);
		u >>= 6;
	}
	c[0] = utf8encodebyte(u, len);

	return len;
}

char utf8encodebyte(Rune u, size_t i)
{
	return utfbyte[i] | (u & ~utfmask[i]);
}

size_t utf8validate(Rune *u, size_t i)
{
	if (!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
		*u = UTF_INVALID;
	for (i = 1; *u > utfmax[i]; ++i)
		;

	return i;
}

void selinit(void)
{
	sel.ob.x = -1;
	sel.xtarget = XInternAtom(xw.dpy, "UTF8_STRING", 0);
	if (sel.xtarget == None)
		sel.xtarget = XA_STRING;
}

int x2col(int x)
{
	x -= borderpx;
	x /= xw.cw;
	return LIMIT(x, 0, term.col-1);
}

int y2row(int y)
{
	y -= borderpx;
	y /= xw.ch;
	return LIMIT(y, 0, term.row-1) + term.scroll;
}

int tlinelen(int y)
{
	int i = term.col;

	if (TLINE(y)[i - 1].mode & ATTR_WRAP)
		return i;

	while (i > 0 && TLINE(y)[i - 1].u == ' ')
		--i;

	return i;
}

void selnormalize(void)
{
	int i;

	if (sel.type == SEL_REGULAR && sel.ob.y != sel.oe.y) {
		sel.nb.x = sel.ob.y < sel.oe.y ? sel.ob.x : sel.oe.x;
		sel.ne.x = sel.ob.y < sel.oe.y ? sel.oe.x : sel.ob.x;
	} else {
		sel.nb.x = MIN(sel.ob.x, sel.oe.x);
		sel.ne.x = MAX(sel.ob.x, sel.oe.x);
	}
	sel.nb.y = MIN(sel.ob.y, sel.oe.y);
	sel.ne.y = MAX(sel.ob.y, sel.oe.y);

	selsnap(&sel.nb.x, &sel.nb.y, -1);
	selsnap(&sel.ne.x, &sel.ne.y, +1);

	/* expand selection over line breaks */
	if (sel.type == SEL_RECTANGULAR)
		return;
	i = tlinelen(sel.nb.y);
	if (i < sel.nb.x)
		sel.nb.x = i;
	if (tlinelen(sel.ne.y) <= sel.ne.x)
		sel.ne.x = term.col - 1;
}

bool selected(int x, int y)
{
	if (sel.mode == SEL_EMPTY)
		return false;

	if (sel.type == SEL_RECTANGULAR)
		return BETWEEN(y, sel.nb.y, sel.ne.y)
		    && BETWEEN(x, sel.nb.x, sel.ne.x);

	return BETWEEN(y, sel.nb.y, sel.ne.y)
	    && (y != sel.nb.y || x >= sel.nb.x)
	    && (y != sel.ne.y || x <= sel.ne.x);
}

void selsnap(int *x, int *y, int direction)
{
	switch (sel.snap) {
	case SNAP_WORD:
		while (BETWEEN(*x, 0, term.col - 1) && !ISDELIM(TLINE(*y)[*x].u))
			*x += direction;
		*x -= direction;
		break;
	case SNAP_LINE:
		*x = (direction < 0) ? 0 : term.col - 1;
		break;
	}
}

void mousereport(XEvent *e)
{
	int x = x2col(e->xbutton.x), y = y2row(e->xbutton.y) - term.scroll;
	int button = e->xbutton.button, state = e->xbutton.state;
	int len;
	char buf[40];
	static int ox, oy;

	/* from urxvt */
	if (e->xbutton.type == MotionNotify) {
		if (x == ox && y == oy)
			return;
		if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY))
			return;
		/* MOUSE_MOTION: no reporting if no button is pressed */
		if (IS_SET(MODE_MOUSEMOTION) && oldbutton == 3)
			return;

		button = oldbutton + 32;
		ox = x;
		oy = y;
	} else {
		if (!IS_SET(MODE_MOUSESGR) && e->xbutton.type == ButtonRelease) {
			button = 3;
		} else {
			button -= Button1;
			if (button >= 3)
				button += 64 - 3;
		}
		if (e->xbutton.type == ButtonPress) {
			oldbutton = button;
			ox = x;
			oy = y;
		} else if (e->xbutton.type == ButtonRelease) {
			oldbutton = 3;
			/* MODE_MOUSEX10: no button release reporting */
			if (IS_SET(MODE_MOUSEX10))
				return;
			if (button == 64 || button == 65)
				return;
		}
	}

	if (!IS_SET(MODE_MOUSEX10)) {
		button += ((state & ShiftMask  ) ? 4  : 0)
			+ ((state & Mod4Mask   ) ? 8  : 0)
			+ ((state & ControlMask) ? 16 : 0);
	}

	if (IS_SET(MODE_MOUSESGR)) {
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
				button, x+1, y+1,
				e->xbutton.type == ButtonRelease ? 'm' : 'M');
	} else if (x < 223 && y < 223) {
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
				32 + button, 32 + x + 1, 32 + y + 1);
	} else {
		return;
	}

	ttywrite(buf, len);
}

void bpress(XEvent *e)
{
	if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forceselmod))
		mousereport(e);

	switch (e->xbutton.button) {
	case Button1:
		selclear(NULL);
		sel.type = SEL_REGULAR;
		int same_pos = sel.oe.x == x2col(e->xbutton.x) && sel.oe.y == y2row(e->xbutton.y);
		sel.snap = same_pos ? MIN(sel.snap + 1, SNAP_LINE) : 0;
		sel.mode = same_pos ? SEL_READY : SEL_EMPTY;
		sel.oe.x = sel.ob.x = x2col(e->xbutton.x);
		sel.oe.y = sel.ob.y = y2row(e->xbutton.y);
		selnormalize();
		break;
	case Button3:
		sel.snap = SNAP_LINE;
		sel.mode = SEL_READY;
		bmotion(e);
		break;
	case Button4:
		tscroll(-5);
		break;
	case Button5:
		tscroll(5);
		break;
	}
}

char *getsel(void)
{
	char *str, *ptr;
	int y, bufsize, lastx, linelen;
	Glyph *gp, *last;

	if (sel.mode == SEL_IDLE)
		return NULL;

	bufsize = (term.col+1) * (sel.ne.y-sel.nb.y+1) * UTF_SIZ;
	ptr = str = malloc(bufsize);

	/* append every set & selected glyph to the selection */
	for (y = sel.nb.y; y <= sel.ne.y; y++) {
		if ((linelen = tlinelen(y)) == 0) {
			*ptr++ = '\n';
			continue;
		}

		Glyph *line = term.hist[y % histsize];

		if (sel.type == SEL_RECTANGULAR) {
			gp = &line[sel.nb.x];
			lastx = sel.ne.x;
		} else {
			gp = &line[sel.nb.y == y ? sel.nb.x : 0];
			lastx = (sel.ne.y == y) ? sel.ne.x : term.col-1;
		}
		last = &line[MIN(lastx, linelen-1)];
		while (last >= gp && last->u == ' ')
			--last;

		for ( ; gp <= last; ++gp)
			ptr += utf8encode(gp->u, ptr);

		/*
		 * Copy and pasting of line endings is inconsistent
		 * in the inconsistent terminal and GUI world.
		 * The best solution seems like to produce '\n' when
		 * something is copied from st and convert '\n' to
		 * '\r', when something to be pasted is received by
		 * st.
		 * FIXME: Fix the computer world.
		 */
		if ((y < sel.ne.y || lastx >= linelen) && !(last->mode & ATTR_WRAP))
			*ptr++ = '\n';
	}
	*ptr = 0;
	return str;
}

void propnotify(XEvent *e)
{
	XPropertyEvent *xpev;
	Atom clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);

	xpev = &e->xproperty;
	if (xpev->state == PropertyNewValue &&
			(xpev->atom == XA_PRIMARY ||
			 xpev->atom == clipboard)) {
		selnotify(e);
	}
}

void selnotify(XEvent *e)
{
	u64 nitems, ofs, rem;
	int format;
	u8 *data, *last, *repl;
	Atom type, incratom, property;

	incratom = XInternAtom(xw.dpy, "INCR", 0);

	ofs = 0;
	if (e->type == SelectionNotify)
		property = e->xselection.property;
	else if (e->type == PropertyNotify)
		property = e->xproperty.atom;
	else
		return;

	if (property == None)
		return;

	do {
		if (XGetWindowProperty(xw.dpy, xw.win, property, ofs,
				BUFSIZ / 4, False, AnyPropertyType,
				&type, &format, &nitems, &rem, &data)) {
			fprintf(stderr, "Clipboard allocation failed\n");
			return;
		}

		if (e->type == PropertyNotify && nitems == 0 && rem == 0) {
			/*
			 * If there is some PropertyNotify with no data, then
			 * this is the signal of the selection owner that all
			 * data has been transferred. We won't need to receive
			 * PropertyNotify events anymore.
			 */
			MODBIT(xw.attrs.event_mask, 0, PropertyChangeMask);
			XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask,
					&xw.attrs);
		}

		if (type == incratom) {
			/*
			 * Activate the PropertyNotify events so we receive
			 * when the selection owner does send us the next
			 * chunk of data.
			 */
			MODBIT(xw.attrs.event_mask, 1, PropertyChangeMask);
			XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);

			/*
			 * Deleting the property is the transfer start signal.
			 */
			XDeleteProperty(xw.dpy, xw.win, (int)property);
			continue;
		}

		/*
		 * As seen in getsel:
		 * Line endings are inconsistent in the terminal and GUI world
		 * copy and pasting. When receiving some selection data,
		 * replace all '\n' with '\r'.
		 * FIXME: Fix the computer world.
		 */
		repl = data;
		last = data + nitems * format / 8;
		while ((repl = memchr(repl, '\n', last - repl))) {
			*repl++ = '\r';
		}

		if (IS_SET(MODE_BRCKTPASTE) && ofs == 0)
			ttywrite("\033[200~", 6);
		ttywrite((char *)data, nitems * format / 8);
		if (IS_SET(MODE_BRCKTPASTE) && rem == 0)
			ttywrite("\033[201~", 6);
		XFree(data);
		/* number of 32-bit chunks returned */
		ofs += nitems * format / 32;
	} while (rem > 0);

	/*
	 * Deleting the property again tells the selection owner to send the
	 * next data chunk in the property.
	 */
	XDeleteProperty(xw.dpy, xw.win, (int)property);
}

void selpaste()
{
	XConvertSelection(xw.dpy, XA_PRIMARY, sel.xtarget, XA_PRIMARY, xw.win, CurrentTime);
}

void clipcopy()
{
	Atom clipboard;

	if (sel.clipboard != NULL)
		free(sel.clipboard);

	if (sel.primary != NULL) {
		sel.clipboard = strdup(sel.primary);
		clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
		XSetSelectionOwner(xw.dpy, clipboard, xw.win, CurrentTime);
	}
}

void clippaste()
{
	Atom clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
	XConvertSelection(xw.dpy, clipboard, sel.xtarget, clipboard, xw.win, CurrentTime);
}

void selclear(XEvent *e)
{
	(void) e;
	sel.mode = SEL_IDLE;
}

void selrequest(XEvent *e)
{
	XSelectionRequestEvent *xsre;
	XSelectionEvent xev;
	Atom xa_targets, string, clipboard;
	char *seltext;

	xsre = (XSelectionRequestEvent *) e;
	xev.type = SelectionNotify;
	xev.requestor = xsre->requestor;
	xev.selection = xsre->selection;
	xev.target = xsre->target;
	xev.time = xsre->time;
	if (xsre->property == None)
		xsre->property = xsre->target;

	/* reject */
	xev.property = None;

	xa_targets = XInternAtom(xw.dpy, "TARGETS", 0);
	if (xsre->target == xa_targets) {
		/* respond with the supported type */
		string = sel.xtarget;
		XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				XA_ATOM, 32, PropModeReplace,
				(u8 *) &string, 1);
		xev.property = xsre->property;
	} else if (xsre->target == sel.xtarget || xsre->target == XA_STRING) {
		/*
		 * xith XA_STRING non ascii characters may be incorrect in the
		 * requestor. It is not our problem, use utf8.
		 */
		clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
		if (xsre->selection == XA_PRIMARY) {
			seltext = sel.primary;
		} else if (xsre->selection == clipboard) {
			seltext = sel.clipboard;
		} else {
			fprintf(stderr,
				"Unhandled clipboard selection 0x%lx\n",
				xsre->selection);
			return;
		}
		if (seltext != NULL) {
			XChangeProperty(xsre->display, xsre->requestor,
					xsre->property, xsre->target,
					8, PropModeReplace,
					(u8 *) seltext, (int) strlen(seltext));
			xev.property = xsre->property;
		}
	}

	/* all done, send a notification to the listener */
	if (!XSendEvent(xsre->display, xsre->requestor, 1, 0, (XEvent *) &xev))
		fprintf(stderr, "Error sending SelectionNotify event\n");
}

void xsetsel(char *str, Time t)
{
	free(sel.primary);
	sel.primary = str;

	XSetSelectionOwner(xw.dpy, XA_PRIMARY, xw.win, t);
	if (XGetSelectionOwner(xw.dpy, XA_PRIMARY) != xw.win)
		selclear(0);
}

void brelease(XEvent *e)
{
	if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forceselmod)) {
		mousereport(e);
		return;
	}

	if (e->xbutton.button == Button2) {
		selpaste();
	} else if (e->xbutton.button == Button1 || e->xbutton.button == Button3) {
		if (sel.mode == SEL_READY)
			xsetsel(getsel(), e->xbutton.time);
		else
			selclear(NULL);
	}
}

void bmotion(XEvent *e)
{
	if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forceselmod)) {
		mousereport(e);
		return;
	}

	if (!sel.mode)
		return;

	sel.mode = SEL_READY;
	sel.alt = IS_SET(MODE_ALTSCREEN);
	sel.oe.x = x2col(e->xbutton.x);
	sel.oe.y = y2row(e->xbutton.y);
	selnormalize();
	sel.type = SEL_REGULAR;
}

void die(const char *errstr, ...)
{
	va_list ap;
	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

void execsh(void)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%lu", xw.win);
	setenv("WINDOWID", buf, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	execvp(opt_cmd[0], opt_cmd);
	_exit(1);
}

void sigchld(int stat)
{
	pid_t p;

	if ((p = waitpid(pid, &stat, WNOHANG)) < 0)
		die("Waiting for pid %hd failed: %s\n", pid, strerror(errno));

	if (pid != p)
		return;

	if (!WIFEXITED(stat) || WEXITSTATUS(stat))
		die("child finished with error '%d'\n", stat);
	exit(0);
}


void ttynew(void)
{
	int m, s;
	struct winsize w = { (unsigned short) term.row, (unsigned short) term.col, 0, 0 };

	/* seems to work fine on linux, openbsd and freebsd */
	if (openpty(&m, &s, NULL, NULL, &w) < 0)
		die("openpty failed: %s\n", strerror(errno));

	switch (pid = fork()) {
	case -1:
		die("fork failed\n");
		break;
	case 0:
		close(iofd);
		setsid(); /* create a new process group */
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		if (ioctl(s, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
		close(s);
		close(m);
		execsh();
		break;
	default:
		close(s);
		cmdfd = m;
		signal(SIGCHLD, sigchld);
		break;
	}
}

size_t ttyread(void)
{
	static char buf[BUFSIZ];
	static Rune rune = 0;
	static int utf_len = 0;
	ssize_t buf_len = read(cmdfd, buf, BUFSIZ);

	if (buf_len < 0) 
		die("Couldn't read from shell: %s\n", strerror(errno));

	// Reset scroll
	if (term.lines > term.scroll)
		term.scroll = term.lines;

	// Decode UTF-8
	for (char *ptr = buf; ptr < buf + buf_len; ++ptr) {
		if (*ptr >= 0) {
			tputc(*ptr);
		} else if (*ptr >= -16) {
			rune = *ptr & 7;
			utf_len = 3;
		} else if (*ptr >= -32) {
			rune = *ptr & 15;
			utf_len = 2;
		} else if (*ptr >= -64) {
			rune = *ptr & 31;
			utf_len = 1;
		} else {
			rune <<= 6;
			rune |= *ptr & 63;
			if (!--utf_len)
				tputc(rune);
		}
	}

	return buf_len;
}

void ttywrite(const char *buf, size_t n)
{
	while (n > 0) {
		ssize_t result = write(cmdfd, buf, n);
		if (result < 0)
			die("write error on tty: %s\n", strerror(errno));
		n -= result;
		buf += result;
	}
}

void tcursor(int mode)
{
	static TCursor c[2];
	int alt = IS_SET(MODE_ALTSCREEN);

	if (mode == CURSOR_SAVE)
		c[alt] = term.c;
	else if (mode == CURSOR_LOAD)
		term.c = c[alt];
}

void treset(void)
{
	term.c = (TCursor) {{
		.fg = defaultfg,
		.bg = defaultbg
	}, .x = 0, .y = 0, .state = 0};

	tsetscroll(0, term.row - 1);
	term.mode = MODE_WRAP;

	for (int i = 0; i < 2; i++) {
		tmoveto(0, 0);
		tcursor(CURSOR_SAVE);
		tclearregion(0, 0, term.col - 1, term.row - 1);
		tswapscreen();
	}
}

void tswapscreen(void)
{
	static int scroll_save = 0;
	term.mode ^= MODE_ALTSCREEN;
	if (IS_SET(MODE_ALTSCREEN)) {
		scroll_save = term.lines;
		term.lines += term.row;
	} else {
		term.lines = scroll_save;
	}
	term.scroll = term.lines;
}

static void scrolldown(void) { tscroll(term.row - 2); }
static void scrollup(void) { tscroll(2 - term.row); }

void tscroll(int n)
{
	term.scroll += n;
	LIMIT(term.scroll, 0, term.lines);
}

void tnewline(bool first_col)
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

/* for absolute user moves, when decom is set */
void tmoveato(int x, int y)
{
	tmoveto(x, y + (IS_SET(MODE_ABSMOVE) ? term.top : 0));
}

void tmoveto(int x, int y)
{
	term.c.x = LIMIT(x, 0, term.col - 1);
	term.c.y = LIMIT(y, 0, term.row - 1);
}

void tsetchar(Glyph *glyph, int x, int y)
{
	static int vt100_0[] = { /* 0x41 - 0x7e */
		0x256c, 0x2592, 0, 0, 0, 0, 0xb0, 0xb1,            /* ` - g */
		0, 0, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0,   /* h - o */
		0, 0x2500, 0, 0, 0x251c, 0x2524, 0x2534, 0x252c,   /* p - w */
		0x2502, 0x2264, 0x2265, 0x3c0, 0x2260, 0xa3, 0xb7, /* x - ~ */
	};

	if (IS_SET(MODE_ALTCHARSET) && BETWEEN(glyph->u, '`', '~'))
		glyph->u = vt100_0[glyph->u - '`'];

	TLINE(y)[x] = *glyph;
}

void tclearregion(int x1, int y1, int x2, int y2)
{
	if (x1 > x2)
		SWAP(x1, x2);
	if (y1 > y2)
		SWAP(y1, y2);

	LIMIT(x1, 0, term.col-1);
	LIMIT(x2, 0, term.col-1);
	LIMIT(y1, 0, term.row-1);
	LIMIT(y2, 0, term.row-1);

	if (sel.nb.y < y2 && sel.ne.y > y1)
		selclear(NULL);

	for (int y = y1; y <= y2; y++) {
		for (int x = x1; x <= x2; x++) {
			Glyph *gp = &TLINE(y)[x];
			gp->fg = term.c.attr.fg;
			gp->bg = term.c.attr.bg;
			gp->mode = 0;
			gp->u = ' ';
		}
	}
}

void tdeletechar(int n)
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

void tinsertblank(int n)
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

u8 tdefcolor(int *attr, int *npar, int l)
{
	u8 color = -1;

	if (attr[*npar + 1] != 5) {
		fprintf(stderr, "erresc(38): gfx attr %d unknown\n", attr[*npar]);
		return 0;
	}

	if (*npar + 2 >= l) {
		fprintf(stderr, "erresc(38): Incorrect number of parameters (%d)\n", *npar);
		return 0;
	}

	*npar += 2;
	if (!BETWEEN(attr[*npar], 0, 255)) {
		fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
		return 0;
	}

	color = (u8) attr[*npar];
	return color;
}

void tsetattr(int *attr, int l)
{
	int i;
	u8 color;

	for (i = 0; i < l; i++) {
		switch (attr[i]) {
		case 0:
			term.c.attr.mode &= ~(
				ATTR_BOLD       |
				ATTR_FAINT      |
				ATTR_ITALIC     |
				ATTR_UNDERLINE  |
				ATTR_REVERSE    |
				ATTR_INVISIBLE  |
				ATTR_STRUCK     );
			term.c.attr.fg = defaultfg;
			term.c.attr.bg = defaultbg;
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
			term.c.attr.fg = (u8) (attr[i] - 30);
			break;
		case 38:
			if ((color = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.fg = color;
			break;
		case 39:
			term.c.attr.fg = defaultfg;
			break;
		case 40 ... 47:
			term.c.attr.bg = (u8) (attr[i] - 40);
			break;
		case 48:
			if ((color = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.bg = color;
			break;
		case 49:
			term.c.attr.bg = defaultbg;
			break;
		case 90 ... 97:
			term.c.attr.fg = (u8) (attr[i] - 90 + 8);
			break;
		case 100 ... 107:
			term.c.attr.bg = (u8) (attr[i] - 100 + 8);
			break;
		default:
			die("erresc: attr %d unknown\n", attr[i]);
		}
	}
}

void tsetscroll(int a, int b)
{
	LIMIT(a, 0, term.row - 1);
	LIMIT(b, 0, term.row - 1);
	term.top = MIN(a, b);
	term.bot = MAX(a, b);
}

void tsetmode(int set, int *args, int narg)
{
	int *lim;
	int alt;

	for (lim = args + narg; args < lim; ++args) {
		switch (*args) {
		case 1: /* DECCKM -- Cursor key */
			MODBIT(term.mode, set, MODE_APPCURSOR);
			break;
		case 6: /* DECOM -- Origin */
			MODBIT(term.mode, set, MODE_ABSMOVE);
			tmoveato(0, 0);
			break;
		case 7: /* DECAWM -- Auto wrap */
			MODBIT(term.mode, set, MODE_WRAP);
			break;
		case 9: /* X10 mouse compatibility mode */
			xsetpointermotion(0);
			MODBIT(term.mode, 0, MODE_MOUSE);
			MODBIT(term.mode, set, MODE_MOUSEX10);
			break;
		case 12:
			//TODO
			break;
		case 25: /* DECTCEM -- Text Cursor Enable Mode */
			MODBIT(term.mode, !set, MODE_HIDE);
			break;
		case 1000: /* 1000: report button press */
			xsetpointermotion(0);
			MODBIT(term.mode, 0, MODE_MOUSE);
			MODBIT(term.mode, set, MODE_MOUSEBTN);
			break;
		case 1002: /* 1002: report motion on button press */
			xsetpointermotion(0);
			MODBIT(term.mode, 0, MODE_MOUSE);
			MODBIT(term.mode, set, MODE_MOUSEMOTION);
			break;
		case 1003: /* 1003: enable all mouse motions */
			xsetpointermotion(set);
			MODBIT(term.mode, 0, MODE_MOUSE);
			MODBIT(term.mode, set, MODE_MOUSEMANY);
			break;
		case 1004: /* 1004: send focus events to tty */
			MODBIT(term.mode, set, MODE_FOCUS);
			break;
		case 1006: /* 1006: extended reporting mode */
			MODBIT(term.mode, set, MODE_MOUSESGR);
			break;
		case 47:
		case 1047:
		case 1048:
		case 1049: /* swap screen & set/restore cursor */
			tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
			alt = IS_SET(MODE_ALTSCREEN);
			if (alt)
				tclearregion(0, 0, term.col-1, term.row-1);
			if (set ^ alt)
				tswapscreen();
			tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
			break;
		case 2004: /* 2004: bracketed paste mode */
			MODBIT(term.mode, set, MODE_BRCKTPASTE);
			break;
		/* Unimplemented mouse modes: 1001, 1005, 1015 */
		default:
			fprintf(stderr, "erresc: unknown set/reset mode %d\n", *args);
			break;
		}
	}
}

void csihandle(void)
{
	char buf[40];
	int len;
	char *p = csiescseq.buf;

	int arg[ESC_ARG_SIZ];
	int narg = 0;

	narg = 0;
	if (*p == '?')
		p++;

	csiescseq.buf[csiescseq.len] = '\0';
	while (p < csiescseq.buf + csiescseq.len) {
		arg[narg++] = (int) strtol(p, &p, 10);
		if (*p != ';' || narg == ESC_ARG_SIZ)
			break;
		++p;
	}

	if (p[0] == ' ' && p[1] == 'q')
		++p;

	switch (*p) {
	case '@': /* ICH -- Insert <n> blank char */
		DEFAULT(arg[0], 1);
		tinsertblank(arg[0]);
		break;
	case 'A': /* CUU -- Cursor <n> Up */
		DEFAULT(arg[0], 1);
		tmoveto(term.c.x, term.c.y - arg[0]);
		break;
	case 'B': /* CUD -- Cursor <n> Down */
	case 'e': /* VPR --Cursor <n> Down */
		DEFAULT(arg[0], 1);
		tmoveto(term.c.x, term.c.y + arg[0]);
		break;
	case 'c': /* DA -- Device Attributes */
		if (arg[0] == 0)
			ttywrite(vtiden, sizeof(vtiden) - 1);
		break;
	case 'C': /* CUF -- Cursor <n> Forward */
	case 'a': /* HPR -- Cursor <n> Forward */
		DEFAULT(arg[0], 1);
		tmoveto(term.c.x + arg[0], term.c.y);
		break;
	case 'D': /* CUB -- Cursor <n> Backward */
		DEFAULT(arg[0], 1);
		tmoveto(term.c.x - arg[0], term.c.y);
		break;
	case 'E': /* CNL -- Cursor <n> Down and first col */
		DEFAULT(arg[0], 1);
		tmoveto(0, term.c.y + arg[0]);
		break;
	case 'F': /* CPL -- Cursor <n> Up and first col */
		DEFAULT(arg[0], 1);
		tmoveto(0, term.c.y - arg[0]);
		break;
	case 'G': /* CHA -- Move to <col> */
	case '`': /* HPA */
		DEFAULT(arg[0], 1);
		tmoveto(arg[0] - 1, term.c.y);
		break;
	case 'H': /* CUP -- Move to <row> <col> */
	case 'f': /* HVP */
		DEFAULT(arg[0], 1);
		DEFAULT(arg[1], 1);
		tmoveato(arg[1] - 1, arg[0] - 1);
		break;
	case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
		DEFAULT(arg[0], 1);
		tputtab(arg[0]);
		break;
	case 'J': /* ED -- Clear screen */
		selclear(NULL);
		switch (arg[0]) {
		case 0: /* below */
			tclearregion(term.c.x, term.c.y, term.col-1, term.c.y);
			if (term.c.y < term.row-1)
				tclearregion(0, term.c.y+1, term.col-1, term.row-1);
			break;
		case 1: /* above */
			if (term.c.y > 1)
				tclearregion(0, 0, term.col-1, term.c.y-1);
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2: /* all */
			tclearregion(0, 0, term.col-1, term.row-1);
			break;
		default:
			goto unknown;
		}
		break;
	case 'K': /* EL -- Clear line */
		tclearregion(arg[0] == 0 ? term.c.x : 0, term.c.y,
			arg[0] == 1 ? term.c.x : term.col - 1, term.c.y);
		break;
	case 'L': /* IL -- Insert <n> blank lines */
		if (!BETWEEN(term.c.y, term.top, term.bot))
			break;
		DEFAULT(arg[0], 1);
		for (int y = term.bot - arg[0]; y >= term.c.y; --y)
			memmove(TLINE(y + arg[0]), TLINE(y), sizeof(TLINE(0)));
		tclearregion(0, term.c.y, term.col - 1, term.c.y + arg[0] - 1);
		break;
	case 'l': /* RM -- Reset Mode */
		tsetmode(0, arg, narg);
		break;
	case 'M': /* DL -- Delete <n> lines */
		if (!BETWEEN(term.c.y, term.top, term.bot))
			break;
		DEFAULT(arg[0], 1);
		for (int y = term.c.y; y + arg[0] <= term.bot; ++y)
			memmove(TLINE(y), TLINE(y + arg[0]), sizeof(TLINE(0)));
		tclearregion(0, term.bot - arg[0] + 1, term.col - 1, term.bot);
		break;
	case 'P': /* DCH -- Delete <n> char */
		DEFAULT(arg[0], 1);
		tdeletechar(arg[0]);
		break;
	case 'S': /* SU -- Scroll <n> line up */
		DEFAULT(arg[0], 1);
		tscroll(-arg[0]);
		break;
	case 'T': /* SD -- Scroll <n> line down */
		DEFAULT(arg[0], 1);
		tscroll(arg[0]);
		break;
	case 'X': /* ECH -- Erase <n> char */
		DEFAULT(arg[0], 1);
		tclearregion(term.c.x, term.c.y, term.c.x + arg[0] - 1, term.c.y);
		break;
	case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
		DEFAULT(arg[0], 1);
		tputtab(-arg[0]);
		break;
	case 'd': /* VPA -- Move to <row> */
		DEFAULT(arg[0], 1);
		tmoveato(term.c.x, arg[0]-1);
		break;
	case 'h': /* SM -- Set terminal mode */
		tsetmode(1, arg, narg);
		break;
	case 'm': /* SGR -- Terminal attribute (color) */
		tsetattr(arg, narg);
		break;
	case 'n': /* DSR – Device Status Report (cursor position) */
		if (arg[0] != 6)
			goto unknown;
		len = snprintf(buf, sizeof(buf),"\033[%i;%iR", term.c.y + 1, term.c.x + 1);
		ttywrite(buf, len);
		break;
	case 'q': /* DECSCUSR -- Set Cursor Style */
		DEFAULT(arg[0], 1);
		if (!BETWEEN(arg[0], 0, 6))
			goto unknown;
		xw.cursor = arg[0];
		break;
	case 'r': /* DECSTBM -- Set Scrolling Region */
		DEFAULT(arg[0], 1);
		DEFAULT(arg[1], term.row);
		tsetscroll(arg[0] - 1, arg[1] - 1);
		tmoveato(0, 0);
		break;
	case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
		tcursor(CURSOR_SAVE);
		break;
	case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
		tcursor(CURSOR_LOAD);
		break;
	default:
	unknown:
		die("erresc: unknown CSI %.*s\n", csiescseq.len, csiescseq.buf);
	}
}

void tputtab(int n)
{
	term.c.x &= ~7;
	term.c.x += n << 3;
}

void tcontrolcode(u8 ascii)
{
	switch (ascii) {
	case '\t':   /* HT */
		tputtab(1);
		break;
	case '\b':   /* BS */
		tmoveto(term.c.x-1, term.c.y);
		break;
	case '\r':   /* CR */
		tmoveto(0, term.c.y);
		break;
	case '\f':   /* LF */
	case '\v':   /* VT */
	case '\n':   /* LF */
		tnewline(false);
		break;
	case '\a':   /* BEL */
		if (!(xw.state & WIN_FOCUSED))
			XkbBell(xw.dpy, xw.win, bellvolume, (Atom) NULL);
		break;
	case '\033': /* ESC */
		memset(&csiescseq, 0, sizeof(csiescseq));
		term.esc = ESC_START;
		break;
	case '\016': /* SO (LS1 -- Locking shift 1) */
	case '\017': /* SI (LS0 -- Locking shift 0) */
		MODBIT(term.mode, ascii == '\016', MODE_ALTCHARSET);
		break;
	case '\030': /* CAN */
		memset(&csiescseq, 0, sizeof(csiescseq));
		break;
	}
}

enum escape_state eschandle(u8 ascii)
{
	switch (ascii) {
	case '[':
		return ESC_CSI;
	case 'P': /* DCS -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
	case ']': /* OSC -- Operating System Command */
	case 'k': /* old title set compatibility */
		return ESC_STR;
	case 'n': /* LS2 -- Locking shift 2 */
	case 'o': /* LS3 -- Locking shift 3 */
	case '\\': /* ST -- String Terminator */
		return ESC_NONE;
	case '(': /* GZD4 -- set primary charset G0 */
	case ')': /* G1D4 -- set secondary charset G1 */
	case '*': /* G2D4 -- set tertiary charset G2 */
	case '+': /* G3D4 -- set quaternary charset G3 */
		return ESC_START; // TODO should be ESC_CHARSET instead
	case '=':
	case '>': // alternate keypad (ignored)
		return ESC_NONE;
	case 'D': /* IND -- Linefeed */
		if (term.c.y == term.bot)
			tscroll(1);
		else
			++term.c.y;
		return ESC_NONE;
	case 'E': /* NEL -- Next line */
		tnewline(true);
		return ESC_NONE;
	case 'M': /* RI -- Reverse index */
		if (term.c.y == term.top)
			tscroll(-1);
		else
			--term.c.y;
		return ESC_NONE;
	case 'Z': /* DECID -- Identify Terminal */
		ttywrite(vtiden, sizeof(vtiden) - 1);
		return ESC_NONE;
	case 'c': /* RIS -- Reset to inital state */
		treset();
		xsettitle(opt_title);
		xloadcolors();
		return ESC_NONE;
	case '7': /* DECSC -- Save Cursor */
		tcursor(CURSOR_SAVE);
		return ESC_NONE;
	case '8': /* DECRC -- Restore Cursor */
		tcursor(CURSOR_LOAD);
		return ESC_NONE;
	default:
		fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
			(u8) ascii, isprint(ascii) ? ascii : '.');
		return ESC_NONE;
	}
}

void tputc(Rune u)
{
	if (term.esc == ESC_STR) {
		term.esc = ISCONTROL(u) ? ESC_NONE : ESC_STR;
		return;
	}

	/*
	 * Actions of control codes must be performed as soon they arrive
	 * because they can be embedded inside a control sequence, and
	 * they must not cause conflicts with sequences.
	 */
	if (ISCONTROL(u)) {
		term.esc = ESC_NONE;
		tcontrolcode((char) u);
		return;
	}

	if (term.esc == ESC_CSI) {
		csiescseq.buf[csiescseq.len++] = (char) u;
		if (BETWEEN(u, 0x40, 0x7E) || csiescseq.len >= sizeof(csiescseq.buf) - 1) {
			term.esc = 0;
			csihandle();
		}
		return;
	}

	if (term.esc == ESC_START) {
		term.esc = eschandle((char) u);
		return;
	}

	if (sel.mode != SEL_IDLE && BETWEEN(term.c.y, sel.ob.y, sel.oe.y))
		selclear(NULL);

	if (term.c.x >= term.col)
		tnewline(true);

	term.c.attr.u = u;
	tsetchar(&term.c.attr, term.c.x, term.c.y);

	if (term.c.x + 1 < term.col) {
		tmoveto(term.c.x + 1, term.c.y);
	} else {
		TLINE(term.c.y)[term.c.x].mode |= ATTR_WRAP;
		tnewline(true);
	}
}

u16 sixd_to_16bit(int x)
{
	return (u16) (x == 0 ? 0 : 0x3737 + 0x2828 * x);
}

static int xloadcolor(int i, Color *ncolor)
{
	if (i < 16)
		return XftColorAllocName(xw.dpy, xw.vis, xw.cmap, colorname[i], ncolor);

	XRenderColor color = { .alpha = 0xffff };

	if (i < 6*6*6+16) { /* same colors as xterm */
		color.red   = sixd_to_16bit((i - 16) / 36 % 6);
		color.green = sixd_to_16bit((i - 16) /  6 % 6);
		color.blue  = sixd_to_16bit((i - 16) /  1 % 6);
	} else { /* greyscale */
		color.red = (u16) (0x0808 + 0x0a0a * (i - (6*6*6+16)));
		color.green = color.blue = color.red;
	}

	return XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, ncolor);
}

void xloadcolors(void)
{
	int i;
	static int loaded;
	Color *cp;

	if (loaded)
		for (cp = dc.col; cp < &dc.col[LEN(dc.col)]; ++cp)
			XftColorFree(xw.dpy, xw.vis, xw.cmap, cp);

	for (i = 0; i < LEN(dc.col); i++)
		if (!xloadcolor(i, &dc.col[i]))
			die("Could not allocate color %d\n", i);
	loaded = 1;
}

/* Absolute coordinates. */
void xclear(int x1, int y1, int x2, int y2)
{
	XftDrawRect(xw.draw, &dc.col[defaultbg], x1, y1, x2-x1, y2-y1);
}

void xhints(void)
{
	XClassHint class = { term_name, term_class };
	XWMHints wm = { .flags = InputHint, .input = 1 };
	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, NULL, &wm, &class);
}

void xloadfont(XftFont **f, FcPattern *pattern)
{
	FcPattern *match;
	FcResult result;
	XGlyphInfo extents;

	match = XftFontMatch(xw.dpy, xw.scr, pattern, &result);
	if (!match)
		die("st: can't open font %s\n", pattern);

	if (!(*f = XftFontOpenPattern(xw.dpy, match))) {
		FcPatternDestroy(match);
		die("st: can't open font %s\n", pattern);
	}

	XftTextExtentsUtf8(xw.dpy, *f, (const FcChar8 *) "Q", 1, &extents);

	/* Setting character width and height. */
	xw.cw = extents.xOff + cw_add;
	xw.ch = (*f)->ascent + (*f)->descent + ch_add;
}

void xloadfonts(char *fontstr)
{
	FcPattern *pattern;

	if (fontstr[0] == '-')
		pattern = XftXlfdParse(fontstr, False, False);
	else
		pattern = FcNameParse((FcChar8 *)fontstr);

	if (!pattern)
		die("st: can't open font %s\n", fontstr);

	FcPatternAddDouble(pattern, FC_SIZE, 12);
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

void xinit(void)
{
	XGCValues gcvalues;
	Window parent = 0;
	pid_t thispid = getpid();

	if (!(xw.dpy = XOpenDisplay(NULL)))
		die("Can't open display\n");
	xw.scr = XDefaultScreen(xw.dpy);
	xw.vis = XDefaultVisual(xw.dpy, xw.scr);

	/* font */
	if (!FcInit())
		die("Could not init fontconfig.\n");

	xloadfonts(fontname);

	/* colors */
	xw.cmap = XDefaultColormap(xw.dpy, xw.scr);
	xloadcolors();

	/* adjust fixed window geometry */
	xw.w = 2 * borderpx + term.col * xw.cw;
	xw.h = 2 * borderpx + term.row * xw.ch;
	if (xw.gm & XNegative)
		xw.l += DisplayWidth(xw.dpy, xw.scr) - xw.w - 2;
	if (xw.gm & YNegative)
		xw.t += DisplayHeight(xw.dpy, xw.scr) - xw.h - 2;

	/* Events */
	xw.attrs.background_pixel = dc.col[defaultbg].pixel;
	xw.attrs.border_pixel = dc.col[defaultbg].pixel;
	xw.attrs.bit_gravity = NorthWestGravity;
	xw.attrs.event_mask = FocusChangeMask | KeyPressMask
		| VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
	xw.attrs.colormap = xw.cmap;

	if (!(opt_embed && (parent = strtol(opt_embed, NULL, 0))))
		parent = XRootWindow(xw.dpy, xw.scr);
	xw.win = XCreateWindow(xw.dpy, parent, xw.l, xw.t, xw.w, xw.h, 0,
			XDefaultDepth(xw.dpy, xw.scr), InputOutput, xw.vis,
			CWBackPixel | CWBorderPixel | CWBitGravity | CWEventMask | CWColormap,
			&xw.attrs);

	memset(&gcvalues, 0, sizeof(gcvalues));
	gcvalues.graphics_exposures = False;
	dc.gc = XCreateGC(xw.dpy, parent, GCGraphicsExposures,
			&gcvalues);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, xw.w, xw.h,
			DefaultDepth(xw.dpy, xw.scr));
	XSetForeground(xw.dpy, dc.gc, dc.col[defaultbg].pixel);
	XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, xw.w, xw.h);

	/* Xft rendering context */
	xw.draw = XftDrawCreate(xw.dpy, xw.buf, xw.vis, xw.cmap);

	/* white cursor, black outline */
	XDefineCursor(xw.dpy, xw.win, XCreateFontCursor(xw.dpy, XC_xterm));

	xw.xembed = XInternAtom(xw.dpy, "_XEMBED", False);
	xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
	xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
	XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

	xw.netwmpid = XInternAtom(xw.dpy, "_NET_WM_PID", False);
	XChangeProperty(xw.dpy, xw.win, xw.netwmpid, XA_CARDINAL, 32,
			PropModeReplace, (u8 *)&thispid, 1);

	/* Various */
	XSetLocaleModifiers("");
	xsettitle(opt_title);
	XMapWindow(xw.dpy, xw.win);
	xhints();
	XSync(xw.dpy, False);
}

int xmakeglyphfontspecs(XftGlyphFontSpec *specs, const Glyph *glyphs, int len, int x, int y)
{
	float winx = borderpx + x * xw.cw, winy = borderpx + y * xw.ch, xp, yp;
	uint32_t mode, prevmode = USHRT_MAX;
	XftFont *font = dc.font;
	float runewidth = xw.cw;
	Rune rune;
	FT_UInt glyphidx;
	int i, numspecs = 0;

	for (i = 0, xp = winx, yp = winy + font->ascent; i < len; ++i) {
		/* Fetch rune and mode for current glyph. */
		rune = glyphs[i].u;
		mode = glyphs[i].mode;

		/* Determine font for glyph if different from previous glyph. */
		if (prevmode != mode) {
			prevmode = mode;
			font = dc.font;
			runewidth = xw.cw * 1.0f;
			if ((mode & ATTR_ITALIC) && (mode & ATTR_BOLD))
				font = dc.ibfont;
			else if (mode & ATTR_ITALIC)
				font = dc.ifont;
			else if (mode & ATTR_BOLD)
				font = dc.bfont;
			yp = winy + font->ascent;
		}

		/* Lookup character index with default font. */
		glyphidx = XftCharIndex(xw.dpy, font, rune);
		if (!glyphidx)
			continue;

		specs[numspecs].font = font;
		specs[numspecs].glyph = glyphidx;
		specs[numspecs].x = (short) xp;
		specs[numspecs].y = (short) yp;
		xp += runewidth;
		numspecs++;
	}

	return numspecs;
}

void xdrawglyphfontspecs(const XftGlyphFontSpec *specs, Glyph base, int len, int x, int y)
{
	int winx = borderpx + x * xw.cw, winy = borderpx + y * xw.ch, width = len * xw.cw;
	Color *fg, *bg, *temp;
	XRenderColor colfg;
	XRectangle r;

	fg = &dc.col[base.fg];
	bg = &dc.col[base.bg];

	if (base.mode & ATTR_REVERSE) {
		temp = fg;
		fg = bg;
		bg = temp;
	}

	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
		colfg.red = fg->color.red / 2;
		colfg.green = fg->color.green / 2;
		colfg.blue = fg->color.blue / 2;
	}

	if (base.mode & ATTR_INVISIBLE)
		fg = bg;

	/* Intelligent cleaning up of the borders. */
	if (x == 0) {
		xclear(0, (y == 0)? 0 : winy, borderpx,
			winy + xw.ch + ((y >= term.row-1)? xw.h : 0));
	}
	if (x + len >= term.col) {
		xclear(winx + width, (y == 0)? 0 : winy, xw.w,
			((y >= term.row-1)? xw.h : (winy + xw.ch)));
	}
	if (y == 0)
		xclear(winx, 0, winx + width, borderpx);
	if (y == term.row - 1)
		xclear(winx, winy + xw.ch, winx + width, xw.h);

	/* Clean up the region we want to draw to. */
	XftDrawRect(xw.draw, bg, winx, winy, width, xw.ch);

	/* Set the clip region because Xft is sometimes dirty. */
	r.x = 0;
	r.y = 0;
	r.height = (u16) xw.ch;
	r.width = (u16) width;
	XftDrawSetClipRectangles(xw.draw, winx, winy, &r, 1);

	/* Render the glyphs. */
	XftDrawGlyphFontSpec(xw.draw, fg, specs, len);

	/* Render underline and strikethrough. */
	if (base.mode & ATTR_UNDERLINE)
		XftDrawRect(xw.draw, fg, winx, winy + dc.font->ascent + 1, width, 1);

	if (base.mode & ATTR_STRUCK)
		XftDrawRect(xw.draw, fg, winx, winy + 2 * dc.font->ascent / 3, width, 1);

	/* Reset clip to none. */
	XftDrawSetClip(xw.draw, 0);
}

void xdrawglyph(Glyph g, int x, int y)
{
	int numspecs;
	XftGlyphFontSpec spec;

	numspecs = xmakeglyphfontspecs(&spec, &g, 1, x, y);
	xdrawglyphfontspecs(&spec, g, numspecs, x, y);
}

void xdrawcursor(void)
{
	static int oldx = 0, oldy = 0;
	int curx = term.c.x;
	Glyph g = {.u = TLINE(term.c.y)[term.c.x].u, 0, defaultbg, defaultfg};

	LIMIT(oldx, 0, term.col-1);
	LIMIT(oldy, 0, term.row-1);

	/* remove the old cursor */
	xdrawglyph(TLINE(oldy)[oldx], oldx, oldy);

	if (IS_SET(MODE_HIDE))
		return;

	/* draw the new one */
	switch ((xw.state & WIN_FOCUSED) ? xw.cursor : 4) {
	case 0: /* Blinking Block */
	case 1: /* Blinking Block (Default) */
	case 2: /* Steady Block */
		xdrawglyph(g, term.c.x, term.c.y);
		break;
	case 3: /* Blinking Underline */
	case 4: /* Steady Underline */
		XftDrawRect(xw.draw, &dc.col[defaultfg], borderpx + curx * xw.cw,
			borderpx + (term.c.y + 1) * xw.ch - 2, xw.cw, 2);
		break;
	case 5: /* Blinking bar */
	case 6: /* Steady bar */
		XftDrawRect(xw.draw, &dc.col[defaultfg], borderpx + curx * xw.cw,
			borderpx + term.c.y * xw.ch, 2, xw.ch);
		break;
	}

	oldx = curx;
	oldy = term.c.y;
}


void xsettitle(char *p)
{
	XTextProperty prop;

	Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle, &prop);
	XSetWMName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmname);
	XFree(prop.value);
}

void draw(void)
{
	drawregion(0, 0, term.col, term.row);
	XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, 0, 0, xw.w, xw.h, 0, 0);
	XSetForeground(xw.dpy, dc.gc, dc.col[defaultbg].pixel);
}

void drawregion(int x1, int y1, int x2, int y2)
{
	int i, x, y, ox, numspecs;
	Glyph base, new;
	XftGlyphFontSpec *specs;
	int ena_sel = sel.mode != SEL_IDLE && sel.alt == IS_SET(MODE_ALTSCREEN);

	if (!(xw.state & WIN_VISIBLE))
		return;

	for (y = y1; y < y2; y++) {
		specs = term.specbuf;
		numspecs = xmakeglyphfontspecs(specs, &TLINE(y)[x1], x2 - x1, x1, y);
		i = ox = 0;
		base = TLINE(y)[x1];

		for (x = x1; x < x2 && i < numspecs; x++) {
			new = TLINE(y)[x];
			if (ena_sel && selected(x, y + term.scroll))
				new.mode ^= ATTR_REVERSE;
			if (i > 0 && ATTRCMP(base, new)) {
				xdrawglyphfontspecs(specs, base, i, ox, y);
				specs += i;
				numspecs -= i;
				i = 0;
			}
			if (i == 0) {
				ox = x;
				base = new;
			}
			i++;
		}
		if (i > 0)
			xdrawglyphfontspecs(specs, base, i, ox, y);
	}
	xdrawcursor();
}

void visibility(XEvent *ev)
{
	XVisibilityEvent *e = &ev->xvisibility;
	MODBIT(xw.state, e->state != VisibilityFullyObscured, WIN_VISIBLE);
}

void unmap(XEvent *ev)
{
	(void) ev;
	xw.state &= ~WIN_VISIBLE;
}

void xsetpointermotion(int set)
{
	MODBIT(xw.attrs.event_mask, set, PointerMotionMask);
	XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
}

void focus(XEvent *ev)
{
	XFocusChangeEvent *e = &ev->xfocus;

	if (e->mode == NotifyGrab)
		return;

	MODBIT(xw.state, ev->type == FocusIn, WIN_FOCUSED);
	if (IS_SET(MODE_FOCUS))
		ttywrite(ev->type == FocusIn ? "\033[I" : "\033[O", 3);
}

int match(u32 mask, u32 state)
{
	return mask == XK_ANY_MOD || mask == (state & ~ignoremod);
}

char* kmap(KeySym k, u32 state)
{
	for (Key *kp = key; kp < key + LEN(key); kp++) {
		if (kp->k != k)
			continue;
		if (!match(kp->mask, state))
			continue;
		if (IS_SET(MODE_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
			continue;
		return kp->s;
	}

	return "";
}

void kpress(XEvent *ev)
{
	XKeyEvent *e = &ev->xkey;
	KeySym ksym = XKeycodeToKeysym(xw.dpy, (char) e->keycode, 1);

	for (Shortcut *bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if (bp->keysym == ksym && match(bp->mod, e->state)) {
			bp->func();
			return;
		}
	}

	if ((ksym & 0xFFFF) >= 0xFD00) {
		char *customkey = kmap(ksym, e->state);
		ttywrite(customkey, strlen(customkey));
	} else {
		char buf[UTF_SIZ + 1];
		int len = XLookupString(e, buf, UTF_SIZ, NULL, NULL);
		ttywrite(buf, len);
	}
}

void resize(int width, int height)
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
	xclear(0, 0, xw.w, xw.h);

	// Send our size to the tty driver so that applications can query it
	struct winsize w = { (u16) term.row, (u16) term.col, 0, 0 };
	if (ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

void configure_notify(XEvent *e)
{
	if (e->xconfigure.width != xw.w || e->xconfigure.height != xw.h)
		resize(e->xconfigure.width, e->xconfigure.height);
}

void run(void)
{
	fd_set rfd;
	int xfd = XConnectionNumber(xw.dpy);
	const struct timespec timeout = { 0, 1000000000 / FPS };
	struct timespec now, last = { 0, 0 };
	bool dirty = true;

	for (;;) {
		FD_ZERO(&rfd);
		FD_SET(cmdfd, &rfd);
		FD_SET(xfd, &rfd);

		int result = pselect(MAX(xfd, cmdfd) + 1, &rfd, NULL, NULL, &timeout, NULL);

		if (result < 0 && errno != EINTR)
			die("select failed: %s\n", strerror(errno));

		dirty |= result > 0;

		if (FD_ISSET(cmdfd, &rfd))
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

void usage(void)
{
	die("usage: %s [-v] [-c class] [-f font] [-n name] [-t title]\n"
		"[-w windowid] [[-e] command [args ...]]\n", argv0);
}

int main(int argc, char *argv[])
{
	ARGBEGIN {
	case 'c':
		term_class = EARGF(usage());
		break;
	case 'e':
		if (argc > 0) {
			--argc;
			++argv;
		}
		goto run;
	case 'n':
		term_name = EARGF(usage());
		break;
	case 't':
		opt_title = EARGF(usage());
		break;
	case 'w':
		opt_embed = EARGF(usage());
		break;
	case 'v':
		die("%s " VERSION " (c) 2010-2016 st engineers\n", argv0);
		break;
	default:
		usage();
	} ARGEND;

run:
	if (argc > 0) {
		/* eat all remaining arguments */
		opt_cmd = argv;
		if (!opt_title)
			opt_title = basename(strdup(argv[0]));
	}

	setlocale(LC_CTYPE, "");
	treset();
	xinit();
	resize(800, 600);
	selinit();
	ttynew();
	run();
}
