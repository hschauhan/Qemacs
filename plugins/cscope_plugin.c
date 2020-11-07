/*
 * cscope mode for QEmacs.
 * Copyright (c) 2020 Himanshu Chauhan
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "qe.h"
#include <pwd.h>
#include <string.h>

typedef struct CscopeOutput {
    char file[1024];
    int line;
    char sym[256];
    char context[1024];
} CscopeOutput;

typedef struct CscopeContext {
    EditBuffer *cso_buffer;
    EditState *os;
    EditState *cos;
    int op;
    char *sym;
    char symdir[1024];
    CscopeOutput *out;
    int entries;
} CscopeState;

int split_horizontal = 1;

ModeDef cscope_mode;

CscopeState cs;

#define OUTBUF_WIN_SZ	1024

void do_load_at_line(EditState *s, const char *filename, int line);

static void cscope_select_file(EditState *s)
{
    int index;
    char fpath[2048];

    index = list_get_pos(s);
    if (index < 0 || index >= cs.entries)
        return;

    snprintf(fpath, sizeof(fpath), "%s/%s", cs.symdir, cs.out[index].file);
    do_load_at_line(cs.os, fpath, cs.out[index].line);
}

int do_cscope_query(char *symdir, int opc, char *sym, char **response, int *len)
{
    char cs_command[128];
    FILE *co;
    int rc;
    int tlen, nr_reads = 1;
    char *outbuf = NULL, *ob;

    snprintf(cs_command, sizeof(cs_command),
             "cscope -p8 -d -f %s/cscope.out -L%d %s", symdir, opc, sym);

    outbuf = malloc(OUTBUF_WIN_SZ);
    if (outbuf == NULL)
        return -1;

    memset(outbuf, 0, OUTBUF_WIN_SZ);

    co = popen(cs_command, "r");
    if (co == NULL) {
        free(outbuf);
        return -1;
    }

    tlen = 0;
    for(;;) {
        rc = fread(outbuf+tlen, 1, OUTBUF_WIN_SZ, co);
        if (rc == 0 || rc < OUTBUF_WIN_SZ) {
            if (ferror(co)) {
                free(outbuf);
                pclose(co);
                return -1;
            }

            if (feof(co)) {
                tlen += rc;
                goto out;
            }
        }

        tlen += rc;
        nr_reads++;
        ob = realloc(outbuf, (nr_reads * OUTBUF_WIN_SZ));
        if (ob == NULL) {
            free(outbuf);
            pclose(co);
            return -1;
        }
        memset(ob + (nr_reads * OUTBUF_WIN_SZ), 0, OUTBUF_WIN_SZ);
        outbuf = ob;
    }

out:
    *response = outbuf;
    *len = tlen;
    pclose(co);

    if (tlen == 0)
        return -1;

    return 0;
}

void parse_cscope_line(char *line, CscopeOutput *out)
{
    int state = 0;
    int i = 0;
    char lstr[8];

    for (;;) {
        switch(state) {
        case 0: /* file name with relative path */
            while (*line != ' ') {
                if (i >= sizeof(out->file)-1) {
                    line++;
                    continue;
                }
                out->file[i] = *line;
                line++;
                i++;
            }
            out->file[i] = '\0';
            state++;
            line++;
            break;

        case 1: /* symbol scope */
            i = 0;
            while (*line != ' ') {
                if (i >= sizeof(out->sym)-1) {
                    line++;
                    continue;
                }
                out->sym[i] = *line;
                line++;
                i++;
            }
            out->sym[i] = '\0';
            state++;
            line++;
            break;

        case 2: /* line number */
            i = 0;
            while (*line != ' ') {
                if (i >= 7) {
                    line++;
                    continue;
                }
                lstr[i] = *line;
                i++;
                line++;
            }
            lstr[7] = '\0';
            out->line = atoi(lstr);
            state++;
            line++;
            break;

        case 3: /* context or preview of the line */
            i = 0;
            while (*line != '\n' ||
                   *line != '\0' ||
                   *line != 0) {
                       if (i >= sizeof(out->context)-1) {
                           out->context[i-1] = '\0';
                           goto out;
                       }
                       out->context[i] = *line;
                       line++;
                       i++;
            }
            out->context[i] = '\0';
            state++;
            goto out;
            break;

        default:
            break;
        }
    }

 out:
    return;
}

CscopeOutput *parse_cscope_output(char *output, int nr_entries)
{
    int i = 0;
    char *tok;
    CscopeOutput *out = malloc(nr_entries * sizeof(CscopeOutput));
    if (out == NULL)
        return NULL;

    tok = strtok(output, "\n");
    while (tok != NULL) {
        parse_cscope_line(tok, &out[i]);
        i++;
        tok = strtok(NULL, "\n");
    }

    return out;
}

/* show a list of buffers */
void do_cscope_query_and_show(EditState *s)
{
    QEmacsState *qs = s->qe_state;
    EditBuffer *b;
    EditState *e;
    int x, y, rlen, ln, cn;
    char *cs_resp;

    if (do_cscope_query(cs.symdir, cs.op, cs.sym, &cs_resp, &rlen) < 0) {
        put_status(s, "cscope query failed");
        return;
    }

    if ((b = eb_find("*cscope*")) == NULL) {
        b = eb_new("*cscope*", BF_READONLY | BF_SYSTEM);
        if (b == NULL)
            return;
    } else
        /* if found a previous buffer, clear it up */
        eb_delete(b, 0, b->total_size);

    /* write the current cscope output */
    eb_write(b, 0, (unsigned char *)cs_resp, rlen);
    eb_get_pos(b, &ln, &cn, b->total_size);
    cs.entries = ln;
    cs.out = parse_cscope_output(cs_resp, ln);
    (void)cn;

    if (!split_horizontal) {
        x = (s->x2 + s->x1) / 2;
        e = edit_new(b, x, s->y1, s->x2 - x,
                     s->y2 - s->y1, WF_MODELINE);

        s->x2 = x;
        s->flags |= WF_RSEPARATOR;
    } else {
        y = (s->y2 + s->y1) / 2;
        e = edit_new(b, s->x1, y,
                     s->x2 - s->x1, s->y2 - y, 
                     WF_MODELINE | (s->flags & WF_RSEPARATOR));
        s->y2 = y;
    }

    do_set_mode(e, &cscope_mode, NULL);

    qs->active_window = e;
    do_refresh(e);
}

static void do_query_symbol(void *opaque, char *reply)
{
    cs.sym = reply;
    do_cscope_query_and_show(cs.os);
}

static void cscope_find_symbol(EditState *s)
{
    cs.op = 0;
    cs.os = s;

    qe_ungrab_keys();
    minibuffer_edit(NULL, "Symbol: ",
                    NULL, NULL,
                    do_query_symbol, (void *)s);
}

static void cscope_find_global_definition(EditState *s)
{
    cs.op = 1;
    cs.os = s;

    qe_ungrab_keys();
    minibuffer_edit(NULL, "Symbol (definition): ",
                    NULL, NULL,
                    do_query_symbol, (void *)s);
}

static void do_query_symbol_directory(void *opaque, char *reply)
{
    EditState *s = (EditState *)opaque;
    const char *homedir;
    char *or = reply;
    struct stat st;
    char cscope_file[2048];

    if (*reply == '~') {
        if ((homedir = getenv("HOME")) == NULL) {
            homedir = getpwuid(getuid())->pw_dir;
        }

        reply++;

        /* since ~file is also valid check if / is given by user
         * and skip it.
         */
        if (*reply == '/') reply++;
        snprintf(cs.symdir, sizeof(cs.symdir), "%s/%s", homedir, reply);
    } else if (*reply == '/') {
        strncpy(cs.symdir, reply, sizeof(cs.symdir)-1);
        cs.symdir[1023] = '\0';
    } else {
        put_status(s, "Please provide absolute path.");
        goto out;
    }

    if (stat(cs.symdir, &st) < 0) {
        if (errno == ENOENT) {
            put_status(s, "Symbol directory doesn't exist");
        } else {
            put_status(s, "Unknown error in checking symbol directory");
        }
        goto out;
    }

    if ((st.st_mode & S_IFMT) != S_IFDIR) {
        put_status(s, "Symbol path is not a directory");
        goto out;
    }

    snprintf(cscope_file, sizeof(cscope_file), "%s/cscope.out", cs.symdir);

    if (stat(cscope_file, &st) < 0) {
        if (errno == ENOENT) {
            put_status(s, "Not cscope database found at: %s", cs.symdir);
        } else {
            put_status(s, "Uknown error in checking cscope database");
        }
        goto out;
    }

    if ((st.st_mode & S_IFMT) != S_IFREG) {
        put_status(s, "%s is not a regular file", cscope_file);
        goto out;
    }

    free(or);

    return;

out:
    memset(cs.symdir, 0, sizeof(cs.symdir));
    free(or);
}

static void do_cscope_set_symbol_directory(EditState *s)
{
    qe_ungrab_keys();
    minibuffer_edit(NULL, "Symbol File Directory: ",
                    NULL, NULL, do_query_symbol_directory, (void *)s);
}

/* specific bufed commands */
static CmdDef cscope_mode_commands[] = {
    CMD0( KEY_RET, KEY_RIGHT, "cscope-select", cscope_select_file)
    CMD1( KEY_CTRL('g'), KEY_NONE, "delete-window", do_delete_window, 0)
    CMD_DEF_END,
};

static CmdDef cscope_global_commands[] = {
    CMD0( KEY_F12, KEY_NONE, "cscope-set-symbol-directory", do_cscope_set_symbol_directory)
    CMD0( KEY_F2, KEY_NONE, "cscope-find-symbol", cscope_find_symbol)
    CMD0( KEY_F3, KEY_NONE, "cscope-find-global-definition",
         cscope_find_global_definition)
    CMD_DEF_END,
};

static int cscope_mode_init(EditState *s, ModeSavedData *saved_data)
{
    list_mode.mode_init(s, saved_data);

    return 0;
}

static void cscope_mode_close(EditState *s)
{
    list_mode.mode_close(s);
}

static int cscope_mode_probe(ModeProbeData *p)
{
    const char *r;

    /* currently, only use the file extension */
    r = strrchr((char *)p->filename, '.');
    if (r) {
        r++;
        if (!strcasecmp(r, "c")   ||
            !strcasecmp(r, "h")   ||
            !strcasecmp(r, "asm") ||
            !strcasecmp(r, "s")   ||
            !strcasecmp(r, "cpp"))
            return 100;
    }
    return 0;
}

static int cscope_init(void)
{
    /* inherit from list mode */
    memcpy(&cscope_mode, &list_mode, sizeof(ModeDef));
    cscope_mode.name = "cscope";
    cscope_mode.instance_size = sizeof(CscopeState);
    cscope_mode.mode_init = cscope_mode_init;
    cscope_mode.mode_probe = cscope_mode_probe;
    cscope_mode.mode_close = cscope_mode_close;

    /* first register mode */
    qe_register_mode(&cscope_mode);

    qe_register_cmd_table(cscope_mode_commands, "cscope");
    qe_register_cmd_table(cscope_global_commands, NULL);

    return 0;
}

qe_module_init(cscope_init);