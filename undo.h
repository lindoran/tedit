/* SPDX-License-Identifier: MIT
 * undo.h  --  undo/redo stack for the editor
 *
 * Op types describe the operation to APPLY (not what was originally done):
 *
 *   OP_DEL_CHARS   delete len chars at (row,col)                 [undo of insert]
 *   OP_INS_CHARS   insert data[len] at (row,col)                 [undo of delete]
 *   OP_OVERWRITE   swap char at (row,col) with data[0]           [undo of overwrite]
 *   OP_JOIN_LINES  append data[col2:] to end of line[row-1],
 *                  then delete line[row]                          [undo of enter]
 *   OP_SPLIT_LINE  truncate line[row-1] at col, insert line[row]
 *                  with data[len]; col2 = indent bytes in data    [undo of line-join]
 *   OP_DEL_RANGE   delete (row,col)-(row2,col2); data for redo   [undo of paste]
 *   OP_INS_BLOB    paste data[len] at (row,col)                  [undo of del-range]
 *
 * cur_row/cur_col = cursor position to restore AFTER the op is applied.
 */

#ifndef UNDO_H
#define UNDO_H

#include "editor.h"

typedef enum {
    OP_DEL_CHARS,
    OP_INS_CHARS,
    OP_OVERWRITE,
    OP_JOIN_LINES,
    OP_SPLIT_LINE,
    OP_DEL_RANGE,
    OP_INS_BLOB
} OpType;

typedef struct {
    OpType  type;
    int     row, col;
    int     row2, col2;   /* end pos for DEL_RANGE; indent skip for JOIN/SPLIT */
    int     len;
    char   *data;         /* heap-allocated, owned; may be NULL */
    int     cur_row, cur_col;
} UndoOp;

/* Allocate undo state and attach to ed->undo_state. */
int  undo_init(Editor *ed);

/* Free undo state (both stacks). */
void undo_free(Editor *ed);

/* Discard both stacks (e.g. after file open). */
void undo_clear(Editor *ed);

/* Push op onto undo stack (takes ownership of op->data) and clear redo. */
void undo_record(Editor *ed, UndoOp *op);

/* Reset coalescing state so following ops won't be merged with previous ones. */
void undo_reset_coalesce(Editor *ed);

/* Apply top undo op, push inverse onto redo stack.
 * Returns 1 if something was undone, 0 if stack empty. */
int  undo_do(Editor *ed);

/* Apply top redo op, push inverse onto undo stack.
 * Returns 1 if something was redone, 0 if stack empty. */
int  redo_do(Editor *ed);

#endif /* UNDO_H */
