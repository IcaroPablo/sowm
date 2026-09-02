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

typedef struct {
	const char *class;
	const char *instance;
	int tag;     /* -1 = don't force */
	int ismax;   /* 1 = open maximized */
} Rule;

struct Client {
	Window win;
	int x, y, w, h;
	int oldx, oldy, oldw, oldh;
	int bw, oldbw;                /* border width; oldbw is the client's own */
	/* WM_NORMAL_HINTS, straight from dwm - without these a client that can
	 * only take certain sizes gets resized anyway and argues back */
	int basew, baseh, incw, inch, maxw, maxh, minw, minh;
	float mina, maxa;
	int isfixed, hintsvalid;
	int neverfocus;               /* WM_HINTS input flag (dwm) */
	int ismax, isfull;
	int hidden;                   /* WE unmapped it, for a tag switch */
	int tag;
	Client *next, *prev;          /* circular list, one per tag */
};
