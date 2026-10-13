/* UniBin Linux display library: X11 window and event loop. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*UBHandler)(void);
typedef struct { char text[512]; int x, y; } UBLabel;
typedef struct { char text[256]; int id, x, y, w, h; UBHandler handler; } UBButton;

static Display *ub_dpy = NULL;
static int ub_screen = 0;
static Window ub_win = 0;
static GC ub_gc;
static int ub_gui_open = 0, ub_gui_should_exit = 0;
static char ub_title[256] = "UniBin";
static int ub_width = 520, ub_height = 320;
static UBLabel ub_labels[128];
static int ub_nlabels = 0;
static UBButton ub_buttons[64];
static int ub_nbuttons = 0;

static void ub_gui_draw(void) {
    if (!ub_dpy || !ub_win) return;
    XClearWindow(ub_dpy, ub_win);
    for (int i = 0; i < ub_nlabels; ++i)
        XDrawString(ub_dpy, ub_win, ub_gc, ub_labels[i].x, ub_labels[i].y,
                    ub_labels[i].text, (int)strlen(ub_labels[i].text));
    for (int i = 0; i < ub_nbuttons; ++i) {
        UBButton *b = &ub_buttons[i];
        XDrawRectangle(ub_dpy, ub_win, ub_gc, b->x, b->y, (unsigned)b->w, (unsigned)b->h);
        int tw = (int)strlen(b->text) * 6;
        XDrawString(ub_dpy, ub_win, ub_gc, b->x + (b->w - tw) / 2,
                    b->y + 20, b->text, (int)strlen(b->text));
    }
}

static void ub_gui_window(const char *title, int w, int h) {
    snprintf(ub_title, sizeof ub_title, "%s", title);
    if (w > 100 && w < 4096) ub_width = w;
    if (h > 80 && h < 4096) ub_height = h;
    if (ub_dpy) return;
    ub_dpy = XOpenDisplay(NULL);
    if (!ub_dpy) {
        fprintf(stderr, "UniBin GUI: cannot open X display (is a desktop session running?)\n");
        return;
    }
    ub_screen = DefaultScreen(ub_dpy);
    ub_win = XCreateSimpleWindow(ub_dpy, RootWindow(ub_dpy, ub_screen), 80, 80,
        (unsigned)ub_width, (unsigned)ub_height, 1,
        BlackPixel(ub_dpy, ub_screen), WhitePixel(ub_dpy, ub_screen));
    XStoreName(ub_dpy, ub_win, ub_title);
    XSelectInput(ub_dpy, ub_win, ExposureMask | ButtonPressMask | StructureNotifyMask);
    ub_gc = XCreateGC(ub_dpy, ub_win, 0, NULL);
    XMapWindow(ub_dpy, ub_win);
    ub_gui_open = 1;
}

static void ub_gui_exit(void) { ub_gui_should_exit = 1; }

static void ub_gui_run(void) {
    if (!ub_dpy || !ub_win) return;
    XEvent e;
    while (!ub_gui_should_exit) {
        XNextEvent(ub_dpy, &e);
        if (e.type == Expose && e.xexpose.count == 0) ub_gui_draw();
        else if (e.type == ConfigureNotify) {
            ub_width = e.xconfigure.width;
            ub_height = e.xconfigure.height;
            for (int i = 0; i < ub_nbuttons; ++i)
                ub_buttons[i].y = ub_height - 65 - (i / 4) * 45;
            ub_gui_draw();
        } else if (e.type == ButtonPress) {
            int x = e.xbutton.x, y = e.xbutton.y;
            for (int i = 0; i < ub_nbuttons; ++i) {
                UBButton *b = &ub_buttons[i];
                if (x >= b->x && x <= b->x + b->w && y >= b->y && y <= b->y + b->h) {
                    if (b->handler) b->handler();
                    break;
                }
            }
        }
    }
    XDestroyWindow(ub_dpy, ub_win);
    XCloseDisplay(ub_dpy);
    ub_dpy = NULL; ub_win = 0; ub_gui_open = 0;
}
