/* smawm config - trimmed from my-dwm-fork2's config.h. Some commands below
 * (the "sp" media script, barpid signalling) are personal scripts from that
 * setup; adjust or remove entries that don't apply to your machine. */

#include <X11/XF86keysym.h>

#define MODKEY Mod4Mask
#define TERMINAL "st"

/* appearance
 * gaps (bar-to-screen-edge, bar-to-window, window-to-edge) are all sized to
 * the bar height at runtime (see `bh` in smawm.c), not configured here. */
static const int borderpx           = 1;
static const char *fontname          = "CozetteVector:pixelsize=13:antialias=true:autohint=true";
/* gruvbox dark */
static const char col_bg_norm[]      = "#282828"; /* bg0 */
static const char col_fg_norm[]      = "#ebdbb2"; /* fg1 */
static const char col_bg_sel[]       = "#fe8019"; /* bright orange, matches col_border_sel */
static const char col_fg_sel[]       = "#282828"; /* bg0, dark text for contrast on orange */
static const char col_bg_urg[]       = "#fb4934"; /* bright red: a tag is asking for attention */
static const char col_border_norm[]  = "#504945"; /* bg2 */
static const char col_border_sel[]   = "#fe8019"; /* bright orange */

/* tags */
static const char *tags[TAGS] = { "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX" };


/* helper for spawning shell commands, dwm-style */
#define SHCMD(cmd) { .com = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }
#define TAGKEYS(KEY,TAG) \
	{ MODKEY,            KEY, view,      {.i = TAG} }, \
	{ MODKEY|ShiftMask,  KEY, movetotag, {.i = TAG} },

static const char *dmenucmd[] = { "dmenu_run", "-fn", "terminus-16", "-nb", col_bg_norm, "-nf", col_fg_norm, "-sb", col_bg_sel, "-sf", col_fg_sel, NULL };
static const char *termcmd[]  = { "st", NULL };

static Key keys[] = {
	/* modifier            key                       function     argument */
	{ MODKEY,              XK_Tab,                    view,             {.i = -1} },
	TAGKEYS(               XK_1,                      0)
	TAGKEYS(               XK_2,                      1)
	TAGKEYS(               XK_3,                      2)
	TAGKEYS(               XK_4,                      3)
	TAGKEYS(               XK_5,                      4)
	TAGKEYS(               XK_6,                      5)
	TAGKEYS(               XK_7,                      6)
	TAGKEYS(               XK_8,                      7)
	TAGKEYS(               XK_9,                      8)

	{ MODKEY|ShiftMask,    XK_q,                      quit,             {0} },
	{ MODKEY,              XK_w,                      killclient,       {0} },
	{ MODKEY,              XK_b,                      togglebar,        {0} },
	{ MODKEY,              XK_r,                      spawn,            SHCMD(TERMINAL " -e ranger") },
	{ MODKEY|ShiftMask,    XK_r,                      spawn,            SHCMD(TERMINAL " -e htop") },
	{ MODKEY,              XK_p,                      spawn,            {.com = dmenucmd} },
	{ MODKEY,              XK_Return,                 spawn,            {.com = termcmd} },
	{ MODKEY,              XK_n,                      spawn,            SHCMD("brave") },

	{ MODKEY,              XK_j,                      focusstack,       {.i = +1} },
	{ MODKEY,              XK_k,                      focusstack,       {.i = -1} },
	{ MODKEY,              XK_space,                  togglemax,        {0} },
	{ MODKEY,              XK_f,                      togglefullscreen, {0} },


	{ MODKEY,              XK_Escape,                 spawn,            SHCMD("slock & xset dpms force off") },
	{ 0,                   XK_Print,                  spawn,            SHCMD("scrot -f -s -q 100 -e 'xclip -selection clipboard -target image/png -i $f && rm $f'") },
	{ MODKEY,              XK_Print,                  spawn,            SHCMD("scrot -f -q 100 -e 'xclip -selection clipboard -target image/png -i $f && rm $f'") },

	/* volume, on MODKEY+z/x/c - the keys the OpenBSD dwm config used. wpctl
	 * talks to WirePlumber, which is what actually runs on this machine. */
	{ MODKEY,              XK_z,                      spawn,            SHCMD("wpctl set-mute @DEFAULT_SINK@ toggle && kill -30 $(cat $HOME/.cache/barpid)") },
	{ MODKEY,              XK_x,                      spawn,            SHCMD("wpctl set-volume @DEFAULT_SINK@ 5%- && kill -30 $(cat $HOME/.cache/barpid)") },
	{ MODKEY,              XK_c,                      spawn,            SHCMD("wpctl set-volume @DEFAULT_SINK@ 5%+ && kill -30 $(cat $HOME/.cache/barpid)") },

	/* the form these replaced. `amixer -D pulse` wants the ALSA PulseAudio
	 * plugin, which this machine has not got - it runs PipeWire/WirePlumber,
	 * so the keys silently did nothing ("Mixer attach pulse error"). Plain
	 * `amixer sset Master ...` without -D works too, if wpctl ever goes away. */
	//{ 0,                   XF86XK_AudioMute,          spawn,            SHCMD("amixer -D pulse sset Master toggle") },
	//{ 0,                   XF86XK_AudioLowerVolume,   spawn,            SHCMD("amixer -D pulse sset Master 5%-") },
	//{ 0,                   XF86XK_AudioRaiseVolume,   spawn,            SHCMD("amixer -D pulse sset Master 5%+") },
	/* ~/.scripts/sp no longer exists - there is no music script on this
	 * machine any more, so these three did nothing when pressed */
	//{ MODKEY,              XF86XK_AudioMute,          spawn,            SHCMD("~/.scripts/sp play") },
	//{ MODKEY,              XF86XK_AudioLowerVolume,   spawn,            SHCMD("~/.scripts/sp prev") },
	//{ MODKEY,              XF86XK_AudioRaiseVolume,   spawn,            SHCMD("~/.scripts/sp next") },
	{ 0,                   XF86XK_MonBrightnessUp,    spawn,            SHCMD("xbacklight -inc 10 && kill -30 $(cat $HOME/.cache/barpid)") },
	{ 0,                   XF86XK_MonBrightnessDown,  spawn,            SHCMD("xbacklight -dec 10 && kill -30 $(cat $HOME/.cache/barpid)") },
};
