/*
 * compiler/core/lex_stuff.h
 *
 * Copyright (C) 1991, 1992 Michael Sample
 *            and the University of British Columbia
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * $Header: /cvs/Darwin/src/live/Security/SecuritySNACCRuntime/compiler/core/lex-stuff.h,v 1.1 2001/06/20 21:27:57 dmitch Exp $
 * $Log: lex-stuff.h,v $
 * Revision 1.1  2001/06/20 21:27:57  dmitch
 * Adding missing snacc compiler files.
 *
 * Revision 1.1.1.1  1999/03/16 18:06:49  aram
 * Originals from SMIME Free Library.
 *
 * Revision 1.4  1995/07/25 19:41:31  rj
 * changed `_' to `-' in file names.
 *
 * Revision 1.3  1994/10/08  03:48:45  rj
 * since i was still irritated by cpp standing for c++ and not the C preprocessor, i renamed them to cxx (which is one known suffix for C++ source files). since the standard #define is __cplusplus, cplusplus would have been the more obvious choice, but it is a little too long.
 *
 * Revision 1.2  1994/09/01  01:16:07  rj
 * decide upon type of yytext thru cxx macro provided by autoconf.
 *
 * Revision 1.1  1994/08/28  09:49:13  rj
 * first check-in. for a list of changes to the snacc-1.1 distribution please refer to the ChangeLog.
 *
 */

extern unsigned long myLineNoG;

#ifdef YYTEXT_POINTER
extern char	*yytext;
#else
extern char	yytext[];
#endif

extern FILE	*yyin;

int LexBeginIntialContext();
int LexBeginMacroDefContext();
int LexBeginBraceBalContext();
int LexBeginCommentContext();


#define COUNT_NEWLINES( cumulativeTotal, nullTermStr)\
{\
     int cnlStrIndex;\
     for (cnlStrIndex = 0; nullTermStr[cnlStrIndex] != '\0'; cnlStrIndex++)\
        if (nullTermStr[cnlStrIndex] == '\n')\
           cumulativeTotal++;\
}
