/* smawm - a small floating window manager built from dwm + sowm parts. */

#include <X11/Xlib.h>

#define TAGS       9
#define LENGTH(X)  (sizeof(X) / sizeof(*X))
#define MAX(a, b)  ((a) > (b) ? (a) : (b))
#define MIN(a, b)  ((a) < (b) ? (a) : (b))

/* taken from dwm: ignore numlock/capslock when matching key/button modifiers */
#define CLEANMASK(mask) ((mask) & ~(numlockmask|LockMask) & \
        (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))

#define ISVISIBLE(C) ((C)->tag == seltag)

/* walk tag T's circular client list. sowm has the same idea in its `for win`
 * macro; the cursor is declared inside the loop so callers need no scratch
 * variable. Not safe if the body detaches or frees C - see cleanup(). */
#define FOREACH(C, T) \
	for (Client *_h = taghead[T], *C = _h; C; C = C->next == _h ? NULL : C->next)
#define WIDTH(C)     ((C)->w + 2 * (C)->bw)
#define HEIGHT(C)    ((C)->h + 2 * (C)->bw)

typedef struct Client Client;

typedef union {
	const char **com;
	int i;
} Arg;

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *arg);
	const Arg arg;
} Key;


struct Client {
	Window win;
	int x, y, w, h;
	int oldx, oldy, oldw, oldh;
	int bw, oldbw;                /* border width; oldbw is the client's own */
	/* WM_NORMAL_HINTS - without these a client that can only take certain
	 * sizes gets resized anyway and argues back. dwm also tracks base size,
	 * resize increments, aspect ratio and isfixed; see updatesizehints(). */
	int maxw, maxh, minw, minh;
	int ismax, isfull, isurgent;
	int hidden;                   /* WE unmapped it, for a tag switch */
	int tag;
	Client *next, *prev;          /* circular list, one per tag */
	/* dwm's focus stack, a second list threaded through every client in
	 * most-recently-focused order. It is what answers "what should take
	 * focus now" when a window closes or a tag is entered. */
	Client *snext;
};
