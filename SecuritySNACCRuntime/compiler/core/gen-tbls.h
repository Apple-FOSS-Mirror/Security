/*
 * compiler/core/gen_tbls.h
 *
 * generates type tables and writes them to a file.
 *
 * MS
 * 93/02/07
 *
 * Copyright (C) 1993 Michael Sample
 *            and the University of British Columbia
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * $Header: /cvs/Darwin/Security/SecuritySNACCRuntime/compiler/core/gen-tbls.h,v 1.1 2001/06/20 21:27:57 dmitch Exp $
 * $Log: gen-tbls.h,v $
 * Revision 1.1  2001/06/20 21:27:57  dmitch
 * Adding missing snacc compiler files.
 *
 * Revision 1.1.1.1  1999/03/16 18:06:48  aram
 * Originals from SMIME Free Library.
 *
 * Revision 1.4  1997/05/07 15:18:35  wan
 * Added (limited) size constraints, bitstring and enumeration names to tables
 *
 * Revision 1.3  1995/07/25 19:41:29  rj
 * changed `_' to `-' in file names.
 *
 * Revision 1.2  1994/10/08  03:48:44  rj
 * since i was still irritated by cpp standing for c++ and not the C preprocessor, i renamed them to cxx (which is one known suffix for C++ source files). since the standard #define is __cplusplus, cplusplus would have been the more obvious choice, but it is a little too long.
 *
 * Revision 1.1  1994/08/28  09:49:11  rj
 * first check-in. for a list of changes to the snacc-1.1 distribution please refer to the ChangeLog.
 *
 */


void GenTypeTbls PROTO ((ModuleList *mods, char *fileName, int tableFileVersion));
