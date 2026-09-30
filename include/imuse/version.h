#ifndef LIBIMUSE_PUBLIC_VERSION_H
#define LIBIMUSE_PUBLIC_VERSION_H

/*
 * libimuse — version + ABI macros.
 *
 * IMUSE_VERSION_{MAJOR,MINOR,PATCH} track the library release.
 * IMUSE_ABI_VERSION tracks the public ABI: bump on any breaking
 * change to a public header (struct layout, function signature,
 * enum value reordering). Consumers can check at compile time:
 *
 *   #if IMUSE_ABI_VERSION != 6
 *   #error "libimuse ABI mismatch"
 *   #endif
 *
 * imuse_version_string() returns a literal "MAJOR.MINOR.PATCH"
 * baked into the library at build time. Useful for diagnostics
 * when a host links against a library it didn't ship with.
 */

#define IMUSE_VERSION_MAJOR 0
#define IMUSE_VERSION_MINOR 6
#define IMUSE_VERSION_PATCH 0

/* Bump on any public-API break. The triple above can advance for
 * additive changes without bumping ABI. */
/* ABI 5 also rejects snapshots predating the ADLB per-part detune field.
 * ABI 6 adds ImuseConfig.waveOutputChannels and the wave-track depth field. */
#define IMUSE_ABI_VERSION 6

#define IMUSE__STR(x) #x
#define IMUSE__XSTR(x) IMUSE__STR(x)

#define IMUSE_VERSION_STRING                                                                                 \
	IMUSE__XSTR(IMUSE_VERSION_MAJOR) "." IMUSE__XSTR(IMUSE_VERSION_MINOR) "." IMUSE__XSTR(IMUSE_VERSION_PATCH)

/* Returns IMUSE_VERSION_STRING. Linkage version useful when the
 * consumer was compiled against a different header than it now
 * links to. */
const char* imuse_version_string(void);

#endif /* LIBIMUSE_PUBLIC_VERSION_H */
