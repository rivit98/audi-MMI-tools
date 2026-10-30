/*
 * libintl.h - declarations only, for libstdc++'s std::messages headers
 * (pulled in by <locale>, which C++20's <chrono> includes). QNX has no
 * gettext; cxxrt.c's gettext() returns the message untranslated, the rest
 * are not linkable.
 */
#ifndef _QNX_CXX_LIBINTL_H
#define _QNX_CXX_LIBINTL_H

#ifdef __cplusplus
extern "C" {
#endif

char *gettext(const char *msgid);
char *dgettext(const char *domain, const char *msgid);
char *textdomain(const char *domain);
char *bindtextdomain(const char *domain, const char *dir);

#ifdef __cplusplus
}
#endif

#endif
