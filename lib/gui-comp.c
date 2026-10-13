/* UniBin GUI component library: labels, buttons, message boxes. */
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>

static void ub_gui_label(const char *text, int x, int y) {
    if (ub_nlabels < 128) {
        snprintf(ub_labels[ub_nlabels].text, sizeof ub_labels[ub_nlabels].text, "%s", text);
        ub_labels[ub_nlabels].x = x;
        ub_labels[ub_nlabels].y = y;
        ub_nlabels++;
    }
}

static void ub_gui_button(const char *text, int id) {
    if (ub_nbuttons < 64) {
        UBButton *b = &ub_buttons[ub_nbuttons++];
        snprintf(b->text, sizeof b->text, "%s", text);
        b->id = id;
        b->x = 20 + ((ub_nbuttons - 1) % 4) * 120;
        b->y = ub_height - 65 - ((ub_nbuttons - 1) / 4) * 45;
        b->w = 105; b->h = 32; b->handler = NULL;
    }
}

static void ub_gui_bind(int id, UBHandler h) {
    for (int i = 0; i < ub_nbuttons; ++i)
        if (ub_buttons[i].id == id) ub_buttons[i].handler = h;
}

static void ub_gui_msgbox(const char *message) {
    if (!ub_dpy) {
        ub_gui_window("Message", 400, 140);
        if (!ub_dpy) return;
    }
    Window w = XCreateSimpleWindow(ub_dpy, RootWindow(ub_dpy, ub_screen), 120, 120,
        400, 140, 1, BlackPixel(ub_dpy, ub_screen), WhitePixel(ub_dpy, ub_screen));
    XStoreName(ub_dpy, w, "UniBin message");
    XSelectInput(ub_dpy, w, ExposureMask | ButtonPressMask);
    XMapWindow(ub_dpy, w);
    for (;;) {
        XEvent e; XNextEvent(ub_dpy, &e);
        if (e.type == Expose) {
            XDrawString(ub_dpy, w, ub_gc, 20, 40, message, (int)strlen(message));
            XDrawRectangle(ub_dpy, w, ub_gc, 160, 80, 80, 30);
            XDrawString(ub_dpy, w, ub_gc, 184, 100, "OK", 2);
        } else if (e.type == ButtonPress) {
            XDestroyWindow(ub_dpy, w);
            break;
        }
    }
}
