/*
Usage:
    mtail [-c ncol] file1 file2 file3 ...

Description:

    Tail multiple files.  The files are shown in a grid of windows, with the
    number of columns specified by the -c option.  By default a single column
    is used.

    e.g. for two columns

        --------n1---------------n4-------
                        |
                        |
                        |
                        |
        --------n2---------------n5-------
                        |
                        |
                        |
                        |
        --------n3---------------n6-------
                        |
                        |
                        |
                        |

    where n1,n2... etc. show where the file names are displayed.  The number of
    rows is automatically adjusted to fit all the files within the specified
    number of columns.

    If the file names become too long, they are truncated to fit the window
    with a preceding ...

    The time each file was last modified is shown on the right side of its
    title bar.  The date is also shown if the file was not modified today.

    Files are followed by name, so if a file is truncated or replaced (e.g. by
    log rotation) the new contents are shown.  The layout is redrawn when the
    terminal is resized.

    To exit the program, hit q or ctrl-c

Dependencies:
    The ncurses library and headers.  On ubuntu/debian you may have to install
    the development package:

        sudo apt-get install libncurses-dev

Copyright (C) 2010  Erin Sheldon (erin dot sheldon at gmail dot com)
                    and Eli Rykoff (erykoff at physics dot ucsb dot edu )

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#define _POSIX_C_SOURCE 200809L
/* for wcwidth; pkg-config for ncursesw may already define it */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <curses.h>

/* how often to check the files for new data, in milliseconds */
#define POLL_MS 1000

#define READ_BUFSIZE 8192

/* smallest usable window */
#define MIN_WIN_ROWS 1
#define MIN_WIN_COLS 4

/* lines kept beyond what fits in the window, so cursor up movements (e.g.
 * nested tqdm progress bars) have somewhere to go */
#define EXTRA_LINES 64

/* longer lines are cut off */
#define MAX_LINE_CHARS 16384

#define TAB_WIDTH 8

/* state of escape sequence parsing */
enum esc_state {
    ESC_NONE,
    ESC_START, /* got ESC */
    ESC_CSI,   /* got ESC [ */
    ESC_OSC,   /* got ESC ], ends with BEL or ESC \ */
    ESC_OSC_ESC
};

struct line {
    wchar_t *s;
    int len;
    int cap;
};

struct tail {
    const char *fname;
    int fd;
    WINDOW *win;

    /* the text is interpreted like a terminal would, so carriage returns
     * and cursor movements, as used by progress bars, work.  These are the
     * most recent lines of the file and the cursor position, as line index
     * and character index */
    struct line *lines;
    int nlines;
    int maxlines;
    int cy;
    int cx;

    mbstate_t mbs;
    enum esc_state esc;
    char escbuf[32];
    int esclen;

    /* modification time of the file as shown in the title bar */
    char timestr[32];

    /* these are row,col in characters on the screen, not the file matrix */
    int startrow;
    int numrows;

    int startcol;
    int numcols;
};

struct mtail {
    int numfiles;

    int ncol; /* number of columns of files */
    int nrow;
    int xmax; /* size of whole screen */
    int ymax;

    int ywinsize;     /* size of each window if perfect fit */
    int extra_ychars; /* extra characters to go in first row */

    int xwinsize;
    int extra_xchars; /* extra characters to go in first column */

    int too_small; /* terminal is too small to show the layout */

    /* one per file + window */
    struct tail *tst;
};

static volatile sig_atomic_t quit_requested = 0;

static void handle_signal(int sig) {
    (void)sig;
    quit_requested = 1;
}

static void usage(FILE *stream) {
    fprintf(stream, "Usage: mtail [-c ncol] file1 [file2 file3 ...]\n");
}

static int parse_command_line(int argc, char *argv[], int *ncol) {
    int c;
    char *end;
    long val;

    *ncol = 1;

    while ((c = getopt(argc, argv, "c:h")) != -1) {
        switch (c) {
            case 'c':
                errno = 0;
                val = strtol(optarg, &end, 10);
                if (errno != 0 || *end != '\0' || val <= 0 || val > 1000) {
                    fprintf(stderr, "number of columns must be an integer > 0\n");
                    exit(EXIT_FAILURE);
                }
                *ncol = (int)val;
                break;
            case 'h':
                usage(stdout);
                exit(EXIT_SUCCESS);
            default:
                usage(stderr);
                exit(EXIT_FAILURE);
        }
    }

    if (optind >= argc) {
        usage(stderr);
        exit(EXIT_FAILURE);
    }

    return optind;
}

static struct mtail *mtail_new(int numfiles, int ncol) {
    struct mtail *mtst;

    mtst = calloc(1, sizeof(*mtst));
    if (mtst == NULL) {
        perror("could not allocate struct mtail");
        exit(EXIT_FAILURE);
    }

    mtst->tst = calloc(numfiles, sizeof(*mtst->tst));
    if (mtst->tst == NULL) {
        perror("could not allocate struct tail");
        exit(EXIT_FAILURE);
    }

    mtst->numfiles = numfiles;

    /* no point in more columns than files */
    mtst->ncol = ncol < numfiles ? ncol : numfiles;

    return mtst;
}

static void free_lines(struct tail *tst) {
    int i;

    for (i = 0; i < tst->nlines; i++) {
        free(tst->lines[i].s);
    }
    free(tst->lines);
    tst->lines = NULL;
    tst->nlines = 0;
}

static void mtail_free(struct mtail *mtst) {
    int i;

    for (i = 0; i < mtst->numfiles; i++) {
        if (mtst->tst[i].win != NULL) {
            delwin(mtst->tst[i].win);
        }
        if (mtst->tst[i].fd >= 0) {
            close(mtst->tst[i].fd);
        }
        free_lines(&mtst->tst[i]);
    }
    free(mtst->tst);
    free(mtst);
}

static void open_files(char *argv[], int ind, struct mtail *mtst) {
    int i, j;
    struct tail *tst = mtst->tst;

    for (i = 0; i < mtst->numfiles; i++) {
        tst[i].fname = argv[ind + i];
        tst[i].fd = open(tst[i].fname, O_RDONLY);
        if (tst[i].fd < 0) {
            fprintf(stderr, "Failed to open %s: %s\n", tst[i].fname, strerror(errno));
            for (j = 0; j < i; j++) {
                close(tst[j].fd);
            }
            exit(EXIT_FAILURE);
        }
    }
}

static void init_screen(void) {
    struct sigaction sa;

    setlocale(LC_ALL, "");

    if (initscr() == NULL) {
        fprintf(stderr, "initscr failed\n");
        exit(EXIT_FAILURE);
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(POLL_MS);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

static int yseparator_position(struct mtail *mtst, int row) {
    if (row == 0) {
        return 0;
    }
    return (mtst->ywinsize + 1) * row + mtst->extra_ychars;
}

/* there is no separator for column 0 */
static int xseparator_position(struct mtail *mtst, int col) {
    return mtst->extra_xchars + mtst->xwinsize * col + (col - 1);
}

/* returns 0 if the terminal is too small to fit the layout */
static int set_geometry(struct mtail *mtst) {
    struct tail *tst = mtst->tst;
    int i, row, col;

    mtst->nrow = (mtst->numfiles + mtst->ncol - 1) / mtst->ncol;
    /* e.g. -c 3 with 4 files only fills 2 columns */
    mtst->ncol = (mtst->numfiles + mtst->nrow - 1) / mtst->nrow;

    getmaxyx(stdscr, mtst->ymax, mtst->xmax);

    /* There will be a separator above each row, so subtract nrow */
    mtst->ywinsize     = (mtst->ymax - mtst->nrow) / mtst->nrow;
    mtst->extra_ychars = mtst->ymax - mtst->ywinsize * mtst->nrow - mtst->nrow;

    /* only separator *between* columns, so only subtract ncol-1 */
    mtst->xwinsize     = (mtst->xmax - (mtst->ncol - 1)) / mtst->ncol;
    mtst->extra_xchars = mtst->xmax - mtst->xwinsize * mtst->ncol - (mtst->ncol - 1);

    if (mtst->ywinsize < MIN_WIN_ROWS || mtst->xwinsize < MIN_WIN_COLS) {
        return 0;
    }

    for (i = 0; i < mtst->numfiles; i++) {
        row = i % mtst->nrow;
        col = i / mtst->nrow;

        if (row == 0) {
            tst[i].numrows = mtst->ywinsize + mtst->extra_ychars;
        } else {
            tst[i].numrows = mtst->ywinsize;
        }
        tst[i].startrow = yseparator_position(mtst, row) + 1;

        if (col == 0) {
            tst[i].startcol = 0;
            tst[i].numcols = mtst->xwinsize + mtst->extra_xchars;
        } else {
            tst[i].startcol = xseparator_position(mtst, col) + 1;
            tst[i].numcols = mtst->xwinsize;
        }
    }

    return 1;
}

static void draw_borders(struct mtail *mtst) {
    struct tail *tst = mtst->tst;
    int i, row, col, x, y;

    for (col = 1; col < mtst->ncol; col++) {
        x = xseparator_position(mtst, col);
        mvvline(0, x, ACS_VLINE, mtst->ymax);
    }

    for (row = 0; row < mtst->nrow; row++) {
        y = yseparator_position(mtst, row);
        mvhline(y, 0, ACS_HLINE, mtst->xmax);
    }

    /* fill in the gaps where the lines cross */
    for (i = 0; i < mtst->numfiles; i++) {
        row = i % mtst->nrow;
        col = i / mtst->nrow;

        if (col != mtst->ncol - 1) {
            y = tst[i].startrow - 1;
            x = tst[i].startcol + tst[i].numcols;
            /* T shape on top so we don't protrude above the line */
            mvaddch(y, x, row == 0 ? ACS_TTEE : ACS_PLUS);
        }
    }
}

static const char *get_basename(const char *path) {
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

/* format the modification time of the file.  Only the time is shown for
 * files modified today, otherwise the date is included.  Returns 1 if the
 * string changed */
static int update_timestr(struct tail *tst) {
    struct stat st;
    struct tm tm_mod, tm_now;
    time_t now;
    char buf[sizeof(tst->timestr)];

    buf[0] = '\0';
    if (fstat(tst->fd, &st) == 0) {
        now = time(NULL);
        if (localtime_r(&st.st_mtime, &tm_mod) != NULL
                && localtime_r(&now, &tm_now) != NULL) {
            if (tm_mod.tm_year == tm_now.tm_year
                    && tm_mod.tm_yday == tm_now.tm_yday) {
                strftime(buf, sizeof(buf), "%H:%M:%S", &tm_mod);
            } else {
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_mod);
            }
        }
    }

    if (strcmp(buf, tst->timestr) == 0) {
        return 0;
    }
    strcpy(tst->timestr, buf);
    return 1;
}

/* draw the separator line above the window, with the file name centered and
 * the modification time on the right */
static void draw_title(struct tail *tst) {
    const char *bname = get_basename(tst->fname);
    int x, y, len, maxlen, tlen, name_end;

    y = tst->startrow - 1;
    mvhline(y, tst->startcol, ACS_HLINE, tst->numcols);

    /* the name gets the space up to the last column, minus the time and a
     * one character gap if there is room for them */
    name_end = tst->startcol + tst->numcols - 1;
    tlen = (int)strlen(tst->timestr);
    if (tlen > 0 && tst->numcols >= tlen + 6) {
        x = name_end - tlen;
        mvaddstr(y, x, tst->timestr);
        name_end = x - 1;
    }

    len = (int)strlen(bname);
    maxlen = name_end - (tst->startcol + 1);

    attron(A_BOLD);
    if (len > maxlen) {
        /* show the end of the name with a preceding ... */
        x = tst->startcol + 1;
        if (maxlen > 3) {
            mvprintw(y, x, "...%s", bname + len - (maxlen - 3));
        } else {
            mvaddstr(y, x, bname + len - maxlen);
        }
    } else {
        /* centered, but pushed left if it would run into the time */
        x = tst->startcol + (tst->numcols - len) / 2;
        if (x + len > name_end) {
            x = name_end - len;
        }
        mvaddstr(y, x, bname);
    }
    attroff(A_BOLD);
}

static void draw_titles(struct mtail *mtst) {
    int i;

    for (i = 0; i < mtst->numfiles; i++) {
        update_timestr(&mtst->tst[i]);
        draw_title(&mtst->tst[i]);
    }
}

static void create_windows(struct mtail *mtst) {
    struct tail *tst = mtst->tst;
    int i;

    for (i = 0; i < mtst->numfiles; i++) {
        tst[i].win = newwin(tst[i].numrows, tst[i].numcols,
                            tst[i].startrow, tst[i].startcol);
        if (tst[i].win == NULL) {
            endwin();
            fprintf(stderr, "failed to create window\n");
            exit(EXIT_FAILURE);
        }
    }
}

static void destroy_windows(struct mtail *mtst) {
    int i;

    for (i = 0; i < mtst->numfiles; i++) {
        if (mtst->tst[i].win != NULL) {
            delwin(mtst->tst[i].win);
            mtst->tst[i].win = NULL;
        }
    }
}

/* forget all text, e.g. when the file is truncated or the window resized */
static void reset_text(struct tail *tst) {
    free_lines(tst);

    tst->maxlines = tst->numrows + EXTRA_LINES;
    tst->lines = calloc((size_t)tst->maxlines, sizeof(*tst->lines));
    if (tst->lines == NULL) {
        endwin();
        perror("could not allocate lines");
        exit(EXIT_FAILURE);
    }
    /* there is always a line for the cursor to be on */
    tst->nlines = 1;
    tst->cy = 0;
    tst->cx = 0;

    memset(&tst->mbs, 0, sizeof(tst->mbs));
    tst->esc = ESC_NONE;
    tst->esclen = 0;
}

static void newline(struct tail *tst) {
    tst->cx = 0;

    if (tst->cy < tst->nlines - 1) {
        tst->cy++;
        return;
    }

    if (tst->nlines == tst->maxlines) {
        /* drop the oldest line */
        free(tst->lines[0].s);
        memmove(&tst->lines[0], &tst->lines[1],
                (size_t)(tst->nlines - 1) * sizeof(*tst->lines));
        tst->nlines--;
    }
    memset(&tst->lines[tst->nlines], 0, sizeof(*tst->lines));
    tst->nlines++;
    tst->cy = tst->nlines - 1;
}

/* write a printable character at the cursor, overwriting what is there */
static void put_wchar(struct tail *tst, wchar_t wc) {
    struct line *ln = &tst->lines[tst->cy];
    wchar_t *s;
    int newcap;

    if (tst->cx >= MAX_LINE_CHARS) {
        return;
    }

    if (tst->cx >= ln->cap) {
        newcap = ln->cap > 0 ? ln->cap : 128;
        while (newcap <= tst->cx) {
            newcap *= 2;
        }
        if (newcap > MAX_LINE_CHARS) {
            newcap = MAX_LINE_CHARS;
        }
        s = realloc(ln->s, (size_t)newcap * sizeof(*s));
        if (s == NULL) {
            return;
        }
        ln->s = s;
        ln->cap = newcap;
    }

    /* the cursor may have been moved past the end, e.g. by a tab */
    while (ln->len < tst->cx) {
        ln->s[ln->len++] = L' ';
    }
    ln->s[tst->cx++] = wc;
    if (ln->len < tst->cx) {
        ln->len = tst->cx;
    }
}

/* handle a complete ESC [ ... sequence; final is the last character */
static void handle_csi(struct tail *tst, int final) {
    struct line *ln = &tst->lines[tst->cy];
    int i, n, has_n;

    /* only the first numeric parameter matters for what we support */
    tst->escbuf[tst->esclen] = '\0';
    n = atoi(tst->escbuf);
    has_n = tst->esclen > 0 && tst->escbuf[0] >= '0' && tst->escbuf[0] <= '9';

    switch (final) {
        case 'A': /* cursor up */
            tst->cy -= has_n && n > 0 ? n : 1;
            if (tst->cy < 0) {
                tst->cy = 0;
            }
            break;
        case 'B': /* cursor down */
            tst->cy += has_n && n > 0 ? n : 1;
            if (tst->cy > tst->nlines - 1) {
                tst->cy = tst->nlines - 1;
            }
            break;
        case 'C': /* cursor forward */
            tst->cx += has_n && n > 0 ? n : 1;
            if (tst->cx > MAX_LINE_CHARS) {
                tst->cx = MAX_LINE_CHARS;
            }
            break;
        case 'D': /* cursor back */
            tst->cx -= has_n && n > 0 ? n : 1;
            if (tst->cx < 0) {
                tst->cx = 0;
            }
            break;
        case 'G': /* cursor to column */
            tst->cx = has_n && n > 0 ? n - 1 : 0;
            if (tst->cx > MAX_LINE_CHARS) {
                tst->cx = MAX_LINE_CHARS;
            }
            break;
        case 'K': /* erase in line */
            if (n == 0) {
                if (ln->len > tst->cx) {
                    ln->len = tst->cx;
                }
            } else if (n == 1) {
                for (i = 0; i <= tst->cx && i < ln->len; i++) {
                    ln->s[i] = L' ';
                }
            } else {
                ln->len = 0;
            }
            break;
        case 'J': /* erase in display */
            if (n == 0) {
                /* from the cursor to the end */
                if (ln->len > tst->cx) {
                    ln->len = tst->cx;
                }
                for (i = tst->cy + 1; i < tst->nlines; i++) {
                    free(tst->lines[i].s);
                }
                tst->nlines = tst->cy + 1;
            } else if (n == 2 || n == 3) {
                /* clear screen */
                reset_text(tst);
            }
            break;
        default:
            /* colors and everything else are ignored */
            break;
    }
}

/* interpret one character of the file */
static void put_char(struct tail *tst, wchar_t wc) {
    switch (tst->esc) {
        case ESC_START:
            if (wc == L'[') {
                tst->esc = ESC_CSI;
                tst->esclen = 0;
            } else if (wc == L']') {
                tst->esc = ESC_OSC;
            } else {
                tst->esc = ESC_NONE;
            }
            return;
        case ESC_CSI:
            if (wc >= 0x40 && wc <= 0x7e) {
                handle_csi(tst, (int)wc);
                tst->esc = ESC_NONE;
            } else if (wc < 0x20 || wc > 0x3f) {
                /* not a valid sequence, give up on it */
                tst->esc = ESC_NONE;
            } else if (tst->esclen < (int)sizeof(tst->escbuf) - 1) {
                tst->escbuf[tst->esclen++] = (char)wc;
            }
            return;
        case ESC_OSC:
            if (wc == L'\a') {
                tst->esc = ESC_NONE;
            } else if (wc == 0x1b) {
                tst->esc = ESC_OSC_ESC;
            }
            return;
        case ESC_OSC_ESC:
            tst->esc = ESC_NONE;
            return;
        case ESC_NONE:
            break;
    }

    switch (wc) {
        case 0x1b:
            tst->esc = ESC_START;
            break;
        case L'\n':
            newline(tst);
            break;
        case L'\r':
            tst->cx = 0;
            break;
        case L'\b':
            if (tst->cx > 0) {
                tst->cx--;
            }
            break;
        case L'\t':
            tst->cx = (tst->cx / TAB_WIDTH + 1) * TAB_WIDTH;
            if (tst->cx > MAX_LINE_CHARS) {
                tst->cx = MAX_LINE_CHARS;
            }
            break;
        default:
            if (iswcntrl((wint_t)wc)) {
                break;
            }
            put_wchar(tst, wcwidth(wc) < 0 ? L'?' : wc);
            break;
    }
}

/* interpret text read from the file.  Multibyte characters may be split
 * across calls */
static void put_text(struct tail *tst, const char *buf, size_t n) {
    size_t i = 0, r;
    wchar_t wc;

    while (i < n) {
        r = mbrtowc(&wc, buf + i, n - i, &tst->mbs);
        if (r == (size_t)-2) {
            /* incomplete character, the rest will come with the next read */
            break;
        }
        if (r == (size_t)-1) {
            memset(&tst->mbs, 0, sizeof(tst->mbs));
            wc = L'?';
            r = 1;
        } else if (r == 0) {
            /* NUL */
            wc = L'?';
            r = 1;
        }
        i += r;
        put_char(tst, wc);
    }
}

static int char_width(wchar_t wc) {
    int w = wcwidth(wc);
    return w < 0 ? 1 : w;
}

/* find the end of the part of the line, starting at character start, that
 * fits on one row of the window */
static int row_end(struct tail *tst, const struct line *ln, int start) {
    int i, w, width = 0;

    for (i = start; i < ln->len; i++) {
        w = char_width(ln->s[i]);
        if (width + w > tst->numcols && width > 0) {
            break;
        }
        width += w;
    }
    return i;
}

static int line_rows(struct tail *tst, const struct line *ln) {
    int start = 0, rows = 1;

    while ((start = row_end(tst, ln, start)) < ln->len) {
        rows++;
    }
    return rows;
}

static void draw_row(struct tail *tst, int row, const wchar_t *s, int n) {
    char mb[MB_LEN_MAX];
    mbstate_t mbs;
    size_t k;
    int i;

    memset(&mbs, 0, sizeof(mbs));
    wmove(tst->win, row, 0);
    for (i = 0; i < n; i++) {
        k = wcrtomb(mb, s[i], &mbs);
        if (k == (size_t)-1) {
            memset(&mbs, 0, sizeof(mbs));
            mb[0] = '?';
            k = 1;
        }
        waddnstr(tst->win, mb, (int)k);
    }
}

/* draw the most recent lines, wrapped to the width of the window */
static void render(struct tail *tst) {
    const struct line *ln;
    int i, first, last, start, end, total = 0, skip, row = 0;

    werase(tst->win);

    /* a trailing newline would leave the bottom row blank, so don't show the
     * empty line after it until more text arrives */
    last = tst->nlines - 1;
    if (last > 0 && tst->lines[last].len == 0) {
        last--;
    }

    for (first = last; first > 0; first--) {
        total += line_rows(tst, &tst->lines[first]);
        if (total >= tst->numrows) {
            break;
        }
    }
    if (first == 0) {
        total += line_rows(tst, &tst->lines[0]);
    }

    /* the first line may only partly fit */
    skip = total > tst->numrows ? total - tst->numrows : 0;

    for (i = first; i <= last; i++) {
        ln = &tst->lines[i];
        start = 0;
        do {
            end = row_end(tst, ln, start);
            if (skip > 0) {
                skip--;
            } else {
                draw_row(tst, row++, ln->s + start, end - start);
            }
            start = end;
        } while (start < ln->len);
    }
}

/* read and interpret everything from the current position to end of file.
 * Returns 1 if any data was read */
static int drain(struct tail *tst) {
    char buf[READ_BUFSIZE];
    ssize_t n;
    int got = 0;

    while ((n = read(tst->fd, buf, sizeof(buf))) > 0) {
        put_text(tst, buf, (size_t)n);
        got = 1;
    }
    return got;
}

/* display roughly the last screenful of the file */
static void load_initial_file_data(struct tail *tst) {
    struct stat st;
    off_t want, start = 0;
    char *buf, *p;
    ssize_t n;
    size_t total = 0;

    reset_text(tst);

    want = (off_t)tst->numrows * tst->numcols;

    if (fstat(tst->fd, &st) == 0 && S_ISREG(st.st_mode)) {
        if (st.st_size > want) {
            start = st.st_size - want;
        }
        lseek(tst->fd, start, SEEK_SET);
    }

    if (start == 0) {
        drain(tst);
        render(tst);
        return;
    }

    buf = malloc((size_t)want);
    if (buf == NULL) {
        drain(tst);
        render(tst);
        return;
    }
    while (total < (size_t)want
           && (n = read(tst->fd, buf + total, (size_t)want - total)) > 0) {
        total += (size_t)n;
    }

    /* we probably landed mid line, so skip to the next newline or carriage
     * return, where the cursor is back at the start of a line.  A progress
     * bar may be one long line rewritten with carriage returns */
    for (p = buf; p < buf + total && *p != '\n' && *p != '\r'; p++) {
    }
    if (p < buf + total) {
        put_text(tst, p + 1, total - (size_t)(p + 1 - buf));
    } else {
        put_text(tst, buf, total);
    }
    free(buf);

    drain(tst);
    render(tst);
}

/* check for new data, truncation or replacement of the file.  Returns 1 if
 * the window was updated */
static int check_file(struct tail *tst) {
    struct stat st_fd, st_name;
    off_t pos;
    int fd, updated = 0;

    if (fstat(tst->fd, &st_fd) < 0) {
        return 0;
    }

    if (S_ISREG(st_fd.st_mode)) {
        pos = lseek(tst->fd, 0, SEEK_CUR);
        if (pos >= 0 && st_fd.st_size < pos) {
            /* file was truncated, start again from the beginning */
            lseek(tst->fd, 0, SEEK_SET);
            reset_text(tst);
            updated = 1;
        }
    }

    updated |= drain(tst);

    /* follow by name, in case the file was replaced, e.g. by log rotation */
    if (stat(tst->fname, &st_name) == 0
            && (st_name.st_ino != st_fd.st_ino || st_name.st_dev != st_fd.st_dev)) {
        fd = open(tst->fname, O_RDONLY);
        if (fd >= 0) {
            close(tst->fd);
            tst->fd = fd;
            updated |= drain(tst);
        }
    }

    if (updated) {
        render(tst);
    }
    return updated;
}

/* (re)build the whole display, e.g. at startup or after a resize */
static void layout(struct mtail *mtst) {
    int i;

    destroy_windows(mtst);
    erase();

    mtst->too_small = !set_geometry(mtst);
    if (mtst->too_small) {
        mvaddstr(0, 0, "Terminal too small");
        refresh();
        return;
    }

    draw_borders(mtst);
    draw_titles(mtst);
    wnoutrefresh(stdscr);

    create_windows(mtst);
    for (i = 0; i < mtst->numfiles; i++) {
        load_initial_file_data(&mtst->tst[i]);
        wnoutrefresh(mtst->tst[i].win);
    }
    doupdate();
}

static void tail_files(struct mtail *mtst) {
    int i, ch, updated, titles_updated;

    layout(mtst);

    while (!quit_requested) {
        /* waits up to POLL_MS for a key press */
        ch = getch();
        if (ch == 'q' || ch == 'Q') {
            break;
        }
        if (ch == KEY_RESIZE) {
            layout(mtst);
            continue;
        }
        if (mtst->too_small) {
            continue;
        }

        updated = 0;
        titles_updated = 0;
        for (i = 0; i < mtst->numfiles; i++) {
            if (check_file(&mtst->tst[i])) {
                wnoutrefresh(mtst->tst[i].win);
                updated = 1;
            }
            /* also catches the date appearing after midnight */
            if (update_timestr(&mtst->tst[i])) {
                draw_title(&mtst->tst[i]);
                titles_updated = 1;
            }
        }
        if (titles_updated) {
            wnoutrefresh(stdscr);
            updated = 1;
        }
        if (updated) {
            doupdate();
        }
    }
}

int main(int argc, char *argv[]) {
    struct mtail *mtst;
    int ncol, ind;

    ind = parse_command_line(argc, argv, &ncol);

    mtst = mtail_new(argc - ind, ncol);
    open_files(argv, ind, mtst);

    init_screen();
    tail_files(mtst);
    endwin();

    mtail_free(mtst);

    return EXIT_SUCCESS;
}
