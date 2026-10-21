#ifndef FEATHEROS_LIBC_STRING_H
#define FEATHEROS_LIBC_STRING_H

#include <stddef.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

void* memcpy(void* destination, const void* source, size_t count);
void* memmove(void* destination, const void* source, size_t count);
void* memset(void* destination, int value, size_t count);
int memcmp(const void* left, const void* right, size_t count);
void* memchr(const void* bytes, int value, size_t count);

size_t strlen(const char* text);
size_t strnlen(const char* text, size_t maximum);
char* strcpy(char* destination, const char* source);
char* strncpy(char* destination, const char* source, size_t count);
char* strcat(char* destination, const char* source);
char* strncat(char* destination, const char* source, size_t count);
int strcmp(const char* left, const char* right);
int strncmp(const char* left, const char* right, size_t count);
char* strchr(const char* text, int character);
char* strrchr(const char* text, int character);
char* strstr(const char* text, const char* pattern);
size_t strspn(const char* text, const char* accept);
size_t strcspn(const char* text, const char* reject);
char* strpbrk(const char* text, const char* accept);
char* strtok(char* text, const char* separators);
char* strtok_r(char* text, const char* separators, char** state);

char* strerror(int error);

__END_DECLS

#endif
