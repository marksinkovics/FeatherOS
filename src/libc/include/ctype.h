#ifndef FEATHEROS_LIBC_CTYPE_H
#define FEATHEROS_LIBC_CTYPE_H

#include <sys/cdefs.h>

// ASCII only: the "C" locale is the only one.
__BEGIN_DECLS

int isalnum(int character);
int isalpha(int character);
int isblank(int character);
int iscntrl(int character);
int isdigit(int character);
int isgraph(int character);
int islower(int character);
int isprint(int character);
int ispunct(int character);
int isspace(int character);
int isupper(int character);
int isxdigit(int character);
int tolower(int character);
int toupper(int character);

__END_DECLS

#endif
