/* Libcrunch contains all the non-inline code that we need for doing run-time 
 * type checks on C code. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <link.h>
#include <sys/time.h>
#include <sys/resource.h>
#ifdef USE_REAL_LIBUNWIND
#include <libunwind.h>
#endif
#include "libcrunch.h"
#include "libcrunch_private.h"
#include "libcrunch_cil_inlines.h"

#define NAME_FOR_UNIQTYPE(u) ((u) ? ((u)->name ?: "(unnamed type)") : "(unknown type)")

/* Heap storage sized using a "loose" data type, like void*,
 * is marked as loose, and becomes non-loose when a cast to a non-loose type.
 * Annoyingly, liballocs eagerly replaces alloc site info with uniqtype
 * info. So making queries on an object will erase its looseness.
 * FIXME: separate this out, so that non-libcrunch clients don't have to
 * explicitly preserve looseness. */
#define STORAGE_CONTRACT_IS_LOOSE(ins, site) \
(((site) != NULL) /* i.e. liballocs only just erased the alloc site */ || \
!(ins)->alloc_site_flag /* or it still hasn't */ || \
((ins)->alloc_site & 0x1ul) /* or it's marked as loose explicitly */)

int __libcrunch_debug_level;
_Bool __libcrunch_is_initialized;

FILE *crunch_stream_err;// __attribute__((visibility("hidden")));

// helper
static const void *typestr_to_uniqtype_from_lib(void *handle, const char *typestr);

// HACK
void __libcrunch_preload_init(void);

/* Some data types like void* and sockaddr appear to be used to size a malloc(), 
 * but are only used because they have the same size as the actual thing being
 * allocated (say a different type of pointer, or a family-specific sockaddr). 
 * We keep a list of these. The user can use the LIBCRUNCH_LAZY_HEAP_TYPES 
 * environment variable to add these. */
static unsigned lazy_heap_types_count;
static const char **lazy_heap_typenames;
static struct uniqtype **lazy_heap_types;

static _Bool verbose;

static const char **suppression_words;
struct suppression
{
	const char *test_type_pat;
	const char *testing_function_pat;
	const char *alloc_type_pat;
};
struct suppression *suppressions;

static int print_type_cb(struct uniqtype *t, void *ignored)
{
	fprintf(crunch_stream_err, "uniqtype addr %p, name %s, size %d bytes\n", 
		t, t->name, t->pos_maxoff);
	fflush(crunch_stream_err);
	return 0;
}

static int match_typename_cb(struct uniqtype *t, void *ignored)
{
	for (unsigned i = 0; i < lazy_heap_types_count; ++i)
	{
		if (!lazy_heap_types[i] && 
			0 == strcmp(t->name, lazy_heap_typenames[i]))
		{
			// install this type in the lazy_heap_type slot
			lazy_heap_types[i] = t;
			
			// keep going -- we might have more to match
			return 0;
		}
	}
	return 0; // keep going
}

void __libcrunch_scan_lazy_typenames(void *typelib_handle)
{
	__liballocs_iterate_types(typelib_handle, match_typename_cb, NULL);

	// for (unsigned i = 0; i < lazy_heap_types_count; ++i)
	// {
	// 	if (lazy_heap_typenames[i] && !lazy_heap_types[i])
	// 	{
	// 		// look up 
	// 		const void *u = typestr_to_uniqtype_from_lib(typelib_handle, lazy_heap_typenames[i]);
	// 		// if we found it, install it
	// 		if (u) lazy_heap_types[i] = (struct uniqtype *) u;
	//	}
	// }
}

static ElfW(Dyn) *get_dynamic_section(void *handle)
{
	return ((struct link_map *) handle)->l_ld;
}

static ElfW(Dyn) *get_dynamic_entry_from_section(void *dynsec, unsigned long tag)
{
	ElfW(Dyn) *dynamic_section = dynsec;
	while (dynamic_section->d_tag != DT_NULL
		&& dynamic_section->d_tag != tag) ++dynamic_section;
	if (dynamic_section->d_tag == DT_NULL) return NULL;
	return dynamic_section;
}

static ElfW(Dyn) *get_dynamic_entry_from_handle(void *handle, unsigned long tag)
{
	return get_dynamic_entry_from_section(((struct link_map *) handle)->l_ld, tag);
}

static _Bool is_lazy_uniqtype(const void *u)
{
	for (unsigned i = 0; i < lazy_heap_types_count; ++i)
	{
		if (lazy_heap_types[i] == u) return 1;
	}
	return 0;
}

static _Bool prefix_pattern_matches(const char *pat, const char *str)
{
	if (!str) return 0;
	
	char *star_pos = strchr(pat, '*');
	
	return 0 == strncmp(pat, str, star_pos ? star_pos - pat : strlen(pat));
}

static _Bool test_site_matches(const char *pat /* will be saved! must not be freed */, 
		const void *test_site)
{
	_Bool result;
	Dl_info site_info = dladdr_with_cache(test_site);
	if (site_info.dli_sname)
	{
		/* okay, we can test the pat */
		result = prefix_pattern_matches(pat, site_info.dli_sname);
	}
	else
	{
		debug_printf(2, "dladdr() failed to find symbol for test site address %p\n", test_site);
		result = prefix_pattern_matches(pat, "");
	}
	return result;
}

static _Bool suppression_matches(struct suppression *s, 
		const char *test_typestr, const void *test_site, const char *alloc_typestr)
{
	/* dladdr is expensive, so 
	 * 
	 * - use it last;
	 * - cache its results (in test_site_matches. 
	 */
		
	return prefix_pattern_matches(s->test_type_pat, test_typestr)
			&& prefix_pattern_matches(s->alloc_type_pat, alloc_typestr)
			&& test_site_matches(s->testing_function_pat, test_site);
}

static _Bool is_suppressed(const char *test_typestr, const void *test_site, 
		const char *alloc_typestr)
{
	if (!suppressions) return 0;
	for (struct suppression *p = &suppressions[0];
				p->test_type_pat != NULL;
				++p)
	{
		if (suppression_matches(p, test_typestr, test_site, alloc_typestr))
		{
			++__libcrunch_failed_and_suppressed;
			return 1;
		}
	}
	return 0;
}

static _Bool done_init;
void __libcrunch_main_init(void) __attribute__((constructor(101)));
// NOTE: runs *before* the constructor in preload.c
void __libcrunch_main_init(void)
{
	assert(!done_init);
	
	done_init = 1;
}

const struct uniqtype *__libcrunch_uniqtype_void; // remember the location of the void uniqtype
const struct uniqtype *__libcrunch_uniqtype_signed_char;
const struct uniqtype *__libcrunch_uniqtype_unsigned_char;
#define LOOKUP_CALLER_TYPE(frag, caller) /* FIXME: use caller not RTLD_DEFAULT -- use interval tree? */ \
    ( \
		(__libcrunch_uniqtype_ ## frag) ? __libcrunch_uniqtype_ ## frag : \
		(__libcrunch_uniqtype_ ## frag = dlsym(RTLD_DEFAULT, "__uniqtype__" #frag), \
			assert(__libcrunch_uniqtype_ ## frag), \
			__libcrunch_uniqtype_ ## frag \
		) \
	)

/* counters */
unsigned long __libcrunch_begun;
#ifdef LIBCRUNCH_EXTENDED_COUNTS
unsigned long __libcrunch_aborted_init;
unsigned long __libcrunch_trivially_succeeded;
#endif
unsigned long __libcrunch_aborted_typestr;
unsigned long __libcrunch_lazy_heap_type_assignment;
unsigned long __libcrunch_failed;
unsigned long __libcrunch_failed_in_alloc;
unsigned long __libcrunch_failed_and_suppressed;
unsigned long __libcrunch_succeeded;

struct __libcrunch_is_a_cache_s /* __thread */ __libcrunch_is_a_cache[LIBCRUNCH_MAX_IS_A_CACHE_SIZE];
unsigned int /* __thread */ __libcrunch_is_a_cache_validity;
unsigned short __libcrunch_is_a_cache_next_victim;
const unsigned short __libcrunch_is_a_cache_size = LIBCRUNCH_MAX_IS_A_CACHE_SIZE;

static unsigned long repeat_suppression_count;
// enum check
// {
// 	IS_A,
// 	LIKE_A,
// 	NAMED_A,
// 	CHECK_ARGS,
// 	IS_A_FUNCTION_REFINING
// };
//static enum check last_repeat_suppressed_check_kind;

static const void *last_failed_site;
static const struct uniqtype *last_failed_deepest_subobject_type;

struct addrlist distinct_failure_sites;

static _Bool should_report_failure_at(void *site)
{
	if (verbose) return 1;
	_Bool is_unseen = !__liballocs_addrlist_contains(&distinct_failure_sites, site);
	if (is_unseen)
	{
		__liballocs_addrlist_add(&distinct_failure_sites, site);
		return 1;
	}
	else return 0;
}

static void print_exit_summary(void)
{
	if (__libcrunch_begun == 0) return;
	
	if (repeat_suppression_count > 0)
	{
		debug_printf(0, "Suppressed %ld further occurrences of the previous error\n", 
				repeat_suppression_count);
	}
	
	fprintf(crunch_stream_err, "====================================================\n");
	fprintf(crunch_stream_err, "libcrunch summary: \n");
	fprintf(crunch_stream_err, "----------------------------------------------------\n");
	fprintf(crunch_stream_err, "checks begun:                              % 9ld\n", __libcrunch_begun);
	fprintf(crunch_stream_err, "----------------------------------------------------\n");
#ifdef LIBCRUNCH_EXTENDED_COUNTS
	fprintf(crunch_stream_err, "checks aborted due to init failure:        % 9ld\n", __libcrunch_aborted_init);
#endif
	fprintf(crunch_stream_err, "checks aborted for bad typename:           % 9ld\n", __libcrunch_aborted_typestr);
#ifdef LIBCRUNCH_EXTENDED_COUNTS
	fprintf(crunch_stream_err, "checks trivially passed:                   % 9ld\n", __libcrunch_trivially_succeeded);
#endif
#ifdef LIBCRUNCH_EXTENDED_COUNTS
	fprintf(crunch_stream_err, "checks remaining                           % 9ld\n", __libcrunch_begun - (__libcrunch_trivially_succeeded + __liballocs_aborted_unknown_storage + __libcrunch_aborted_typestr + __libcrunch_aborted_init));
#else
	fprintf(crunch_stream_err, "checks remaining                           % 9ld\n", __libcrunch_begun - (__liballocs_aborted_unknown_storage + __libcrunch_aborted_typestr));
#endif	
	fprintf(crunch_stream_err, "----------------------------------------------------\n");
	fprintf(crunch_stream_err, "   of which did lazy heap type assignment: % 9ld\n", __libcrunch_lazy_heap_type_assignment);
	fprintf(crunch_stream_err, "----------------------------------------------------\n");
	fprintf(crunch_stream_err, "checks failed inside allocation functions: % 9ld\n", __libcrunch_failed_in_alloc);
	fprintf(crunch_stream_err, "checks failed otherwise:                   % 9ld\n", __libcrunch_failed);
	fprintf(crunch_stream_err, "   of which user suppression list matched: % 9ld\n", __libcrunch_failed_and_suppressed);
	fprintf(crunch_stream_err, "checks nontrivially passed:                % 9ld\n", __libcrunch_succeeded);
	fprintf(crunch_stream_err, "----------------------------------------------------\n");
	fprintf(crunch_stream_err, "   of which hit __is_a cache:              % 9ld\n", __libcrunch_is_a_hit_cache);
	fprintf(crunch_stream_err, "----------------------------------------------------\n");
	fprintf(crunch_stream_err, "====================================================\n");
	if (!verbose)
	{
		fprintf(crunch_stream_err, "re-run with LIBCRUNCH_VERBOSE=1 for repeat failures\n");
	}

	if (getenv("LIBCRUNCH_DUMP_SMAPS_AT_EXIT"))
	{
		char buffer[4096];
		size_t bytes;
		FILE *smaps = fopen("/proc/self/smaps", "r");
		if (smaps)
		{
			while (0 < (bytes = fread(buffer, 1, sizeof(buffer), smaps)))
			{
				fwrite(buffer, 1, bytes, stream_err);
			}
		}
		else fprintf(crunch_stream_err, "Couldn't read from smaps!\n");
	}
}

static unsigned count_separated_words(const char *str, char sep)
{
	unsigned count = 1;
	/* Count the lazy heap types */
	const char *pos = str;
	while ((pos = strchr(pos, sep)) != NULL) { ++count; ++pos; }
	return count;
}
static void fill_separated_words(const char **out, const char *str, char sep, unsigned max)
{
	unsigned n_added = 0;
	if (max == 0) return;

	const char *pos = str;
	const char *spacepos;
	do 
	{
		spacepos = strchrnul(pos, sep);
		if (spacepos - pos > 0) 
		{
			assert(n_added < max);
			out[n_added++] = strndup(pos, spacepos - pos);
		}

		pos = spacepos;
		while (*pos == sep) ++pos;
	} while (*pos != '\0' && n_added < max);
}

/* This is *not* a constructor. We don't want to be called too early,
 * because it might not be safe to open the -uniqtypes.so handle yet.
 * So, initialize on demand. */
int __libcrunch_global_init(void)
{
	if (__libcrunch_is_initialized) return 0; // we are okay

	// don't try more than once to initialize
	static _Bool tried_to_initialize;
	if (tried_to_initialize) return -1;
	tried_to_initialize = 1;

	// print a summary when the program exits
	atexit(print_exit_summary);
	
	// we must have initialized liballocs
	__liballocs_ensure_init();
	
	// delay start-up here if the user asked for it
	if (getenv("LIBCRUNCH_DELAY_STARTUP"))
	{
		sleep(10);
	}

	// figure out where our output goes
	const char *errvar = getenv("LIBCRUNCH_ERR");
	if (errvar)
	{
		// try opening it
		crunch_stream_err = fopen(errvar, "w");
		if (!stream_err)
		{
			crunch_stream_err = stderr;
			debug_printf(0, "could not open %s for writing\n", errvar);
		}
	} else crunch_stream_err = stderr;
	assert(crunch_stream_err);

	// no need to grab the executable's basename -- liballocs has done it for us
// 	char exename[4096];
// 	ssize_t readlink_ret = readlink("/proc/self/exe", exename, sizeof exename);
// 	if (readlink_ret != -1)
// 	{
// 		exe_basename = basename(exename); // GNU basename
// 	}
	
	const char *debug_level_str = getenv("LIBCRUNCH_DEBUG_LEVEL");
	if (debug_level_str) __libcrunch_debug_level = atoi(debug_level_str);
	
	verbose = __libcrunch_debug_level >= 1 || getenv("LIBCRUNCH_VERBOSE");

	/* We always include "signed char" in the lazy heap types. (FIXME: this is a 
	 * C-specificity we'd rather not have here, but live with it for now.
	 * Perhaps the best way is to have "uninterpreted_sbyte" and make signed_char
	 * an alias for it.) We count the other ones. */
	const char *lazy_heap_types_str = getenv("LIBCRUNCH_LAZY_HEAP_TYPES");
	lazy_heap_types_count = 1;
	unsigned upper_bound = 2; // signed char plus one string with zero spaces
	if (lazy_heap_types_str)
	{
		unsigned count = count_separated_words(lazy_heap_types_str, ' ');
		upper_bound += count;
		lazy_heap_types_count += count;
	}
	/* Allocate and populate. */
	lazy_heap_typenames = calloc(upper_bound, sizeof (const char *));
	lazy_heap_types = calloc(upper_bound, sizeof (struct uniqtype *));

	// the first entry is always signed char
	lazy_heap_typenames[0] = "signed char";
	if (lazy_heap_types_str)
	{
		fill_separated_words(&lazy_heap_typenames[1], lazy_heap_types_str, ' ',
				upper_bound - 1);
	}
	
	/* We have to scan for lazy heap types *in link order*, so that we see
	 * the first linked definition of any type that is multiply-defined.
	 * Do a scan now; we also scan when loading a types object, and when loading
	 * a user-dlopen()'d object. 
	 * 
	 * We don't use dl_iterate_phdr because it doesn't give us the link_map * itself. 
	 * Instead, walk the link map directly, like a debugger would
	 *                                           (like I always knew somebody should). */
	// grab the executable's end address
	dlerror();
	void *executable_handle = dlopen(NULL, RTLD_NOW | RTLD_NOLOAD);
	assert(executable_handle != NULL);
	void *exec_dynamic = ((struct link_map *) executable_handle)->l_ld;
	assert(exec_dynamic != NULL);
	ElfW(Dyn) *dt_debug = get_dynamic_entry_from_section(exec_dynamic, DT_DEBUG);
	struct r_debug *r_debug = (struct r_debug *) dt_debug->d_un.d_ptr;
	for (struct link_map *l = r_debug->r_map; l; l = l->l_next)
	{
		__libcrunch_scan_lazy_typenames(l);
	}
	
	/* Load the suppression list from LIBCRUNCH_SUPPRESS. It's a space-separated
	 * list of triples <test-type-pat, testing-function-pat, alloc-type-pat>
	 * where patterns can end in "*" to indicate prefixing. */
	unsigned suppressions_count = 0;
	const char *suppressions_str = getenv("LIBCRUNCH_SUPPRESSIONS");
	if (suppressions_str)
	{
		unsigned suppressions_count = count_separated_words(suppressions_str, ' ');
		suppression_words = calloc(1 + suppressions_count, sizeof (char *));
		assert(suppression_words);
		suppressions = calloc(1 + suppressions_count, sizeof (struct suppression));
		assert(suppressions);
		
		fill_separated_words(&suppression_words[0], suppressions_str, ' ', suppressions_count);
		
		for (const char **p_word = &suppression_words[0];
				*p_word;
				++p_word)
		{
			unsigned n_comma_sep = count_separated_words(*p_word, ',');
			if (n_comma_sep != 3)
			{
				debug_printf(1, "invalid suppression: %s\n", *p_word);
			}
			else
			{
				fill_separated_words(
					&suppressions[p_word - &suppression_words[0]].test_type_pat,
					*p_word,
					',',
					3);
			}
		}
	}

	__libcrunch_is_initialized = 1;

	debug_printf(1, "libcrunch successfully initialized\n");
	
	return 0;
}

static void clear_alloc_site_metadata(const void *alloc_start)
{
	/* In cases where heap classification failed, we null out the allocsite 
	 * to avoid repeated searching. We only do this for non-debug
	 * builds because it makes debugging a bit harder.
	 */

	struct insert *ins = lookup_object_info((void*) alloc_start, NULL, NULL, NULL);
	assert(INSERT_DESCRIBES_OBJECT(ins));
	/* Update the heap chunk's info to null the alloc site. 
	 * PROBLEM: we need to make really sure that we're not nulling
	 * out a redirected (deep) chunk's alloc site. 
	 * 
	 * NOTE that we don't want the insert to look like a deep-index
	 * terminator, so we set the flag.
	 */
	if (ins)
	{
#ifdef NDEBUG
		ins->alloc_site_flag = 1;
		ins->alloc_site = 0;
#endif
		assert(INSERT_DESCRIBES_OBJECT(ins));
		assert(!INSERT_IS_TERMINATOR(ins));
	}
}

static void cache_is_a(const void *obj, const struct uniqtype *t, _Bool result,
	unsigned short period, unsigned short n_pos, unsigned short n_neg)
{
	unsigned pos = __libcrunch_is_a_cache_next_victim;
	__libcrunch_is_a_cache[pos] = (struct __libcrunch_is_a_cache_s) {
		.obj = obj,
		.uniqtype = (unsigned long long) t,
		.result = result,
		.period = period,
		.n_pos = n_pos,
		.n_neg = n_neg
	};
	__libcrunch_is_a_cache_validity |= 1u<<pos;
	// make sure this entry is not the next victim
	__libcrunch_is_a_cache_next_victim = (pos + 1) % __libcrunch_is_a_cache_size;
}

void __libcrunch_uncache_all(const void *allocptr, size_t size)
{
	__libcrunch_uncache_is_a(allocptr, size);
}
void __libcrunch_uncache_is_a(const void *allocptr, size_t size)
{
	for (unsigned i = 0; i < __libcrunch_is_a_cache_size; ++i)
	{
		if (__libcrunch_is_a_cache_validity & (1u << i))
		{
			if ((char*) __libcrunch_is_a_cache[i].obj >= (char*) allocptr
					 && (char*) __libcrunch_is_a_cache[i].obj < (char*) allocptr + size)
			{
				// unset validity and make this the next victim
				__libcrunch_is_a_cache_validity &= ~(1u<<i);
				__libcrunch_is_a_cache_next_victim = i;
			}
		}
	}
}

/* Optimised version, for when you already know the uniqtype address. */
int __is_a_internal(const void *obj, const void *arg)
{
	/* We might not be initialized yet (recall that __libcrunch_global_init is 
	 * not a constructor, because it's not safe to call super-early). */
	__libcrunch_check_init();
	
	const struct uniqtype *test_uniqtype = (const struct uniqtype *) arg;
	memory_kind k = UNKNOWN;
	const void *alloc_start;
	unsigned long alloc_size_bytes;
	struct uniqtype *alloc_uniqtype = (struct uniqtype *)0;
	const void *alloc_site;
	
	struct liballocs_err *err = __liballocs_get_alloc_info(obj, 
		&k,
		&alloc_start,
		&alloc_size_bytes,
		&alloc_uniqtype,
		&alloc_site);
	if (__builtin_expect(err == &__liballocs_err_unrecognised_alloc_site, 0))
	{
		clear_alloc_site_metadata(alloc_start);
	}
	
	if (__builtin_expect(err != NULL, 0)) return 1; // liballocs has already counted this abort

	signed target_offset_within_uniqtype = (char*) obj - (char*) alloc_start;
	unsigned short period = (alloc_uniqtype->pos_maxoff > 0) ? alloc_uniqtype->pos_maxoff : 0;
	unsigned short n_pos;
	unsigned short n_neg;
	/* If we're searching in a heap array, we need to take the offset modulo the 
	 * element size. Otherwise just take the whole-block offset. */
	if (ALLOC_IS_DYNAMICALLY_SIZED(alloc_start, alloc_site)
			&& alloc_uniqtype
			&& alloc_uniqtype->pos_maxoff != 65535 /* HACK test for -1 */
			&& alloc_uniqtype->neg_maxoff == 0)
	{
		// HACK: for now, assume that the repetition continues to the end
		n_pos = ((alloc_size_bytes - target_offset_within_uniqtype) / alloc_uniqtype->pos_maxoff) - 1;
		n_neg = target_offset_within_uniqtype / alloc_uniqtype->pos_maxoff;
		target_offset_within_uniqtype %= alloc_uniqtype->pos_maxoff;
	}
	else
	{
		n_neg = 0;
		n_pos = 0;
	}
	_Bool is_cacheable = ALLOC_IS_DYNAMICALLY_SIZED(alloc_start, alloc_site);
	
	struct uniqtype *cur_obj_uniqtype = alloc_uniqtype;
	struct uniqtype *cur_containing_uniqtype = NULL;
	struct contained *cur_contained_pos = NULL;

	signed cumulative_offset_searched = 0;
	_Bool success = __liballocs_find_matching_subobject(target_offset_within_uniqtype, 
			cur_obj_uniqtype, (struct uniqtype *) test_uniqtype, &cur_obj_uniqtype, 
			&target_offset_within_uniqtype, &cumulative_offset_searched);
	
	if (__builtin_expect(success, 1))
	{
		/* populate cache */
		if (is_cacheable) cache_is_a(obj, test_uniqtype, 1,
			period, n_pos, n_neg);

		++__libcrunch_succeeded;
		return 1;
	}
	
	// if we got here, we might still need to apply lazy heap typing
	if (__builtin_expect(k == HEAP
			&& is_lazy_uniqtype(alloc_uniqtype)
			&& !__currently_allocating, 0))
	{
		struct insert *ins = lookup_object_info(obj, NULL, NULL, NULL);
		assert(ins);
		if (STORAGE_CONTRACT_IS_LOOSE(ins, alloc_site))
		{
			++__libcrunch_lazy_heap_type_assignment;
			
			// update the heap chunk's info to say that its type is (strictly) our test_uniqtype
			ins->alloc_site_flag = 1;
			ins->alloc_site = (uintptr_t) test_uniqtype;
			if (is_cacheable) cache_is_a(obj, test_uniqtype, 1, period, n_pos, n_neg);
		
			return 1;
		}
	}
	
	// if we got here, the check failed
	if (is_cacheable) cache_is_a(obj, test_uniqtype, 0, period, n_pos, n_neg);
	if (__currently_allocating || __currently_freeing)
	{
		++__libcrunch_failed_in_alloc;
		// suppress warning
	}
	else
	{
		++__libcrunch_failed;
		
		if (!is_suppressed(test_uniqtype->name, __builtin_return_address(0), alloc_uniqtype ? alloc_uniqtype->name : NULL))
		{
			if (should_report_failure_at(__builtin_return_address(0)))
			{
				if (last_failed_site == __builtin_return_address(0)
						&& last_failed_deepest_subobject_type == cur_obj_uniqtype)
				{
					++repeat_suppression_count;
				}
				else
				{
					if (repeat_suppression_count > 0)
					{
						debug_printf(0, "Suppressed %ld further occurrences of the previous error\n", 
								repeat_suppression_count);
					}

					debug_printf(0, "Failed check __is_a(%p, %p a.k.a. \"%s\") at %p (%s); "
							"obj is %ld bytes into an allocation of a %s%s%s "
							"(deepest subobject: %s at offset %d) "
							"originating at %p\n", 
						obj, test_uniqtype, test_uniqtype->name,
						__builtin_return_address(0), format_symbolic_address(__builtin_return_address(0)), 
						(long)((char*) obj - (char*) alloc_start),
						name_for_memory_kind(k), 
						(ALLOC_IS_DYNAMICALLY_SIZED(alloc_start, alloc_site) && alloc_uniqtype && alloc_size_bytes > alloc_uniqtype->pos_maxoff) ? " block of " : " ", 
						NAME_FOR_UNIQTYPE(alloc_uniqtype), 
						(cur_obj_uniqtype ? 
							((cur_obj_uniqtype == alloc_uniqtype) ? "(the same)" : cur_obj_uniqtype->name) 
							: "(none)"), 
						cumulative_offset_searched, 
						alloc_site);

					last_failed_site = __builtin_return_address(0);
					last_failed_deepest_subobject_type = cur_obj_uniqtype;

					repeat_suppression_count = 0;
				}
			}
		}
	}
	return 1; // HACK: so that the program will continue
}

/* Optimised version, for when you already know the uniqtype address. */
int __like_a_internal(const void *obj, const void *arg)
{
	// FIXME: use our recursive subobject search here? HMM -- semantics are non-obvious.
	
	/* We might not be initialized yet (recall that __libcrunch_global_init is 
	 * not a constructor, because it's not safe to call super-early). */
	__libcrunch_check_init();
	
	const struct uniqtype *test_uniqtype = (const struct uniqtype *) arg;
	memory_kind k;
	const void *alloc_start;
	unsigned long alloc_size_bytes;
	struct uniqtype *alloc_uniqtype = (struct uniqtype *)0;
	const void *alloc_site;
	void *caller_address = __builtin_return_address(0);
	
	struct liballocs_err *err = __liballocs_get_alloc_info(obj, 
		&k,
		&alloc_start,
		&alloc_size_bytes,
		&alloc_uniqtype, 
		&alloc_site);
	
	if (__builtin_expect(err == &__liballocs_err_unrecognised_alloc_site, 0))
	{
		clear_alloc_site_metadata(alloc_start);
	}
	
	if (__builtin_expect(err != NULL, 0)) return 1; // liballocs has already counted this abort
	
	signed target_offset_within_uniqtype = (char*) obj - (char*) alloc_start;
	/* If we're searching in a heap array, we need to take the offset modulo the 
	 * element size. Otherwise just take the whole-block offset. */
	if (ALLOC_IS_DYNAMICALLY_SIZED(alloc_start, alloc_site)
			&& alloc_uniqtype
			&& alloc_uniqtype->pos_maxoff != 0 
			&& alloc_uniqtype->neg_maxoff == 0)
	{
		target_offset_within_uniqtype %= alloc_uniqtype->pos_maxoff;
	}
	
	struct uniqtype *cur_obj_uniqtype = alloc_uniqtype;
	struct uniqtype *cur_containing_uniqtype = NULL;
	struct contained *cur_contained_pos = NULL;
	
	/* Descend the subobject hierarchy until our target offset is zero, i.e. we 
	 * find the outermost thing in the subobject tree that starts at the address
	 * we were passed (obj). */
	while (target_offset_within_uniqtype != 0)
	{
		_Bool success = __liballocs_first_subobject_spanning(
				&target_offset_within_uniqtype, &cur_obj_uniqtype, &cur_containing_uniqtype,
				&cur_contained_pos);
		if (!success) goto like_a_failed;
	}
	
	// trivially, identical types are like one another
	if (test_uniqtype == cur_obj_uniqtype) goto like_a_succeeded;
	
	// arrays are special
	_Bool matches;
	if (__builtin_expect((cur_obj_uniqtype->is_array || test_uniqtype->is_array), 0))
	{
		matches = 
			test_uniqtype == cur_obj_uniqtype
		||  (test_uniqtype->is_array && test_uniqtype->array_len == 1 
				&& test_uniqtype->contained[0].ptr == cur_obj_uniqtype)
		||  (cur_obj_uniqtype->is_array && cur_obj_uniqtype->array_len == 1
				&& cur_obj_uniqtype->contained[0].ptr == test_uniqtype);
		/* We don't need to allow an array of one blah to be like a different
		 * array of one blah, because they should be the same type. 
		 * FIXME: there's a difficult case: an array of statically unknown length, 
		 * which happens to have length 1. */
		if (matches) goto like_a_succeeded; else goto like_a_failed;
	}
	
	/* If we're not an array and nmemb is zero, we might have base types with
	 * signedness complements. */
	if (!cur_obj_uniqtype->is_array && !test_uniqtype->is_array
			&&  cur_obj_uniqtype->nmemb == 0 && test_uniqtype->nmemb == 0)
	{
		/* Does the cur obj type have a signedness complement matching the test type? */
		if (cur_obj_uniqtype->contained[0].ptr == test_uniqtype) goto like_a_succeeded;
		/* Does the test type have a signedness complement matching the cur obj type? */
		if (test_uniqtype->contained[0].ptr == cur_obj_uniqtype) goto like_a_succeeded;
	}
			
	
	/* Okay, we can start the like-a test: for each element in the test type, 
	 * do we have a type-equivalent in the object type?
	 * 
	 * We make an exception for arrays of char (signed or unsigned): if an
	 * element in the test type is such an array, we skip over any number of
	 * fields in the object type, until we reach the offset of the end element.  */
	unsigned i_obj_subobj = 0, i_test_subobj = 0;
	for (; 
		i_obj_subobj < cur_obj_uniqtype->nmemb && i_test_subobj < test_uniqtype->nmemb; 
		++i_test_subobj, ++i_obj_subobj)
	{
		if (__builtin_expect(test_uniqtype->contained[i_test_subobj].ptr->is_array
			&& (test_uniqtype->contained[i_test_subobj].ptr->contained[0].ptr
					== LOOKUP_CALLER_TYPE(signed_char, caller_address)
			|| test_uniqtype->contained[i_test_subobj].ptr->contained[0].ptr
					== LOOKUP_CALLER_TYPE(unsigned_char, caller_address)), 0))
		{
			// we will skip this field in the test type
			signed target_off =
				test_uniqtype->nmemb > i_test_subobj + 1
			 ?  test_uniqtype->contained[i_test_subobj + 1].offset
			 :  test_uniqtype->contained[i_test_subobj].offset
			      + test_uniqtype->contained[i_test_subobj].ptr->pos_maxoff;
			
			// ... if there's more in the test type, advance i_obj_subobj
			while (i_obj_subobj + 1 < cur_obj_uniqtype->nmemb &&
				cur_obj_uniqtype->contained[i_obj_subobj + 1].offset < target_off) ++i_obj_subobj;
			/* We fail if we ran out of stuff in the target object type
			 * AND there is more to go in the test type. */
			if (i_obj_subobj + 1 >= cur_obj_uniqtype->nmemb
			 && test_uniqtype->nmemb > i_test_subobj + 1) goto like_a_failed;
				
			continue;
		}
		matches = 
				test_uniqtype->contained[i_test_subobj].offset == cur_obj_uniqtype->contained[i_obj_subobj].offset
		 && 	test_uniqtype->contained[i_test_subobj].ptr == cur_obj_uniqtype->contained[i_obj_subobj].ptr;
		if (!matches) goto like_a_failed;
	}
	// if we terminated because we ran out of fields in the target type, fail
	if (i_test_subobj < test_uniqtype->nmemb) goto like_a_failed;
	
like_a_succeeded:
	++__libcrunch_succeeded;
	return 1;
	
	// if we got here, we've failed
	// if we got here, the check failed
like_a_failed:
	if (__currently_allocating || __currently_freeing) 
	{
		++__libcrunch_failed_in_alloc;
		// suppress warning
	}
	else
	{
		++__libcrunch_failed;
		if (!is_suppressed(test_uniqtype->name, __builtin_return_address(0), alloc_uniqtype ? alloc_uniqtype->name : NULL))
		{
			if (should_report_failure_at(__builtin_return_address(0)))
			{
				debug_printf(0, "Failed check __like_a(%p, %p a.k.a. \"%s\") at %p (%s), allocation was a %s%s%s originating at %p\n", 
					obj, test_uniqtype, test_uniqtype->name,
					__builtin_return_address(0), format_symbolic_address(__builtin_return_address(0)),
					name_for_memory_kind(k), 
					(ALLOC_IS_DYNAMICALLY_SIZED(alloc_start, alloc_site) && alloc_uniqtype && alloc_size_bytes > alloc_uniqtype->pos_maxoff) ? " block of " : " ", 
					NAME_FOR_UNIQTYPE(alloc_uniqtype), 
					alloc_site);
			}
		}
	}
	return 1; // HACK: so that the program will continue
}

int __named_a_internal(const void *obj, const void *arg)
{
	// FIXME: use our recursive subobject search here? HMM -- semantics are non-obvious.
	
	/* We might not be initialized yet (recall that __libcrunch_global_init is 
	 * not a constructor, because it's not safe to call super-early). */
	__libcrunch_check_init();
	
	const char* test_typestr = (const char *) arg;
	memory_kind k;
	const void *alloc_start;
	unsigned long alloc_size_bytes;
	struct uniqtype *alloc_uniqtype = (struct uniqtype *)0;
	const void *alloc_site;
	void *caller_address = __builtin_return_address(0);
	
	struct liballocs_err *err = __liballocs_get_alloc_info(obj, 
		&k,
		&alloc_start,
		&alloc_size_bytes,
		&alloc_uniqtype, 
		&alloc_site);
	
	if (__builtin_expect(err == &__liballocs_err_unrecognised_alloc_site, 0))
	{
		clear_alloc_site_metadata(alloc_start);
	}
	if (__builtin_expect(err != NULL, 0)) return 1; // we've already counted it
	
	signed target_offset_within_uniqtype = (char*) obj - (char*) alloc_start;
	/* If we're searching in a heap array, we need to take the offset modulo the 
	 * element size. Otherwise just take the whole-block offset. */
	if (ALLOC_IS_DYNAMICALLY_SIZED(alloc_start, alloc_site)
			&& alloc_uniqtype
			&& alloc_uniqtype->pos_maxoff != 0 
			&& alloc_uniqtype->neg_maxoff == 0)
	{
		target_offset_within_uniqtype %= alloc_uniqtype->pos_maxoff;
	}
	
	struct uniqtype *cur_obj_uniqtype = alloc_uniqtype;
	struct uniqtype *cur_containing_uniqtype = NULL;
	struct contained *cur_contained_pos = NULL;
	signed cumulative_offset_searched = 0;

	/* Look for a matching subobject. */
	_Bool success;
	do 
	{
		_Bool success = __liballocs_find_matching_subobject(target_offset_within_uniqtype, 
			cur_obj_uniqtype, NULL, &cur_obj_uniqtype, 
			&target_offset_within_uniqtype, &cumulative_offset_searched);
		if (__builtin_expect(success, 1))
		{
			/* This means we got a subobject of *some* type. Does it match
			 * the name? */
			// FIXME: cache names
			if 	(0 == strcmp(test_typestr, cur_obj_uniqtype->name)) goto named_a_succeeded;
			else
			{
				/* If we can descend to the first member of this type
				 * and try again, do it.
				 * 
				 * FIXME: it's not the first that matters; it's all zero-offset
				 * members. Ideally we want to refactor find_matching_subobject 
				 * so that it can match by name, but that seems to bring callbacks,
				 * meaning we must be careful not to forestall compiler optimisations. */
				if (cur_obj_uniqtype->nmemb > 0
						&& cur_obj_uniqtype->contained[0].offset == 0)
				{
					cur_obj_uniqtype = cur_obj_uniqtype->contained[0].ptr;
					continue;
				} else goto named_a_failed;
			}
		}
	} while (1);
	

named_a_succeeded:
	++__libcrunch_succeeded;
	return 1;
	
	// if we got here, we've failed
	// if we got here, the check failed
named_a_failed:
	if (__currently_allocating || __currently_freeing) 
	{
		++__libcrunch_failed_in_alloc;
		// suppress warning
	}
	else
	{
		++__libcrunch_failed;
		if (!is_suppressed(test_typestr, __builtin_return_address(0), alloc_uniqtype ? alloc_uniqtype->name : NULL))
		{
			if (should_report_failure_at(__builtin_return_address(0)))
			{
				debug_printf(0, "Failed check __named_a(%p, \"%s\") at %p (%s), allocation was a %s%s%s originating at %p\n", 
					obj, test_typestr,
					__builtin_return_address(0), format_symbolic_address(__builtin_return_address(0)),
					name_for_memory_kind(k), 
					(ALLOC_IS_DYNAMICALLY_SIZED(alloc_start, alloc_site) && alloc_uniqtype && alloc_size_bytes > alloc_uniqtype->pos_maxoff) ? " block of " : " ", 
					NAME_FOR_UNIQTYPE(alloc_uniqtype),
					alloc_site);
			}
		}
	}
	return 1; // HACK: so that the program will continue
}

int 
__check_args_internal(const void *obj, int nargs, ...)
{
	/* We might not be initialized yet (recall that __libcrunch_global_init is 
	 * not a constructor, because it's not safe to call super-early). */
	__libcrunch_check_init();

	memory_kind k;
	const void *alloc_start;
	unsigned long alloc_size_bytes;
	struct uniqtype *alloc_uniqtype = (struct uniqtype *)0;
	const void *alloc_site;
	signed target_offset_within_uniqtype;

	struct uniqtype *fun_uniqtype = NULL;
	
	struct liballocs_err *err = __liballocs_get_alloc_info(obj, 
		&k,
		&alloc_start,
		&alloc_size_bytes,
		&fun_uniqtype,
		&alloc_site);
	
	if (err != NULL) return 1;
	
	assert(fun_uniqtype);
	assert(alloc_start == obj);
	assert(UNIQTYPE_IS_SUBPROGRAM(fun_uniqtype));
	
	/* Walk the arguments that the function expects. Simultaneously, 
	 * walk our arguments. */
	va_list ap;
	va_start(ap, nargs);
	
	// FIXME: this function screws with the __libcrunch_begun count somehow
	// -- try hello-funptr
	
	_Bool success = 1;
	int i;
	for (i = 0; i < nargs && i < fun_uniqtype->array_len; ++i)
	{
		void *argval = va_arg(ap, void*);
		/* contained[0] is the return type */
		struct uniqtype *expected_arg = fun_uniqtype->contained[i+1].ptr;
		/* We only check casts that are to pointer targets types.
		 * How to test this? */
		if (UNIQTYPE_IS_POINTER_TYPE(expected_arg))
		{
			struct uniqtype *expected_arg_pointee_type = UNIQTYPE_POINTEE_TYPE(expected_arg);
			success &= __is_aU(argval, expected_arg_pointee_type);
		}
		if (!success) break;
	}
	if (i == nargs && i < fun_uniqtype->array_len)
	{
		/* This means we exhausted nargs before we got to the end of the array.
		 * In other words, the function takes more arguments than we were passed
		 * for checking, i.e. more arguments than the call site passes. 
		 * Not good! */
		success = 0;
	}
	if (i < nargs && i == fun_uniqtype->array_len)
	{
		/* This means we were passed more args than the uniqtype told us about. 
		 * FIXME: check for its varargs-ness. If it's varargs, we're allowed to
		 * pass more. For now, fail. */
		success = 0;
	}
	
	va_end(ap);
	
	/* NOTE that __check_args is not just one "test"; it's many. 
	 * So we don't maintain separate counts here; our use of __is_aU above
	 * will create many counts. */
	
	return success ? 0 : i; // 0 means success here
}

int __is_a_function_refining_internal(const void *obj, const void *arg)
{
	/* We might not be initialized yet (recall that __libcrunch_global_init is 
	 * not a constructor, because it's not safe to call super-early). */
	__libcrunch_check_init();
	
	const struct uniqtype *test_uniqtype = (const struct uniqtype *) arg;
	memory_kind k = UNKNOWN;
	const void *alloc_start;
	unsigned long alloc_size_bytes;
	struct uniqtype *alloc_uniqtype = (struct uniqtype *)0;
	const void *alloc_site;
	signed target_offset_within_uniqtype;
	
	struct liballocs_err *err = __liballocs_get_alloc_info(obj, 
		&k,
		&alloc_start,
		&alloc_size_bytes,
		&alloc_uniqtype,
		&alloc_site);
	
	if (__builtin_expect(err != NULL, 0))
	{
		return 1;
	}
	
	/* If we're offset-zero, that's good... */
	if (alloc_start == obj)
	{
		/* If we're an exact match, that's good.... */
		if (alloc_uniqtype == arg)
		{
			++__libcrunch_succeeded;
			return 1;
		}
		else
		{
			/* If we're not a function, that's bad. */
			if (UNIQTYPE_IS_SUBPROGRAM(alloc_uniqtype))
			{
				/* If our argument counts don't match, that's bad. */
				if (alloc_uniqtype->array_len == test_uniqtype->array_len)
				{
					/* For each argument, we want to make sure that 
					 * the "implicit" cast done on the argument, from
					 * the cast-from type to the cast-to type, i.e. that 
					 * the passed argument *is_a* received argument, i.e. that
					 * the cast-to argument *is_a* cast-from argument. */
					_Bool success = 1;
					/* Recall: return type is in [0] and arguments are in 1..array_len. */
					
					/* Would the cast from the return value to the post-cast return value
					 * always succeed? If so, this cast is okay. */
					struct uniqtype *alloc_return_type = alloc_uniqtype->contained[0].ptr;
					struct uniqtype *cast_return_type = test_uniqtype->contained[0].ptr;
					
					/* HACK: a little bit of C-specifity is creeping in here.
					 * FIXME: adjust this to reflect sloppy generic-pointer-pointer matches! 
					      (only if LIBCRUNCH_STRICT_GENERIC_POINTERS not set) */
					#define would_always_succeed(from, to) \
						( \
							!UNIQTYPE_IS_POINTER_TYPE((to)) \
						||  (UNIQTYPE_POINTEE_TYPE((to)) == &__uniqtype__void) \
						||  (UNIQTYPE_POINTEE_TYPE((to)) == &__uniqtype__signed_char) \
						||  (UNIQTYPE_IS_POINTER_TYPE((from)) && \
							__liballocs_find_matching_subobject( \
							/* target_offset_within_uniqtype */ 0, \
							/* cur_obj_uniqtype */ UNIQTYPE_POINTEE_TYPE((from)), \
							/* test_uniqtype */ UNIQTYPE_POINTEE_TYPE((to)), \
							/* last_attempted_uniqtype */ NULL, \
							/* last_uniqtype_offset */ NULL, \
							/* p_cumulative_offset_searched */ NULL)) \
						)
						
					/* ARGH. Are these the right way round?  
					 * The "implicit cast" is from the alloc'd return type to the 
					 * cast-to return type. */
					success &= would_always_succeed(alloc_return_type, cast_return_type);
					
					if (success) for (int i = 1; i <= alloc_uniqtype->array_len; ++i)
					{
						/* ARGH. Are these the right way round?  
						 * The "implicit cast" is from the cast-to arg type to the 
						 * alloc'd arg type. */
						success &= would_always_succeed(
							test_uniqtype->contained[i].ptr,
							alloc_uniqtype->contained[i].ptr
						);

						if (!success) break;
					}
					
					if (success)
					{
						++__libcrunch_succeeded;
						return 1;
					}
				}
			}
		}
	}
	
	// if we got here, the check failed
	if (__currently_allocating || __currently_freeing)
	{
		++__libcrunch_failed_in_alloc;
		// suppress warning
	}
	else
	{
		++__libcrunch_failed;
		if (!is_suppressed(test_uniqtype->name, __builtin_return_address(0), alloc_uniqtype ? alloc_uniqtype->name : NULL))
		{
			if (should_report_failure_at(__builtin_return_address(0)))
			{
				if (last_failed_site == __builtin_return_address(0)
						&& last_failed_deepest_subobject_type == alloc_uniqtype)
				{
					++repeat_suppression_count;
				}
				else
				{
					if (repeat_suppression_count > 0)
					{
						debug_printf(0, "Suppressed %ld further occurrences of the previous error\n", 
								repeat_suppression_count);
					}

					debug_printf(0, "Failed check __is_a_function_refining(%p, %p a.k.a. \"%s\") at %p (%s), "
							"found an allocation of a %s%s%s "
							"originating at %p\n", 
						obj, test_uniqtype, test_uniqtype->name,
						__builtin_return_address(0), format_symbolic_address(__builtin_return_address(0)), 
						name_for_memory_kind(k), 
						(ALLOC_IS_DYNAMICALLY_SIZED(alloc_start, alloc_site) && alloc_uniqtype && alloc_size_bytes > alloc_uniqtype->pos_maxoff) ? " block of " : " ", 
						NAME_FOR_UNIQTYPE(alloc_uniqtype),
						alloc_site);
					last_failed_site = __builtin_return_address(0);
					last_failed_deepest_subobject_type = alloc_uniqtype;
					repeat_suppression_count = 0;
				}
			}
		}
	}
	return 1; // HACK: so that the program will continue
}

/* This helper is short-circuiting: it doesn't tell you the precise degree 
 * of the pointer, only whether it's at least d. */
static _Bool pointer_has_degree(struct uniqtype *t, int d)
{
	while (d > 0)
	{
		if (!UNIQTYPE_IS_POINTER_TYPE(t)) return 0;
		t = UNIQTYPE_POINTEE_TYPE(t);
		assert(t);
		--d;
	}
	return 1;
}

static _Bool pointer_degree_and_ultimate_pointee_type(struct uniqtype *t, int *out_d, 
		struct uniqtype **out_ultimate_pointee_type)
{
	int d = 0;
	while (UNIQTYPE_IS_POINTER_TYPE(t))
	{
		++d;
		t = UNIQTYPE_POINTEE_TYPE(t);
	}
	*out_d = d;
	*out_ultimate_pointee_type = t;
	return 1;
}

static _Bool is_generic_ultimate_pointee(struct uniqtype *ultimate_pointee_type)
{
	return ultimate_pointee_type == &__uniqtype__void 
		|| ultimate_pointee_type == &__uniqtype__signed_char
		|| ultimate_pointee_type == &__uniqtype__unsigned_char;
}

static int pointer_degree(struct uniqtype *t)
{
	_Bool success;
	int d;
	struct uniqtype *dontcare;
	success = pointer_degree_and_ultimate_pointee_type(t, &d, &dontcare);
	return success ? d : -1;
}

static _Bool pointer_is_generic(struct uniqtype *t)
{
	_Bool success;
	int d;
	struct uniqtype *ultimate;
	success = pointer_degree_and_ultimate_pointee_type(t, &d, &ultimate);
	return success ? (d >= 1 && is_generic_ultimate_pointee(ultimate)) : 0;
}

int __is_a_pointer_of_degree_internal(const void *obj, int d)
{
	/* We might not be initialized yet (recall that __libcrunch_global_init is 
	 * not a constructor, because it's not safe to call super-early). */
	__libcrunch_check_init();
	
	memory_kind k = UNKNOWN;
	const void *alloc_start;
	unsigned long alloc_size_bytes;
	struct uniqtype *alloc_uniqtype = (struct uniqtype *)0;
	const void *alloc_site;
	
	struct liballocs_err *err = __liballocs_get_alloc_info(obj, 
		&k,
		&alloc_start,
		&alloc_size_bytes,
		&alloc_uniqtype,
		&alloc_site);
	
	if (__builtin_expect(err != NULL, 0))
	{
		return 1;
	}
	
	signed target_offset_within_uniqtype = (char*) obj - (char*) alloc_start;
	/* If we're searching in a heap array, we need to take the offset modulo the 
	 * element size. Otherwise just take the whole-block offset. */
	if (ALLOC_IS_DYNAMICALLY_SIZED(alloc_start, alloc_site)
			&& alloc_uniqtype
			&& alloc_uniqtype->pos_maxoff != 0 
			&& alloc_uniqtype->neg_maxoff == 0)
	{
		target_offset_within_uniqtype %= alloc_uniqtype->pos_maxoff;
	}
	/* Unlike other checks, we want to preserve looseness of the target block's 
	 * type, if it's a pointer type. So set the loose flag if necessary. */
	if (ALLOC_IS_DYNAMICALLY_SIZED(alloc_start, alloc_site) 
			&& alloc_site != NULL
			&& UNIQTYPE_IS_POINTER_TYPE(alloc_uniqtype))
	{
		struct insert *ins = __liballocs_insert_for_chunk_and_usable_size(
			(void*) alloc_start, malloc_usable_size((void*) alloc_start)
		);
		if (ins->alloc_site_flag)
		{
			assert(0 == ins->alloc_site & 0x1ul);
			ins->alloc_site |= 0x1ul;
		}
	}
	
	struct uniqtype *cur_obj_uniqtype = alloc_uniqtype;
	struct uniqtype *cur_containing_uniqtype = NULL;
	struct contained *cur_contained_pos = NULL;
	
	/* Descend the subobject hierarchy until we can't go any further (since pointers
	 * are atomic. */
	_Bool success = 1;
	while (success)
	{
		success = __liballocs_first_subobject_spanning(
			&target_offset_within_uniqtype, &cur_obj_uniqtype, &cur_containing_uniqtype,
			&cur_contained_pos);
	}
	
	if (target_offset_within_uniqtype == 0 && UNIQTYPE_IS_POINTER_TYPE(cur_obj_uniqtype))
	{
		_Bool depth_okay = pointer_has_degree(cur_obj_uniqtype, d);
		if (depth_okay)
		{
			++__libcrunch_succeeded;
			return 1;
		}
	}
	
is_a_pointer_failed:
	++__libcrunch_failed;
	debug_printf(0, "Failed check __is_a_pointer_of_degree(%p, %d) at %p (%s), "
			"found an allocation of a %s%s%s "
			"originating at %p\n", 
		obj, d,
		__builtin_return_address(0), format_symbolic_address(__builtin_return_address(0)), 
		name_for_memory_kind(k), 
		(ALLOC_IS_DYNAMICALLY_SIZED(alloc_start, alloc_site) && alloc_uniqtype && alloc_size_bytes > alloc_uniqtype->pos_maxoff) ? " block of " : " ", 
		NAME_FOR_UNIQTYPE(alloc_uniqtype),
		alloc_site);
	return 1; // so that program will continue
}

/* If we're writing into a non-generic pointer, 
 * __is_a(value, target's pointee type) must hold. It could hold at
 * any level in the stack of subobjects that "value" points into, so
 * we need the full __is_a check.
 * 
 * If we're writing into a generic pointer, we're more relaxed, but 
 * if target has degree 3, "value" must be the address of a degree2 pointer.
 */
struct match_cb_args
{
	struct uniqtype *type_of_pointer_being_stored_to;
	signed target_offset;
};
static int match_pointer_subobj_strict_cb(struct uniqtype *spans, signed span_start_offset, 
		unsigned depth, struct uniqtype *containing, struct contained *contained_pos, void *arg)
{
	/* We're storing a pointer that is legitimately a pointer to t (among others) */
	struct uniqtype *t = spans;
	struct match_cb_args *args = (struct match_cb_args *) arg;
	struct uniqtype *type_we_can_store = UNIQTYPE_POINTEE_TYPE(args->type_of_pointer_being_stored_to);
	
	if (span_start_offset == args->target_offset && type_we_can_store == t)
	{
		return 1;
	}
	return 0;
}
static int match_pointer_subobj_generic_cb(struct uniqtype *spans, signed span_start_offset, 
		unsigned depth, struct uniqtype *containing, struct contained *contained_pos, void *arg)
{
	/* We're storing a pointer that is legitimately a pointer to t (among others) */
	struct uniqtype *t = spans;
	struct match_cb_args *args = (struct match_cb_args *) arg;
	
	int degree_of_pointer_stored_to = pointer_degree(args->type_of_pointer_being_stored_to);

	if (span_start_offset == 0 && pointer_has_degree(t, degree_of_pointer_stored_to - 1))
	{
		return 1;
	}
	else return 0;
}
int __can_hold_pointer_internal(const void *obj, const void *value)
{
	/* We might not be initialized yet (recall that __libcrunch_global_init is 
	 * not a constructor, because it's not safe to call super-early). */
	__libcrunch_check_init();

	/* To hold a pointer, we must be a pointer. Find the pointer subobject at `obj'. */
	memory_kind obj_k = UNKNOWN;
	const void *obj_alloc_start;
	unsigned long obj_alloc_size_bytes;
	struct uniqtype *obj_alloc_uniqtype = (struct uniqtype *)0;
	const void *obj_alloc_site;
	
	struct liballocs_err *obj_err = __liballocs_get_alloc_info(obj, 
		&obj_k,
		&obj_alloc_start,
		&obj_alloc_size_bytes,
		&obj_alloc_uniqtype,
		&obj_alloc_site);
	
	if (__builtin_expect(obj_err != NULL, 0))
	{
		return 1;
	}
	
	signed obj_target_offset_within_uniqtype = (char*) obj - (char*) obj_alloc_start;
	/* If we're searching in a heap array, we need to take the offset modulo the 
	 * element size. Otherwise just take the whole-block offset. */
	if (ALLOC_IS_DYNAMICALLY_SIZED(obj_alloc_start, obj_alloc_site)
			&& obj_alloc_uniqtype
			&& obj_alloc_uniqtype->pos_maxoff != 0 
			&& obj_alloc_uniqtype->neg_maxoff == 0)
	{
		obj_target_offset_within_uniqtype %= obj_alloc_uniqtype->pos_maxoff;
	}
	
	struct uniqtype *cur_obj_uniqtype = obj_alloc_uniqtype;
	struct uniqtype *cur_containing_uniqtype = NULL;
	struct contained *cur_contained_pos = NULL;
	
	/* Descend the subobject hierarchy until we can't go any further (since pointers
	 * are atomic. */
	_Bool success = 1;
	while (success)
	{
		success = __liballocs_first_subobject_spanning(
			&obj_target_offset_within_uniqtype, &cur_obj_uniqtype, &cur_containing_uniqtype,
			&cur_contained_pos);
	}
	struct uniqtype *type_of_pointer_being_stored_to = cur_obj_uniqtype;
	
	memory_kind value_k = UNKNOWN;
	const void *value_alloc_start = NULL;
	unsigned long value_alloc_size_bytes = (unsigned long) -1;
	struct uniqtype *value_alloc_uniqtype = (struct uniqtype *)0;
	const void *value_alloc_site = NULL;
	_Bool value_contract_is_specialisable = 0;
	
	/* Might we have a pointer? */
	if (obj_target_offset_within_uniqtype == 0 && UNIQTYPE_IS_POINTER_TYPE(cur_obj_uniqtype))
	{
		int d;
		struct uniqtype *ultimate_pointee_type;
		pointer_degree_and_ultimate_pointee_type(type_of_pointer_being_stored_to, &d, &ultimate_pointee_type);
		assert(d > 0);
		assert(ultimate_pointee_type);
		
		/* Is this a generic pointer, of zero degree? */
		_Bool is_generic = is_generic_ultimate_pointee(ultimate_pointee_type);
		if (d == 1 && is_generic)
		{
			/* We pass if the value as (at least) equal degree.
			 * Note that the value is "off-by-one" in degree: 
			 * if target has degree 1, any address will do. */
			++__libcrunch_succeeded;
			return 1;
		}
		
		/* If we got here, we're going to have to understand `value',
		 * whether we're generic or not. */
		
// 		if (is_generic_ultimate_pointee(ultimate_pointee_type))
// 		{
// 			/* if target has degree 2, "value" must be the address of a degree1 pointer.
// 			 * if target has degree 3, "value" must be the address of a degree2 pointer.
// 			 * Etc. */
// 			struct uniqtype *value_pointee_uniqtype
// 			 = __liballocs_get_alloc_type_innermost(value);
// 			assert(value_pointee_uniqtype);
// 			if (pointer_has_degree(value_pointee_uniqtype, d - 1))
// 			{
// 				++__libcrunch_succeeded;
// 				return 1;
// 			}
// 		}

		struct liballocs_err *value_err = __liballocs_get_alloc_info(value, 
			&value_k,
			&value_alloc_start,
			&value_alloc_size_bytes,
			&value_alloc_uniqtype,
			&value_alloc_site);

		if (__builtin_expect(value_err == &__liballocs_err_unrecognised_alloc_site, 0))
		{
			clear_alloc_site_metadata(value_alloc_start);
		}
		if (__builtin_expect(value_err != NULL, 0)) return 1; // liballocs has already counted this abort
		
		signed value_target_offset_within_uniqtype = (char*) value - (char*) value_alloc_start;
		/* If we're searching in a heap array, we need to take the offset modulo the 
		 * element size. Otherwise just take the whole-block offset. */
		if (ALLOC_IS_DYNAMICALLY_SIZED(value_alloc_start, value_alloc_site)
				&& value_alloc_uniqtype
				&& value_alloc_uniqtype->pos_maxoff != 0 
				&& value_alloc_uniqtype->neg_maxoff == 0)
		{
			value_target_offset_within_uniqtype %= value_alloc_uniqtype->pos_maxoff;
		}
		/* Preserve looseness of value. */
		if (ALLOC_IS_DYNAMICALLY_SIZED(value_alloc_start, value_alloc_site) 
				&& value_alloc_site != NULL
				&& UNIQTYPE_IS_POINTER_TYPE(value_alloc_uniqtype))
		{
			struct insert *ins = __liballocs_insert_for_chunk_and_usable_size(
				(void*) value_alloc_start, malloc_usable_size((void*) value_alloc_start)
			);
			if (ins->alloc_site_flag)
			{
				assert(0 == ins->alloc_site & 0x1ul);
				ins->alloc_site |= 0x1ul;
			}
		}

		/* See if the top-level object matches */
		struct match_cb_args args = {
			.type_of_pointer_being_stored_to = type_of_pointer_being_stored_to,
			.target_offset = value_target_offset_within_uniqtype
		};
		int ret = (is_generic ? match_pointer_subobj_generic_cb : match_pointer_subobj_strict_cb)(
			value_alloc_uniqtype,
			0,
			0,
			NULL, NULL,
			&args
		);
		/* Here we walk the subobject hierarchy until we hit 
		 * one that is at the right offset and equals test_uniqtype.
		 
		 __liballocs_walk_subobjects_starting(
		 
		 ) ... with a cb that tests equality with test_uniqtype and returns 
		 
		 */
		if (!ret) ret = __liballocs_walk_subobjects_spanning(value_target_offset_within_uniqtype, 
			value_alloc_uniqtype, 
			is_generic ? match_pointer_subobj_generic_cb : match_pointer_subobj_strict_cb, 
			&args);
		
		if (ret)
		{
			++__libcrunch_succeeded;
			return 1;
		}
	}
	/* Can we specialise the contract of
	 * 
	 *  either the written-to pointer
	 * or
	 *  the object pointed to 
	 *
	 * so that the check would succeed?
	 * 
	 * We can only specialise the contract of as-yet-"unused" objects.
	 * Might the written-to pointer be as-yet-"unused"?
	 * We know the check failed, so currently it can't point to the
	 * value we want it to, either because it's generic but has too-high degree
	 * or because it's non-generic and doesn't match "value".
	 * These don't seem like cases we want to specialise. The only one
	 * that makes sense is replacing it with a lower degree, and I can't see
	 * any practical case where that would arise (e.g. allocating sizeof void***
	 * when you actually want void** -- possible but weird).
	 * 
	 * Might the "value" object be as-yet-unused?
	 * Yes, certainly.
	 * The check failed, so it's the wrong type.
	 * If a refinement of its type yields a "right" type,
	 * we might be in business.
	 * What's a "right" type?
	 * If the written-to pointer is not generic, then it's that target type.
	 */
	// FIXME: use value_alloc_start to avoid another heap lookup
	struct insert *value_object_info = lookup_object_info(value, NULL, NULL, NULL);
	/* HACK: until we have a "loose" bit */
	struct uniqtype *pointee = UNIQTYPE_POINTEE_TYPE(type_of_pointer_being_stored_to);

	if (!pointer_is_generic(type_of_pointer_being_stored_to)
		&& value_alloc_uniqtype
		&& UNIQTYPE_IS_POINTER_TYPE(value_alloc_uniqtype)
		&& pointer_is_generic(value_alloc_uniqtype)
		&& value_object_info
		&& STORAGE_CONTRACT_IS_LOOSE(value_object_info, value_alloc_site))
	{
		value_object_info->alloc_site_flag = 1;
		value_object_info->alloc_site = (uintptr_t) pointee; // i.e. *not* loose!
		debug_printf(0, "libcrunch: specialised allocation at %p from %s to %s\n", 
			value, NAME_FOR_UNIQTYPE(value_alloc_uniqtype), NAME_FOR_UNIQTYPE(pointee));
		++__libcrunch_lazy_heap_type_assignment;
		return 1;
	}

can_hold_pointer_failed:
	++__libcrunch_failed;
	debug_printf(0, "Failed check __can_hold_pointer(%p, %p) at %p (%s), "
			"target pointer is a %s, %ld bytes into an allocation of a %s%s%s originating at %p, "
			"value points %ld bytes into an allocation of a %s%s%s originating at %p\n", 
		obj, value,
		__builtin_return_address(0), format_symbolic_address(__builtin_return_address(0)), 
		NAME_FOR_UNIQTYPE(type_of_pointer_being_stored_to),
		(long)((char*) obj - (char*) obj_alloc_start),
		name_for_memory_kind(obj_k), 
		(ALLOC_IS_DYNAMICALLY_SIZED(obj_alloc_start, obj_alloc_site) && obj_alloc_uniqtype && obj_alloc_size_bytes > obj_alloc_uniqtype->pos_maxoff) ? " block of " : " ", 
		NAME_FOR_UNIQTYPE(obj_alloc_uniqtype),
		obj_alloc_site,
		(long)((char*) value - (char*) value_alloc_start),
		name_for_memory_kind(value_k), 
		(ALLOC_IS_DYNAMICALLY_SIZED(value_alloc_start, value_alloc_site) && value_alloc_uniqtype && value_alloc_size_bytes > value_alloc_uniqtype->pos_maxoff) ? " block of " : " ", 
		NAME_FOR_UNIQTYPE(value_alloc_uniqtype),
		value_alloc_site
		);
	return 1; // fail, but program continues
	
}

/* Provide a non-inlined version of __is_aU(). This means the Clang sanitiser
 * doesn't have to recreate the entire function (including caching) in
 * hand-written LLVM IR. */
int __is_aU_not_inlined(const void *obj, const void *uniqtype)
{
	__is_aU(obj, uniqtype);
}
