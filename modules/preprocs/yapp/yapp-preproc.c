/* $IdPath$
 * YAPP preprocessor (mimics NASM's preprocessor)
 *
 *  Copyright (C) 2001  Michael Urman
 *
 *  This file is part of YASM.
 *
 *  YASM is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  YASM is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "util.h"
/*@unused@*/ RCSID("$IdPath$");

#include "errwarn.h"

#include "objfmt.h"
#include "preproc.h"

#include "hamt.h"

#include "src/preprocs/yapp/yapp-preproc.h"
#include "src/preprocs/yapp/yapp-token.h"

static int is_interactive;
static int saved_length;

static HAMT *macro_table;

YAPP_Output current_output;
YYSTYPE yapp_preproc_lval;

int isatty(int);
void yapp_lex_initialize(FILE *f);


/*****************************************************************************/
/* macro support - to be moved to a separate file later (?)                  */
/*****************************************************************************/

typedef struct YAPP_Macro_s {
    SLIST_HEAD(macro_head, source_s) macro_head, param_head;
    enum {
	YAPP_MACRO = 0,
	YAPP_DEFINE
    } type;
    int args;
    int fillargs;
} YAPP_Macro;


YAPP_Macro *
yapp_macro_insert (char *name, int argc, int fillargs)
{
    YAPP_Macro *ym = xmalloc(sizeof(YAPP_Macro));
    ym->type = YAPP_MACRO;
    ym->args = argc;
    ym->fillargs = fillargs;

    memcpy(&ym->macro_head, &macro_head, sizeof(macro_head));

    SLIST_INIT(&macro_head);
    macro_tail = SLIST_FIRST(&macro_head);
    return ym;
}

void
yapp_macro_error_exists (YAPP_Macro *v)
{
    if (v) Error(_("Redefining macro of the same name %d:%d"), v->type, v->args);
}

YAPP_Macro *
yapp_define_insert (char *name, int argc, int fillargs)
{
    int zero = 0;

    YAPP_Macro *ym = xmalloc(sizeof(YAPP_Macro));
    ym->type = YAPP_DEFINE;
    ym->args = argc;
    ym->fillargs = fillargs;

    /*printf ("]]Inserting %s:%d:%d\n", name, argc, fillargs);*/

    memcpy(&ym->macro_head, &macro_head, sizeof(macro_head));
    memcpy(&ym->param_head, &param_head, sizeof(param_head));

    HAMT_insert(macro_table, name, (void *)ym, &zero, (void (*)(void *))yapp_macro_error_exists);

    SLIST_INIT(&macro_head);
    SLIST_INIT(&param_head);

    macro_tail = SLIST_FIRST(&macro_head);
    param_tail = SLIST_FIRST(&param_head);

    return ym;
}

void
yapp_macro_delete (YAPP_Macro *ym)
{
    while (!SLIST_EMPTY(&ym->macro_head)) {
	source *s = SLIST_FIRST(&ym->macro_head);
	free(s);
	SLIST_REMOVE_HEAD(&ym->macro_head, next);
    }
    free(ym);
}

static YAPP_Macro *
yapp_macro_get (const char *key)
{
    return (YAPP_Macro *)HAMT_search(macro_table, key);
}

/*****************************************************************************/



static void
yapp_preproc_initialize(FILE *f)
{
    is_interactive = f ? (isatty(fileno(f)) > 0) : 0;
    if (is_interactive) current_file = "<STDIN>";
    else if (!current_file) current_file = "<?>"; /* FIXME */
    yapp_lex_initialize(f);
    SLIST_INIT(&output_head);
    SLIST_INIT(&source_head);
    SLIST_INIT(&macro_head);
    SLIST_INIT(&param_head);
    out = xmalloc(sizeof(output));
    out->out = current_output = YAPP_OUTPUT;
    SLIST_INSERT_HEAD(&output_head, out, next);

    macro_table = HAMT_new();

    source_tail = SLIST_FIRST(&source_head);
    macro_tail = SLIST_FIRST(&macro_head);
    param_tail = SLIST_FIRST(&param_head);

    current_head = &source_head;
    current_tail = &source_tail;
}

/* Generate a new level of if* context
 * if val is true, this module of the current level will be output IFF the
 * surrounding one is.
 */
static void
push_if(int val)
{
    out = xmalloc(sizeof(output));
    out->out = current_output;
    SLIST_INSERT_HEAD(&output_head, out, next);

    switch (current_output)
    {
	case YAPP_OUTPUT:
	    current_output = val ? YAPP_OUTPUT : YAPP_NO_OUTPUT;
	    break;

	case YAPP_NO_OUTPUT:
	case YAPP_OLD_OUTPUT:
	case YAPP_BLOCKED_OUTPUT:
	    current_output = YAPP_BLOCKED_OUTPUT;
	    break;
    }

}

/* Generate a new module in the current if* context
 * if val is true and this level hasn't had a true, it will be output if the
 * surrounding level is.
 */
static void
push_else(int val)
{
    switch (current_output)
    {
	/* if it was NO, turn to output IFF val is true */
	case YAPP_NO_OUTPUT:
	    current_output = val ? YAPP_OUTPUT : YAPP_NO_OUTPUT;
	    break;

	/* if it was yes, make it OLD */
	case YAPP_OUTPUT:
	    current_output = YAPP_OLD_OUTPUT;
	    break;

	/* leave OLD as OLD, BLOCKED as BLOCKED */
	case YAPP_OLD_OUTPUT:
	case YAPP_BLOCKED_OUTPUT:
	    break;
    }
}

/* Clear the curent if* context level */
static void
pop_if()
{
    out = SLIST_FIRST(&output_head);
    current_output = out->out;
    SLIST_REMOVE_HEAD(&output_head, next);
    free(out);
}

/* undefine a symbol */
static void
yapp_undef(const char *key)
{
    int zero = 0;
    HAMT_insert(macro_table, key, NULL, &zero, (void (*)(void *))yapp_macro_delete);
}

/* Is a symbol known to YAPP? */
static int
yapp_defined(const char *key)
{
    return yapp_macro_get(key) != NULL;
}

void
append_token(int token)
{
    if (current_output != YAPP_OUTPUT) return;

    src = xmalloc(sizeof(source));
    src->token.type = token;
    switch (token)
    {
	case INTNUM:
	    src->token.str = xstrdup(yapp_preproc_lval.int_str_val.str);
	    src->token.val.int_val = yapp_preproc_lval.int_str_val.val;
	    break;

	case FLTNUM:
	    src->token.str = xstrdup(yapp_preproc_lval.double_str_val.str);
	    src->token.val.double_val = yapp_preproc_lval.double_str_val.val;
	    break;

	case STRING:
	    src->token.str = xstrdup(yapp_preproc_lval.str_val);
	    break;

	case IDENT:
	    src->token.str = xstrdup(yapp_preproc_lval.str_val);
	    break;

	case '+': case '-': case '*': case '/': case '%': case ',': case '\n':
	    src->token.str = xmalloc(2);
	    src->token.str[0] = (char)token;
	    src->token.str[1] = '\0';
	    break;

	case LINE:
	    src->token.str = xmalloc(23+strlen(current_file));
	    sprintf(src->token.str, "%%line %d+1 %s\n", line_number, current_file);
	    break;

	default:
	    free(src);
	    return;
    }
    if (*current_tail) {
	SLIST_INSERT_AFTER(*current_tail, src, next);
    }
    else {
	SLIST_INSERT_HEAD(current_head, src, next);
    }
    *current_tail = src;
    if (current_head == &source_head)
	saved_length += strlen(src->token.str) + 1;
}

int
append_through_return()
{
    int token;
    do {
	token = yapp_preproc_lex();
	if (token == 0)
	    return 0;
	append_token(token);
    } while (token != '\n');
    return '\n';
}

int
eat_through_return()
{
    int token;
    while ((token = yapp_preproc_lex()) != '\n') {
	if (token == 0)
	    return 0;
	Error(_("Skipping possibly valid %%define stuff"));
    }
    append_token('\n');
    return '\n';
}

int
yapp_get_ident(char *synlvl)
{
    int token = yapp_preproc_lex();
    if (token != IDENT) {
	Error(_("Identifier expected after %%%s"), synlvl);
    }
    return token;
}

void
copy_token(YAPP_Token *tok)
{
    src = xmalloc(sizeof(source));
    src->token.type = tok->type;
    src->token.str = xstrdup(tok->str);

    if (*current_tail) {
	SLIST_INSERT_AFTER(*current_tail, src, next);
    }
    else {
	SLIST_INSERT_HEAD(current_head, src, next);
    }
    *current_tail = src;
    if (current_head == &source_head)
	saved_length += strlen(src->token.str) + 1;
}

void
expand_macro(YAPP_Macro *ym)
{
    if (ym->type == YAPP_DEFINE) {
	if (ym->args == -1) {
	    /* no parens to deal with */
	    src = SLIST_FIRST(&ym->macro_head);
	    while (src != NULL) {
		if (src->token.type == IDENT) {
		    if (yapp_defined(src->token.str)) {
			expand_macro(yapp_macro_get(src->token.str));
		    }
		    else {
			copy_token(&src->token);
		    }
		}
		else {
		    copy_token(&src->token);
		}
		src = SLIST_NEXT(src, next);
	    }
	}
	else
	    InternalError(_("Invoking Defines with argument lists not yet supported"));
    }
    else
	InternalError(_("Invoking Macros not yet supported"));
}

static size_t
yapp_preproc_input(char *buf, size_t max_size)
{
    static YAPP_State state = YAPP_STATE_INITIAL;
    int n = 0;
    int token;
    int need_line_directive = 0;

    while (saved_length < max_size && state != YAPP_STATE_EOF)
    {
	token = yapp_preproc_lex();

	switch (state) {
	    case YAPP_STATE_INITIAL:
		switch (token)
		{
		    char *s;
		    default:
			append_token(token);
			if (append_through_return()==0) state=YAPP_STATE_EOF;
			/*Error(_("YAPP got an unhandled token."));*/
			break;

		    case IDENT:
			if (yapp_defined(yapp_preproc_lval.str_val)) {
			    expand_macro(yapp_macro_get(yapp_preproc_lval.str_val));
			}
			else {
			    append_token(token);
			}
			break;

		    case 0:
			state = YAPP_STATE_EOF;
			break;

		    case '\n':
			append_token(token);
			break;

		    case CLEAR:
			HAMT_delete(macro_table, (void (*)(void *))yapp_macro_delete);
			macro_table = HAMT_new();
			break;

		    case DEFINE:
			token = yapp_get_ident("define");
			s = xstrdup(yapp_preproc_lval.str_val);

			/* three cases: newline or stuff or left paren */
			token = yapp_preproc_lex();
			if (token == '\n') {
			    /* no args or content - just insert it */
			    yapp_define_insert(s, -1, 0);
			    append_token('\n');
			}
			else if (token == '(') {
			    /* get all params of the parameter list */
			    /* ensure they alternate IDENT and ',' */
			    int param_count = 0;
			    int last_token = ',';
			    current_head = &param_head;
			    current_tail = &param_tail;

			    while ((token = yapp_preproc_lex())!=')') {
				if (last_token == ',' && token == IDENT) {
				    append_token(token);
				    param_count++;
				}
				else if (token == 0) {
				    state = YAPP_STATE_EOF;
				    break;
				}
				else if (last_token == ',' || token != ',')
				    Error(_("Unexpected token in %%define parameters"));
				last_token = token;
			    }
			    if (token == ')') {
				/* after paramlist and ')' */
				/* everything is what it's defined to be */
				current_head = &macro_head;
				current_tail = &macro_tail;
				if(append_through_return()==0) state=YAPP_STATE_EOF;
				else {
				    yapp_define_insert(s, param_count, 0);
				}
			    }
			    current_head = &source_head;
			    current_tail = &source_tail;
			    append_token('\n');
			}
			else {
			    /* no parens */
			    current_head = &macro_head;
			    current_tail = &macro_tail;
			    append_token(token);
			    if(append_through_return()==0) state=YAPP_STATE_EOF;
			    else {
				yapp_define_insert(s, -1, 0);
			    }
			    current_head = &source_head;
			    current_tail = &source_tail;
			    append_token('\n');
			}
			break;

		    case UNDEF:
			token = yapp_get_ident("undef");
			yapp_undef(yapp_preproc_lval.str_val);
			state = YAPP_STATE_NEED_EOL;
			break;

		    case IFDEF:
			token = yapp_get_ident("ifdef");
			push_if(yapp_defined(yapp_preproc_lval.str_val));
			state = YAPP_STATE_NEED_EOL;
			break;

		    case IFNDEF:
			token = yapp_get_ident("ifndef");
			push_if(!yapp_defined(yapp_preproc_lval.str_val));
			state = YAPP_STATE_NEED_EOL;
			break;

		    case ELSE:
			push_else(1);
			if (current_output == YAPP_OUTPUT) need_line_directive = 1;
			state = YAPP_STATE_NEED_EOL;
			break;

		    case ELIFDEF:
			token = yapp_get_ident("elifdef");
			push_else(yapp_defined(yapp_preproc_lval.str_val));
			if (current_output == YAPP_OUTPUT) need_line_directive = 1;
			state = YAPP_STATE_NEED_EOL;
			break;

		    case ELIFNDEF:
			token = yapp_get_ident("elifndef");
			push_else(!yapp_defined(yapp_preproc_lval.str_val));
			if (current_output == YAPP_OUTPUT) need_line_directive = 1;
			state = YAPP_STATE_NEED_EOL;
			break;

		    case ENDIF:
			/* there's got to be another way to do this:   */
			/* only set if going from non-output to output */
			if (current_output != YAPP_OUTPUT) need_line_directive = 1;
			pop_if();
			if (current_output != YAPP_OUTPUT) need_line_directive = 0;
			state = YAPP_STATE_NEED_EOL;
			break;

		    case INCLUDE:
		    case LINE:
			need_line_directive = 1;
			break;
		}
		if (state == YAPP_STATE_NEED_EOL)
		{
		    if (eat_through_return()==0) state=YAPP_STATE_EOF;
		    else state=YAPP_STATE_INITIAL;
		}
		break;
	    default:
		Error(_("YAPP got into a bad state"));
	}
	if (need_line_directive) {
	    append_token(LINE);
	    need_line_directive = 0;
	}
    }

    /* convert saved stuff into output.  we either have enough, or are EOF */
    while (n < max_size && saved_length)
    {
	source *next;
	src = SLIST_FIRST(&source_head);
	next = SLIST_NEXT(src, next);
	if (max_size - n - 1 >= strlen(src->token.str)) {
	    strcpy(buf+n, src->token.str);
	    n += strlen(src->token.str);
	    switch (src->token.type) {
		default:
		    buf[n++] = ' ';
		    break;

		case IDENT:
		    /* don't space before commas, etc */
		    if (next && next->token.type == IDENT)
			buf[n++] = ' ';
		    break;

		    /* don't space after operators, parens */
		case '+': case '-': case '/': case '*': case '(': case ')':
		case '\n':
		case LINE:
		case INCLUDE:
		    break;
	    }

	    saved_length -= strlen(src->token.str) + 1;
	    SLIST_REMOVE_HEAD(&source_head, next);
	    free(src->token.str);
	    free(src);
	}
    }

    return n;
}

/* Define preproc structure -- see preproc.h for details */
preproc yapp_preproc = {
    "YAPP preprocessing (NASM style)",
    "yapp",
    yapp_preproc_initialize,
    yapp_preproc_input
};
