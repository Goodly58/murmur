// When the program dies on an exception (an access violation, say) it leaves a note: what the exception was, which
// address it touched, and the calls that led there as module + offset, which addr2line turns into function names
// for a build that still has its symbols. The note goes to stderr, when there is one, and to crash.log beside
// murmur.log. Afterwards the default handling carries on, so the exit code is what it always was.
#pragma once
#include "common.h"

void installCrashHandler();
void crashLogTo(const wstring& path);  // another place for the note (the self-tests use a scratch folder)
