#pragma once
// What the program is called, which version it is, who made it and where its source lives, written once
// for everything that says so: the windows and the About page, the NGX project identity, the files the
// program writes beside itself, the version record in res/app.rc and the CMake project version.
//
// WAIVER(R31): the resource compiler knows only the preprocessor, so these are macros, and every use in
// C++ is the same macro, spliced into a longer literal where the text is part of one.

#define DSCREEN_VERSION_MAJOR 1
#define DSCREEN_VERSION_MINOR 1
#define DSCREEN_VERSION_PATCH 0

#define DSCREEN_PRODUCT_NAME "Full-Screen Wrapper for DLSS5"
#define DSCREEN_FILE_STEM "FullScreenWrapperForDLSS5" // the executable's name, and how the files it writes beside itself begin
#define DSCREEN_COMPANY_NAME "ThioJoe"
#define DSCREEN_COPYRIGHT "Copyright (C) 2026 ThioJoe"
#define DSCREEN_REPOSITORY "github.com/ThioJoe/Full-Screen-DLSS5-Wrapper"
#define DSCREEN_REPOSITORY_URL "https://" DSCREEN_REPOSITORY

// "0.2.0" from the three numbers: the argument is expanded first, then spelled out as one literal.
#define DSCREEN_STRINGIZE_(x) #x
#define DSCREEN_STRINGIZE(x) DSCREEN_STRINGIZE_(x)
#define DSCREEN_VERSION_STRING DSCREEN_STRINGIZE(DSCREEN_VERSION_MAJOR.DSCREEN_VERSION_MINOR.DSCREEN_VERSION_PATCH)

// The same text as a wide literal, for the windows. The resource compiler has no use for it.
#ifndef RC_INVOKED
#define DSCREEN_WIDE_(s) L##s
#define DSCREEN_WIDE(s) DSCREEN_WIDE_(s)
#endif
