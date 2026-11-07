#ifndef FEATHEROS_LIBC_REGEX_H
#define FEATHEROS_LIBC_REGEX_H

#include <stddef.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NEWLINE 4
#define REG_NOSUB 8

#define REG_NOTBOL 1
#define REG_NOTEOL 2

#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ECOLLATE 3
#define REG_ECTYPE 4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK 7
#define REG_EPAREN 8
#define REG_EBRACE 9
#define REG_BADBR 10
#define REG_ERANGE 11
#define REG_ESPACE 12
#define REG_BADRPT 13

#define RE_DUP_MAX 255

typedef ssize_t regoff_t;

typedef struct
{
    size_t re_nsub;
    void* __program;
} regex_t;

typedef struct
{
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

__BEGIN_DECLS

int regcomp(regex_t* regex, const char* pattern, int flags);
int regexec(const regex_t* regex, const char* text, size_t count, regmatch_t* matches, int flags);
size_t regerror(int code, const regex_t* regex, char* buffer, size_t size);
void regfree(regex_t* regex);

__END_DECLS

#endif
