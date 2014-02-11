/* What we don't (yet) trap: 
 * 
 *  fork(), vfork(), clone()     -- FIXME: do we care about the fork-without-exec case?
 *
 *  dlopen(), dlclose()  -- hoping that mmap is enough
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "libcrunch_private.h"

#define safe_to_use_prefix_tree (__libcrunch_is_initialized)

static const char *filename_for_fd(int fd)
{
	/* We read from /proc into a thread-local buffer. */
	static char __thread out_buf[8192];
	
	static char __thread proc_path[4096];
	int ret = snprintf(proc_path, sizeof proc_path, "/proc/%d/fd/%d", getpid(), fd);
	assert(ret > 0);
	ret = readlink(proc_path, out_buf, sizeof out_buf);
	assert(ret != -1);
	
	return out_buf;
}

/* NOTE that our wrappers are all init-on-use. This is because 
 * we might get called very early, and even if we're not trying to
 * intercept the early calls, we still need to be able to delegate. 
 * For that, we need our underyling function pointers. */

/* NOTE / HACK / glibc-specificity: we know about two different mmap entry 
 * points: mmap and mmap64. 
 * 
 * on x86-64, mmap64 has 8-byte size_t length and 8-byte off_t offset.
 * on x86-64, mmap has 8-byte size_t length and 8-byte off_t offset.
 * So I think the differences are only on 32-bit platforms. 
 * For now, just alias mmap64 to mmap. */

void *mmap(void *addr, size_t length, int prot, int flags,
                  int fd, off_t offset)
{
	/* HACK: let through the memtable mmaps and other things that 
	 * run before our constructor. These will get called *very* early,
	 * before malloc is initialized, hence before it's safe to call
	 * libdl. Therefore, instead of orig_mmap, we use syscall() 
	 * in these cases.
	 */

	if (!safe_to_use_prefix_tree)
	{
		// call via syscall
		return (void*) (uintptr_t) syscall(SYS_mmap, addr, length, prot, flags, fd, offset);
	}

	static void *(*orig_mmap)(void *, size_t, int, int, int, off_t);
	if (!orig_mmap)
	{
		orig_mmap = dlsym(RTLD_NEXT, "mmap");
		assert(orig_mmap);
	}
	
	void *ret = orig_mmap(addr, length, prot, flags, fd, offset);
	if (ret != MAP_FAILED)
	{
		/* Add to the prefix tree */
		if (fd != -1)
		{
			prefix_tree_add(ret, length, MAPPED_FILE, filename_for_fd(fd));
		}
		else
		{
			prefix_tree_add(ret, length, HEAP, NULL);
		}
	}
	return ret;
}
void *mmap64(void *addr, size_t length, int prot, int flags,
                  int fd, off_t offset) __attribute__((alias("mmap")));

int munmap(void *addr, size_t length)
{
	static int (*orig_munmap)(void *, size_t);
	if (!orig_munmap)
	{
		orig_munmap = dlsym(RTLD_NEXT, "munmap");
		assert(orig_munmap);
	}
	
	if (!safe_to_use_prefix_tree) return orig_munmap(addr, length);
	else
	{
		int ret = orig_munmap(addr, length);
		if (ret == 0)
		{
			prefix_tree_del(addr, length);
		}
		return ret;
	}
}

void *mremap(void *old_addr, size_t old_size, size_t new_size, int flags, ... /* void *new_address */)
{
	static void *(*orig_mremap)(void *, size_t, size_t, int, ...);
	va_list ap;
	if (!orig_mremap)
	{
		orig_mremap = dlsym(RTLD_NEXT, "mremap");
		assert(orig_mremap);
	}
	
	void *new_address;
	if (flags & MREMAP_FIXED)
	{
		va_start(ap, flags);
		new_address = va_arg(ap, void *);
		va_end(ap);
	}
	
#define orig_call ((flags & MREMAP_FIXED)  \
			? orig_mremap(old_addr, old_size, new_size, flags, new_address) \
			: orig_mremap(old_addr, old_size, new_size, flags))
	
	if (!safe_to_use_prefix_tree) 
	{
		return orig_call;
	}
	else
	{
		void *ret = orig_call;
		if (ret != MAP_FAILED)
		{
			struct prefix_tree_node *cur = prefix_tree_deepest_match_from_root(old_addr, NULL);
			assert(cur != NULL);
			unsigned kind = cur->kind;
			const void *data_ptr = cur->data_ptr;
			prefix_tree_del(old_addr, old_size);
			prefix_tree_add(ret, new_size, kind, data_ptr);
		}
		return ret;
	}
#undef orig_call
}

int __libcrunch_add_all_mappings_cb(struct dl_phdr_info *info, size_t size, void *data)
{
	const char *filename = (const char *) data;
	if (filename == NULL || 0 == strcmp(filename, info->dlpi_name))
	{
		// this is the file we care about, so iterate over its phdrs
		for (int i = 0; i < info->dlpi_phnum; ++i)
		{
			// if this phdr's a LOAD
			if (info->dlpi_phdr[i].p_type == PT_LOAD)
			{
				// add it to the tree
				prefix_tree_add((unsigned char *) info->dlpi_addr + info->dlpi_phdr[i].p_vaddr, 
					info->dlpi_phdr[i].p_memsz, STATIC, info->dlpi_name);
			}
		}
		
		// if we're looking for a single file, can stop now
		if (filename != NULL) return 1;
	}
	
	// keep going
	return 0;
	
}

void *dlopen(const char *filename, int flag)
{
	static void *(*orig_dlopen)(const char *, int);
	if (!orig_dlopen)
	{
		orig_dlopen = dlsym(RTLD_NEXT, "dlopen");
		assert(orig_dlopen);
	}
	
	if (!safe_to_use_prefix_tree) return orig_dlopen(filename, flag);
	else
	{
		void *ret = orig_dlopen(filename, flag);
		if (ret != NULL)
		{
			/* Note that in general we will get one mapping for every 
			 * LOAD phdr. So we use dl_iterate_phdr. */
			int dlpi_ret = dl_iterate_phdr(__libcrunch_add_all_mappings_cb, 
				((struct link_map *) ret)->l_name);
			assert(dlpi_ret != 0);
		}
		return ret;
	}
}

#define MAX_MAPPINGS 16
struct mapping
{
	void *base;
	size_t size;
};
struct mapping_set
{
	const char *filename;
	void *handle;
	int nmappings;
	struct mapping mappings[MAX_MAPPINGS];
};

static int gather_mappings_cb(struct dl_phdr_info *info, size_t size, void *data)
{
	struct mapping_set *mappings = (struct mapping_set *) data;
	const char *filename = mappings->filename;
	if (0 == strcmp(filename, info->dlpi_name))
	{
		// this is the file we care about, so iterate over its phdrs
		for (int i = 0; i < info->dlpi_phnum; ++i)
		{
			// if this phdr's a LOAD and matches our filename, 
			if (info->dlpi_phdr[i].p_type == PT_LOAD)
			{
				assert(mappings->nmappings < MAX_MAPPINGS);
				mappings->mappings[mappings->nmappings++] = (struct mapping) {
					(unsigned char *) ((struct link_map *) mappings->handle)->l_addr + info->dlpi_phdr[i].p_vaddr, 
					info->dlpi_phdr[i].p_memsz, 
				};
			}
		}
	
		// can stop now
		return 1;
	} // else keep going
	else return 0;
}

int dlclose(void *handle)
{
	static int (*orig_dlclose)(void *);
	static void *(*orig_dlopen)(const char *, int);
	if(!orig_dlclose)
	{
		orig_dlclose = dlsym(RTLD_NEXT, "dlclose");
		orig_dlopen = dlsym(RTLD_NEXT, "dlopen");
		assert(orig_dlclose);
	}
	
	if (!safe_to_use_prefix_tree) return orig_dlclose(handle);
	else
	{
		char *copied_filename = strdup(((struct link_map *) handle)->l_name);
		assert(copied_filename != NULL);
		
		/* Use dl_iterate_phdr to gather the mappings that we will 
		 * remove from the tree *if* the dlclose() actually unloads
		 * the library. */
		struct mapping_set mappings;
		mappings.filename = copied_filename;
		mappings.handle = handle;
		mappings.nmappings = 0;
		int dlpi_ret = dl_iterate_phdr(gather_mappings_cb, &mappings);
		assert(dlpi_ret != 0);
		
		int ret = orig_dlclose(handle);
		/* NOTE that a successful dlclose doesn't necessarily unload 
		 * the library! To see whether it's really unloaded, we use 
		 * dlopen *again* with RTLD_NOLOAD. */
		if (ret == 0)
		{
			// was it really unloaded?
			void *h = orig_dlopen(copied_filename, RTLD_LAZY | RTLD_NOLOAD);
			if (h == NULL)
			{
				// yes, it was unloaded
				for (int i = 0; i < mappings.nmappings; ++i)
				{
					prefix_tree_del(mappings.mappings[i].base, 
						mappings.mappings[i].size);
				}
			}
			else 
			{
				// it wasn't unloaded, so we do nothing
			}
		}
	
	// out:
		free(copied_filename);
		return ret;
	}
}