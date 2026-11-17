/*
 * Two functions libmidi needs that the full solver does not provide.
 *
 * A side module may only import what its main module exports, and
 * mm_solver is linked with MAIN_MODULE=2: it keeps the libc functions GPAC
 * itself uses and drops the rest. Loading libmidi on mm_solver_1.0.5 found
 * two of its imports missing - which the dynamic linker only reports when the
 * function is first called, as "resolved is not a function".
 *
 * Both are defined here instead, hidden, so the link resolves libmidi's own
 * calls to them and nothing is asked of the solver. That keeps libmidi working
 * on the published mm_solver_1.0.5, with no new solver version.
 */
#include <stdio.h>
#include <emscripten.h>

/*
 * libmidi carries its instrument patches with --embed-file. The constructor
 * that emscripten's file packager generates hands a table of
 * {name, length, data} triples to _emscripten_fs_load_embedded_files, which
 * is a JavaScript library function of the main module. mm_solver_minimal
 * includes it (DEFAULT_LIBRARY_FUNCS_TO_INCLUDE); mm_solver does not, which is
 * the whole reason libmidi used to be "minimal only".
 *
 * Same table walk as upstream (src/lib/libcore.js), written with what the
 * solver does keep: FS.mkdirTree and FS.writeFile, reached through EM_ASM,
 * which a side module registers with its main module when it is loaded. The
 * constructor runs after the runtime - and so the filesystem - is up:
 * libdylink defers side modules' constructors to __ATINIT__ until then.
 */
__attribute__((visibility("hidden")))
void _emscripten_fs_load_embedded_files(void *table)
{
	EM_ASM({
		var p = $0;
		do {
			var nameAddr = HEAPU32[p >> 2]; p += 4;
			var len = HEAPU32[p >> 2]; p += 4;
			var content = HEAPU32[p >> 2]; p += 4;
			var name = UTF8ToString(nameAddr);
			var slash = name.lastIndexOf('/');
			if (slash > 0) FS.mkdirTree(name.substring(0, slash));
			FS.writeFile(name, HEAPU8.subarray(content, content + len));
		} while (HEAPU32[p >> 2]);
	}, table);
}

/* TiMidity's libarc (url_file.c) rewinds the files it reads. The solver
 * exports fseek and clearerr, and rewind is exactly those two (C11 7.21.9.5). */
__attribute__((visibility("hidden")))
void rewind(FILE *stream)
{
	(void) fseek(stream, 0L, SEEK_SET);
	clearerr(stream);
}
