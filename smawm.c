/* smawm - a small floating window manager.
 *
 * The client and tag model is dwm's, trimmed to one screen and one active
 * tag (no monitors, no bitmask, no tiling). Mouse move/resize, the grab
 * trick (MODKEY+button grabbed on the root, subwindow says what was
 * clicked), tag switching by unmapping, and the shape of the code are
 * sowm's. The bar is drawn with Xft rather than through dwm's drw.c: it
 * keeps the one thing drw.c really buys - an off-screen pixmap, since
 * painting straight onto the bar windows flickers - and drops the rest.
 * Two bar windows: tags on the left, status text on the right.
 *
 * What is left of dwm's ICCCM/EWMH plumbing is the load-bearing set, and
 * only that: WM_STATE, synthetic ConfigureNotify, the _NET_SUPPORTED /
 * _NET_SUPPORTING_WM_CHECK advertisement, _NET_WM_STATE_FULLSCREEN, and
 * UnmapNotify. It is the part sowm leaves out, and the part real toolkit
 * applications need in order to behave. FEATURES.txt says what was cut and
 * what each remaining piece costs.
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xft/Xft.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "smawm.h"

enum { WMState, WMLast };
/* the whole netatom array is published as _NET_SUPPORTED, exactly as dwm
 * does it: without that (and _NET_SUPPORTING_WM_CHECK) toolkits decide no
 * EWMH window manager is running and start doing fullscreen/maximise by
 * hand, fighting whatever the WM does */
enum { NetActiveWindow, NetSupported, NetWMName, NetWMState, NetWMCheck,
       NetWMStateFullscreen, NetWMWindowType, NetWMWindowTypeDialog,
       NetLast };

static void drawbar(void);
static void drawborder(Client *c, int focused);
static void focus(Client *c);
static void focusstack(const Arg *arg);
static void grabinput(void);
static void killclient(const Arg *arg);
static void movetotag(const Arg *arg);
static void resizeclient(Client *c, int x, int y, int w, int h);
static void setclientstate(Client *c, long state);
static void showhide(Client *c, int show);
static void spawn(const Arg *arg);
static int textw(const char *s);
static void togglebar(const Arg *arg);
static void togglemax(const Arg *arg);
static void togglefullscreen(const Arg *arg);
static void quit(const Arg *arg);
static void updatesizehints(Client *c);
static void updatewindowtype(Client *c);
static void updatewmhints(Client *c);
static void view(const Arg *arg);

#include "config.h"

static Display *dpy;
static int screen;
/* the whole X screen. Xinerama would say where the monitors inside it
 * are; smawm never asks, so two screens are simply one wide screen -
 * which is exactly what sowm does. */
static int sw, sh;
static Window root, wmcheckwin;
static int running;
static unsigned int numlockmask;
static int (*xerrorxlib)(Display *, XErrorEvent *);

static XftFont *font;
static GC gc;
static Visual *visual;
static Colormap cmap;
static int bh; /* bar height */
static unsigned long bg_norm, bg_sel, bg_urg, border_norm, border_sel;
static XftColor xftfg_norm, xftfg_sel;
static Window tagwin, statuswin;
static int showbar = 1;

static Atom wmatom[WMLast], netatom[NetLast];
static char stext[256];

static int wx, wy, ww, wh; /* window area: the screen minus the bar */
static int seltag;
static int prevtag; /* the tag we came from, for MODKEY+Tab */
static Client *taghead[TAGS];
static Client *sel;
static Client *dragc;
static int dragbutton, dragorigx, dragorigy, dragwx, dragwy, dragww, dragwh;

/* ---- screen geometry / tags ---- */

int computevis(int *vis, int *widths) {
	int nvis = 0, t;

	for (t = 0; t < TAGS; t++)
		if (taghead[t] || t == seltag) {
			vis[nvis] = t;
			widths[nvis] = textw(tags[t]);
			nvis++;
		}
	return nvis;
}

void updatebarpos(void) {
	/* every gap (external, around the bars; internal, bar-to-window and
	 * window-to-edge) is one bar height wide, so they read as a uniform
	 * grid; adjoining gaps (e.g. below the bar) simply add up. */
	wx = 0;
	ww = sw;
	if (showbar) {
		wy = 2 * bh + 2 * borderpx;
		wh = sh - 2 * bh - 2 * borderpx;
	} else {
		wy = 0;
		wh = sh;
	}
}

/* smawm asks the server how big the screen is and nothing else. Xinerama
 * would report the monitors inside it; not asking is what makes a dual-head
 * setup behave as one wide screen, and is why there is no Monitor type. */
int updategeom(void) {
	int w = DisplayWidth(dpy, screen), h = DisplayHeight(dpy, screen);

	if (w == sw && h == sh)
		return 0;
	sw = w;
	sh = h;
	updatebarpos();
	return 1;
}

/* ---- client list management ---- */

void attach(Client *c) {
	Client **head = &taghead[c->tag];

	if (*head) {
		Client *h = *head;
		c->prev = h->prev;
		c->next = h;
		h->prev->next = c;
		h->prev = c;
	} else {
		*head = c;
		c->prev = c->next = c;
	}
}

void detach(Client *c) {
	Client **head = &taghead[c->tag];

	if (c->next == c) {
		*head = NULL;
	} else {
		c->prev->next = c->next;
		c->next->prev = c->prev;
		if (*head == c)
			*head = c->next;
	}
	c->next = c->prev = NULL;
}

Client *wintoclient(Window w) {
	int t;

	for (t = 0; t < TAGS; t++)
		FOREACH(c, t)
			if (c->win == w)
				return c;
	return NULL;
}

/* tell a client where it actually ended up. ICCCM requires this whenever the
 * window is moved without being resized, or a ConfigureRequest is not granted
 * as asked; dwm calls it configure(). Without it clients never learn their
 * absolute position (menus and popups then open in the wrong place) and keep
 * re-asking for the geometry they wanted. */
void configure(Client *c) {
	XConfigureEvent ce;

	ce.type = ConfigureNotify;
	ce.display = dpy;
	ce.event = c->win;
	ce.window = c->win;
	ce.x = c->x;
	ce.y = c->y;
	ce.width = c->w;
	ce.height = c->h;
	ce.border_width = c->bw;
	ce.above = None;
	ce.override_redirect = False;
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

/* dwm's applysizehints, minus the tiling branch (everything floats here, so
 * WM_NORMAL_HINTS always apply) and minus the base/increment/aspect
 * arithmetic - see updatesizehints() for why those were dropped. */
int applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact) {
	*w = MAX(1, *w);
	*h = MAX(1, *h);
	if (interact) {
		if (*x > sw)
			*x = sw - WIDTH(c);
		if (*y > sh)
			*y = sh - HEIGHT(c);
		if (*x + *w + 2 * c->bw < 0)
			*x = 0;
		if (*y + *h + 2 * c->bw < 0)
			*y = 0;
	} else {
		if (*x >= wx + ww)
			*x = wx + ww - WIDTH(c);
		if (*y >= wy + wh)
			*y = wy + wh - HEIGHT(c);
		if (*x + *w + 2 * c->bw <= wx)
			*x = wx;
		if (*y + *h + 2 * c->bw <= wy)
			*y = wy;
	}
	if (*h < bh)
		*h = bh;
	if (*w < bh)
		*w = bh;
	*w = MAX(*w, c->minw);
	*h = MAX(*h, c->minh);
	if (c->maxw)
		*w = MIN(*w, c->maxw);
	if (c->maxh)
		*h = MIN(*h, c->maxh);
	return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

void resize(Client *c, int x, int y, int w, int h, int interact) {
	if (applysizehints(c, &x, &y, &w, &h, interact))
		resizeclient(c, x, y, w, h);
}

void resizeclient(Client *c, int x, int y, int w, int h) {
	XWindowChanges wc;

	c->x = wc.x = x;
	c->y = wc.y = y;
	c->w = wc.width = MAX(w, 1);
	c->h = wc.height = MAX(h, 1);
	wc.border_width = c->bw;
	XConfigureWindow(dpy, c->win, CWX|CWY|CWWidth|CWHeight|CWBorderWidth, &wc);
	configure(c);
	XSync(dpy, False);
}

void manage(Window w) {
	XWindowAttributes wa;
	Client *c, *t = NULL;
	Window trans = None;

	if (!XGetWindowAttributes(dpy, w, &wa) || wa.override_redirect)
		return;
	if (wintoclient(w))
		return;

	if (!(c = calloc(1, sizeof(Client))))
		exit(1);
	c->win = w;
	c->x = wa.x;
	c->y = wa.y;
	c->w = wa.width;
	c->h = wa.height;
	c->bw = borderpx;

	/* dwm: a transient (dialog, file picker, ...) belongs wherever the
	 * window it is transient for lives, not on whatever tag is selected */
	if (XGetTransientForHint(dpy, w, &trans) && (t = wintoclient(trans)))
		c->tag = t->tag;
	else
		c->tag = seltag;

	if (c->x == 0 && c->y == 0) {
		c->x = wx + MAX(0, (ww - WIDTH(c)) / 2);
		c->y = wy + MAX(0, (wh - HEIGHT(c)) / 2);
	}
	/* dwm keeps a new window inside the screen it opens on */
	if (c->x + WIDTH(c) > wx + ww)
		c->x = wx + ww - WIDTH(c);
	if (c->y + HEIGHT(c) > wy + wh)
		c->y = wy + wh - HEIGHT(c);
	c->x = MAX(c->x, wx);
	c->y = MAX(c->y, wy);

	c->oldx = c->x;
	c->oldy = c->y;
	c->oldw = c->w;
	c->oldh = c->h;

	XSetWindowBorderWidth(dpy, w, c->bw);
	drawborder(c, 0);
	configure(c); /* propagates border_width, if size doesn't change */
	updatewindowtype(c); /* may already want to be fullscreen */
	updatesizehints(c);
	updatewmhints(c); /* it may be asking for attention before we ever see it */
	XSelectInput(dpy, w, EnterWindowMask|PropertyChangeMask|StructureNotifyMask);
	attach(c);

	if (!c->isfull) /* updatewindowtype already sized a fullscreen window */
		resizeclient(c, c->x, c->y, c->w, c->h);
	setclientstate(c, NormalState);

	showhide(c, ISVISIBLE(c));
	if (ISVISIBLE(c))
		focus(c);
	drawbar();
}

/* Adopt the windows that were already on screen when smawm started, so
 * restarting the window manager does not orphan everything that was running.
 * This is sowm's win_init, from its `watch` branch: ask the server for the
 * root's children and manage each one. dwm's scan() does the same job in 39
 * sloc with getstate(), reading WM_STATE so a window the previous WM left
 * iconified comes back iconified; smawm has no iconified state, so that round
 * trip buys nothing here.
 *
 * What is kept from dwm is the IsViewable test. sowm maps every child it
 * finds, which would drag a window the application had deliberately hidden
 * back onto the screen - the same zombie unmanotify() exists to prevent.
 * manage() rejects override-redirect windows on its own, which is what keeps
 * the two bar windows out.
 *
 * Transients are managed in whatever order the server lists them, so one that
 * comes before the window it belongs to lands on the current tag instead of
 * its parent's. dwm spends a second pass over the list on that; at one dialog
 * on the wrong tag after a restart, smawm does not. */
void scan(void) {
	unsigned int i, n;
	Window d1, d2, *wins = NULL;
	XWindowAttributes wa;

	if (!XQueryTree(dpy, root, &d1, &d2, &wins, &n))
		return;
	for (i = 0; i < n; i++)
		if (XGetWindowAttributes(dpy, wins[i], &wa) && wa.map_state == IsViewable)
			manage(wins[i]);
	if (wins)
		XFree(wins);
}

void unmanage(Window w) {
	Client *c = wintoclient(w);
	int wasfocused;

	if (!c)
		return;
	if (dragc == c) /* never keep dragging a client we are about to free */
		dragc = NULL;
	if ((wasfocused = (sel == c)))
		sel = NULL;
	detach(c);
	free(c);
	if (wasfocused)
		focus(taghead[seltag]);
	drawbar();
}

/* ---- tags: switching, moving windows ---- */

/* smawm hides the tags you are not looking at by unmapping their windows,
 * which is sowm's ws_go; dwm instead parks them off screen. The difference
 * matters because an UnmapNotify then means two different things, so record
 * in c->hidden that this particular unmap was ours - see unmapnotify(). */
void showhide(Client *c, int show) {
	if (!c)
		return;
	c->hidden = !show;
	if (show)
		XMapWindow(dpy, c->win);
	else
		XUnmapWindow(dpy, c->win);
}

void viewtag(int tag) {
	if (tag < 0 || tag >= TAGS || tag == seltag)
		return;
	FOREACH(c, seltag)
		showhide(c, 0);
	prevtag = seltag;
	seltag = tag;
	FOREACH(c, tag)
		showhide(c, 1);
	focus(taghead[tag]);
	drawbar();
}

void view(const Arg *arg) {
	/* a negative index means "the tag I came from". dwm gets this from
	 * keeping two tag sets and flipping between them (view with arg 0);
	 * with a single seltag the same thing is one saved int. Because
	 * viewtag() then records the tag being left, pressing the key again
	 * comes straight back - so it cycles between the same two tags. */
	viewtag(arg->i < 0 ? prevtag : arg->i);
}

void movetotag(const Arg *arg) {
	Client *c = sel;
	int vis;

	if (!c || arg->i < 0 || arg->i >= TAGS || arg->i == c->tag)
		return;
	vis = ISVISIBLE(c);
	detach(c);
	c->tag = arg->i;
	attach(c);
	if (vis)
		showhide(c, 0);
	focus(taghead[seltag]);
	drawbar();
}

void focusstack(const Arg *arg) {
	Client *c = sel, *n;

	if (!c)
		return;
	n = arg->i > 0 ? c->next : c->prev;
	if (n != c) {
		focus(n);
		XRaiseWindow(dpy, n->win);
	}
}

/* ---- focus / borders ---- */

void drawborder(Client *c, int focused) {
	XSetWindowBorder(dpy, c->win, focused ? border_sel : border_norm);
}

/* Focus does NOT raise, in either parent: dwm raises in restack() and sowm
 * on a mod+click, and both leave win_focus/focus to the keyboard alone. smawm
 * used to raise here, and since enternotify() calls focus(), the raise put a
 * different window under the pointer, the server sent the crossing event that
 * follows from that, and it came straight back in as another focus + raise.
 * With a fullscreen window overlapping another one on the same tag, every
 * restack flips which window the pointer is in, so the two windows traded
 * places ~40k times a second: the event loop never went idle, the bar stopped
 * being redrawn and the screen showed nothing but flickering borders. Raising
 * is now the four deliberate acts below - mod+click, mod+j/k, maximizing and
 * a window going fullscreen - none of which an X event can trigger in turn. */
void focus(Client *c) {
	/* focusing an unmapped window is a BadMatch that leaves the keyboard
	 * pointing at nothing - dwm guards the same way */
	if (c && !ISVISIBLE(c))
		c = NULL;
	if (sel && sel != c)
		drawborder(sel, 0);
	sel = c;
	if (c) {
		/* looking at the window is what answers its request for attention.
		 * Guarded: focus() runs on every crossing, and updatewmhints is a
		 * server round trip. */
		if (c->isurgent)
			updatewmhints(c);
		drawborder(c, 1);
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
		XChangeProperty(dpy, root, netatom[NetActiveWindow], XA_WINDOW, 32,
				PropModeReplace, (unsigned char *)&(c->win), 1);
	} else {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
}

/* ---- maximize / fullscreen ---- */

/* fill the window area, a bar height of gap on every side - the same gap
 * updatebarpos leaves around the bar itself */
void resizemax(Client *c) {
	resize(c, wx + bh, wy + bh,
			ww - 2 * c->bw - 2 * bh,
			wh - 2 * c->bw - 2 * bh, 0);
}

void togglemax(const Arg *arg) {
	Client *c = sel;

	(void)arg;
	if (!c || c->isfull)
		return;
	if (c->ismax) {
		c->ismax = 0;
		resize(c, c->oldx, c->oldy, c->oldw, c->oldh, 0);
	} else {
		c->oldx = c->x;
		c->oldy = c->y;
		c->oldw = c->w;
		c->oldh = c->h;
		c->ismax = 1;
		resizemax(c);
		XRaiseWindow(dpy, c->win);
	}
}

void setfullscreen(Client *c, int full) {
	if (full && !c->isfull) {
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
				PropModeReplace, (unsigned char *)&netatom[NetWMStateFullscreen], 1);
		/* a maximized window is already keeping its natural size in old*;
		 * overwriting it here would make un-fullscreening restore to the
		 * maximized geometry and lose the real one for good */
		if (!c->ismax) {
			c->oldx = c->x;
			c->oldy = c->y;
			c->oldw = c->w;
			c->oldh = c->h;
		}
		c->isfull = 1;
		c->oldbw = c->bw;
		c->bw = 0;
		/* resizeclient, not resize: a fullscreen window gets the screen
		 * exactly, size hints do not get a say */
		resizeclient(c, 0, 0, sw, sh);
		XRaiseWindow(dpy, c->win);
	} else if (!full && c->isfull) {
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
				PropModeReplace, (unsigned char *)0, 0);
		c->isfull = 0;
		c->bw = c->oldbw;
		if (c->ismax)
			resizemax(c);
		else
			resizeclient(c, c->oldx, c->oldy, c->oldw, c->oldh);
	}
}

void togglefullscreen(const Arg *arg) {
	Client *c = sel;

	(void)arg;
	if (c)
		setfullscreen(c, !c->isfull);
}

/* ---- bar ---- */

void togglebar(const Arg *arg) {
	int t;

	(void)arg;
	showbar = !showbar;
	updatebarpos();
	/* maximized clients fill wx/wy/ww/wh, which updatebarpos just moved */
	for (t = 0; t < TAGS; t++)
		FOREACH(c, t)
			if (c->ismax)
				resizemax(c);
	drawbar();
}

int textw(const char *s) {
	XGlyphInfo ext;

	XftTextExtentsUtf8(dpy, font, (const FcChar8 *)s, (int)strlen(s), &ext);
	return ext.xOff + 16;
}

/* The bar shows which tags exist and which one is selected - nothing that
 * depends on the focused window - so drawbar only runs when a tag gains or
 * loses clients, when the selection changes, on Expose, and when the status
 * string changes (once a second, from a status script). Painting the
 * background and then the text straight onto the bar window makes that
 * visible as flicker; dwm's drw.c avoids it by drawing into an off-screen
 * pixmap and blitting the finished result, which is what these two helpers
 * do. */
static Pixmap barpixmap(int w) {
	return XCreatePixmap(dpy, root, MAX(w, 1), bh, DefaultDepth(dpy, screen));
}

static void barblit(Pixmap pm, Window win, int w) {
	XCopyArea(dpy, pm, win, gc, 0, 0, MAX(w, 1), bh, 0, 0);
	XFreePixmap(dpy, pm);
}

/* one cell of the bar: a filled box with a string in it. The tag cells and
 * the status cell differ only in their colours. */
static void barcell(Pixmap pm, XftDraw *xd, int x, int w, const char *s, int cur,
		int urg) {
	XSetForeground(dpy, gc, urg ? bg_urg : cur ? bg_sel : bg_norm);
	XFillRectangle(dpy, pm, gc, x, 0, w, bh);
	XftDrawStringUtf8(xd, cur || urg ? &xftfg_sel : &xftfg_norm, font, x + 8,
			font->ascent + 2, (const FcChar8 *)s, (int)strlen(s));
}

void drawbar(void) {
	int vis[TAGS], widths[TAGS], nvis, i, x, tw;
	Pixmap pm;
	XftDraw *xd;

	if (!showbar) {
		XUnmapWindow(dpy, tagwin);
		XUnmapWindow(dpy, statuswin);
		return;
	}

	XMapWindow(dpy, tagwin);
	XMapWindow(dpy, statuswin);

	nvis = computevis(vis, widths);
	tw = 0;
	for (i = 0; i < nvis; i++)
		tw += widths[i];
	tw = MAX(tw, 1);
	XMoveResizeWindow(dpy, tagwin, bh, bh, tw, bh);
	pm = barpixmap(tw);
	xd = XftDrawCreate(dpy, pm, visual, cmap);
	x = 0;
	for (i = 0; i < nvis; i++) {
		int urg = 0;

		FOREACH(c, vis[i])
			if (c->isurgent) {
				urg = 1;
				break;
			}
		barcell(pm, xd, x, widths[i], tags[vis[i]], vis[i] == seltag, urg);
		x += widths[i];
	}
	XftDrawDestroy(xd);
	barblit(pm, tagwin, tw);

	tw = MAX(textw(stext), 1);
	XMoveResizeWindow(dpy, statuswin, sw - tw - bh - 2 * borderpx, bh, tw, bh);
	pm = barpixmap(tw);
	xd = XftDrawCreate(dpy, pm, visual, cmap);
	barcell(pm, xd, 0, tw, stext, 0, 0);
	XftDrawDestroy(xd);
	barblit(pm, statuswin, tw);
}

void updatestatus(void) {
	XTextProperty tp;

	if (XGetWMName(dpy, root, &tp) && tp.value && tp.nitems) {
		strncpy(stext, (char *)tp.value, sizeof(stext) - 1);
		stext[sizeof(stext) - 1] = '\0';
		XFree(tp.value);
	} else {
		strcpy(stext, "smawm");
	}
	drawbar();
}

/* ---- mouse move/resize (from sowm) ---- */

void buttonpress(XEvent *e) {
	XButtonEvent *ev = &e->xbutton;
	Client *c;

	if (ev->window == tagwin) {
		int vis[TAGS], widths[TAGS], nvis, i, x = 0;
		nvis = computevis(vis, widths);
		for (i = 0; i < nvis; i++) {
			if (ev->x < x + widths[i]) {
				viewtag(vis[i]);
				return;
			}
			x += widths[i];
		}
		return;
	}
	if (!ev->subwindow)
		return;
	c = wintoclient(ev->subwindow);
	if (!c)
		return;
	focus(c);
	XRaiseWindow(dpy, c->win);
	/* dwm's movemouse simply refuses a fullscreen window, which leaves no
	 * way to move one but to un-fullscreen it by hand first. Telegram's
	 * image preview opens fullscreen (it sets _NET_WM_STATE_FULLSCREEN
	 * before mapping), so that refusal is what "I cannot move the preview"
	 * turned out to be. Taking hold of a window is a clear enough statement
	 * that you want it windowed, so drop it out of fullscreen and drag it. */
	if (c->isfull)
		setfullscreen(c, 0);
	c->ismax = 0; /* dragging it is how you stop it being maximized */
	dragc = c;
	dragbutton = ev->button;
	dragorigx = ev->x_root;
	dragorigy = ev->y_root;
	dragwx = c->x;
	dragwy = c->y;
	dragww = c->w;
	dragwh = c->h;
}

void buttonrelease(XEvent *e) {
	(void)e;
	dragc = NULL;
}

void motionnotify(XEvent *e) {
	int xd, yd;

	if (!dragc || dragc->isfull)
		return;
	while (XCheckTypedEvent(dpy, MotionNotify, e));
	xd = e->xbutton.x_root - dragorigx;
	yd = e->xbutton.y_root - dragorigy;
	if (dragbutton == 1)
		resize(dragc, dragwx + xd, dragwy + yd, dragww, dragwh, 1);
	else if (dragbutton == 3)
		resize(dragc, dragwx, dragwy, dragww + xd, dragwh + yd, 1);
}

/* ---- misc event handlers ---- */

/* dwm's configurerequest. The version this replaced was sowm's, which just
 * forwards the request to the server with the mask the client sent: that is
 * safe in sowm, which tracks no geometry and draws no borders, but here it
 * let a client wipe its own border (CWBorderWidth), restack itself over the
 * bar (CWSibling/CWStackMode), and undo whatever the WM had just done -
 * which is what made a self-positioning window impossible to drag. */
void configurerequest(XEvent *e) {
	XConfigureRequestEvent *ev = &e->xconfigurerequest;
	Client *c;

	if ((c = wintoclient(ev->window))) {
		/* CWBorderWidth is deliberately never passed on: dwm lets a client
		 * set its own border width, but smawm has a single borderpx for
		 * everything, and toolkits routinely ask for 0 - which is how the
		 * focus border silently disappeared off windows */
		if (c->isfull || c == dragc) {
			/* A fullscreen window asked for that state; it does not also
			 * get to pick where fullscreen is. The same applies while the
			 * pointer is dragging a window: some clients re-assert their
			 * own position on every ConfigureNotify, so honouring the
			 * request mid-drag means the window snaps back once per motion
			 * event and cannot be moved at all. dwm has the fullscreen half
			 * of this and fights the client in the drag case; smawm lets
			 * the drag win for as long as the button is held. */
			configure(c);
		} else {
			if (ev->value_mask & CWX)
				c->x = ev->x;
			if (ev->value_mask & CWY)
				c->y = ev->y;
			if (ev->value_mask & CWWidth)
				c->w = ev->width;
			if (ev->value_mask & CWHeight)
				c->h = ev->height;
			if ((c->x + c->w) > sw)
				c->x = sw / 2 - WIDTH(c) / 2; /* center in x */
			if ((c->y + c->h) > sh)
				c->y = sh / 2 - HEIGHT(c) / 2; /* center in y */
			if ((ev->value_mask & (CWX|CWY)) && !(ev->value_mask & (CWWidth|CWHeight)))
				configure(c);
			if (ISVISIBLE(c))
				XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
		}
	} else {
		/* not ours (an override-redirect window, or one not managed yet):
		 * grant it as asked, same as sowm and dwm both do */
		XWindowChanges wc;

		wc.x = ev->x;
		wc.y = ev->y;
		wc.width = ev->width;
		wc.height = ev->height;
		wc.border_width = ev->border_width;
		wc.sibling = ev->above;
		wc.stack_mode = ev->detail;
		XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
	}
	XSync(dpy, False);
}

void configurenotify(XEvent *e) {
	if (e->xconfigure.window == root && updategeom())
		drawbar();
}

void maprequest(XEvent *e) {
	Window w = e->xmaprequest.window;
	if (!wintoclient(w))
		manage(w);
}

void destroynotify(XEvent *e) {
	unmanage(e->xdestroywindow.window);
}

/* The handler smawm was missing entirely. Plenty of programs close a window
 * by unmapping it and keeping it around to show again later - Qt's hide() is
 * exactly this, which is what Telegram's image preview does. Without this,
 * the client stayed on its tag forever: the tag bar kept showing that tag as
 * occupied ("like I hadn't changed tags"), switching to it mapped the closed
 * window back onto the screen, focus could land on a window that was not
 * there, and the app's own request to show it again was swallowed by
 * maprequest, because smawm still thought it was managing it. */
void unmapnotify(XEvent *e) {
	XUnmapEvent *ev = &e->xunmap;
	Client *c;

	if (!(c = wintoclient(ev->window)))
		return;
	if (c->hidden && !ev->send_event)
		return; /* our own unmap, from a tag switch - see showhide() */
	/* a synthetic UnmapNotify is a client withdrawing the window (ICCCM
	 * 4.1.4); a real one means it is gone from the screen. Either way it is
	 * no longer ours to manage. */
	unmanage(ev->window);
}

void enternotify(XEvent *e) {
	Client *c;
	XCrossingEvent *ev = &e->xcrossing;

	/* ignore crossings that aren't real pointer movement into a window (e.g.
	 * generated when a window is destroyed/unmapped and the pointer is left
	 * hovering whatever is newly revealed underneath) - dwm filters these
	 * the same way, and without it a closed window can silently steal focus
	 * or leave selection state out of sync with what's actually on screen.
	 * sowm's "skip to the newest EnterNotify in the queue" loop used to run
	 * before this test, so a batch of crossings ending in a grab-generated
	 * one (every MODKEY+drag ends in one) threw away the real crossing with
	 * it and focus quietly stopped following the mouse. dwm does not
	 * compress; neither do we any more. */
	if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) && ev->window != root)
		return;
	if (!(c = wintoclient(ev->window)) || c == sel)
		return;
	focus(c);
}

void expose(XEvent *e) {
	if (e->xexpose.count == 0 &&
	    (e->xexpose.window == tagwin || e->xexpose.window == statuswin))
		drawbar();
}

/* Two properties are watched. The root window's name is how a status script
 * feeds the bar (xsetroot -name, in a loop); WM_HINTS on a client is how an
 * application asks for attention. dwm also tracks size-hint and window-type
 * changes here; those went with the rest of the ICCCM refinements - size
 * hints are read once when the window is managed, and fullscreen still
 * arrives as a ClientMessage. */
void propertynotify(XEvent *e) {
	XPropertyEvent *ev = &e->xproperty;
	Client *c;

	if (ev->window == root && ev->atom == XA_WM_NAME)
		updatestatus();
	else if (ev->state != PropertyDelete && ev->atom == XA_WM_HINTS &&
			(c = wintoclient(ev->window))) {
		updatewmhints(c);
		drawbar();
	}
}

void clientmessage(XEvent *e) {
	XClientMessageEvent *cme = &e->xclient;
	Client *c = wintoclient(cme->window);

	if (!c || cme->message_type != netatom[NetWMState])
		return;
	if ((Atom)cme->data.l[1] == netatom[NetWMStateFullscreen] ||
	    (Atom)cme->data.l[2] == netatom[NetWMStateFullscreen]) {
		int add = cme->data.l[0] == 1 || (cme->data.l[0] == 2 && !c->isfull);
		setfullscreen(c, add);
	}
}

void mappingnotify(XEvent *e) {
	XMappingEvent *ev = &e->xmapping;

	XRefreshKeyboardMapping(ev);
	if (ev->request == MappingKeyboard)
		grabinput();
}

void keypress(XEvent *e) {
	XKeyEvent *ev = &e->xkey;
	KeySym keysym = XkbKeycodeToKeysym(dpy, ev->keycode, 0, 0);
	unsigned int i;

	for (i = 0; i < LENGTH(keys); i++)
		if (keysym == keys[i].keysym &&
		    CLEANMASK(keys[i].mod) == CLEANMASK(ev->state) && keys[i].func)
			keys[i].func(&keys[i].arg);
}

/* ---- spawning / killing / quitting ---- */

void spawn(const Arg *arg) {
	struct sigaction sa;

	if (fork() != 0)
		return; /* the parent, or fork failed */
	if (dpy)
		close(ConnectionNumber(dpy));
	setsid();
	/* an *ignored* signal disposition survives execve (a handler does not),
	 * so without this the program we launch inherits SIGCHLD=SIG_IGN and
	 * cannot wait() for its own children. dwm fixed this in e81f17d after
	 * reports of mpv & co. refusing to start. */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = SIG_DFL;
	sigaction(SIGCHLD, &sa, NULL);
	execvp(((char **)arg->com)[0], (char **)arg->com);
	_exit(1);
}

void killclient(const Arg *arg) {
	Client *c = sel;

	(void)arg;
	if (!c)
		return;
	/* sowm does exactly this. Without the WM_DELETE_WINDOW handshake an
	 * application is killed outright rather than asked to close, so it gets
	 * no chance to save first. */
	XKillClient(dpy, c->win);
}

void quit(const Arg *arg) {
	(void)arg;
	running = 0;
}

/* ---- client properties (all from dwm) ---- */

/* WM_STATE. ICCCM requires the WM to keep this on every window it manages;
 * toolkits read it to tell "mapped" from "iconified" from "not managed". */
void setclientstate(Client *c, long state) {
	long data[] = { state, None };

	XChangeProperty(dpy, c->win, wmatom[WMState], wmatom[WMState], 32,
			PropModeReplace, (unsigned char *)data, 2);
}

Atom getatomprop(Client *c, Atom prop) {
	int di;
	unsigned long dl;
	unsigned char *p = NULL;
	Atom da, atom = None;

	if (XGetWindowProperty(dpy, c->win, prop, 0L, sizeof(atom), False, XA_ATOM,
			&da, &di, &dl, &dl, &p) == Success && p) {
		atom = *(Atom *)p;
		XFree(p);
	}
	return atom;
}

/* A window is allowed to be fullscreen from the moment it is mapped, by
 * having _NET_WM_STATE set before the map - that is the normal way an app
 * opens a fullscreen viewer. smawm only ever listened for the client message
 * form, so such a window was managed as an ordinary one: the app painted
 * itself as fullscreen inside a window that was not, and dragging it started
 * a tug of war with the app over its geometry. */
void updatewindowtype(Client *c) {
	if (getatomprop(c, netatom[NetWMState]) == netatom[NetWMStateFullscreen])
		setfullscreen(c, 1);
	/* dwm also floats _NET_WM_WINDOW_TYPE_DIALOG here; everything floats in
	 * smawm, so a dialog needs nothing special */
}

/* WM_NORMAL_HINTS, reduced to the two limits clients here actually set.
 * dwm reads the base size, the resize increments and the aspect ratio as
 * well, because in dwm they decide whether a window may be tiled at all
 * (a fixed-size window is forced to float). smawm floats everything, so
 * that decision does not exist, and measuring what real clients publish
 * found nothing left for the arithmetic to do: st asks for an increment
 * of 1x1 because it wants smooth resizing rather than cell stepping, and
 * nothing sets an aspect ratio or a base distinct from its minimum. See
 * FEATURES.txt. ICCCM lets min and base stand in for each other, so a
 * client that publishes only a base still yields a minimum here. */
/* dwm's updatewmhints, the urgency half of it. An application that wants you
 * while you are looking at another tag raises XUrgencyHint on its own window;
 * making that visible is the window manager's job, and with one tag on screen
 * and vacant tags hidden from the bar it is the only signal there is that
 * something happened elsewhere. dwm's version also reads the input hint into
 * c->neverfocus, which went with the rest of WM_HINTS in the tier 2 cut. */
void updatewmhints(Client *c) {
	XWMHints *wmh;

	if (!(wmh = XGetWMHints(dpy, c->win)))
		return;
	if (c == sel && (wmh->flags & XUrgencyHint)) {
		/* you are already looking at it, so answer on the client's own
		 * window too - left set, the hint fires again on every property
		 * change and the tag never stops being urgent */
		wmh->flags &= ~XUrgencyHint;
		XSetWMHints(dpy, c->win, wmh);
		c->isurgent = 0;
	} else {
		c->isurgent = (wmh->flags & XUrgencyHint) != 0;
	}
	XFree(wmh);
}

void updatesizehints(Client *c) {
	long msize;
	XSizeHints size;

	if (!XGetWMNormalHints(dpy, c->win, &size, &msize))
		size.flags = PSize; /* size is uninitialized, ignore its flags */
	if (size.flags & PMinSize) {
		c->minw = size.min_width;
		c->minh = size.min_height;
	} else if (size.flags & PBaseSize) {
		c->minw = size.base_width;
		c->minh = size.base_height;
	} else {
		c->minw = c->minh = 0;
	}
	if (size.flags & PMaxSize) {
		c->maxw = size.max_width;
		c->maxh = size.max_height;
	} else {
		c->maxw = c->maxh = 0;
	}
}

/* ---- setup ---- */

/* dwm's error handlers. The old one returned 0 for everything, which hid
 * every real X error and made a second window manager on the same display
 * look like it had started fine. */
int xerror(Display *d, XErrorEvent *ee) {
	if (ee->error_code == BadWindow
	    || (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch)
	    || (ee->request_code == X_PolyText8 && ee->error_code == BadDrawable)
	    || (ee->request_code == X_PolyFillRectangle && ee->error_code == BadDrawable)
	    || (ee->request_code == X_PolySegment && ee->error_code == BadDrawable)
	    || (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch)
	    || (ee->request_code == X_GrabButton && ee->error_code == BadAccess)
	    || (ee->request_code == X_GrabKey && ee->error_code == BadAccess)
	    || (ee->request_code == X_CopyArea && ee->error_code == BadDrawable))
		return 0;
	fprintf(stderr, "smawm: fatal error: request code=%d, error code=%d\n",
			ee->request_code, ee->error_code);
	return xerrorxlib(d, ee); /* may call exit */
}

int xerrorstart(Display *d, XErrorEvent *ee) {
	(void)d;
	(void)ee;
	fprintf(stderr, "smawm: another window manager is already running\n");
	exit(1);
	return -1;
}

void checkotherwm(void) {
	xerrorxlib = XSetErrorHandler(xerrorstart);
	/* this causes an error if some other window manager is running */
	XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);
	XSync(dpy, False);
	XSetErrorHandler(xerror);
	XSync(dpy, False);
}

/* hand every window back mapped and unmanaged, so quitting smawm doesn't
 * strand the windows sitting on inactive tags. The list is mutated as we go,
 * so this cannot use FOREACH. */
void cleanup(void) {
	Client *c;
	int t;

	for (t = 0; t < TAGS; t++)
		while ((c = taghead[t])) {
			showhide(c, 1);
			unmanage(c->win);
		}
	XDestroyWindow(dpy, tagwin);
	XDestroyWindow(dpy, statuswin);
	XDestroyWindow(dpy, wmcheckwin);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
}

void updatenumlockmask(void) {
	XModifierKeymap *modmap = XGetModifierMapping(dpy);
	int i, j;

	numlockmask = 0;
	for (i = 0; i < 8; i++)
		for (j = 0; j < modmap->max_keypermod; j++)
			if (modmap->modifiermap[i * modmap->max_keypermod + j] ==
			    XKeysymToKeycode(dpy, XK_Num_Lock))
				numlockmask = 1 << i;
	XFreeModifiermap(modmap);
}

void grabinput(void) {
	unsigned int i, j, modifiers[4];
	KeyCode code;

	updatenumlockmask();
	modifiers[0] = 0;
	modifiers[1] = LockMask;
	modifiers[2] = numlockmask;
	modifiers[3] = numlockmask | LockMask;

	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	for (i = 0; i < LENGTH(keys); i++)
		if ((code = XKeysymToKeycode(dpy, keys[i].keysym)))
			for (j = 0; j < LENGTH(modifiers); j++)
				XGrabKey(dpy, code, keys[i].mod | modifiers[j], root,
						True, GrabModeAsync, GrabModeAsync);

	XUngrabButton(dpy, AnyButton, AnyModifier, root);
	for (i = 1; i < 4; i += 2)
		for (j = 0; j < LENGTH(modifiers); j++)
			XGrabButton(dpy, i, MODKEY | modifiers[j], root, True,
					ButtonPressMask|ButtonReleaseMask|PointerMotionMask,
					GrabModeAsync, GrabModeAsync, None, None);
}

static unsigned long getcolor(const char *name) {
	XColor c;

	return XAllocNamedColor(dpy, cmap, name, &c, &c) ? c.pixel
	                                                 : BlackPixel(dpy, screen);
}

void setup(void) {
	Atom utf8string;
	struct sigaction sa;
	XSetWindowAttributes wa;

	/* do not turn the programs we spawn into zombies (dwm) */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);
	while (waitpid(-1, NULL, WNOHANG) > 0); /* zombies inherited from .xinitrc */

	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	visual = DefaultVisual(dpy, screen);
	cmap = DefaultColormap(dpy, screen);

	font = XftFontOpenName(dpy, screen, fontname);
	if (!font)
		font = XftFontOpenName(dpy, screen, "monospace:size=10");
	if (!font) {
		fprintf(stderr, "smawm: no fonts could be loaded\n");
		exit(1);
	}
	bh = font->ascent + font->descent + 4;
	gc = XCreateGC(dpy, root, 0, NULL);

	bg_norm = getcolor(col_bg_norm);
	bg_sel = getcolor(col_bg_sel);
	bg_urg = getcolor(col_bg_urg);
	border_norm = getcolor(col_border_norm);
	border_sel = getcolor(col_border_sel);
	XftColorAllocName(dpy, visual, cmap, col_fg_norm, &xftfg_norm);
	XftColorAllocName(dpy, visual, cmap, col_fg_sel, &xftfg_sel);

	utf8string = XInternAtom(dpy, "UTF8_STRING", False);
	wmatom[WMState] = XInternAtom(dpy, "WM_STATE", False);
	netatom[NetActiveWindow] = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	netatom[NetSupported] = XInternAtom(dpy, "_NET_SUPPORTED", False);
	netatom[NetWMName] = XInternAtom(dpy, "_NET_WM_NAME", False);
	netatom[NetWMState] = XInternAtom(dpy, "_NET_WM_STATE", False);
	netatom[NetWMCheck] = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
	netatom[NetWMStateFullscreen] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	netatom[NetWMWindowType] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	netatom[NetWMWindowTypeDialog] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);

	/* the two bar windows, made once - there is only ever one screen.
	 * drawbar() sizes and places them; they start 1px wide. */
	wa.override_redirect = True;
	wa.background_pixel = bg_norm;
	wa.border_pixel = border_sel;
	wa.event_mask = ExposureMask;
	tagwin = XCreateWindow(dpy, root, 0, 0, 1, bh, borderpx, CopyFromParent,
			CopyFromParent, CopyFromParent,
			CWOverrideRedirect|CWBackPixel|CWBorderPixel|CWEventMask, &wa);
	statuswin = XCreateWindow(dpy, root, 0, 0, 1, bh, borderpx, CopyFromParent,
			CopyFromParent, CopyFromParent,
			CWOverrideRedirect|CWBackPixel|CWBorderPixel|CWEventMask, &wa);
	XSelectInput(dpy, tagwin, ExposureMask|ButtonPressMask);
	XSelectInput(dpy, statuswin, ExposureMask|ButtonPressMask);
	updategeom();

	/* Tell the world an EWMH window manager is here, dwm's setup() verbatim.
	 * smawm advertised nothing at all, so toolkits concluded there was no
	 * EWMH WM running: Qt and GTK then stop asking for fullscreen through
	 * _NET_WM_STATE_FULLSCREEN (which clientmessage() handles) and go do it
	 * themselves by resizing to the screen and raising - permanently at odds
	 * with the WM. An image viewer opened from such an app is the worst case
	 * of it. */
	wmcheckwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMCheck], XA_WINDOW, 32,
			PropModeReplace, (unsigned char *)&wmcheckwin, 1);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMName], utf8string, 8,
			PropModeReplace, (unsigned char *)"smawm", 5);
	XChangeProperty(dpy, root, netatom[NetWMCheck], XA_WINDOW, 32,
			PropModeReplace, (unsigned char *)&wmcheckwin, 1);
	XChangeProperty(dpy, root, netatom[NetSupported], XA_ATOM, 32,
			PropModeReplace, (unsigned char *)netatom, NetLast);

	XSelectInput(dpy, root, SubstructureRedirectMask|SubstructureNotifyMask|
			PropertyChangeMask|EnterWindowMask|ButtonPressMask);
	XDefineCursor(dpy, root, XCreateFontCursor(dpy, XC_left_ptr));
	grabinput();
	updatestatus();
}

static void (*handler[LASTEvent])(XEvent *e) = {
	[ButtonPress]      = buttonpress,
	[ButtonRelease]    = buttonrelease,
	[ClientMessage]    = clientmessage,
	[ConfigureNotify]  = configurenotify,
	[ConfigureRequest] = configurerequest,
	[DestroyNotify]    = destroynotify,
	[EnterNotify]      = enternotify,
	[Expose]           = expose,
	[KeyPress]         = keypress,
	[MapRequest]       = maprequest,
	[MappingNotify]    = mappingnotify,
	[MotionNotify]     = motionnotify,
	[PropertyNotify]   = propertynotify,
	[UnmapNotify]      = unmapnotify,
};

void run(void) {
	XEvent ev;

	running = 1;
	while (running && !XNextEvent(dpy, &ev))
		if (handler[ev.type])
			handler[ev.type](&ev);
}

int main(void) {
	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "smawm: cannot open display\n");
		return 1;
	}
	checkotherwm();
	setup();
	scan();
	XSync(dpy, False);
	run();
	cleanup();
	XCloseDisplay(dpy);
	return 0;
}
