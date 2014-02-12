/*
 * elf_info.c
 *
 * Copyright (C) 2011  NEC Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <gelf.h>
#include <libelf.h>

#include "common.h"
#include "print_info.h"
#include "elf_info.h"
#include "makedumpfile.h"

#define ELF32		(1)
#define ELF64		(2)

#define VMCOREINFO_NOTE_NAME		"VMCOREINFO"
#define VMCOREINFO_NOTE_NAME_BYTES	(sizeof(VMCOREINFO_NOTE_NAME))
#define VMCOREINFO_XEN_NOTE_NAME	"VMCOREINFO_XEN"
#define VMCOREINFO_XEN_NOTE_NAME_BYTES	(sizeof(VMCOREINFO_XEN_NOTE_NAME))

#define XEN_ELFNOTE_CRASH_INFO	(0x1000001)

struct pt_load_segment {
	off_t			file_offset;
	unsigned long long	phys_start;
	unsigned long long	phys_end;
	unsigned long long	virt_start;
	unsigned long long	virt_end;
};

static int			nr_cpus;             /* number of cpu */

/*
 * File information about /proc/vmcore:
 */
static int			fd_memory;
static char			*name_memory;

static int			flags_memory;
#define MEMORY_ELF64		(1 << 0)
#define MEMORY_XEN		(1 << 1)

/*
 * PT_LOAD information about /proc/vmcore:
 */
static unsigned int		num_pt_loads;
static struct pt_load_segment	*pt_loads;
static off_t			offset_pt_load_memory;

/*
 * PT_NOTE information about /proc/vmcore:
 */
static off_t			offset_pt_note_memory;
static unsigned long		size_pt_note_memory;

/*
 * vmcoreinfo in /proc/vmcore:
 */
static off_t			offset_vmcoreinfo;
static unsigned long		size_vmcoreinfo;
static off_t			offset_vmcoreinfo_xen;
static unsigned long		size_vmcoreinfo_xen;

/*
 * erased information in /proc/vmcore:
 */
static off_t			offset_eraseinfo;
static unsigned long		size_eraseinfo;

/*
 * Xen information:
 */
static off_t			offset_xen_crash_info;
static unsigned long		size_xen_crash_info;
static unsigned long		xen_p2m_mfn;


/*
 * Internal functions.
 */
static int
get_elf64_phdr(int fd, char *filename, int index, Elf64_Phdr *phdr)
{
	off_t offset;

	offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) * index;

	if (lseek(fd, offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	if (read(fd, phdr, sizeof(Elf64_Phdr)) != sizeof(Elf64_Phdr)) {
		ERRMSG("Can't read %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	return TRUE;
}

static int
get_elf32_phdr(int fd, char *filename, int index, Elf32_Phdr *phdr)
{
	off_t offset;

	offset = sizeof(Elf32_Ehdr) + sizeof(Elf32_Phdr) * index;

	if (lseek(fd, offset, SEEK_SET) < 0) {
		ERRMSG("Can't seek %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	if (read(fd, phdr, sizeof(Elf32_Phdr)) != sizeof(Elf32_Phdr)) {
		ERRMSG("Can't read %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	return TRUE;
}

static int
check_elf_format(int fd, char *filename, int *phnum, unsigned int *num_load)
{
	int i;
	Elf64_Ehdr ehdr64;
	Elf64_Phdr load64;
	Elf32_Ehdr ehdr32;
	Elf32_Phdr load32;

	if (lseek(fd, 0, SEEK_SET) < 0) {
		ERRMSG("Can't seek %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	if (read(fd, &ehdr64, sizeof(Elf64_Ehdr)) != sizeof(Elf64_Ehdr)) {
		ERRMSG("Can't read %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	if (lseek(fd, 0, SEEK_SET) < 0) {
		ERRMSG("Can't seek %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	if (read(fd, &ehdr32, sizeof(Elf32_Ehdr)) != sizeof(Elf32_Ehdr)) {
		ERRMSG("Can't read %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	(*num_load) = 0;
	if ((ehdr64.e_ident[EI_CLASS] == ELFCLASS64)
	    && (ehdr32.e_ident[EI_CLASS] != ELFCLASS32)) {
		(*phnum) = ehdr64.e_phnum;
		for (i = 0; i < ehdr64.e_phnum; i++) {
			if (!get_elf64_phdr(fd, filename, i, &load64)) {
				ERRMSG("Can't find Phdr %d.\n", i);
				return FALSE;
			}
			if (load64.p_type == PT_LOAD)
				(*num_load)++;
		}
		return ELF64;

	} else if ((ehdr64.e_ident[EI_CLASS] != ELFCLASS64)
	    && (ehdr32.e_ident[EI_CLASS] == ELFCLASS32)) {
		(*phnum) = ehdr32.e_phnum;
		for (i = 0; i < ehdr32.e_phnum; i++) {
			if (!get_elf32_phdr(fd, filename, i, &load32)) {
				ERRMSG("Can't find Phdr %d.\n", i);
				return FALSE;
			}
			if (load32.p_type == PT_LOAD)
				(*num_load)++;
		}
		return ELF32;
	}
	ERRMSG("Can't get valid ehdr.\n");
	return FALSE;
}

static int
dump_Elf_load(Elf64_Phdr *prog, int num_load)
{
	struct pt_load_segment *pls;

	if (prog->p_type != PT_LOAD) {
		ERRMSG("Not PT_LOAD.\n");
		return FALSE;
	}

	pls = &pt_loads[num_load];
	pls->phys_start  = prog->p_paddr;
	pls->phys_end    = pls->phys_start + prog->p_filesz;
	pls->virt_start  = prog->p_vaddr;
	pls->virt_end    = pls->virt_start + prog->p_filesz;
	pls->file_offset = prog->p_offset;

	DEBUG_MSG("LOAD (%d)\n", num_load);
	DEBUG_MSG("  phys_start : %llx\n", pls->phys_start);
	DEBUG_MSG("  phys_end   : %llx\n", pls->phys_end);
	DEBUG_MSG("  virt_start : %llx\n", pls->virt_start);
	DEBUG_MSG("  virt_end   : %llx\n", pls->virt_end);

	return TRUE;
}

static off_t
offset_next_note(void *note)
{
	off_t offset;
	Elf64_Nhdr *note64;
	Elf32_Nhdr *note32;

	/*
	 * Both name and desc in ELF Note elements are padded to
	 * 4 byte boundary.
	 */
	if (is_elf64_memory()) {
		note64 = (Elf64_Nhdr *)note;
		offset = sizeof(Elf64_Nhdr)
		    + roundup(note64->n_namesz, 4)
		    + roundup(note64->n_descsz, 4);
	} else {
		note32 = (Elf32_Nhdr *)note;
		offset = sizeof(Elf32_Nhdr)
		    + roundup(note32->n_namesz, 4)
		    + roundup(note32->n_descsz, 4);
	}
	return offset;
}

static int
note_type(void *note)
{
	int type;
	Elf64_Nhdr *note64;
	Elf32_Nhdr *note32;

	if (is_elf64_memory()) {
		note64 = (Elf64_Nhdr *)note;
		type = note64->n_type;
	} else {
		note32 = (Elf32_Nhdr *)note;
		type = note32->n_type;
	}
	return type;
}

static int
note_descsz(void *note)
{
	int size;
	Elf64_Nhdr *note64;
	Elf32_Nhdr *note32;

	if (is_elf64_memory()) {
		note64 = (Elf64_Nhdr *)note;
		size = note64->n_descsz;
	} else {
		note32 = (Elf32_Nhdr *)note;
		size = note32->n_descsz;
	}
	return size;
}

static off_t
offset_note_desc(void *note)
{
	off_t offset;
	Elf64_Nhdr *note64;
	Elf32_Nhdr *note32;

	if (is_elf64_memory()) {
		note64 = (Elf64_Nhdr *)note;
		offset = sizeof(Elf64_Nhdr) + roundup(note64->n_namesz, 4);
	} else {
		note32 = (Elf32_Nhdr *)note;
		offset = sizeof(Elf32_Nhdr) + roundup(note32->n_namesz, 4);
	}
	return offset;
}

static int
get_pt_note_info(void)
{
	int n_type, size_desc;
	unsigned long p2m_mfn;
	off_t offset, offset_desc, off_p2m = 0;
	char buf[VMCOREINFO_XEN_NOTE_NAME_BYTES];
	char note[MAX_SIZE_NHDR];

	nr_cpus = 0;
	offset = offset_pt_note_memory;
	while (offset < offset_pt_note_memory + size_pt_note_memory) {
		if (lseek(fd_memory, offset, SEEK_SET) < 0) {
			ERRMSG("Can't seek the dump memory(%s). %s\n",
			    name_memory, strerror(errno));
			return FALSE;
		}
		if (read(fd_memory, note, sizeof(note)) != sizeof(note)) {
			ERRMSG("Can't read the dump memory(%s). %s\n",
			    name_memory, strerror(errno));
			return FALSE;
		}
		if (read(fd_memory, &buf, sizeof(buf)) != sizeof(buf)) {
			ERRMSG("Can't read the dump memory(%s). %s\n",
			    name_memory, strerror(errno));
			return FALSE;
		}
		n_type = note_type(note);

		if (n_type == NT_PRSTATUS) {
			nr_cpus++;
			offset += offset_next_note(note);
			continue;
		}
		offset_desc = offset + offset_note_desc(note);
		size_desc   = note_descsz(note);

		/*
		 * Check whether /proc/vmcore contains vmcoreinfo,
		 * and get both the offset and the size.
		 *
		 * NOTE: The owner name of xen should be checked at first,
		 *       because its name is "VMCOREINFO_XEN" and the one
		 *       of linux is "VMCOREINFO".
		 */
		if (!strncmp(VMCOREINFO_XEN_NOTE_NAME, buf,
		    VMCOREINFO_XEN_NOTE_NAME_BYTES)) {
			offset_vmcoreinfo_xen = offset_desc;
			size_vmcoreinfo_xen   = size_desc;
		} else if (!strncmp(VMCOREINFO_NOTE_NAME, buf,
		    VMCOREINFO_NOTE_NAME_BYTES)) {
			set_vmcoreinfo(offset_desc, size_desc);

		/*
		 * Check whether /proc/vmcore contains xen's note.
		 */
		} else if (n_type == XEN_ELFNOTE_CRASH_INFO) {
			flags_memory |= MEMORY_XEN;
			offset_xen_crash_info = offset_desc;
			size_xen_crash_info   = size_desc;

			off_p2m = offset + offset_next_note(note)
					 - sizeof(p2m_mfn);
			if (lseek(fd_memory, off_p2m, SEEK_SET) < 0){
				ERRMSG("Can't seek the dump memory(%s). %s\n",
				    name_memory, strerror(errno));
				return FALSE;
			}
			if (read(fd_memory, &p2m_mfn, sizeof(p2m_mfn))
			     != sizeof(p2m_mfn)) {
				ERRMSG("Can't read the dump memory(%s). %s\n",
				    name_memory, strerror(errno));
				return FALSE;
			}
			xen_p2m_mfn = p2m_mfn;

		/*
		 * Check whether a source dumpfile contains eraseinfo.
		 *   /proc/vmcore does not contain eraseinfo, because eraseinfo
		 *   is added only by makedumpfile and makedumpfile does not
		 *   create /proc/vmcore.
		 */
		} else if (!strncmp(ERASEINFO_NOTE_NAME, buf,
		    ERASEINFO_NOTE_NAME_BYTES)) {
			set_eraseinfo(offset_desc, size_desc);
		}
		offset += offset_next_note(note);
	}
	if (is_xen_memory())
		DEBUG_MSG("Xen kdump\n");
	else
		DEBUG_MSG("Linux kdump\n");

	return TRUE;
}


/*
 * External functions.
 */

/*
 * Convert Physical Address to File Offset.
 *  If this function returns 0x0, File Offset isn't found.
 *  The File Offset 0x0 is in the ELF header.
 *  It is not in the memory image.
 */
off_t
paddr_to_offset(unsigned long long paddr)
{
	int i;
	off_t offset;
	struct pt_load_segment *pls;

	for (i = offset = 0; i < num_pt_loads; i++) {
		pls = &pt_loads[i];
		if ((paddr >= pls->phys_start)
		    && (paddr < pls->phys_end)) {
			offset = (off_t)(paddr - pls->phys_start) +
				pls->file_offset;
			break;
		}
	}
	return offset;
}

/*
 * Same as paddr_to_offset() but makes sure that the specified offset (hint)
 * in the segment.
 */
off_t
paddr_to_offset2(unsigned long long paddr, off_t hint)
{
	int i;
	off_t offset;
	unsigned long long len;
	struct pt_load_segment *pls;

	for (i = offset = 0; i < num_pt_loads; i++) {
		pls = &pt_loads[i];
		len = pls->phys_end - pls->phys_start;
		if ((paddr >= pls->phys_start)
		    && (paddr < pls->phys_end)
		    && (hint >= pls->file_offset)
		    && (hint < pls->file_offset + len)) {
			offset = (off_t)(paddr - pls->phys_start) +
				pls->file_offset;
			break;
		}
	}
	return offset;
}

unsigned long long
vaddr_to_paddr_general(unsigned long long vaddr)
{
	int i;
	unsigned long long paddr = NOT_PADDR;
	struct pt_load_segment *pls;

	if (pt_loads == NULL)
		return NOT_PADDR;

	for (i = 0; i < num_pt_loads; i++) {
		pls = &pt_loads[i];
		if ((vaddr >= pls->virt_start)
		    && (vaddr < pls->virt_end)) {
			paddr = (off_t)(vaddr - pls->virt_start) +
				pls->phys_start;
			break;
		}
	}
	return paddr;
}

/*
 * This function is slow because it doesn't use the memory.
 * It is useful at few calls like get_str_osrelease_from_vmlinux().
 */
off_t
vaddr_to_offset_slow(int fd, char *filename, unsigned long long vaddr)
{
	off_t offset = 0;
	int i, phnum, flag_elf64, elf_format;
	unsigned int num_load;
	Elf64_Phdr load64;
	Elf32_Phdr load32;

	elf_format = check_elf_format(fd, filename, &phnum, &num_load);
	if (elf_format == ELF64)
		flag_elf64 = TRUE;
	else if (elf_format == ELF32)
		flag_elf64 = FALSE;
	else
		return 0;

	for (i = 0; i < phnum; i++) {
		if (flag_elf64) { /* ELF64 */
			if (!get_elf64_phdr(fd, filename, i, &load64)) {
				ERRMSG("Can't find Phdr %d.\n", i);
				return 0;
			}
			if (load64.p_type != PT_LOAD)
				continue;

			if ((vaddr < load64.p_vaddr)
			    || (load64.p_vaddr + load64.p_filesz <= vaddr))
				continue;

			offset = load64.p_offset + (vaddr - load64.p_vaddr);
			break;
		} else {         /* ELF32 */
			if (!get_elf32_phdr(fd, filename, i, &load32)) {
				ERRMSG("Can't find Phdr %d.\n", i);
				return 0;
			}
			if (load32.p_type != PT_LOAD)
				continue;

			if ((vaddr < load32.p_vaddr)
			    || (load32.p_vaddr + load32.p_filesz <= vaddr))
				continue;

			offset = load32.p_offset + (vaddr - load32.p_vaddr);
			break;
		}
	}
	return offset;
}

unsigned long long
get_max_paddr(void)
{
	int i;
	unsigned long long max_paddr = 0;
	struct pt_load_segment *pls;

	for (i = 0; i < num_pt_loads; i++) {
		pls = &pt_loads[i];
		if (max_paddr < pls->phys_end)
			max_paddr = pls->phys_end;
	}
	return max_paddr;
}

int
get_elf64_ehdr(int fd, char *filename, Elf64_Ehdr *ehdr)
{
	if (lseek(fd, 0, SEEK_SET) < 0) {
		ERRMSG("Can't seek %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	if (read(fd, ehdr, sizeof(Elf64_Ehdr)) != sizeof(Elf64_Ehdr)) {
		ERRMSG("Can't read %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
		ERRMSG("Can't get valid e_ident.\n");
		return FALSE;
	}
	return TRUE;
}

int
get_elf32_ehdr(int fd, char *filename, Elf32_Ehdr *ehdr)
{
	if (lseek(fd, 0, SEEK_SET) < 0) {
		ERRMSG("Can't seek %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	if (read(fd, ehdr, sizeof(Elf32_Ehdr)) != sizeof(Elf32_Ehdr)) {
		ERRMSG("Can't read %s. %s\n", filename, strerror(errno));
		return FALSE;
	}
	if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
		ERRMSG("Can't get valid e_ident.\n");
		return FALSE;
	}
	return TRUE;
}

/*
 * Get ELF information about /proc/vmcore.
 */
int
get_elf_info(int fd, char *filename)
{
	int i, j, phnum, elf_format;
	Elf64_Phdr phdr;

	/*
	 * Check ELF64 or ELF32.
	 */
	elf_format = check_elf_format(fd, filename, &phnum, &num_pt_loads);
	if (elf_format == ELF64)
		flags_memory |= MEMORY_ELF64;
	else if (elf_format != ELF32)
		return FALSE;

	if (!num_pt_loads) {
		ERRMSG("Can't get the number of PT_LOAD.\n");
		return FALSE;
	}

	/*
	 * The below file information will be used as /proc/vmcore.
	 */
	fd_memory   = fd;
	name_memory = filename;

	pt_loads = calloc(sizeof(struct pt_load_segment), num_pt_loads);
	if (pt_loads == NULL) {
		ERRMSG("Can't allocate memory for the PT_LOAD. %s\n",
		    strerror(errno));
		return FALSE;
	}
	for (i = 0, j = 0; i < phnum; i++) {
		if (!get_phdr_memory(i, &phdr))
			return FALSE;

		if (phdr.p_type == PT_NOTE) {
			set_pt_note(phdr.p_offset, phdr.p_filesz);
		}
		if (phdr.p_type != PT_LOAD)
			continue;

		if (j == 0) {
			offset_pt_load_memory = phdr.p_offset;
			if (offset_pt_load_memory == 0) {
				ERRMSG("Can't get the offset of page data.\n");
				return FALSE;
			}
		}
		if (j >= num_pt_loads)
			return FALSE;
		if(!dump_Elf_load(&phdr, j))
			return FALSE;
		j++;
	}
	if (!has_pt_note()) {
		ERRMSG("Can't find PT_NOTE Phdr.\n");
		return FALSE;
	}
	if (!get_pt_note_info()) {
		ERRMSG("Can't get PT_NOTE information.\n");
		return FALSE;
	}
	return TRUE;
}

void
free_elf_info(void)
{
	free(pt_loads);
	pt_loads = NULL;
}

int
is_elf64_memory(void)
{
	return (flags_memory & MEMORY_ELF64);
}

int
is_xen_memory(void)
{
	return (flags_memory & MEMORY_XEN);
}

int
get_phnum_memory(void)
{
	int phnum;
	Elf64_Ehdr ehdr64;
	Elf32_Ehdr ehdr32;

	if (is_elf64_memory()) { /* ELF64 */
		if (!get_elf64_ehdr(fd_memory, name_memory, &ehdr64)) {
			ERRMSG("Can't get ehdr64.\n");
			return FALSE;
		}
		phnum = ehdr64.e_phnum;
	} else {                /* ELF32 */
		if (!get_elf32_ehdr(fd_memory, name_memory, &ehdr32)) {
			ERRMSG("Can't get ehdr32.\n");
			return FALSE;
		}
		phnum = ehdr32.e_phnum;
	}
	return phnum;
}

int
get_phdr_memory(int index, Elf64_Phdr *phdr)
{
	Elf32_Phdr phdr32;

	if (is_elf64_memory()) { /* ELF64 */
		if (!get_elf64_phdr(fd_memory, name_memory, index, phdr)) {
			ERRMSG("Can't find Phdr %d.\n", index);
			return FALSE;
		}
	} else {
		if (!get_elf32_phdr(fd_memory, name_memory, index, &phdr32)) {
			ERRMSG("Can't find Phdr %d.\n", index);
			return FALSE;
		}
		memset(phdr, 0, sizeof(Elf64_Phdr));
		phdr->p_type   = phdr32.p_type;
		phdr->p_flags  = phdr32.p_flags;
		phdr->p_offset = phdr32.p_offset;
		phdr->p_vaddr  = phdr32.p_vaddr;
		phdr->p_paddr  = phdr32.p_paddr;
		phdr->p_filesz = phdr32.p_filesz;
		phdr->p_memsz  = phdr32.p_memsz;
		phdr->p_align  = phdr32.p_align;
	}
	return TRUE;
}

off_t
get_offset_pt_load_memory(void)
{
	return offset_pt_load_memory;
}

int
get_pt_load(int idx,
	unsigned long long *phys_start,
	unsigned long long *phys_end,
	unsigned long long *virt_start,
	unsigned long long *virt_end)
{
	struct pt_load_segment *pls;

	if (num_pt_loads <= idx)
		return FALSE;

	pls = &pt_loads[idx];

	if (phys_start)
		*phys_start = pls->phys_start;
	if (phys_end)
		*phys_end   = pls->phys_end;
	if (virt_start)
		*virt_start = pls->virt_start;
	if (virt_end)
		*virt_end   = pls->virt_end;

	return TRUE;
}

unsigned int
get_num_pt_loads(void)
{
	return num_pt_loads;
}

void
set_nr_cpus(int num)
{
	nr_cpus = num;
}

int
get_nr_cpus(void)
{
	return nr_cpus;
}

int
has_pt_note(void)
{
	if (info->flag_sadump) {
		if (size_pt_note_memory)
			return TRUE;
	} else if (offset_pt_note_memory && size_pt_note_memory)
		return TRUE;
	return FALSE;
}

void
set_pt_note(off_t offset, unsigned long size)
{
	offset_pt_note_memory = offset;
	size_pt_note_memory   = size;
}

void
get_pt_note(off_t *offset, unsigned long *size)
{
	if (offset)
		*offset = offset_pt_note_memory;
	if (size)
		*size   = size_pt_note_memory;
}

int
has_vmcoreinfo(void)
{
	if (offset_vmcoreinfo && size_vmcoreinfo)
		return TRUE;
	return FALSE;
}

void
set_vmcoreinfo(off_t offset, unsigned long size)
{
	offset_vmcoreinfo = offset;
	size_vmcoreinfo   = size;
}

void
get_vmcoreinfo(off_t *offset, unsigned long *size)
{
	if (offset)
		*offset = offset_vmcoreinfo;
	if (size)
		*size   = size_vmcoreinfo;
}

int
has_vmcoreinfo_xen(void)
{
	if (offset_vmcoreinfo_xen && size_vmcoreinfo_xen)
		return TRUE;
	return FALSE;
}

void
get_vmcoreinfo_xen(off_t *offset, unsigned long *size)
{
	if (offset)
		*offset = offset_vmcoreinfo_xen;
	if (size)
		*size   = size_vmcoreinfo_xen;
}

void
get_xen_crash_info(off_t *offset, unsigned long *size)
{
	if (offset)
		*offset = offset_xen_crash_info;
	if (size)
		*size   = size_xen_crash_info;
}

int
has_eraseinfo(void)
{
	if (offset_eraseinfo && size_eraseinfo)
		return TRUE;
	return FALSE;
}

void
get_eraseinfo(off_t *offset, unsigned long *size)
{
	if (offset)
		*offset = offset_eraseinfo;
	if (size)
		*size   = size_eraseinfo;
}

void
set_eraseinfo(off_t offset, unsigned long size)
{
	offset_eraseinfo = offset;
	size_eraseinfo   = size;
}

unsigned long
get_xen_p2m_mfn(void)
{
	return xen_p2m_mfn;
}

