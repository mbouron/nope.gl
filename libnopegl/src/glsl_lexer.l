%{
/* Generated from glsl_lexer.l with Flex. Do not edit glsl_lexer.c directly.
 * Regenerate with: meson compile -C builddir/libnopegl updateglsllexer
 *
 * Identifier and block-comment rules adapted from Mesa's glcpp-lex.l.
 *
 * Compiler/preprocessor actions are replaced with source-span tracking; no
 * macro expansion, keyword classification or GLSL validation is performed.
 */

/*
 * Copyright © 2010 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "glsl_lexer.h"
#include "utils/memory.h"

struct ngli_glsl_lexer {
    yyscan_t scanner;
    char *buffer;
    const char *cursor;
    bool line_start;
    bool directive;
};

#define YY_DECL static int lex_next(struct ngli_glsl_token *token, yyscan_t yyscanner)
#define YY_USER_ACTION yyextra->cursor += yyleng;
%}

%option reentrant noyywrap never-interactive
%option nounput noinput nounistd
%option noyyalloc noyyrealloc noyyfree
%option prefix="ngli_glsl_yy"
%option extra-type="struct ngli_glsl_lexer *"
%option warn nodefault

%x COMMENT LINE_COMMENT

SPLICE      \\\r?\n
IDENTIFIER  [_a-zA-Z]([_a-zA-Z0-9]|{SPLICE})*
DIGIT_WORD  [0-9]([_a-zA-Z0-9]|{SPLICE})*

%%

"//"                    { BEGIN LINE_COMMENT; }
<LINE_COMMENT>{SPLICE}   ;
<LINE_COMMENT>[^\\\n]+   ;
<LINE_COMMENT>\\         ;
<LINE_COMMENT>\n         { BEGIN INITIAL; yyextra->line_start = true; yyextra->directive = false; }

"/*"                    { BEGIN COMMENT; }
<COMMENT>[^*\n]+         ;
<COMMENT>\n              ;
<COMMENT>"*"+[^*/\n]*    ;
<COMMENT>"*"+"/"         { BEGIN INITIAL; }

{SPLICE}                ;
[ \t\r]+                ;
\n                      { yyextra->line_start = true; yyextra->directive = false; }

"#" {
    if (yyextra->line_start)
        yyextra->directive = true;
    yyextra->line_start = false;
}

{IDENTIFIER} {
    *token = (struct ngli_glsl_token){
        .start = yyextra->cursor - yyleng,
        .end = yyextra->cursor,
        .text = yytext,
        .directive = yyextra->directive,
    };
    /* Compact only the scanner's copy, keeping the original source untouched. */
    for (size_t i = 0; i < (size_t)yyleng;) {
        if (yytext[i] == '\\') {
            i += yytext[i + 1] == '\r' ? 3 : 2;
            token->spliced_lines++;
        } else {
            yytext[token->length++] = yytext[i++];
        }
    }
    yytext[token->length] = 0;
    yyextra->line_start = false;
    return 1;
}

{DIGIT_WORD}            { yyextra->line_start = false; }
.                       { yyextra->line_start = false; }
<*><<EOF>>              { return 0; }

%%

struct ngli_glsl_lexer *ngli_glsl_lexer_create(const char *source)
{
    struct ngli_glsl_lexer *s = ngli_calloc(1, sizeof(*s));
    s->cursor = source;
    s->line_start = true;

    /* Flex requires two trailing NULs. */
    const size_t size = strlen(source);
    const size_t buffer_size = size + 2;

    s->buffer = ngli_calloc(buffer_size, 1);
    memcpy(s->buffer, source, size);

    yylex_init_extra(s, &s->scanner);
    yy_scan_buffer(s->buffer, size + 2, s->scanner);

    return s;
}

bool ngli_glsl_lexer_next(struct ngli_glsl_lexer *s, struct ngli_glsl_token *token)
{
    return lex_next(token, s->scanner) != 0;
}

void ngli_glsl_lexer_freep(struct ngli_glsl_lexer **sp)
{
    struct ngli_glsl_lexer *s = *sp;
    if (!s)
        return;
    yylex_destroy(s->scanner);
    ngli_free(s->buffer);
    ngli_freep(sp);
}

void *yyalloc(yy_size_t size, yyscan_t scanner)
{
    return ngli_malloc(size);
}

void *yyrealloc(void *ptr, yy_size_t size, yyscan_t scanner)
{
    return ngli_realloc(ptr, size, 1);
}

void yyfree(void *ptr, yyscan_t scanner)
{
    ngli_free(ptr);
}
