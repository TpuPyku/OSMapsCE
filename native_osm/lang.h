// lang.h - the interface's languages (the user 2026-11-06): Russian in the sources, the others in Lang\<code>.txt beside
// the exe: "#name English", then lines "Russian<TAB>translation" (UTF-8). A new language is a new file; a string it does
// not have stays Russian.
#pragma once
#include <windows.h>

// The language files in the exe's folder, this one (ini: lang) on; "" or unknown: Russian.
void LangInit(const char* code);
// The translation of a Russian string, or the string itself. Cheap; with Russian on it does nothing.
const wchar_t* Tr(const wchar_t* ru);
// The next language (dir: +1, -1), Russian among them.
void LangStep(int dir);
const char* LangCode();           // the one on, "" Russian
const wchar_t* LangName();        // its name, in itself

// ---- for the tests: a file's bytes into the language on, as LangInit reads them; the lines it kept
int LangFromBytes(const char* utf8, int len);
void LangOff();
