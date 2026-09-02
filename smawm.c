/* smawm - a small floating window manager.
 *
 * Client/monitor/tag handling is dwm's model trimmed to a single active tag
 * per monitor (no bitmask, no tiling). Mouse move/resize, the event grabbing
 * trick (MODKEY+button grabbed on root, subwindow tells us what was
 * clicked), and the overall shape of the code come from sowm. The bar is
 * drawn with Xft (for real fonts like CozetteVector), but without dwm's
 * drw.c abstraction: it keeps drw.c's off-screen pixmap, since drawing
 * straight onto the bar windows flickers, and drops the rest. There are two
 * bar windows per monitor - a left one for tags, a right one for status
 * text.
 *
 * The ICCCM/EWMH plumbing (WM_STATE, synthetic ConfigureNotify, _NET_*
 * properties, UnmapNotify, size hints) is dwm's, unchanged: it is the part
 * sowm leaves out and the part real toolkit applications need.
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xft/Xft.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xinerama.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "smawm.h"

enum { WMProtocols, WMDelete, WMState, WMTakeFocus, WMLast };
/* the whole netatom array is published as _NET_SUPPORTED, exactly as dwm
 * does it: without that (and _NET_SUPPORTING_WM_CHECK) toolkits decide no
 * EWMH window manager is running and start doing fullscreen/maximise by
 * hand, fighting whatever the WM does */
enum { NetActiveWindow, NetSupported, NetWMName, NetWMState, NetWMCheck,
       NetWMStateFullscreen, NetWMWindowType, NetWMWindowTypeDialog,
       NetClientList, NetLast };

static void applyrules(Client *c);
static int applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact);
static void attach(Client *c);
static void buttonpress(XEvent *e);
static void buttonrelease(XEvent *e);
static void checkotherwm(void);
static void cleanup(void);
static void clientmessage(XEvent *e);
static void configure(Client *c);
static void configurenotify(XEvent *e);
static void configurerequest(XEvent *e);
static Monitor *createmon(void);
static int computevis(Monitor *m, int *vis, int *widths);
static void destroynotify(XEvent *e);
static void detach(Client *c);
static Monitor *dirmon(int dir);
static void drawbar(Monitor *m);
static void drawborder(Client *c, int sel);
static void enternotify(XEvent *e);
static void expose(XEvent *e);
static void focus(Client *c);
static void focusin(XEvent *e);
static void focusmon(const Arg *arg);
static void focusstack(const Arg *arg);
static Atom getatomprop(Client *c, Atom prop);
static long getstate(Window w);
static void grabinput(void);
static void keypress(XEvent *e);
static void killclient(const Arg *arg);
static void manage(Window w);
static void mappingnotify(XEvent *e);
static void maprequest(XEvent *e);
static Monitor *monat(int num);
static void motionnotify(XEvent *e);
static void movetotag(const Arg *arg);
static void propertynotify(XEvent *e);
static Monitor *recttomon(int x, int y);
static void resize(Client *c, int x, int y, int w, int h, int interact);
static void resizeclient(Client *c, int x, int y, int w, int h);
static void resizemax(Client *c);
static void run(void);
static void scan(void);
static int sendevent(Client *c, Atom proto);
static void setclientstate(Client *c, long state);
static void setfocus(Client *c);
static void setfullscreen(Client *c, int full);
static void setselmon(Monitor *m);
static void setup(void);
static void showhide(Client *c, int show);
static void spawn(const Arg *arg);
static void tagmon(const Arg *arg);
static int textw(const char *s);
static void togglebar(const Arg *arg);
static void togglefullscreen(const Arg *arg);
static void togglemax(const Arg *arg);
static void quit(const Arg *arg);
static void unmanage(Window w, int destroyed);
static void unmapnotify(XEvent *e);
static void updatebarpos(Monitor *m);
static void updateclientlist(void);
static void updatenumlockmask(void);
static int updategeom(void);
static void updatesizehints(Client *c);
static void updatestatus(void);
static void updatewindowtype(Client *c);
static void updatewmhints(Client *c);
static void view(const Arg *arg);
static void viewtag(Monitor *m, int tag);
static Client *wintoclient(Window w);
static int xerror(Display *dpy, XErrorEvent *ee);
static int xerrordummy(Display *dpy, XErrorEvent *ee);
static int xerrorstart(Display *dpy, XErrorEvent *ee);

#include "config.h"

static Display *dpy;
static int screen;
static int sw, sh; /* whole X screen, for interactive clamping (dwm) */
static Window root, wmcheckwin;
static Monitor *mons, *selmon;
static int running;
static unsigned int numlockmask;
static int (*xerrorxlib)(Display *, XErrorEvent *);

static XftFont *font;
static GC gc;
static Visual *visual;
static Colormap cmap;
static int bh; /* bar height */
static unsigned long bg_norm, bg_sel, border_norm, border_sel;
static XftColor xftfg_norm, xftfg_sel;

static Atom wmatom[WMLast], netatom[NetLast];
static char stext[256];

static Client *focused;
static Client *dragc;
static int dragbutton, dragorigx, dragorigy, dragwx, dragwy, dragww, dragwh;

static void (*handler[LASTEvent])(XEvent *e) = {
	[ButtonPress]      = buttonpress,
	[ButtonRelease]    = buttonrelease,
	[ClientMessage]    = clientmessage,
	[ConfigureNotify]  = configurenotify,
	[ConfigureRequest] = configurerequest,
	[DestroyNotify]    = destroynotify,
	[EnterNotify]      = enternotify,
	[Expose]           = expose,
	[FocusIn]          = focusin,
	[KeyPress]         = keypress,
	[MapRequest]       = maprequest,
	[MappingNotify]    = mappingnotify,
	[MotionNotify]     = motionnotify,
	[PropertyNotify]   = propertynotify,
	[UnmapNotify]      = unmapnotify,
};

/* ---- monitors / tags ---- */

int
computevis(Monitor *m, int *vis, int *widths)
{
	int nvis = 0, t;

	for (t = 0; t < TAGS; t++)
		if (m->taghead[t] || t == m->seltag) {
			vis[nvis] = t;
			widths[nvis] = textw(tags[t]);
			nvis++;
		}
	return nvis;
}

Monitor *
createmon(void)
{
	Monitor *m = calloc(1, sizeof(Monitor));
	XSetWindowAttributes wa = {
		.override_redirect = True,
		.background_pixel = bg_norm,
		.border_pixel = border_sel,
		.event_mask = ExposureMask,
	};

	if (!m)
		exit(1);
	m->seltag = 0;
	m->showbar = 1;
	m->tagwin = XCreateWindow(dpy, root, 0, 0, 1, bh, borderpx, CopyFromParent,
			CopyFromParent, CopyFromParent,
			CWOverrideRedirect|CWBackPixel|CWBorderPixel|CWEventMask, &wa);
	m->statuswin = XCreateWindow(dpy, root, 0, 0, 1, bh, borderpx, CopyFromParent,
			CopyFromParent, CopyFromParent,
			CWOverrideRedirect|CWBackPixel|CWBorderPixel|CWEventMask, &wa);
	XSelectInput(dpy, m->tagwin, ExposureMask|ButtonPressMask);
	XSelectInput(dpy, m->statuswin, ExposureMask|ButtonPressMask);
	/* tagwin is shown on every monitor; statuswin only on selmon (see drawbar) */
	return m;
}

Monitor *
monat(int num)
{
	Monitor *m;

	for (m = mons; m; m = m->next)
		if (m->num == num)
			return m;
	return NULL;
}

Monitor *
dirmon(int dir)
{
	Monitor *m;

	if (dir > 0) {
		m = selmon->next;
		return m ? m : mons;
	}
	if (selmon == mons) {
		for (m = mons; m->next; m = m->next);
		return m;
	}
	for (m = mons; m->next != selmon; m = m->next);
	return m;
}

Monitor *
recttomon(int x, int y)
{
	Monitor *m;

	for (m = mons; m; m = m->next)
		if (x >= m->mx && x < m->mx + m->mw && y >= m->my && y < m->my + m->mh)
			return m;
	return selmon;
}

void
updatebarpos(Monitor *m)
{
	/* every gap (external, around the bars; internal, bar-to-window and
	 * window-to-edge) is one bar height wide, so they read as a uniform
	 * grid; adjoining gaps (e.g. below the bar) simply add up. */
	m->wx = m->mx;
	m->ww = m->mw;
	if (m->showbar) {
		m->wy = m->my + 2 * bh + 2 * borderpx;
		m->wh = m->mh - 2 * bh - 2 * borderpx;
	} else {
		m->wy = m->my;
		m->wh = m->mh;
	}
	XMoveResizeWindow(dpy, m->tagwin, m->mx + bh, m->my + bh, 1, bh);
	XMoveResizeWindow(dpy, m->statuswin, m->mx + m->mw - 1 - bh - 2 * borderpx, m->my + bh, 1, bh);
}

/* move every client of `from` onto `to`, keeping tag numbers */
static void
migrateclients(Monitor *from, Monitor *to)
{
	Client *c;
	int t;

	for (t = 0; t < TAGS; t++) {
		while ((c = from->taghead[t])) {
			detach(c);
			c->mon = to;
			attach(c);
			showhide(c, ISVISIBLE(c));
		}
	}
}

int
updategeom(void)
{
	int dirty = 0;

	if (XineramaIsActive(dpy)) {
		int i, n, nn;
		XineramaScreenInfo *info = XineramaQueryScreens(dpy, &nn);
		Monitor *m;

		if (!info || nn < 1) {
			if (info)
				XFree(info);
			return 0; /* query failed or reported no screens: leave geometry alone */
		}

		for (n = 0, m = mons; m; m = m->next, n++);

		if (n < nn) {
			for (i = 0; i < nn - n; i++) {
				Monitor *nm = createmon();
				if (!mons) {
					mons = nm;
				} else {
					for (m = mons; m->next; m = m->next);
					m->next = nm;
				}
			}
			dirty = 1;
		} else if (n > nn) {
			for (i = 0; i < n - nn; i++) {
				Monitor **pm = &mons;
				while ((*pm)->next)
					pm = &(*pm)->next;
				m = *pm;
				*pm = NULL;
				migrateclients(m, mons);
				XDestroyWindow(dpy, m->tagwin);
				XDestroyWindow(dpy, m->statuswin);
				free(m);
			}
			dirty = 1;
		}

		for (i = 0, m = mons; i < nn && m; i++, m = m->next) {
			if (dirty || m->mx != info[i].x_org || m->my != info[i].y_org ||
			    m->mw != info[i].width || m->mh != info[i].height) {
				dirty = 1;
				m->num = i;
				m->mx = info[i].x_org;
				m->my = info[i].y_org;
				m->mw = info[i].width;
				m->mh = info[i].height;
				updatebarpos(m);
			}
		}
		if (info)
			XFree(info);
	} else {
		if (!mons) {
			mons = createmon();
			dirty = 1;
		}
		if (dirty || mons->mw != DisplayWidth(dpy, screen) ||
		    mons->mh != DisplayHeight(dpy, screen)) {
			dirty = 1;
			mons->num = 0;
			mons->mx = mons->my = 0;
			mons->mw = DisplayWidth(dpy, screen);
			mons->mh = DisplayHeight(dpy, screen);
			updatebarpos(mons);
		}
	}
	if (dirty)
		selmon = mons;
	return dirty;
}

/* ---- client list management ---- */

void
attach(Client *c)
{
	Client **head = &c->mon->taghead[c->tag];

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

void
detach(Client *c)
{
	Client **head = &c->mon->taghead[c->tag];

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

Client *
wintoclient(Window w)
{
	Monitor *m;
	int t;

	for (m = mons; m; m = m->next)
		for (t = 0; t < TAGS; t++) {
			Client *c = m->taghead[t], *s = c;
			if (!c)
				continue;
			do {
				if (s->win == w)
					return s;
				s = s->next;
			} while (s != c);
		}
	return NULL;
}

void
applyrules(Client *c)
{
	XClassHint ch = { NULL, NULL };
	unsigned int i;

	if (!XGetClassHint(dpy, c->win, &ch))
		return;
	for (i = 0; i < LENGTH(rules); i++) {
		const Rule *r = &rules[i];
		if ((!r->class || (ch.res_class && strstr(ch.res_class, r->class))) &&
		    (!r->instance || (ch.res_name && strstr(ch.res_name, r->instance)))) {
			if (r->tag >= 0)
				c->tag = r->tag;
			if (r->monitor >= 0) {
				Monitor *m = monat(r->monitor);
				if (m)
					c->mon = m;
			}
			if (r->ismax)
				c->ismax = 1;
		}
	}
	if (ch.res_class)
		XFree(ch.res_class);
	if (ch.res_name)
		XFree(ch.res_name);
}

/* tell a client where it actually ended up. ICCCM requires this whenever the
 * window is moved without being resized, or a ConfigureRequest is not granted
 * as asked; dwm calls it configure(). Without it clients never learn their
 * absolute position (menus and popups then open in the wrong place) and keep
 * re-asking for the geometry they wanted. */
void
configure(Client *c)
{
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

/* dwm's applysizehints, minus the tiling branch: everything floats here, so
 * WM_NORMAL_HINTS always apply */
int
applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact)
{
	Monitor *m = c->mon;
	int baseismin;

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
		if (*x >= m->wx + m->ww)
			*x = m->wx + m->ww - WIDTH(c);
		if (*y >= m->wy + m->wh)
			*y = m->wy + m->wh - HEIGHT(c);
		if (*x + *w + 2 * c->bw <= m->wx)
			*x = m->wx;
		if (*y + *h + 2 * c->bw <= m->wy)
			*y = m->wy;
	}
	if (*h < bh)
		*h = bh;
	if (*w < bh)
		*w = bh;
	if (!c->hintsvalid)
		updatesizehints(c);
	/* see last two sentences in ICCCM 4.1.2.3 */
	baseismin = c->basew == c->minw && c->baseh == c->minh;
	if (!baseismin) { /* temporarily remove base dimensions */
		*w -= c->basew;
		*h -= c->baseh;
	}
	if (c->mina > 0 && c->maxa > 0) {
		if (c->maxa < (float)*w / *h)
			*w = *h * c->maxa + 0.5;
		else if (c->mina < (float)*h / *w)
			*h = *w * c->mina + 0.5;
	}
	if (baseismin) { /* increment calculation requires this */
		*w -= c->basew;
		*h -= c->baseh;
	}
	if (c->incw)
		*w -= *w % c->incw;
	if (c->inch)
		*h -= *h % c->inch;
	*w = MAX(*w + c->basew, c->minw);
	*h = MAX(*h + c->baseh, c->minh);
	if (c->maxw)
		*w = MIN(*w, c->maxw);
	if (c->maxh)
		*h = MIN(*h, c->maxh);
	return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

void
resize(Client *c, int x, int y, int w, int h, int interact)
{
	if (applysizehints(c, &x, &y, &w, &h, interact))
		resizeclient(c, x, y, w, h);
}

void
resizeclient(Client *c, int x, int y, int w, int h)
{
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

/* fill c->mon's window area, bar height worth of gap on every side, same as
 * the internal/external gaps updatebarpos keeps around the bar itself */
void
resizemax(Client *c)
{
	Monitor *m = c->mon;

	resize(c, m->wx + bh, m->wy + bh,
			m->ww - 2 * c->bw - 2 * bh,
			m->wh - 2 * c->bw - 2 * bh, 0);
}

void
manage(Window w)
{
	XWindowAttributes wa;
	Client *c, *t = NULL;
	Monitor *m;
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
	c->oldbw = wa.border_width;
	c->bw = borderpx;

	/* dwm: a transient (dialog, file picker, ...) belongs wherever the
	 * window it is transient for lives, not on whatever tag is selected */
	if (XGetTransientForHint(dpy, w, &trans) && (t = wintoclient(trans))) {
		c->mon = t->mon;
		c->tag = t->tag;
	} else {
		c->mon = selmon;
		c->tag = selmon->seltag;
		applyrules(c);
	}
	m = c->mon;

	if (c->x == 0 && c->y == 0) {
		c->x = m->wx + MAX(0, (m->ww - WIDTH(c)) / 2);
		c->y = m->wy + MAX(0, (m->wh - HEIGHT(c)) / 2);
	}
	/* dwm keeps a new window inside the monitor it opens on */
	if (c->x + WIDTH(c) > m->wx + m->ww)
		c->x = m->wx + m->ww - WIDTH(c);
	if (c->y + HEIGHT(c) > m->wy + m->wh)
		c->y = m->wy + m->wh - HEIGHT(c);
	c->x = MAX(c->x, m->wx);
	c->y = MAX(c->y, m->wy);

	/* c->ismax may already be set by a matching rule (see applyrules); such
	 * windows open maximized, MODKEY+space restores this natural size */
	c->oldx = c->x;
	c->oldy = c->y;
	c->oldw = c->w;
	c->oldh = c->h;

	XSetWindowBorderWidth(dpy, w, c->bw);
	drawborder(c, 0);
	configure(c); /* propagates border_width, if size doesn't change */
	updatewindowtype(c); /* may already want to be fullscreen */
	updatesizehints(c);
	updatewmhints(c);
	XSelectInput(dpy, w, EnterWindowMask|FocusChangeMask|StructureNotifyMask|
			PropertyChangeMask);
	attach(c);
	XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW, 32,
			PropModeAppend, (unsigned char *)&(c->win), 1);

	if (!c->isfull) { /* updatewindowtype already sized a fullscreen window */
		if (c->ismax)
			resizemax(c);
		else
			resizeclient(c, c->x, c->y, c->w, c->h);
	}
	setclientstate(c, NormalState);

	showhide(c, ISVISIBLE(c));
	if (ISVISIBLE(c)) {
		setselmon(m);
		focus(c);
	}
	drawbar(m);
}

void
unmanage(Window w, int destroyed)
{
	Client *c = wintoclient(w);
	Monitor *m;
	XWindowChanges wc;
	int wasfocused;

	if (!c)
		return;
	m = c->mon;
	wasfocused = (focused == c);
	if (dragc == c) /* never keep dragging a client we are about to free */
		dragc = NULL;
	detach(c);
	if (m->sel == c)
		m->sel = NULL;
	if (!destroyed) {
		/* dwm's unmanage: hand the window back the way we found it */
		wc.border_width = c->oldbw;
		XGrabServer(dpy); /* avoid race conditions */
		XSetErrorHandler(xerrordummy);
		XSelectInput(dpy, c->win, NoEventMask);
		XConfigureWindow(dpy, c->win, CWBorderWidth, &wc); /* restore border */
		setclientstate(c, WithdrawnState);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
	free(c);
	if (wasfocused) {
		focused = NULL;
		focus(m->taghead[m->seltag]);
	}
	updateclientlist();
	drawbar(m);
}

/* ---- tags / monitors: switching, moving windows ---- */

/* smawm hides the tags you are not looking at by unmapping their windows,
 * which is sowm's ws_go; dwm instead parks them off screen. The difference
 * matters because an UnmapNotify then means two different things, so record
 * in c->hidden that this particular unmap was ours - see unmapnotify(). */
void
showhide(Client *c, int show)
{
	if (!c)
		return;
	c->hidden = !show;
	if (show)
		XMapWindow(dpy, c->win);
	else
		XUnmapWindow(dpy, c->win);
}

void
viewtag(Monitor *m, int tag)
{
	Client *c, *s;

	if (tag < 0 || tag >= TAGS)
		return;
	if (tag == m->seltag) {
		setselmon(m);
		drawbar(m);
		return;
	}
	if ((c = m->taghead[m->seltag])) {
		s = c;
		do {
			showhide(s, 0);
			s = s->next;
		} while (s != c);
	}
	m->seltag = tag;
	if ((c = m->taghead[tag])) {
		s = c;
		do {
			showhide(s, 1);
			s = s->next;
		} while (s != c);
	}
	setselmon(m);
	focus(m->taghead[tag]);
	drawbar(m);
}

void
view(const Arg *arg)
{
	viewtag(selmon, arg->i);
}

void
movetotag(const Arg *arg)
{
	Client *c = selmon->sel;
	int vis;

	if (!c || arg->i < 0 || arg->i >= TAGS || arg->i == c->tag)
		return;
	vis = ISVISIBLE(c);
	detach(c);
	c->tag = arg->i;
	attach(c);
	if (vis)
		showhide(c, 0);
	focus(selmon->taghead[selmon->seltag]);
	drawbar(selmon);
}

void
focusmon(const Arg *arg)
{
	Monitor *m = dirmon(arg->i);

	if (m == selmon)
		return;
	setselmon(m);
	focus(m->sel); /* focus() redraws selmon's bar even when m->sel is NULL */
}

void
tagmon(const Arg *arg)
{
	Client *c = selmon->sel;
	Monitor *m;
	int wasvisible;

	if (!c)
		return;
	m = dirmon(arg->i);
	if (m == c->mon)
		return;
	wasvisible = ISVISIBLE(c);
	detach(c);
	c->mon = m;
	attach(c);
	if (wasvisible != ISVISIBLE(c))
		showhide(c, ISVISIBLE(c));
	focus(selmon->taghead[selmon->seltag]);
	drawbar(selmon);
	drawbar(m);
}

void
focusstack(const Arg *arg)
{
	Client *c = selmon->sel, *n;

	if (!c)
		return;
	n = arg->i > 0 ? c->next : c->prev;
	if (n != c)
		focus(n);
}

/* ---- focus / borders ---- */

void
drawborder(Client *c, int sel)
{
	XSetWindowBorder(dpy, c->win, sel ? border_sel : border_norm);
}

/* switch the active monitor, hiding the bar on the one we're leaving.
 * only selmon's bar is ever shown (see drawbar). */
void
setselmon(Monitor *m)
{
	Monitor *old = selmon;

	if (m == old)
		return;
	selmon = m;
	if (old)
		drawbar(old);
}

/* dwm's setfocus: WM_TAKE_FOCUS is how clients that ask not to be focused
 * directly (the "globally active" ICCCM model) get told they are current */
void
setfocus(Client *c)
{
	if (!c->neverfocus) {
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
		XChangeProperty(dpy, root, netatom[NetActiveWindow], XA_WINDOW, 32,
				PropModeReplace, (unsigned char *)&(c->win), 1);
	}
	sendevent(c, wmatom[WMTakeFocus]);
}

void
focus(Client *c)
{
	/* focusing an unmapped window is a BadMatch that leaves the keyboard
	 * pointing at nothing - dwm guards the same way */
	if (c && !ISVISIBLE(c))
		c = NULL;
	if (focused && focused != c)
		drawborder(focused, 0);
	focused = c;
	if (c) {
		c->mon->sel = c;
		drawborder(c, 1);
		setfocus(c);
		XRaiseWindow(dpy, c->win);
		setselmon(c->mon);
	} else {
		/* every focus(NULL) call site has already made selmon point at
		 * the monitor whose selection just went empty */
		selmon->sel = NULL;
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
	drawbar(selmon);
}

/* ---- maximize / fullscreen ---- */

void
togglemax(const Arg *arg)
{
	Client *c = selmon->sel;

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
	}
}

void
setfullscreen(Client *c, int full)
{
	Monitor *m = c->mon;

	if (full && !c->isfull) {
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
				PropModeReplace, (unsigned char *)&netatom[NetWMStateFullscreen], 1);
		c->oldx = c->x;
		c->oldy = c->y;
		c->oldw = c->w;
		c->oldh = c->h;
		c->isfull = 1;
		c->oldbw = c->bw;
		c->bw = 0;
		/* resizeclient, not resize: a fullscreen window gets the monitor
		 * exactly, size hints do not get a say */
		resizeclient(c, m->mx, m->my, m->mw, m->mh);
		XRaiseWindow(dpy, c->win);
	} else if (!full && c->isfull) {
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
				PropModeReplace, (unsigned char *)0, 0);
		c->isfull = 0;
		c->bw = c->oldbw;
		resizeclient(c, c->oldx, c->oldy, c->oldw, c->oldh);
	}
}

void
togglefullscreen(const Arg *arg)
{
	Client *c = selmon->sel;

	(void)arg;
	if (c)
		setfullscreen(c, !c->isfull);
}

/* ---- bar ---- */

void
togglebar(const Arg *arg)
{
	Monitor *m = selmon;
	int t;

	(void)arg;
	m->showbar = !m->showbar;
	updatebarpos(m);
	/* maximized clients fill m->wx/wy/ww/wh, which updatebarpos just moved -
	 * redimension them so they keep occupying that space, gaps included */
	for (t = 0; t < TAGS; t++) {
		Client *c = m->taghead[t], *s = c;
		if (!c)
			continue;
		do {
			if (s->ismax)
				resizemax(s);
			s = s->next;
		} while (s != c);
	}
	drawbar(m);
}

int
textw(const char *s)
{
	XGlyphInfo ext;

	XftTextExtentsUtf8(dpy, font, (const FcChar8 *)s, (int)strlen(s), &ext);
	return ext.xOff + 16;
}

/* drawbar runs on every focus change, so on every crossing of the pointer
 * between windows. Painting the background and then the text straight onto
 * the bar window makes that visible as flicker, especially while a window is
 * being dragged over the bar; dwm's drw.c avoids it by drawing into an
 * off-screen pixmap and blitting the finished result, which is what these
 * two helpers do. */
static Pixmap
barpixmap(int w)
{
	return XCreatePixmap(dpy, root, MAX(w, 1), bh, DefaultDepth(dpy, screen));
}

static void
barblit(Pixmap pm, Window win, int w)
{
	XCopyArea(dpy, pm, win, gc, 0, 0, MAX(w, 1), bh, 0, 0);
	XFreePixmap(dpy, pm);
}

void
drawbar(Monitor *m)
{
	int vis[TAGS], widths[TAGS], nvis, i, x, tw;
	Pixmap pm;
	XftDraw *xd;

	if (!m->showbar) {
		XUnmapWindow(dpy, m->tagwin);
		XUnmapWindow(dpy, m->statuswin);
		return;
	}

	XMapWindow(dpy, m->tagwin);
	if (m == selmon)
		XMapWindow(dpy, m->statuswin);
	else
		XUnmapWindow(dpy, m->statuswin);

	nvis = computevis(m, vis, widths);
	tw = 0;
	for (i = 0; i < nvis; i++)
		tw += widths[i];
	tw = MAX(tw, 1);
	XMoveResizeWindow(dpy, m->tagwin, m->mx + bh, m->my + bh, tw, bh);
	pm = barpixmap(tw);
	xd = XftDrawCreate(dpy, pm, visual, cmap);
	x = 0;
	for (i = 0; i < nvis; i++) {
		int sel = vis[i] == m->seltag;
		XSetForeground(dpy, gc, sel ? bg_sel : bg_norm);
		XFillRectangle(dpy, pm, gc, x, 0, widths[i], bh);
		XftDrawStringUtf8(xd, sel ? &xftfg_sel : &xftfg_norm, font, x + 8,
				font->ascent + 2, (const FcChar8 *)tags[vis[i]],
				(int)strlen(tags[vis[i]]));
		x += widths[i];
	}
	XftDrawDestroy(xd);
	barblit(pm, m->tagwin, tw);

	if (m != selmon)
		return;

	tw = MAX(textw(stext), 1);
	XMoveResizeWindow(dpy, m->statuswin, m->mx + m->mw - tw - bh - 2 * borderpx, m->my + bh, tw, bh);
	pm = barpixmap(tw);
	XSetForeground(dpy, gc, bg_norm);
	XFillRectangle(dpy, pm, gc, 0, 0, tw, bh);
	xd = XftDrawCreate(dpy, pm, visual, cmap);
	XftDrawStringUtf8(xd, &xftfg_norm, font, 8, font->ascent + 2,
			(const FcChar8 *)stext, (int)strlen(stext));
	XftDrawDestroy(xd);
	barblit(pm, m->statuswin, tw);
}

void
updatestatus(void)
{
	XTextProperty tp;
	Monitor *m;

	if (XGetWMName(dpy, root, &tp) && tp.value && tp.nitems) {
		strncpy(stext, (char *)tp.value, sizeof(stext) - 1);
		stext[sizeof(stext) - 1] = '\0';
		XFree(tp.value);
	} else {
		strcpy(stext, "smawm");
	}
	for (m = mons; m; m = m->next)
		drawbar(m);
}

/* ---- mouse move/resize (from sowm) ---- */

void
buttonpress(XEvent *e)
{
	XButtonEvent *ev = &e->xbutton;
	Monitor *m;
	Client *c;

	for (m = mons; m; m = m->next) {
		if (ev->window == m->tagwin) {
			int vis[TAGS], widths[TAGS], nvis, i, x = 0;
			nvis = computevis(m, vis, widths);
			for (i = 0; i < nvis; i++) {
				if (ev->x < x + widths[i]) {
					viewtag(m, vis[i]);
					return;
				}
				x += widths[i];
			}
			return;
		}
		if (ev->window == m->statuswin)
			return;
	}

	if (!ev->subwindow)
		return;
	c = wintoclient(ev->subwindow);
	if (!c)
		return;
	setselmon(c->mon);
	focus(c);
	if (c->isfull)
		return; /* dwm's movemouse refuses fullscreen windows too */
	c->ismax = 0;
	dragc = c;
	dragbutton = ev->button;
	dragorigx = ev->x_root;
	dragorigy = ev->y_root;
	dragwx = c->x;
	dragwy = c->y;
	dragww = c->w;
	dragwh = c->h;
}

void
buttonrelease(XEvent *e)
{
	Client *c = dragc;
	Monitor *m, *old;

	(void)e;
	dragc = NULL;
	if (!c)
		return;
	/* dwm's movemouse ends the same way: a window dragged onto another
	 * monitor becomes that monitor's, otherwise it stays on the old
	 * monitor's tag list while sitting on a different screen */
	if ((m = recttomon(c->x + c->w / 2, c->y + c->h / 2)) == c->mon)
		return;
	old = c->mon;
	detach(c);
	c->mon = m;
	c->tag = m->seltag;
	attach(c);
	setselmon(m);
	focus(c);
	drawbar(old);
	drawbar(m);
}

void
motionnotify(XEvent *e)
{
	int xd, yd;

	if (!dragc || dragc->isfull)
		return;
	while (XCheckTypedEvent(dpy, MotionNotify, e));
	xd = e->xbutton.x_root - dragorigx;
	yd = e->xbutton.y_root - dragorigy;
	if (dragbutton == 1)
		resize(dragc, dragwx + xd, dragwy + yd, dragww, dragwh, 1);
	else if (dragbutton == 3)
		resize(dragc, dragwx, dragwy, MAX(20, dragww + xd), MAX(20, dragwh + yd), 1);
}

/* ---- misc event handlers ---- */

/* dwm's configurerequest. The version this replaced was sowm's, which just
 * forwards the request to the server with the mask the client sent: that is
 * safe in sowm, which tracks no geometry and draws no borders, but here it
 * let a client wipe its own border (CWBorderWidth), restack itself over the
 * bar (CWSibling/CWStackMode), and undo whatever the WM had just done -
 * which is what made a self-positioning window impossible to drag. */
void
configurerequest(XEvent *e)
{
	XConfigureRequestEvent *ev = &e->xconfigurerequest;
	Client *c;
	Monitor *m;
	XWindowChanges wc;

	if ((c = wintoclient(ev->window))) {
		/* CWBorderWidth is deliberately never passed on: dwm lets a client
		 * set its own border width, but smawm has a single borderpx for
		 * everything, and toolkits routinely ask for 0 - which is how the
		 * focus border silently disappeared off windows */
		if (c->isfull) {
			/* it asked to be fullscreen; it does not also get to pick
			 * where fullscreen is */
			configure(c);
		} else {
			m = c->mon;
			if (ev->value_mask & CWX)
				c->x = m->mx + ev->x;
			if (ev->value_mask & CWY)
				c->y = m->my + ev->y;
			if (ev->value_mask & CWWidth)
				c->w = ev->width;
			if (ev->value_mask & CWHeight)
				c->h = ev->height;
			if ((c->x + c->w) > m->mx + m->mw)
				c->x = m->mx + (m->mw / 2 - WIDTH(c) / 2); /* center in x */
			if ((c->y + c->h) > m->my + m->mh)
				c->y = m->my + (m->mh / 2 - HEIGHT(c) / 2); /* center in y */
			if ((ev->value_mask & (CWX|CWY)) && !(ev->value_mask & (CWWidth|CWHeight)))
				configure(c);
			if (ISVISIBLE(c))
				XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
		}
	} else {
		/* not ours (an override-redirect window, or one not managed yet):
		 * grant it as asked, same as sowm and dwm both do */
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

void
configurenotify(XEvent *e)
{
	if (e->xconfigure.window == root && updategeom()) {
		Monitor *m;
		for (m = mons; m; m = m->next)
			drawbar(m);
	}
}

void
maprequest(XEvent *e)
{
	Window w = e->xmaprequest.window;
	if (!wintoclient(w))
		manage(w);
}

void
destroynotify(XEvent *e)
{
	unmanage(e->xdestroywindow.window, 1);
}

/* The handler smawm was missing entirely. Plenty of programs close a window
 * by unmapping it and keeping it around to show again later - Qt's hide() is
 * exactly this, which is what Telegram's image preview does. Without this,
 * the client stayed on its tag forever: the tag bar kept showing that tag as
 * occupied ("like I hadn't changed tags"), switching to it mapped the closed
 * window back onto the screen, focus could land on a window that was not
 * there, and the app's own request to show it again was swallowed by
 * maprequest, because smawm still thought it was managing it. */
void
unmapnotify(XEvent *e)
{
	XUnmapEvent *ev = &e->xunmap;
	Client *c;

	if (!(c = wintoclient(ev->window)))
		return;
	if (c->hidden && !ev->send_event)
		return; /* our own unmap, from a tag switch - see showhide() */
	/* a synthetic UnmapNotify is a client withdrawing the window (ICCCM
	 * 4.1.4); a real one means it is gone from the screen. Either way it is
	 * no longer ours to manage. */
	unmanage(ev->window, 0);
}

void
enternotify(XEvent *e)
{
	Client *c;
	Monitor *m;
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
	c = wintoclient(ev->window);
	m = c ? c->mon : recttomon(ev->x_root, ev->y_root);
	if (m != selmon) {
		setselmon(m);
		if (!c)
			c = m->sel;
	} else if (!c || c == selmon->sel) {
		return;
	}
	focus(c);
}

/* dwm's focusin: a client that takes the input focus behind our back gets it
 * handed straight back to whatever is actually selected */
void
focusin(XEvent *e)
{
	XFocusChangeEvent *ev = &e->xfocus;

	if (selmon->sel && ev->window != selmon->sel->win)
		setfocus(selmon->sel);
}

void
expose(XEvent *e)
{
	Monitor *m;

	if (e->xexpose.count != 0)
		return;
	for (m = mons; m; m = m->next)
		if (e->xexpose.window == m->tagwin || e->xexpose.window == m->statuswin) {
			drawbar(m);
			return;
		}
}

/* dwm's propertynotify. smawm selects PropertyChangeMask on every client but
 * used to look at nothing except the root window's name, so a client that
 * changed its size hints or its window type after mapping was never noticed. */
void
propertynotify(XEvent *e)
{
	XPropertyEvent *ev = &e->xproperty;
	Client *c;

	if (ev->window == root && ev->atom == XA_WM_NAME) {
		updatestatus();
	} else if (ev->state == PropertyDelete) {
		return; /* ignore */
	} else if ((c = wintoclient(ev->window))) {
		switch (ev->atom) {
		default:
			break;
		case XA_WM_NORMAL_HINTS:
			c->hintsvalid = 0;
			break;
		case XA_WM_HINTS:
			updatewmhints(c);
			break;
		}
		if (ev->atom == netatom[NetWMWindowType])
			updatewindowtype(c);
	}
	/* dwm also re-floats a window that becomes transient here; in smawm
	 * everything floats already, so there is nothing to do */
}

void
clientmessage(XEvent *e)
{
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

void
mappingnotify(XEvent *e)
{
	XMappingEvent *ev = &e->xmapping;

	XRefreshKeyboardMapping(ev);
	if (ev->request == MappingKeyboard)
		grabinput();
}

void
keypress(XEvent *e)
{
	XKeyEvent *ev = &e->xkey;
	KeySym keysym = XkbKeycodeToKeysym(dpy, ev->keycode, 0, 0);
	unsigned int i;

	for (i = 0; i < LENGTH(keys); i++)
		if (keysym == keys[i].keysym &&
		    CLEANMASK(keys[i].mod) == CLEANMASK(ev->state) && keys[i].func)
			keys[i].func(&keys[i].arg);
}

/* ---- spawning / killing / quitting ---- */

void
spawn(const Arg *arg)
{
	struct sigaction sa;

	if (fork())
		return;
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

int
sendevent(Client *c, Atom proto)
{
	int n, exists = 0;
	Atom *protocols;
	XEvent ev;

	if (XGetWMProtocols(dpy, c->win, &protocols, &n)) {
		while (!exists && n--)
			exists = protocols[n] == proto;
		XFree(protocols);
	}
	if (exists) {
		ev.type = ClientMessage;
		ev.xclient.window = c->win;
		ev.xclient.message_type = wmatom[WMProtocols];
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = proto;
		ev.xclient.data.l[1] = CurrentTime;
		ev.xclient.data.l[2] = 0;
		ev.xclient.data.l[3] = 0;
		ev.xclient.data.l[4] = 0;
		XSendEvent(dpy, c->win, False, NoEventMask, &ev);
	}
	return exists;
}

void
killclient(const Arg *arg)
{
	Client *c = selmon->sel;

	(void)arg;
	if (!c)
		return;
	if (!sendevent(c, wmatom[WMDelete]))
		XKillClient(dpy, c->win);
}

void
quit(const Arg *arg)
{
	(void)arg;
	running = 0;
}

/* ---- client properties (all from dwm) ---- */

/* WM_STATE. ICCCM requires the WM to keep this on every window it manages;
 * toolkits read it to tell "mapped" from "iconified" from "not managed". */
void
setclientstate(Client *c, long state)
{
	long data[] = { state, None };

	XChangeProperty(dpy, c->win, wmatom[WMState], wmatom[WMState], 32,
			PropModeReplace, (unsigned char *)data, 2);
}

long
getstate(Window w)
{
	int format;
	long result = -1;
	unsigned char *p = NULL;
	unsigned long n, extra;
	Atom real;

	if (XGetWindowProperty(dpy, w, wmatom[WMState], 0L, 2L, False, wmatom[WMState],
			&real, &format, &n, &extra, (unsigned char **)&p) != Success)
		return -1;
	if (n != 0)
		result = *p;
	XFree(p);
	return result;
}

Atom
getatomprop(Client *c, Atom prop)
{
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
void
updatewindowtype(Client *c)
{
	if (getatomprop(c, netatom[NetWMState]) == netatom[NetWMStateFullscreen])
		setfullscreen(c, 1);
	/* dwm also floats _NET_WM_WINDOW_TYPE_DIALOG here; everything floats in
	 * smawm, so a dialog needs nothing special */
}

void
updatesizehints(Client *c)
{
	long msize;
	XSizeHints size;

	if (!XGetWMNormalHints(dpy, c->win, &size, &msize))
		size.flags = PSize; /* size is uninitialized, ignore its flags */
	if (size.flags & PBaseSize) {
		c->basew = size.base_width;
		c->baseh = size.base_height;
	} else if (size.flags & PMinSize) {
		c->basew = size.min_width;
		c->baseh = size.min_height;
	} else {
		c->basew = c->baseh = 0;
	}
	if (size.flags & PResizeInc) {
		c->incw = size.width_inc;
		c->inch = size.height_inc;
	} else {
		c->incw = c->inch = 0;
	}
	if (size.flags & PMaxSize) {
		c->maxw = size.max_width;
		c->maxh = size.max_height;
	} else {
		c->maxw = c->maxh = 0;
	}
	if (size.flags & PMinSize) {
		c->minw = size.min_width;
		c->minh = size.min_height;
	} else if (size.flags & PBaseSize) {
		c->minw = size.base_width;
		c->minh = size.base_height;
	} else {
		c->minw = c->minh = 0;
	}
	if (size.flags & PAspect) {
		c->mina = (float)size.min_aspect.y / size.min_aspect.x;
		c->maxa = (float)size.max_aspect.x / size.max_aspect.y;
	} else {
		c->maxa = c->mina = 0.0;
	}
	c->isfixed = (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh);
	c->hintsvalid = 1;
}

void
updatewmhints(Client *c)
{
	XWMHints *wmh;

	if ((wmh = XGetWMHints(dpy, c->win))) {
		/* dwm also tracks XUrgencyHint here to colour the tag; smawm's bar
		 * has no urgent state, so only the input hint matters */
		c->neverfocus = (wmh->flags & InputHint) ? !wmh->input : 0;
		XFree(wmh);
	}
}

void
updateclientlist(void)
{
	Monitor *m;
	int t;

	XDeleteProperty(dpy, root, netatom[NetClientList]);
	for (m = mons; m; m = m->next)
		for (t = 0; t < TAGS; t++) {
			Client *c = m->taghead[t], *s = c;
			if (!c)
				continue;
			do {
				XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW,
						32, PropModeAppend, (unsigned char *)&(s->win), 1);
				s = s->next;
			} while (s != c);
		}
}

/* ---- setup ---- */

/* dwm's error handlers. The old one returned 0 for everything, which hid
 * every real X error and made a second window manager on the same display
 * look like it had started fine. */
int
xerror(Display *d, XErrorEvent *ee)
{
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

int
xerrordummy(Display *d, XErrorEvent *ee)
{
	(void)d;
	(void)ee;
	return 0;
}

int
xerrorstart(Display *d, XErrorEvent *ee)
{
	(void)d;
	(void)ee;
	fprintf(stderr, "smawm: another window manager is already running\n");
	exit(1);
	return -1;
}

void
checkotherwm(void)
{
	xerrorxlib = XSetErrorHandler(xerrorstart);
	/* this causes an error if some other window manager is running */
	XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);
	XSync(dpy, False);
	XSetErrorHandler(xerror);
	XSync(dpy, False);
}

/* adopt the windows that were already on screen when we started - dwm's
 * scan(), and the same thing sowm's `watch` branch added */
void
scan(void)
{
	unsigned int i, num;
	Window d1, d2, *wins = NULL;
	XWindowAttributes wa;

	if (!XQueryTree(dpy, root, &d1, &d2, &wins, &num))
		return;
	for (i = 0; i < num; i++) {
		if (!XGetWindowAttributes(dpy, wins[i], &wa) || wa.override_redirect ||
		    XGetTransientForHint(dpy, wins[i], &d1))
			continue;
		if (wa.map_state == IsViewable || getstate(wins[i]) == IconicState)
			manage(wins[i]);
	}
	for (i = 0; i < num; i++) { /* now the transients */
		if (!XGetWindowAttributes(dpy, wins[i], &wa))
			continue;
		if (XGetTransientForHint(dpy, wins[i], &d1) &&
		    (wa.map_state == IsViewable || getstate(wins[i]) == IconicState))
			manage(wins[i]);
	}
	if (wins)
		XFree(wins);
}

/* hand every window back mapped, unmanaged and with its own border, so
 * quitting smawm doesn't strand the windows sitting on inactive tags */
void
cleanup(void)
{
	Monitor *m;
	Client *c;
	int t;

	for (m = mons; m; m = m->next)
		for (t = 0; t < TAGS; t++)
			while ((c = m->taghead[t])) {
				showhide(c, 1);
				unmanage(c->win, 0);
			}
	while (mons) {
		m = mons->next;
		XDestroyWindow(dpy, mons->tagwin);
		XDestroyWindow(dpy, mons->statuswin);
		free(mons);
		mons = m;
	}
	XDestroyWindow(dpy, wmcheckwin);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
}

void
updatenumlockmask(void)
{
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

void
grabinput(void)
{
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

void
setup(void)
{
	XColor c;
	Atom utf8string;
	struct sigaction sa;

	/* do not turn the programs we spawn into zombies (dwm) */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);
	while (waitpid(-1, NULL, WNOHANG) > 0); /* zombies inherited from .xinitrc */

	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	sw = DisplayWidth(dpy, screen);
	sh = DisplayHeight(dpy, screen);
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

#define ALLOC(name, var) \
	var = XAllocNamedColor(dpy, cmap, name, &c, &c) ? c.pixel : BlackPixel(dpy, screen)
	ALLOC(col_bg_norm, bg_norm);
	ALLOC(col_bg_sel, bg_sel);
	ALLOC(col_border_norm, border_norm);
	ALLOC(col_border_sel, border_sel);
#undef ALLOC
	XftColorAllocName(dpy, visual, cmap, col_fg_norm, &xftfg_norm);
	XftColorAllocName(dpy, visual, cmap, col_fg_sel, &xftfg_sel);

	utf8string = XInternAtom(dpy, "UTF8_STRING", False);
	wmatom[WMProtocols] = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wmatom[WMDelete] = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	wmatom[WMState] = XInternAtom(dpy, "WM_STATE", False);
	wmatom[WMTakeFocus] = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
	netatom[NetActiveWindow] = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	netatom[NetSupported] = XInternAtom(dpy, "_NET_SUPPORTED", False);
	netatom[NetWMName] = XInternAtom(dpy, "_NET_WM_NAME", False);
	netatom[NetWMState] = XInternAtom(dpy, "_NET_WM_STATE", False);
	netatom[NetWMCheck] = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
	netatom[NetWMStateFullscreen] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	netatom[NetWMWindowType] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	netatom[NetWMWindowTypeDialog] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	netatom[NetClientList] = XInternAtom(dpy, "_NET_CLIENT_LIST", False);

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
	XDeleteProperty(dpy, root, netatom[NetClientList]);

	XSelectInput(dpy, root, SubstructureRedirectMask|SubstructureNotifyMask|
			PropertyChangeMask|EnterWindowMask|ButtonPressMask);
	XDefineCursor(dpy, root, XCreateFontCursor(dpy, XC_left_ptr));
	grabinput();
	updatestatus();
}

void
run(void)
{
	XEvent ev;

	running = 1;
	while (running && !XNextEvent(dpy, &ev))
		if (handler[ev.type])
			handler[ev.type](&ev);
}

int
main(void)
{
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
