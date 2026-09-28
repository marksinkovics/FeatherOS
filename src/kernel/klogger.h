#ifndef KLOGGER_H
#define KLOGGER_H

namespace FeatherOS
{

class Term;

class KLogger
{
public:
    void Init(Term* term);
    void Debug(const char* message);
    void Info(const char* message);
    void Warning(const char* message);
    void Error(const char* message);

private:
    void Write(const char* level, const char* message);

    Term* m_term = nullptr;
};

}

#endif