#ifndef OPENDOOR_CURSES_COMPAT_H
#define OPENDOOR_CURSES_COMPAT_H

#if defined(__has_include)
#  if __has_include(<ncursesw/curses.h>)
#    include <ncursesw/curses.h>
#    define OD_SYSTEM_CURSES_HEADER 1
#  elif __has_include(<ncurses.h>)
#    include <ncurses.h>
#    define OD_SYSTEM_CURSES_HEADER 1
#  endif
#endif

#ifndef OD_SYSTEM_CURSES_HEADER
#include <stdbool.h>

typedef struct _win_st WINDOW;
typedef unsigned long mmask_t;
typedef struct {
    short id;
    int x;
    int y;
    int z;
    mmask_t bstate;
} MEVENT;

WINDOW *initscr(void);
int endwin(void);
int noecho(void);
int cbreak(void);
int curs_set(int visibility);
int keypad(WINDOW *window, bool enabled);
void wtimeout(WINDOW *window, int delay);
int wgetch(WINDOW *window);
int getmaxx(const WINDOW *window);
int getmaxy(const WINDOW *window);
int werase(WINDOW *window);
int wmove(WINDOW *window, int y, int x);
int waddnstr(WINDOW *window, const char *text, int count);
int wattrset(WINDOW *window, int attributes);
int wnoutrefresh(WINDOW *window);
int doupdate(void);
int start_color(void);
int use_default_colors(void);
int has_colors(void);
int init_pair(short pair, short foreground, short background);
mmask_t mousemask(mmask_t newmask, mmask_t *oldmask);
int getmouse(MEVENT *event);

#define OK 0
#define ERR (-1)
#define KEY_DOWN 0402
#define KEY_UP 0403
#define KEY_LEFT 0404
#define KEY_RIGHT 0405
#define KEY_HOME 0406
#define KEY_BACKSPACE 0407
#define KEY_NPAGE 0522
#define KEY_PPAGE 0523
#define KEY_END 0550
#define KEY_MOUSE 0631
#define KEY_RESIZE 0632
#define ALL_MOUSE_EVENTS ((mmask_t)0x1fffffffUL)
#define REPORT_MOUSE_POSITION ((mmask_t)0x10000000UL)
#define BUTTON1_CLICKED ((mmask_t)0x00000004UL)
#define BUTTON1_DOUBLE_CLICKED ((mmask_t)0x00000008UL)
#define BUTTON4_PRESSED ((mmask_t)0x00010000UL)
#define BUTTON5_PRESSED ((mmask_t)0x00200000UL)
#define A_REVERSE 0x00040000
#define A_BOLD 0x00200000
#define COLOR_PAIR(number) ((number) << 8)
#endif

#endif
