#ifndef ZOD_VERSION_H
#define ZOD_VERSION_H
/*
 * Release number of the Warp build -- the ONE place it is set.
 *
 * Shown in: `Version zod_060` ($VER), the game log, the in-game main menu,
 * the ZodLaunch window title and $VER. tools/make_release.sh reads it from
 * here and refuses a binary whose $VER does not carry it.
 *
 * Scheme: 0.xx beta, 1.xx official releases. Every build handed to players
 * raises the number.
 */
#define ZOD_VERSION      "0.12"
#define ZOD_VERSION_DATE "18.11.2026"
#define ZOD_VERSION_TEXT "Zod Engine Warp " ZOD_VERSION

#endif
