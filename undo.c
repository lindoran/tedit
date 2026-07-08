/* SPDX-License-Identifier: MIT
 * undo.c  --  undo/redo stack implementation
 */

#include "undo.h"
#include "buffer.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* -----------------------------------------------------------------------
 * Stack
 * ----------------------------------------------------------------------- */

#define STACK_INIT_CAP  64
#define STACK_MAX      2048   /* oldest op evicted when exceeded */

typedef struct {
    UndoOp *ops;
    int      count;
    int      cap;
} Stack;

typedef struct {
    Stack undo_stack;
    Stack redo_stack;
    /* timestamp (ms) of last recorded op for coalescing */
    long long last_time_ms;
} UndoState;

/* Free all op data in the stack, reset count to 0 (keeps allocation). */
static void stack_clear(Stack *s)
{
    int i;
    for (i = 0; i < s->count; i++)
        free(s->ops[i].data);
    s->count = 0;
}

/* Free all op data AND the array itself. */
static void stack_free(Stack *s)
{
    stack_clear(s);
    free(s->ops);
    s->ops = NULL;
    s->cap = 0;
}

/* Push op onto stack (takes ownership of op->data).
 * Evicts oldest if at STACK_MAX.  Returns 1 on success, 0 on alloc fail
 * (in which case op->data is freed to avoid leak). */
static int stack_push(Stack *s, UndoOp *op)
{
    /* Evict oldest entry when hard limit reached */
    if (s->count >= STACK_MAX) {
        free(s->ops[0].data);
        memmove(s->ops, s->ops + 1,
                (size_t)(s->count - 1) * sizeof(UndoOp));
        s->count--;
    }

    /* Grow backing array if needed */
    if (s->count >= s->cap) {
        int new_cap = s->cap ? s->cap * 2 : STACK_INIT_CAP;
        UndoOp *p = (UndoOp *)realloc(s->ops,
                                       (size_t)new_cap * sizeof(UndoOp));
        if (!p) {
            /* Failed: drop the op cleanly */
            free(op->data);
            op->data = NULL;
            return 0;
        }
        s->ops = p;
        s->cap = new_cap;
    }

    s->ops[s->count++] = *op;
    return 1;
}

/* Pop the top op (caller owns the returned data). */
static UndoOp stack_pop(Stack *s)
{
    return s->ops[--s->count];
}

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

int undo_init(Editor *ed)
{
    UndoState *st = (UndoState *)calloc(1, sizeof(UndoState));
    if (!st) return -1;
    ed->undo_state = st;
    return 0;
}

void undo_free(Editor *ed)
{
    UndoState *st = (UndoState *)ed->undo_state;
    if (!st) return;
    stack_free(&st->undo_stack);
    stack_free(&st->redo_stack);
    free(st);
    ed->undo_state = NULL;
}

void undo_clear(Editor *ed)
{
    UndoState *st = (UndoState *)ed->undo_state;
    if (!st) return;
    stack_clear(&st->undo_stack);
    stack_clear(&st->redo_stack);
    st->last_time_ms = 0;
}

void undo_reset_coalesce(Editor *ed)
{
    UndoState *st = (UndoState *)ed->undo_state;
    if (!st) return;
    st->last_time_ms = 0;
}

void undo_record(Editor *ed, UndoOp *op)
{
    UndoState *st = (UndoState *)ed->undo_state;
    if (!st) {
        free(op->data);
        return;
    }
    /* New edit: kill redo history */
    stack_clear(&st->redo_stack);
    /* Try to coalesce with previous op when appropriate. */
    {
        long long now_ms = 0;
        struct timeval tv;
        if (gettimeofday(&tv, NULL) == 0)
            now_ms = (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;

        /* Use editor-configured coalesce timeout (ms). 0 disables coalescing. */
        int cfg_ms = ed->coalesce_ms;

        if (cfg_ms > 0 && st->undo_stack.count > 0 && now_ms - st->last_time_ms <= cfg_ms) {
            UndoOp *top = &st->undo_stack.ops[st->undo_stack.count - 1];

            /* Merge consecutive forward typing: both are OP_DEL_CHARS,
             * same row, and new op starts immediately after top. */
            if (op->type == OP_DEL_CHARS && top->type == OP_DEL_CHARS &&
                op->row == top->row && op->col == top->col + top->len &&
                op->data == NULL && top->data == NULL) {
                top->len += op->len;
                /* keep top->col and top->cur_row/cur_col as original */
                free(op->data);
                st->last_time_ms = now_ms;
                return;
            }

            /* Merge consecutive backspaces/deletes: both are OP_INS_CHARS,
             * same row, and the new op's deleted region is immediately to the
             * left of the existing saved region: new.col + new.len == top.col
             * We prepend the new data to top->data. */
            if (op->type == OP_INS_CHARS && top->type == OP_INS_CHARS &&
                op->row == top->row && (op->col + op->len) == top->col &&
                op->data != NULL) {
                int new_len = top->len + op->len;
                char *buf = (char *)malloc((size_t)new_len);
                if (buf) {
                    /* new data represents earlier chars (left side), then old */
                    if (op->data) memcpy(buf, op->data, (size_t)op->len);
                    if (top->data) memcpy(buf + op->len, top->data, (size_t)top->len);
                    free(top->data);
                    top->data = buf;
                    top->col = op->col;
                    top->len = new_len;
                    /* keep top->cur_row/cur_col as the original (pre-first-delete) */
                    free(op->data);
                    st->last_time_ms = now_ms;
                    return;
                }
                /* fall through on alloc fail to push as separate op */
            }
        }

        /* Not coalesced: push as new op */
        stack_push(&st->undo_stack, op);
        st->last_time_ms = now_ms;
    }
}

/* -----------------------------------------------------------------------
 * Apply a single op, push its inverse onto *inv_stack.
 * Called by both undo_do (inv=redo) and redo_do (inv=undo).
 * ----------------------------------------------------------------------- */
static void apply_op(Editor *ed, UndoOp *op, Stack *inv_stack)
{
    UndoOp inv;
    memset(&inv, 0, sizeof(inv));

    switch (op->type) {

    /* ------------------------------------------------------------------ */
    case OP_DEL_CHARS: {
        /* Delete len chars at (row,col); save them for the inverse insert. */
        Line *l  = &ed->tb.lines[op->row];
        int   col = op->col, len = op->len, i;
        char *saved = NULL;

        if (len > 0 && col >= 0 && col + len <= l->len) {
            saved = (char *)malloc((size_t)len);
            if (saved) memcpy(saved, l->data + col, (size_t)len);
            for (i = 0; i < len; i++)
                line_delete_char(l, col);
        }

        ed->cur_row = op->cur_row;
        ed->cur_col = op->cur_col;
        ed_clamp_and_scroll(ed);

        /* Inverse: OP_INS_CHARS — cursor after re-insert = col+len */
        inv.type    = OP_INS_CHARS;
        inv.row     = op->row;  inv.col = op->col;
        inv.len     = len;      inv.data = saved;
        inv.cur_row = op->row;  inv.cur_col = op->col + len;
        stack_push(inv_stack, &inv);
        break;
    }

    /* ------------------------------------------------------------------ */
    case OP_INS_CHARS: {
        /* Insert data[len] at (row,col). */
        Line *l = &ed->tb.lines[op->row];
        int   i;
        for (i = 0; i < op->len; i++)
            line_insert_char(l, op->col + i, op->data[i]);

        ed->cur_row = op->cur_row;
        ed->cur_col = op->cur_col;
        ed_clamp_and_scroll(ed);

        /* Inverse: OP_DEL_CHARS — cursor after re-delete = col */
        inv.type    = OP_DEL_CHARS;
        inv.row     = op->row;  inv.col = op->col;
        inv.len     = op->len;  inv.data = NULL;
        inv.cur_row = op->row;  inv.cur_col = op->col;
        stack_push(inv_stack, &inv);
        break;
    }

    /* ------------------------------------------------------------------ */
    case OP_OVERWRITE: {
        /* Swap char at (row,col) with data[0]. */
        Line *l = &ed->tb.lines[op->row];
        char *saved = (char *)malloc(1);
        if (saved) *saved = l->data[op->col];
        l->data[op->col] = op->data[0];

        ed->cur_row = op->cur_row;
        ed->cur_col = op->cur_col;
        ed_clamp_and_scroll(ed);

        /* Inverse: another OVERWRITE with the displaced char.
         * cur_col alternates: if this op put cursor at col, redo should
         * advance to col+1, and vice-versa. */
        inv.type    = OP_OVERWRITE;
        inv.row     = op->row;  inv.col = op->col;
        inv.len     = 1;        inv.data = saved;
        inv.cur_row = op->row;
        inv.cur_col = (op->cur_col == op->col) ? op->col + 1 : op->col;
        stack_push(inv_stack, &inv);
        break;
    }

    /* ------------------------------------------------------------------ */
    case OP_JOIN_LINES: {
        /*
         * Append data[col2 ..] to end of line[row-1], delete line[row].
         * op->col2 = number of leading indent bytes to skip in data.
         *
         * Before: line[row-1] = truncated (len = split_col),
         *         line[row]   = data (full content, may have indent prefix).
         * After:  line[row-1] = original (truncated + carried).
         */
        int   row      = op->row;
        int   col2     = op->col2;    /* indent skip */
        Line *prev     = &ed->tb.lines[row - 1];
        Line *cur_line = &ed->tb.lines[row];

        /* Capture full content of line[row] for the redo (OP_SPLIT_LINE). */
        int   full_len = cur_line->len;
        char *full_data = NULL;
        if (full_len > 0) {
            full_data = (char *)malloc((size_t)full_len);
            if (full_data) memcpy(full_data, cur_line->data, (size_t)full_len);
        }

        /* split_col = current length of line[row-1] = the split point for redo */
        int split_col = prev->len;

        /* Perform join: append carried (skip indent prefix) */
        line_append(prev, op->data + col2, op->len - col2);
        tb_delete_line(&ed->tb, row);

        ed->cur_row = op->cur_row;
        ed->cur_col = op->cur_col;
        ed_clamp_and_scroll(ed);

        /* Inverse: OP_SPLIT_LINE — re-split at split_col, put full content on new line */
        inv.type    = OP_SPLIT_LINE;
        inv.row     = row;        inv.col  = split_col;
        inv.col2    = col2;       /* indent in data for cursor placement */
        inv.len     = full_len;   inv.data = full_data;
        inv.cur_row = row;        inv.cur_col = col2;   /* cursor at indent on new line */
        stack_push(inv_stack, &inv);
        break;
    }

    /* ------------------------------------------------------------------ */
    case OP_SPLIT_LINE: {
        /*
         * Truncate line[row-1] at col, insert line[row] with data[len].
         * op->col2 = indent bytes in data (for computing redo cursor/join data).
         *
         * Before: line[row-1] = A ++ B  (full original, len = col + len(B))
         * After:  line[row-1] = A (truncated to col)
         *         line[row]   = data (full content of new line, includes indent)
         */
        int   row = op->row;

        /* Truncate then insert; after tb_insert_line the lines array may move. */
        line_truncate(&ed->tb.lines[row - 1], op->col);
        tb_insert_line(&ed->tb, row, op->data, op->len);

        ed->cur_row = op->cur_row;
        ed->cur_col = op->cur_col;
        ed_clamp_and_scroll(ed);

        /* Inverse: OP_JOIN_LINES — append data[col2:] to line[row-1], del line[row] */
        {
            char *inv_data = NULL;
            if (op->len > 0) {
                inv_data = (char *)malloc((size_t)op->len);
                if (inv_data && op->data)
                    memcpy(inv_data, op->data, (size_t)op->len);
            }
            inv.type    = OP_JOIN_LINES;
            inv.row     = row;       inv.col  = 0;   /* unused */
            inv.col2    = op->col2;  /* indent skip = same as split */
            inv.len     = op->len;   inv.data = inv_data;
            inv.cur_row = row - 1;   inv.cur_col = op->col;  /* cursor at split point */
            stack_push(inv_stack, &inv);
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    case OP_DEL_RANGE: {
        /*
         * Delete range (row,col)-(row2,col2).
         * op->data holds the pasted content (for redo to re-insert).
         */
        char *redo_data = NULL;
        if (op->len > 0 && op->data) {
            redo_data = (char *)malloc((size_t)op->len);
            if (redo_data) memcpy(redo_data, op->data, (size_t)op->len);
        }

        ed_delete_range(ed, op->row, op->col, op->row2, op->col2);

        ed->cur_row = op->cur_row;
        ed->cur_col = op->cur_col;
        ed_clamp_and_scroll(ed);

        /* Inverse: OP_INS_BLOB — re-insert at (row,col); redo cursor = (row2,col2) */
        inv.type    = OP_INS_BLOB;
        inv.row     = op->row;   inv.col     = op->col;
        inv.row2    = op->row2;  inv.col2    = op->col2;
        inv.len     = op->len;   inv.data    = redo_data;
        inv.cur_row = op->row2;  inv.cur_col = op->col2;
        stack_push(inv_stack, &inv);
        break;
    }

    /* ------------------------------------------------------------------ */
    case OP_INS_BLOB: {
        /*
         * Paste data[len] at (row,col).
         * ed_paste_blob sets cursor to end of inserted text.
         */
        char *redo_data = NULL;
        if (op->len > 0 && op->data) {
            redo_data = (char *)malloc((size_t)op->len);
            if (redo_data) memcpy(redo_data, op->data, (size_t)op->len);
        }

        ed->cur_row = op->row;
        ed->cur_col = op->col;
        ed_clamp_and_scroll(ed);

        ed_paste_blob(ed, op->data, op->len);

        {
            int end_row = ed->cur_row;
            int end_col = ed->cur_col;

            /* Inverse: OP_DEL_RANGE — delete the re-inserted blob */
            inv.type    = OP_DEL_RANGE;
            inv.row     = op->row;   inv.col  = op->col;
            inv.row2    = end_row;   inv.col2 = end_col;
            inv.len     = op->len;   inv.data = redo_data;
            inv.cur_row = op->row;   inv.cur_col = op->col;
            stack_push(inv_stack, &inv);
        }
        break;
    }

    } /* switch */

    ed->modified = 1;
}

/* -----------------------------------------------------------------------
 * Public undo / redo
 * ----------------------------------------------------------------------- */

int undo_do(Editor *ed)
{
    UndoState *st = (UndoState *)ed->undo_state;
    UndoOp op;

    if (!st || st->undo_stack.count == 0) return 0;

    op = stack_pop(&st->undo_stack);
    apply_op(ed, &op, &st->redo_stack);
    free(op.data);
    return 1;
}

int redo_do(Editor *ed)
{
    UndoState *st = (UndoState *)ed->undo_state;
    UndoOp op;

    if (!st || st->redo_stack.count == 0) return 0;

    op = stack_pop(&st->redo_stack);
    apply_op(ed, &op, &st->undo_stack);
    free(op.data);
    return 1;
}
