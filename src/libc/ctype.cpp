#include <ctype.h>

int isdigit(int character)
{
    return character >= '0' && character <= '9';
}

int islower(int character)
{
    return character >= 'a' && character <= 'z';
}

int isupper(int character)
{
    return character >= 'A' && character <= 'Z';
}

int isalpha(int character)
{
    return islower(character) || isupper(character);
}

int isalnum(int character)
{
    return isalpha(character) || isdigit(character);
}

int isxdigit(int character)
{
    return isdigit(character) || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

int isblank(int character)
{
    return character == ' ' || character == '\t';
}

int isspace(int character)
{
    return character == ' ' || (character >= '\t' && character <= '\r');
}

int iscntrl(int character)
{
    return (character >= 0 && character < ' ') || character == 0x7F;
}

int isprint(int character)
{
    return character >= ' ' && character < 0x7F;
}

int isgraph(int character)
{
    return character > ' ' && character < 0x7F;
}

int ispunct(int character)
{
    return isgraph(character) && !isalnum(character);
}

int tolower(int character)
{
    return isupper(character) ? character - 'A' + 'a' : character;
}

int toupper(int character)
{
    return islower(character) ? character - 'a' + 'A' : character;
}
