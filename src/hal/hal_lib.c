/** HAL stands for Hardware Abstraction Layer, and is used by EMC to
    transfer realtime data to and from I/O devices and other low-level
    modules.
*/
/********************************************************************
* Description:  hal_libc.c
*               This file, 'hal_lib.c', implements the HAL API, for 
*               both user space and realtime modules.  It uses the 
*               RTAPI and ULAPI #define symbols to determine which 
*               version to compile.
*
* Author: John Kasunich
* License: LGPL Version 2
*    
* Copyright (c) 2003 All rights reserved.
*
* Last change: 
********************************************************************/

/** Copyright (C) 2003 John Kasunich
                       <jmkasunich AT users DOT sourceforge DOT net>

    Other contributors:
                       Paul Fox
                       <pgf AT foxharp DOT boston DOT ma DOT us>
		       Alex Joni
		       <alex_joni AT users DOT sourceforge DOT net>
*/

/** This library is free software; you can redistribute it and/or
    modify it under the terms of version 2.1 of the GNU Lesser General
    Public License as published by the Free Software Foundation.
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111 USA

    THE AUTHORS OF THIS LIBRARY ACCEPT ABSOLUTELY NO LIABILITY FOR
    ANY HARM OR LOSS RESULTING FROM ITS USE.  IT IS _EXTREMELY_ UNWISE
    TO RELY ON SOFTWARE ALONE FOR SAFETY.  Any machinery capable of
    harming persons must have provisions for completely removing power
    from all motors, etc, before persons enter any danger area.  All
    machinery must be designed to comply with local and national safety
    codes, and the authors of this software can not, and do not, take
    any responsibility for such compliance.

    This code was written as part of the EMC HAL project.  For more
    information, go to www.linuxcnc.org.

*/

#include "rtapi.h"		/* RTAPI realtime OS API */
#include "hal.h"		/* HAL public API decls */
#include "hal_priv.h"		/* HAL private decls */

#include "rtapi_string.h"

#ifdef RTAPI
#include "rtapi_app.h"
/* module information */
MODULE_AUTHOR("John Kasunich");
MODULE_DESCRIPTION("Hardware Abstraction Layer for EMC");
MODULE_LICENSE("GPL");
#endif /* RTAPI */

#if defined(ULAPI)
#include <sys/types.h>		/* pid_t */
#include <unistd.h>		/* getpid() */
#endif

char *hal_shmem_base = 0;
hal_data_t *hal_data = 0;
static int lib_module_id = -1;	/* RTAPI module ID for library module */
static int lib_mem_id = 0;	/* RTAPI shmem ID for library module */

/***********************************************************************
*                  LOCAL FUNCTION DECLARATIONS                         *
************************************************************************/

/* These functions are used internally by this file.  The code is at
   the end of the file.  */

/** init_hal_data() initializes the entire HAL data structure, only
    if the structure has not already been initialized.  (The init
    is done by the first HAL component to be loaded.
*/
static int init_hal_data(void);

/** The 'shmalloc_xx()' functions allocate blocks of shared memory.
    Each function allocates a block that is 'size' bytes long.
    If 'size' is 3 or more, the block is aligned on a 4 byte
    boundary.  If 'size' is 2, it is aligned on a 2 byte boundary,
    and if 'size' is 1, it is unaligned.
    These functions do not test a mutex - they are called from
    within the hal library by code that already has the mutex.
    (The public function 'hal_malloc()' is a wrapper that gets the
    mutex and then calls 'shmalloc_up()'.)
    The only difference between the two functions is the location
    of the allocated memory.  'shmalloc_up()' allocates from the
    base of shared memory and works upward, while 'shmalloc_dn()'
    starts at the top and works down.
    This is done to improve realtime performance.  'shmalloc_up()'
    is used to allocate data that will be accessed by realtime
    code, while 'shmalloc_dn()' is used to allocate the much
    larger structures that are accessed only occaisionally during
    init.  This groups all the realtime data together, inproving
    cache performance.
*/
static void *shmalloc_up(long int size);
static void *shmalloc_dn(long int size);

/** The alloc_xxx_struct() functions allocate a structure of the
    appropriate type and return a pointer to it, or 0 if they fail.
    They attempt to re-use freed structs first, if none are
    available, then they call hal_malloc() to create a new one.
    The free_xxx_struct() functions add the structure at 'p' to
    the appropriate free list, for potential re-use later.
    All of these functions assume that the caller has already
    grabbed the hal_data mutex.
*/
hal_comp_t *halpr_alloc_comp_struct(void);
static hal_pin_t *alloc_pin_struct(void);
static hal_sig_t *alloc_sig_struct(void);
static hal_param_t *alloc_param_struct(void);
static hal_oldname_t *halpr_alloc_oldname_struct(void);
#ifdef RTAPI
static hal_funct_t *alloc_funct_struct(void);
#endif /* RTAPI */
static hal_funct_entry_t *alloc_funct_entry_struct(void);
#ifdef RTAPI
static hal_thread_t *alloc_thread_struct(void);
#endif /* RTAPI */

static void free_comp_struct(hal_comp_t * comp);
static void unlink_pin(hal_pin_t * pin);
static void free_pin_struct(hal_pin_t * pin);
static void free_sig_struct(hal_sig_t * sig);
static void free_param_struct(hal_param_t * param);
static void free_oldname_struct(hal_oldname_t * oldname);
#ifdef RTAPI
static void free_funct_struct(hal_funct_t * funct);
#endif /* RTAPI */
static void free_funct_entry_struct(hal_funct_entry_t * funct_entry);
#ifdef RTAPI
static void free_thread_struct(hal_thread_t * thread);
#endif /* RTAPI */

#ifdef RTAPI
/** 'thread_task()' is a function that is invoked as a realtime task.
    It implements a thread, by running down the thread's function list
    and calling each function in turn.
*/
static void thread_task(void *arg);
#endif /* RTAPI */

/***********************************************************************
*                  PUBLIC (API) FUNCTION CODE                          *
************************************************************************/

static int ref_cnt = 0;

int hal_init(const char *name)
{
    int comp_id;
#ifdef ULAPI
    int retval;
    void *mem;
#endif
    char rtapi_name[RTAPI_NAME_LEN + 1];
    char hal_name[HAL_NAME_LEN + 1];
    hal_comp_t *comp;

    if (name == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: no component name\n");
	return -EINVAL;
    }
    if (strlen(name) > HAL_NAME_LEN) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: component name '%s' is too long\n", name);
	return -EINVAL;
    }

#ifdef ULAPI
    if(!lib_mem_id) {
	rtapi_print_msg(RTAPI_MSG_DBG, "HAL: initializing hal_lib\n");
	rtapi_snprintf(rtapi_name, RTAPI_NAME_LEN, "HAL_LIB_%d", (int)getpid());
	lib_module_id = rtapi_init(rtapi_name);

	/* get HAL shared memory block from RTAPI */
	lib_mem_id = rtapi_shmem_new(HAL_KEY, lib_module_id, HAL_SIZE);
	if (lib_mem_id < 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: could not open shared memory\n");
	    rtapi_exit(lib_module_id);
	    return -EINVAL;
	}
	/* get address of shared memory area */
	retval = rtapi_shmem_getptr(lib_mem_id, &mem);
	if (retval < 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: could not access shared memory\n");
	    rtapi_exit(lib_module_id);
	    return -EINVAL;
	}
	/* set up internal pointers to shared mem and data structure */
        hal_shmem_base = (char *) mem;
        hal_data = (hal_data_t *) mem;
	/* perform a global init if needed */
	retval = init_hal_data();
	if ( retval ) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: could not init shared memory\n");
	    rtapi_exit(lib_module_id);
	    return -EINVAL;
	}
    }
#endif
    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: initializing component '%s'\n",
	name);
    /* copy name to local vars, truncating if needed */
    rtapi_snprintf(rtapi_name, RTAPI_NAME_LEN, "HAL_%s", name);
    rtapi_snprintf(hal_name, sizeof(hal_name), "%s", name);
    /* do RTAPI init */
    comp_id = rtapi_init(rtapi_name);
    if (comp_id < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: rtapi init failed\n");
	return -EINVAL;
    }
    /* get mutex before manipulating the shared data */
    rtapi_mutex_get(&(hal_data->mutex));
    /* make sure name is unique in the system */
    if (halpr_find_comp_by_name(hal_name) != 0) {
	/* a component with this name already exists */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: duplicate component name '%s'\n", hal_name);
	rtapi_exit(comp_id);
	return -EINVAL;
    }
    /* allocate a new component structure */
    comp = halpr_alloc_comp_struct();
    if (comp == 0) {
	/* couldn't allocate structure */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: insufficient memory for component '%s'\n", hal_name);
	rtapi_exit(comp_id);
	return -ENOMEM;
    }
    /* initialize the structure */
    comp->comp_id = comp_id;
#ifdef RTAPI
    comp->type = 1;
    comp->pid = 0;
#else /* ULAPI */
    comp->type = 0;
    comp->pid = getpid();
#endif
    comp->ready = 0;
    comp->shmem_base = hal_shmem_base;
    comp->insmod_args = 0;
    rtapi_snprintf(comp->name, sizeof(comp->name), "%s", hal_name);
    /* insert new structure at head of list */
    comp->next_ptr = hal_data->comp_list_ptr;
    hal_data->comp_list_ptr = SHMOFF(comp);
    /* done with list, release mutex */
    rtapi_mutex_give(&(hal_data->mutex));
    /* done */
    rtapi_print_msg(RTAPI_MSG_DBG,
	"HAL: component '%s' initialized, ID = %02d\n", hal_name, comp_id);
    ref_cnt ++;
    return comp_id;
}

int hal_exit(int comp_id)
{
    int *prev, next;
    hal_comp_t *comp;
    char name[HAL_NAME_LEN + 1];

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: exit called before init\n");
	return -EINVAL;
    }
    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: removing component %02d\n", comp_id);
    /* grab mutex before manipulating list */
    rtapi_mutex_get(&(hal_data->mutex));
    /* search component list for 'comp_id' */
    prev = &(hal_data->comp_list_ptr);
    next = *prev;
    if (next == 0) {
	/* list is empty - should never happen, but... */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: component %d not found\n", comp_id);
	return -EINVAL;
    }
    comp = SHMPTR(next);
    while (comp->comp_id != comp_id) {
	/* not a match, try the next one */
	prev = &(comp->next_ptr);
	next = *prev;
	if (next == 0) {
	    /* reached end of list without finding component */
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: component %d not found\n", comp_id);
	    return -EINVAL;
	}
	comp = SHMPTR(next);
    }
    /* found our component, unlink it from the list */
    *prev = comp->next_ptr;
    /* save component name for later */
    rtapi_snprintf(name, sizeof(name), "%s", comp->name);
    /* get rid of the component */
    free_comp_struct(comp);
/*! \todo Another #if 0 */
#if 0
    /*! \todo FIXME - this is the beginning of a two pronged approach to managing
       shared memory.  Prong 1 - re-init the shared memory allocator whenever 
       it is known to be safe.  Prong 2 - make a better allocator that can
       reclaim memory allocated by components when those components are
       removed. To be finished later. */
    /* was that the last component? */
    if (hal_data->comp_list_ptr == 0) {
	/* yes, are there any signals or threads defined? */
	if ((hal_data->sig_list_ptr == 0) && (hal_data->thread_list_ptr == 0)) {
	    /* no, invalidate "magic" number so shmem will be re-inited when
	       a new component is loaded */
	    hal_data->magic = 0;
	}
    }
#endif
    /* release mutex */
    rtapi_mutex_give(&(hal_data->mutex));
    --ref_cnt;
#ifdef ULAPI
    if(ref_cnt == 0) {
	/* release RTAPI resources */
	rtapi_shmem_delete(lib_mem_id, lib_module_id);
	rtapi_exit(lib_module_id);
	lib_mem_id = 0;
	lib_module_id = -1;
	hal_shmem_base = NULL;
	hal_data = NULL;
    }
#endif
    rtapi_exit(comp_id);
    /* done */
    rtapi_print_msg(RTAPI_MSG_DBG,
	"HAL: component %02d removed, name = '%s'\n", comp_id, name);


    return 0;
}

void *hal_malloc(long int size)
{
    void *retval;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: hal_malloc called before init\n");
	return 0;
    }
    /* get the mutex */
    rtapi_mutex_get(&(hal_data->mutex));
    /* allocate memory */
    retval = shmalloc_up(size);
    /* release the mutex */
    rtapi_mutex_give(&(hal_data->mutex));
    /* check return value */
    if (retval == 0) {
	rtapi_print_msg(RTAPI_MSG_DBG,
	    "HAL: hal_malloc() can't allocate %ld bytes\n", size);
    }
    return retval;
}

#ifdef RTAPI
int hal_set_constructor(int comp_id, constructor make) {
    int next;
    hal_comp_t *comp;

    rtapi_mutex_get(&(hal_data->mutex));

    /* search component list for 'comp_id' */
    next = hal_data->comp_list_ptr;
    if (next == 0) {
	/* list is empty - should never happen, but... */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: component %d not found\n", comp_id);
	return -EINVAL;
    }

    comp = SHMPTR(next);
    while (comp->comp_id != comp_id) {
	/* not a match, try the next one */
	next = comp->next_ptr;
	if (next == 0) {
	    /* reached end of list without finding component */
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: component %d not found\n", comp_id);
	    return -EINVAL;
	}
	comp = SHMPTR(next);
    }
    
    comp->make = make;

    rtapi_mutex_give(&(hal_data->mutex));
    return 0;
}
#endif

int hal_ready(int comp_id) {
    int next;
    hal_comp_t *comp;

    rtapi_mutex_get(&(hal_data->mutex));

    /* search component list for 'comp_id' */
    next = hal_data->comp_list_ptr;
    if (next == 0) {
	/* list is empty - should never happen, but... */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: component %d not found\n", comp_id);
	return -EINVAL;
    }

    comp = SHMPTR(next);
    while (comp->comp_id != comp_id) {
	/* not a match, try the next one */
	next = comp->next_ptr;
	if (next == 0) {
	    /* reached end of list without finding component */
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: component %d not found\n", comp_id);
	    return -EINVAL;
	}
	comp = SHMPTR(next);
    }
    if(comp->ready > 0) {
        rtapi_print_msg(RTAPI_MSG_ERR,
                "HAL: ERROR: Component '%s' already ready\n", comp->name);
        rtapi_mutex_give(&(hal_data->mutex));
        return -EINVAL;
    }
    comp->ready = 1;
    rtapi_mutex_give(&(hal_data->mutex));
    return 0;
}

char *hal_comp_name(int comp_id)
{
    hal_comp_t *comp;
    char *result = NULL;
    rtapi_mutex_get(&(hal_data->mutex));
    comp = halpr_find_comp_by_id(comp_id);
    if(comp) result = comp->name;
    rtapi_mutex_give(&(hal_data->mutex));
    return result;
}

/***********************************************************************
*                      "LOCKING" FUNCTIONS                             *
************************************************************************/
/** The 'hal_set_lock()' function sets locking based on one of the 
    locking types defined in hal.h
*/
int hal_set_lock(unsigned char lock_type) {
    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: set_lock called before init\n");
	return -EINVAL;
    }
    hal_data->lock = lock_type;
    return 0;
}

/** The 'hal_get_lock()' function returns the current locking level 
    locking types defined in hal.h
*/

unsigned char hal_get_lock() {
    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: get_lock called before init\n");
	return -EINVAL;
    }
    return hal_data->lock;
}


/***********************************************************************
*                        "PIN" FUNCTIONS                               *
************************************************************************/

/* wrapper functs for typed pins - these call the generic funct below */

int hal_pin_bit_new(const char *name, hal_pin_dir_t dir,
    hal_bit_t ** data_ptr_addr, int comp_id)
{
    return hal_pin_new(name, HAL_BIT, dir, (void **) data_ptr_addr, comp_id);
}

int hal_pin_float_new(const char *name, hal_pin_dir_t dir,
    hal_float_t ** data_ptr_addr, int comp_id)
{
    return hal_pin_new(name, HAL_FLOAT, dir, (void **) data_ptr_addr,
	comp_id);
}

int hal_pin_u32_new(const char *name, hal_pin_dir_t dir,
    hal_u32_t ** data_ptr_addr, int comp_id)
{
    return hal_pin_new(name, HAL_U32, dir, (void **) data_ptr_addr, comp_id);
}

int hal_pin_s32_new(const char *name, hal_pin_dir_t dir,
    hal_s32_t ** data_ptr_addr, int comp_id)
{
    return hal_pin_new(name, HAL_S32, dir, (void **) data_ptr_addr, comp_id);
}

static int hal_pin_newfv(hal_type_t type, hal_pin_dir_t dir,
    void ** data_ptr_addr, int comp_id, const char *fmt, va_list ap)
{
    char name[HAL_NAME_LEN + 1];
    int sz;
    sz = rtapi_vsnprintf(name, sizeof(name), fmt, ap);
    if(sz == -1 || sz > HAL_NAME_LEN) {
        rtapi_print_msg(RTAPI_MSG_ERR,
	    "hal_pin_newfv: length %d too long for name starting '%s'\n",
	    sz, name);
        return -ENOMEM;
    }
    return hal_pin_new(name, type, dir, data_ptr_addr, comp_id);
}

int hal_pin_bit_newf(hal_pin_dir_t dir,
    hal_bit_t ** data_ptr_addr, int comp_id, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = hal_pin_newfv(HAL_BIT, dir, (void**)data_ptr_addr, comp_id, fmt, ap);
    va_end(ap);
    return ret;
}

int hal_pin_float_newf(hal_pin_dir_t dir,
    hal_float_t ** data_ptr_addr, int comp_id, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = hal_pin_newfv(HAL_FLOAT, dir, (void**)data_ptr_addr, comp_id, fmt, ap);
    va_end(ap);
    return ret;
}

int hal_pin_u32_newf(hal_pin_dir_t dir,
    hal_u32_t ** data_ptr_addr, int comp_id, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = hal_pin_newfv(HAL_U32, dir, (void**)data_ptr_addr, comp_id, fmt, ap);
    va_end(ap);
    return ret;
}

int hal_pin_s32_newf(hal_pin_dir_t dir,
    hal_s32_t ** data_ptr_addr, int comp_id, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = hal_pin_newfv(HAL_S32, dir, (void**)data_ptr_addr, comp_id, fmt, ap);
    va_end(ap);
    return ret;
}


/* this is a generic function that does the majority of the work. */

int hal_pin_new(const char *name, hal_type_t type, hal_pin_dir_t dir,
    void **data_ptr_addr, int comp_id)
{
    int *prev, next, cmp;
    hal_pin_t *new, *ptr;
    hal_comp_t *comp;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin_new called before init\n");
	return -EINVAL;
    }

    if(*data_ptr_addr) 
    {
        rtapi_print_msg(RTAPI_MSG_ERR,
            "HAL: ERROR: pin_new(%s) called with already-initialized memory\n",
            name);
    }
    if (type != HAL_BIT && type != HAL_FLOAT && type != HAL_S32 && type != HAL_U32) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin type not one of HAL_BIT, HAL_FLOAT, HAL_S32 or HAL_U32\n");
	return -EINVAL;
    }
    
    if(dir != HAL_IN && dir != HAL_OUT && dir != HAL_IO) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin direction not one of HAL_IN, HAL_OUT, or HAL_IO\n");
	return -EINVAL;
    }
    if (strlen(name) > HAL_NAME_LEN) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin name '%s' is too long\n", name);
	return -EINVAL;
    }
    if (hal_data->lock & HAL_LOCK_LOAD)  {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin_new called while HAL locked\n");
	return -EPERM;
    }

    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: creating pin '%s'\n", name);
    /* get mutex before accessing shared data */
    rtapi_mutex_get(&(hal_data->mutex));
    /* validate comp_id */
    comp = halpr_find_comp_by_id(comp_id);
    if (comp == 0) {
	/* bad comp_id */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: component %d not found\n", comp_id);
	return -EINVAL;
    }
    /* validate passed in pointer - must point to HAL shmem */
    if (! SHMCHK(data_ptr_addr)) {
	/* bad pointer */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: data_ptr_addr not in shared memory\n");
	return -EINVAL;
    }
    if(comp->ready) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin_new called after hal_ready\n");
	return -EINVAL;
    }
    /* allocate a new variable structure */
    new = alloc_pin_struct();
    if (new == 0) {
	/* alloc failed */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: insufficient memory for pin '%s'\n", name);
	return -ENOMEM;
    }
    /* initialize the structure */
    new->data_ptr_addr = SHMOFF(data_ptr_addr);
    new->owner_ptr = SHMOFF(comp);
    new->type = type;
    new->dir = dir;
    new->signal = 0;
    memset(&new->dummysig, 0, sizeof(hal_data_u));
    rtapi_snprintf(new->name, sizeof(new->name), "%s", name);
    /* make 'data_ptr' point to dummy signal */
    *data_ptr_addr = comp->shmem_base + SHMOFF(&(new->dummysig));
    /* search list for 'name' and insert new structure */
    prev = &(hal_data->pin_list_ptr);
    next = *prev;
    while (1) {
	if (next == 0) {
	    /* reached end of list, insert here */
	    new->next_ptr = next;
	    *prev = SHMOFF(new);
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	ptr = SHMPTR(next);
	cmp = strcmp(ptr->name, new->name);
	if (cmp > 0) {
	    /* found the right place for it, insert here */
	    new->next_ptr = next;
	    *prev = SHMOFF(new);
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	if (cmp == 0) {
	    /* name already in list, can't insert */
	    free_pin_struct(new);
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: duplicate variable '%s'\n", name);
	    return -EINVAL;
	}
	/* didn't find it yet, look at next one */
	prev = &(ptr->next_ptr);
	next = *prev;
    }
}

int hal_pin_alias(const char *pin_name, const char *alias)
{
    int *prev, next, cmp;
    hal_pin_t *pin, *ptr;
    hal_oldname_t *oldname;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin_alias called before init\n");
	return -EINVAL;
    }
    if (hal_data->lock & HAL_LOCK_CONFIG)  {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin_alias called while HAL locked\n");
	return -EPERM;
    }
    if (alias != NULL ) {
	if (strlen(alias) > HAL_NAME_LEN) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
	        "HAL: ERROR: alias name '%s' is too long\n", alias);
	    return -EINVAL;
	}
    }
    /* get mutex before accessing shared data */
    rtapi_mutex_get(&(hal_data->mutex));
    if (alias != NULL ) {
	pin = halpr_find_pin_by_name(alias);
	if ( pin != NULL ) {
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
	        "HAL: ERROR: duplicate pin/alias name '%s'\n", alias);
	    return -EINVAL;
	}
    }
    /* once we unlink the pin from the list, we don't want to have to
       abort the change and repair things.  So we allocate an oldname
       struct here, then free it (which puts it on the free list).  This
       allocation might fail, in which case we abort the command.  But
       if we actually need the struct later, the next alloc is guaranteed
       to succeed since at least one struct is on the free list. */
    oldname = halpr_alloc_oldname_struct();
    if ( oldname == NULL ) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: insufficient memory for pin_alias\n");
	return -EINVAL;
    }
    free_oldname_struct(oldname);
    /* find the pin and unlink it from pin list */
    prev = &(hal_data->pin_list_ptr);
    next = *prev;
    while (1) {
	if (next == 0) {
	    /* reached end of list, not found */
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: pin '%s' not found\n", pin_name);
	    return -EINVAL;
	}
	pin = SHMPTR(next);
	if ( strcmp(pin->name, pin_name) == 0 ) {
	    /* found it, unlink from list */
	    *prev = pin->next_ptr;
	    break;
	}
	if (pin->oldname != 0 ) {
	    oldname = SHMPTR(pin->oldname);
	    if (strcmp(oldname->name, pin_name) == 0) {
		/* found it, unlink from list */
		*prev = pin->next_ptr;
		break;
	    }
	}
	/* didn't find it yet, look at next one */
	prev = &(pin->next_ptr);
	next = *prev;
    }
    if ( alias != NULL ) {
	/* adding a new alias */
	if ( pin->oldname == 0 ) {
	    /* save old name (only if not already saved) */
	    oldname = halpr_alloc_oldname_struct();
	    pin->oldname = SHMOFF(oldname);
	    rtapi_snprintf(oldname->name, sizeof(oldname->name), "%s", pin->name);
	}
	/* change pin's name to 'alias' */
	rtapi_snprintf(pin->name, sizeof(pin->name), "%s", alias);
    } else {
	/* removing an alias */
	if ( pin->oldname != 0 ) {
	    /* restore old name (only if pin is aliased) */
	    oldname = SHMPTR(pin->oldname);
	    rtapi_snprintf(pin->name, sizeof(pin->name), "%s", oldname->name);
	    pin->oldname = 0;
	    free_oldname_struct(oldname);
	}
    }
    /* insert pin back into list in proper place */
    prev = &(hal_data->pin_list_ptr);
    next = *prev;
    while (1) {
	if (next == 0) {
	    /* reached end of list, insert here */
	    pin->next_ptr = next;
	    *prev = SHMOFF(pin);
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	ptr = SHMPTR(next);
	cmp = strcmp(ptr->name, pin->name);
	if (cmp > 0) {
	    /* found the right place for it, insert here */
	    pin->next_ptr = next;
	    *prev = SHMOFF(pin);
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	/* didn't find it yet, look at next one */
	prev = &(ptr->next_ptr);
	next = *prev;
    }
}

/***********************************************************************
*                      "SIGNAL" FUNCTIONS                              *
************************************************************************/

int hal_signal_new(const char *name, hal_type_t type)
{

    int *prev, next, cmp;
    hal_sig_t *new, *ptr;
    void *data_addr;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: signal_new called before init\n");
	return -EINVAL;
    }

    if (strlen(name) > HAL_NAME_LEN) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: signal name '%s' is too long\n", name);
	return -EINVAL;
    }
    if (hal_data->lock & HAL_LOCK_CONFIG) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: signal_new called while HAL is locked\n");
	return -EPERM;
    }

    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: creating signal '%s'\n", name);
    /* get mutex before accessing shared data */
    rtapi_mutex_get(&(hal_data->mutex));
    /* check for an existing signal with the same name */
    if (halpr_find_sig_by_name(name) != 0) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: duplicate signal '%s'\n", name);
	return -EINVAL;
    }
    /* allocate memory for the signal value */
    switch (type) {
    case HAL_BIT:
	data_addr = shmalloc_up(sizeof(hal_bit_t));
	break;
    case HAL_S32:
	data_addr = shmalloc_up(sizeof(hal_s32_t));
	break;
    case HAL_U32:
	data_addr = shmalloc_up(sizeof(hal_u32_t));
	break;
    case HAL_FLOAT:
	data_addr = shmalloc_up(sizeof(hal_float_t));
	break;
    default:
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: illegal signal type %d'\n", type);
	return -EINVAL;
	break;
    }
    /* allocate a new signal structure */
    new = alloc_sig_struct();
    if ((new == 0) || (data_addr == 0)) {
	/* alloc failed */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: insufficient memory for signal '%s'\n", name);
	return -ENOMEM;
    }
    /* initialize the signal value */
    switch (type) {
    case HAL_BIT:
	*((hal_bit_t *) data_addr) = 0;
	break;
    case HAL_S32:
	*((hal_s32_t *) data_addr) = 0;
        break;
    case HAL_U32:
	*((hal_u32_t *) data_addr) = 0;
        break;
    case HAL_FLOAT:
	*((hal_float_t *) data_addr) = 0.0;
	break;
    default:
	break;
    }
    /* initialize the structure */
    new->data_ptr = SHMOFF(data_addr);
    new->type = type;
    new->readers = 0;
    new->writers = 0;
    new->bidirs = 0;
    rtapi_snprintf(new->name, sizeof(new->name), "%s", name);
    /* search list for 'name' and insert new structure */
    prev = &(hal_data->sig_list_ptr);
    next = *prev;
    while (1) {
	if (next == 0) {
	    /* reached end of list, insert here */
	    new->next_ptr = next;
	    *prev = SHMOFF(new);
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	ptr = SHMPTR(next);
	cmp = strcmp(ptr->name, new->name);
	if (cmp > 0) {
	    /* found the right place for it, insert here */
	    new->next_ptr = next;
	    *prev = SHMOFF(new);
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	/* didn't find it yet, look at next one */
	prev = &(ptr->next_ptr);
	next = *prev;
    }
}

int hal_signal_delete(const char *name)
{
    hal_sig_t *sig;
    int *prev, next;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: signal_delete called before init\n");
	return -EINVAL;
    }
    
    if (hal_data->lock & HAL_LOCK_CONFIG)  {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: signal_delete called while HAL locked\n");
	return -EPERM;
    }
    
    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: deleting signal '%s'\n", name);
    /* get mutex before accessing shared data */
    rtapi_mutex_get(&(hal_data->mutex));
    /* search for the signal */
    prev = &(hal_data->sig_list_ptr);
    next = *prev;
    while (next != 0) {
	sig = SHMPTR(next);
	if (strcmp(sig->name, name) == 0) {
	    /* this is the right signal, unlink from list */
	    *prev = sig->next_ptr;
	    /* and delete it */
	    free_sig_struct(sig);
	    /* done */
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	/* no match, try the next one */
	prev = &(sig->next_ptr);
	next = *prev;
    }
    /* if we get here, we didn't find a match */
    rtapi_mutex_give(&(hal_data->mutex));
    rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: signal '%s' not found\n",
	name);
    return -EINVAL;
}

int hal_link(const char *pin_name, const char *sig_name)
{
    hal_pin_t *pin;
    hal_sig_t *sig;
    hal_comp_t *comp;
    void **data_ptr_addr, *data_addr;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: link called before init\n");
	return -EINVAL;
    }

    if (hal_data->lock & HAL_LOCK_CONFIG)  {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: link called while HAL locked\n");
	return -EPERM;
    }
    /* make sure we were given a pin name */
    if (pin_name == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: pin name not given\n");
	return -EINVAL;
    }
    /* make sure we were given a signal name */
    if (sig_name == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: signal name not given\n");
	return -EINVAL;
    }
    rtapi_print_msg(RTAPI_MSG_DBG,
	"HAL: linking pin '%s' to '%s'\n", pin_name, sig_name);
    /* get mutex before accessing data structures */
    rtapi_mutex_get(&(hal_data->mutex));
    /* locate the pin */
    pin = halpr_find_pin_by_name(pin_name);
    if (pin == 0) {
	/* not found */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin '%s' not found\n", pin_name);
	return -EINVAL;
    }
    /* locate the signal */
    sig = halpr_find_sig_by_name(sig_name);
    if (sig == 0) {
	/* not found */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: signal '%s' not found\n", sig_name);
	return -EINVAL;
    }
    /* found both pin and signal, are they already connected? */
    if (SHMPTR(pin->signal) == sig) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_WARN,
	    "HAL: Warning: pin '%s' already linked to '%s'\n", pin_name, sig_name);
	return 0;
    }
    /* is the pin connected to something else? */
    if(pin->signal) {
	rtapi_mutex_give(&(hal_data->mutex));
	sig = SHMPTR(pin->signal);
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin '%s' is linked to '%s', cannot link to '%s'\n",
	    pin_name, sig->name, sig_name);
	return -EINVAL;
    }
    /* check types */
    if (pin->type != sig->type) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: type mismatch '%s' <- '%s'\n", pin_name, sig_name);
	return -EINVAL;
    }
    /* linking output pin to sig that already has output or I/O pins? */
    if ((pin->dir == HAL_OUT) && ((sig->writers > 0) || (sig->bidirs > 0 ))) {
	/* yes, can't do that */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: signal '%s' already has output or I/O pin(s)\n", sig_name);
	return -EINVAL;
    }
    /* linking bidir pin to sig that already has output pin? */
    if ((pin->dir == HAL_IO) && (sig->writers > 0)) {
	/* yes, can't do that */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: signal '%s' already has output pin\n", sig_name);
	return -EINVAL;
    }
    /* everything is OK, make the new link */
    data_ptr_addr = SHMPTR(pin->data_ptr_addr);
    comp = SHMPTR(pin->owner_ptr);
    data_addr = comp->shmem_base + sig->data_ptr;
    *data_ptr_addr = data_addr;
    if (( sig->readers == 0 ) && ( sig->writers == 0 ) && ( sig->bidirs == 0 )) {
	/* this is the first pin for this signal, copy value from pin's "dummy" field */
	data_addr = hal_shmem_base + sig->data_ptr;

        // assure proper typing on assignment, assigning a hal_data_u is
        // a surefire cause for memory corrupion as hal_data_u is larger
        // than hal_bit_t, hal_s32_t, and hal_u32_t - this works only for 
        // hal_float_t (!)
        // my old, buggy code:
        //*((hal_data_u *)data_addr) = pin->dummysig;

        switch (pin->type) {
        case HAL_BIT:
            *((hal_bit_t *) data_addr) = pin->dummysig.b;
            break;
        case HAL_S32:
            *((hal_s32_t *) data_addr) = pin->dummysig.s;
            break;
        case HAL_U32:
            *((hal_u32_t *) data_addr) = pin->dummysig.u;
            break;
        case HAL_FLOAT:
            *((hal_float_t *) data_addr) = pin->dummysig.f;
            break;
        default:
            rtapi_print_msg(RTAPI_MSG_ERR,
                          "HAL: BUG: pin '%s' has invalid type %d !!\n",
                          pin->name, pin->type);
            return -EINVAL;
        }
    }
    /* update the signal's reader/writer/bidir counts */
    if ((pin->dir & HAL_IN) != 0) {
	sig->readers++;
    }
    if (pin->dir == HAL_OUT) {
	sig->writers++;
    }
    if (pin->dir == HAL_IO) {
	sig->bidirs++;
    }
    /* and update the pin */
    pin->signal = SHMOFF(sig);
    /* done, release the mutex and return */
    rtapi_mutex_give(&(hal_data->mutex));
    return 0;
}

int hal_unlink(const char *pin_name)
{
    hal_pin_t *pin;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: unlink called before init\n");
	return -EINVAL;
    }

    if (hal_data->lock & HAL_LOCK_CONFIG)  {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: unlink called while HAL locked\n");
	return -EPERM;
    }
    /* make sure we were given a pin name */
    if (pin_name == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: pin name not given\n");
	return -EINVAL;
    }
    rtapi_print_msg(RTAPI_MSG_DBG,
	"HAL: unlinking pin '%s'\n", pin_name);
    /* get mutex before accessing data structures */
    rtapi_mutex_get(&(hal_data->mutex));
    /* locate the pin */
    pin = halpr_find_pin_by_name(pin_name);
    if (pin == 0) {
	/* not found */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin '%s' not found\n", pin_name);
	return -EINVAL;
    }
    /* found pin, unlink it */
    unlink_pin(pin);
    /* done, release the mutex and return */
    rtapi_mutex_give(&(hal_data->mutex));
    return 0;
}

/***********************************************************************
*                       "PARAM" FUNCTIONS                              *
************************************************************************/

/* wrapper functs for typed params - these call the generic funct below */

int hal_param_bit_new(const char *name, hal_param_dir_t dir, hal_bit_t * data_addr,
    int comp_id)
{
    return hal_param_new(name, HAL_BIT, dir, (void *) data_addr, comp_id);
}

int hal_param_float_new(const char *name, hal_param_dir_t dir, hal_float_t * data_addr,
    int comp_id)
{
    return hal_param_new(name, HAL_FLOAT, dir, (void *) data_addr, comp_id);
}

int hal_param_u32_new(const char *name, hal_param_dir_t dir, hal_u32_t * data_addr,
    int comp_id)
{
    return hal_param_new(name, HAL_U32, dir, (void *) data_addr, comp_id);
}

int hal_param_s32_new(const char *name, hal_param_dir_t dir, hal_s32_t * data_addr,
    int comp_id)
{
    return hal_param_new(name, HAL_S32, dir, (void *) data_addr, comp_id);
}

static int hal_param_newfv(hal_type_t type, hal_param_dir_t dir,
	void *data_addr, int comp_id, const char *fmt, va_list ap) {
    char name[HAL_NAME_LEN + 1];
    int sz;
    sz = rtapi_vsnprintf(name, sizeof(name), fmt, ap);
    if(sz == -1 || sz > HAL_NAME_LEN) {
        rtapi_print_msg(RTAPI_MSG_ERR,
	    "hal_param_newfv: length %d too long for name starting '%s'\n",
	    sz, name);
	return -ENOMEM;
    }
    return hal_param_new(name, type, dir, (void *) data_addr, comp_id);
}

int hal_param_bit_newf(hal_param_dir_t dir, hal_bit_t * data_addr,
    int comp_id, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = hal_param_newfv(HAL_BIT, dir, (void*)data_addr, comp_id, fmt, ap);
    va_end(ap);
    return ret;
}

int hal_param_float_newf(hal_param_dir_t dir, hal_float_t * data_addr,
    int comp_id, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = hal_param_newfv(HAL_FLOAT, dir, (void*)data_addr, comp_id, fmt, ap);
    va_end(ap);
    return ret;
}

int hal_param_u32_newf(hal_param_dir_t dir, hal_u32_t * data_addr,
    int comp_id, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = hal_param_newfv(HAL_U32, dir, (void*)data_addr, comp_id, fmt, ap);
    va_end(ap);
    return ret;
}

int hal_param_s32_newf(hal_param_dir_t dir, hal_s32_t * data_addr,
    int comp_id, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = hal_param_newfv(HAL_S32, dir, (void*)data_addr, comp_id, fmt, ap);
    va_end(ap);
    return ret;
}


/* this is a generic function that does the majority of the work. */

int hal_param_new(const char *name, hal_type_t type, hal_param_dir_t dir, void *data_addr,
    int comp_id)
{
    int *prev, next, cmp;
    hal_param_t *new, *ptr;
    hal_comp_t *comp;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: param_new called before init\n");
	return -EINVAL;
    }

    if (type != HAL_BIT && type != HAL_FLOAT && type != HAL_S32 && type != HAL_U32) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: pin type not one of HAL_BIT, HAL_FLOAT, HAL_S32 or HAL_U32\n");
	return -EINVAL;
    }

    if(dir != HAL_RO && dir != HAL_RW) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: param direction not one of HAL_RO, or HAL_RW\n");
	return -EINVAL;
    }

    if (strlen(name) > HAL_NAME_LEN) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: parameter name '%s' is too long\n", name);
	return -EINVAL;
    }
    if (hal_data->lock & HAL_LOCK_LOAD)  {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: param_new called while HAL locked\n");
	return -EPERM;
    }

    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: creating parameter '%s'\n", name);
    /* get mutex before accessing shared data */
    rtapi_mutex_get(&(hal_data->mutex));
    /* validate comp_id */
    comp = halpr_find_comp_by_id(comp_id);
    if (comp == 0) {
	/* bad comp_id */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: component %d not found\n", comp_id);
	return -EINVAL;
    }
    /* validate passed in pointer - must point to HAL shmem */
    if (! SHMCHK(data_addr)) {
	/* bad pointer */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: data_addr not in shared memory\n");
	return -EINVAL;
    }
    if(comp->ready) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: param_new called after hal_ready\n");
	return -EINVAL;
    }
    /* allocate a new parameter structure */
    new = alloc_param_struct();
    if (new == 0) {
	/* alloc failed */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: insufficient memory for parameter '%s'\n", name);
	return -ENOMEM;
    }
    /* initialize the structure */
    new->owner_ptr = SHMOFF(comp);
    new->data_ptr = SHMOFF(data_addr);
    new->type = type;
    new->dir = dir;
    rtapi_snprintf(new->name, sizeof(new->name), "%s", name);
    /* search list for 'name' and insert new structure */
    prev = &(hal_data->param_list_ptr);
    next = *prev;
    while (1) {
	if (next == 0) {
	    /* reached end of list, insert here */
	    new->next_ptr = next;
	    *prev = SHMOFF(new);
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	ptr = SHMPTR(next);
	cmp = strcmp(ptr->name, new->name);
	if (cmp > 0) {
	    /* found the right place for it, insert here */
	    new->next_ptr = next;
	    *prev = SHMOFF(new);
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	if (cmp == 0) {
	    /* name already in list, can't insert */
	    free_param_struct(new);
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: duplicate parameter '%s'\n", name);
	    return -EINVAL;
	}
	/* didn't find it yet, look at next one */
	prev = &(ptr->next_ptr);
	next = *prev;
    }
}

/* wrapper functs for typed params - these call the generic funct below */

int hal_param_bit_set(const char *name, int value)
{
    return hal_param_set(name, HAL_BIT, &value);
}

int hal_param_float_set(const char *name, double value)
{
    return hal_param_set(name, HAL_FLOAT, &value);
}

int hal_param_u32_set(const char *name, unsigned long value)
{
    return hal_param_set(name, HAL_U32, &value);
}

int hal_param_s32_set(const char *name, signed long value)
{
    return hal_param_set(name, HAL_S32, &value);
}

/* this is a generic function that does the majority of the work */

int hal_param_set(const char *name, hal_type_t type, void *value_addr)
{
    hal_param_t *param;
    void *d_ptr;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: param_set called before init\n");
	return -EINVAL;
    }
    
    if (hal_data->lock & HAL_LOCK_PARAMS)  {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: param_set called while HAL locked\n");
	return -EPERM;
    }
    
    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: setting parameter '%s'\n", name);
    /* get mutex before accessing shared data */
    rtapi_mutex_get(&(hal_data->mutex));

    /* search param list for name */
    param = halpr_find_param_by_name(name);
    if (param == 0) {
	/* parameter not found */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: parameter '%s' not found\n", name);
	return -EINVAL;
    }
    /* found it, is type compatible? */
    if (param->type != type) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: type mismatch setting param '%s'\n", name);
	return -EINVAL;
    }
    /* is it read only? */
    if (param->dir == HAL_RO) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: param '%s' is not writable\n", name);
	return -EINVAL;
    }
    /* everything is OK, set the value */
    d_ptr = SHMPTR(param->data_ptr);
    switch (param->type) {
    case HAL_BIT:
	if (*((int *) value_addr) == 0) {
	    *(hal_bit_t *) (d_ptr) = 0;
	} else {
	    *(hal_bit_t *) (d_ptr) = 1;
	}
	break;
    case HAL_FLOAT:
	*((hal_float_t *) (d_ptr)) = *((double *) (value_addr));
	break;
    case HAL_S32:
	*((hal_s32_t *) (d_ptr)) = *((signed long *) (value_addr));
	break;
    case HAL_U32:
	*((hal_u32_t *) (d_ptr)) = *((unsigned long *) (value_addr));
	break;
    default:
	/* Shouldn't get here, but just in case... */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: bad type %d setting param\n", param->type);
	return -EINVAL;
    }
    rtapi_mutex_give(&(hal_data->mutex));
    return 0;
}

int hal_param_alias(const char *param_name, const char *alias)
{
    int *prev, next, cmp;
    hal_param_t *param, *ptr;
    hal_oldname_t *oldname;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: param_alias called before init\n");
	return -EINVAL;
    }
    if (hal_data->lock & HAL_LOCK_CONFIG)  {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: param_alias called while HAL locked\n");
	return -EPERM;
    }
    if (alias != NULL ) {
	if (strlen(alias) > HAL_NAME_LEN) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
	        "HAL: ERROR: alias name '%s' is too long\n", alias);
	    return -EINVAL;
	}
    }
    /* get mutex before accessing shared data */
    rtapi_mutex_get(&(hal_data->mutex));
    if (alias != NULL ) {
	param = halpr_find_param_by_name(alias);
	if ( param != NULL ) {
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
	        "HAL: ERROR: duplicate pin/alias name '%s'\n", alias);
	    return -EINVAL;
	}
    }
    /* once we unlink the param from the list, we don't want to have to
       abort the change and repair things.  So we allocate an oldname
       struct here, then free it (which puts it on the free list).  This
       allocation might fail, in which case we abort the command.  But
       if we actually need the struct later, the next alloc is guaranteed
       to succeed since at least one struct is on the free list. */
    oldname = halpr_alloc_oldname_struct();
    if ( oldname == NULL ) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: insufficient memory for param_alias\n");
	return -EINVAL;
    }
    free_oldname_struct(oldname);
    /* find the param and unlink it from pin list */
    prev = &(hal_data->param_list_ptr);
    next = *prev;
    while (1) {
	if (next == 0) {
	    /* reached end of list, not found */
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: param '%s' not found\n", param_name);
	    return -EINVAL;
	}
	param = SHMPTR(next);
	if ( strcmp(param->name, param_name) == 0 ) {
	    /* found it, unlink from list */
	    *prev = param->next_ptr;
	    break;
	}
	if (param->oldname != 0 ) {
	    oldname = SHMPTR(param->oldname);
	    if (strcmp(oldname->name, param_name) == 0) {
		/* found it, unlink from list */
		*prev = param->next_ptr;
		break;
	    }
	}
	/* didn't find it yet, look at next one */
	prev = &(param->next_ptr);
	next = *prev;
    }
    if ( alias != NULL ) {
	/* adding a new alias */
	if ( param->oldname == 0 ) {
	    /* save old name (only if not already saved) */
	    oldname = halpr_alloc_oldname_struct();
	    param->oldname = SHMOFF(oldname);
	    rtapi_snprintf(oldname->name, sizeof(oldname->name), "%s", param->name);
	}
	/* change param's name to 'alias' */
	rtapi_snprintf(param->name, sizeof(param->name), "%s", alias);
    } else {
	/* removing an alias */
	if ( param->oldname != 0 ) {
	    /* restore old name (only if param is aliased) */
	    oldname = SHMPTR(param->oldname);
	    rtapi_snprintf(param->name, sizeof(param->name), "%s", oldname->name);
	    param->oldname = 0;
	    free_oldname_struct(oldname);
	}
    }
    /* insert param back into list in proper place */
    prev = &(hal_data->param_list_ptr);
    next = *prev;
    while (1) {
	if (next == 0) {
	    /* reached end of list, insert here */
	    param->next_ptr = next;
	    *prev = SHMOFF(param);
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	ptr = SHMPTR(next);
	cmp = strcmp(ptr->name, param->name);
	if (cmp > 0) {
	    /* found the right place for it, insert here */
	    param->next_ptr = next;
	    *prev = SHMOFF(param);
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	/* didn't find it yet, look at next one */
	prev = &(ptr->next_ptr);
	next = *prev;
    }
}

/***********************************************************************
*                   EXECUTION RELATED FUNCTIONS                        *
************************************************************************/

#ifdef RTAPI

int hal_export_funct(const char *name, void (*funct) (void *, long),
    void *arg, int uses_fp, int reentrant, int comp_id)
{
    int *prev, next, cmp;
    hal_funct_t *new, *fptr;
    hal_comp_t *comp;
    char buf[HAL_NAME_LEN + 1];

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: export_funct called before init\n");
	return -EINVAL;
    }

    if (strlen(name) > HAL_NAME_LEN) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: function name '%s' is too long\n", name);
	return -EINVAL;
    }
    if (hal_data->lock & HAL_LOCK_LOAD)  {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: export_funct called while HAL locked\n");
	return -EPERM;
    }
    
    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: exporting function '%s'\n", name);
    /* get mutex before accessing shared data */
    rtapi_mutex_get(&(hal_data->mutex));
    /* validate comp_id */
    comp = halpr_find_comp_by_id(comp_id);
    if (comp == 0) {
	/* bad comp_id */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: component %d not found\n", comp_id);
	return -EINVAL;
    }
    if (comp->type == 0) {
	/* not a realtime component */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: component %d is not realtime\n", comp_id);
	return -EINVAL;
    }
    if(comp->ready) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: export_funct called after hal_ready\n");
	return -EINVAL;
    }
    /* allocate a new function structure */
    new = alloc_funct_struct();
    if (new == 0) {
	/* alloc failed */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: insufficient memory for function '%s'\n", name);
	return -ENOMEM;
    }
    /* initialize the structure */
    new->uses_fp = uses_fp;
    new->owner_ptr = SHMOFF(comp);
    new->reentrant = reentrant;
    new->users = 0;
    new->arg = arg;
    new->funct = funct;
    rtapi_snprintf(new->name, sizeof(new->name), "%s", name);
    /* search list for 'name' and insert new structure */
    prev = &(hal_data->funct_list_ptr);
    next = *prev;
    while (1) {
	if (next == 0) {
	    /* reached end of list, insert here */
	    new->next_ptr = next;
	    *prev = SHMOFF(new);
	    /* break out of loop and init the new function */
	    break;
	}
	fptr = SHMPTR(next);
	cmp = strcmp(fptr->name, new->name);
	if (cmp > 0) {
	    /* found the right place for it, insert here */
	    new->next_ptr = next;
	    *prev = SHMOFF(new);
	    /* break out of loop and init the new function */
	    break;
	}
	if (cmp == 0) {
	    /* name already in list, can't insert */
	    free_funct_struct(new);
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: duplicate function '%s'\n", name);
	    return -EINVAL;
	}
	/* didn't find it yet, look at next one */
	prev = &(fptr->next_ptr);
	next = *prev;
    }
    /* at this point we have a new function and can yield the mutex */
    rtapi_mutex_give(&(hal_data->mutex));

    /* create a pin with the function's runtime in it */
    if (hal_pin_s32_newf(HAL_OUT, &(new->runtime), comp_id,"%s.time",name)) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	   "HAL: ERROR: fail to create pin '%s.time'\n", name);
	return -EINVAL;
    }
    *(new->runtime) = 0;

    /* note that failure to successfully create the following params
       does not cause the "export_funct()" call to fail - they are
       for debugging and testing use only */
    /* create a parameter with the function's maximum runtime in it */
    rtapi_snprintf(buf, sizeof(buf), "%s.tmax", name);
    new->maxtime = 0;
    hal_param_s32_new(buf, HAL_RW, &(new->maxtime), comp_id);

    /* create a parameter with the function's maximum runtime in it */
    rtapi_snprintf(buf, sizeof(buf), "%s.tmax-increased", name);
    new->maxtime_increased = 0;
    hal_param_bit_new(buf, HAL_RO, &(new->maxtime_increased), comp_id);

    return 0;
}

int hal_create_thread(const char *name, unsigned long period_nsec, int uses_fp)
{
    int next, cmp, prev_priority;
    int retval, n;
    hal_thread_t *new, *tptr;
    long prev_period, curr_period;
    char buf[HAL_NAME_LEN + 1];

    rtapi_print_msg(RTAPI_MSG_DBG,
	"HAL: creating thread %s, %ld nsec\n", name, period_nsec);
    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: create_thread called before init\n");
	return -EINVAL;
    }
    if (period_nsec == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: create_thread called with period of zero\n");
	return -EINVAL;
    }

    if (strlen(name) > HAL_NAME_LEN) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: thread name '%s' is too long\n", name);
	return -EINVAL;
    }
    if (hal_data->lock & HAL_LOCK_CONFIG) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: create_thread called while HAL is locked\n");
	return -EPERM;
    }

    /* get mutex before accessing shared data */
    rtapi_mutex_get(&(hal_data->mutex));
    /* make sure name is unique on thread list */
    next = hal_data->thread_list_ptr;
    while (next != 0) {
	tptr = SHMPTR(next);
	cmp = strcmp(tptr->name, name);
	if (cmp == 0) {
	    /* name already in list, can't insert */
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: duplicate thread name %s\n", name);
	    return -EINVAL;
	}
	/* didn't find it yet, look at next one */
	next = tptr->next_ptr;
    }
    /* allocate a new thread structure */
    new = alloc_thread_struct();
    if (new == 0) {
	/* alloc failed */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: insufficient memory to create thread\n");
	return -ENOMEM;
    }
    /* initialize the structure */
    new->uses_fp = uses_fp;
    rtapi_snprintf(new->name, sizeof(new->name), "%s", name);
    /* have to create and start a task to run the thread */
    if (hal_data->thread_list_ptr == 0) {
	/* this is the first thread created */
	/* is timer started? if so, what period? */
	curr_period = rtapi_clock_set_period(0);
	if (curr_period == 0) {
	    /* not running, start it */
	    curr_period = rtapi_clock_set_period(period_nsec);
	    if (curr_period < 0) {
		rtapi_mutex_give(&(hal_data->mutex));
		rtapi_print_msg(RTAPI_MSG_ERR,
		    "HAL_LIB: ERROR: clock_set_period returned %ld\n",
		    curr_period);
		return -EINVAL;
	    }
	}
	/* make sure period <= desired period (allow 1% roundoff error) */
	if (curr_period > (period_nsec + (period_nsec / 100))) {
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL_LIB: ERROR: clock period too long: %ld\n", curr_period);
	    return -EINVAL;
	}
	if(hal_data->exact_base_period) {
		hal_data->base_period = period_nsec;
	} else {
		hal_data->base_period = curr_period;
	}
	/* reserve the highest priority (maybe for a watchdog?) */
	prev_priority = rtapi_prio_highest();
	/* no previous period to worry about */
	prev_period = 0;
    } else {
	/* there are other threads, slowest (and lowest
	   priority) is at head of list */
	tptr = SHMPTR(hal_data->thread_list_ptr);
	prev_period = tptr->period;
	prev_priority = tptr->priority;
    }
    if ( period_nsec < hal_data->base_period) { 
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL_LIB: ERROR: new thread period %ld is less than clock period %ld\n",
	     period_nsec, hal_data->base_period);
	return -EINVAL;
    }
    /* make period an integer multiple of the timer period */
    n = (period_nsec + hal_data->base_period / 2) / hal_data->base_period;
    new->period = hal_data->base_period * n;
    if ( new->period < prev_period ) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL_LIB: ERROR: new thread period %ld is less than existing thread period %ld\n",
	     period_nsec, prev_period);
	return -EINVAL;
    }
    /* make priority one lower than previous */
    new->priority = rtapi_prio_next_lower(prev_priority);
    /* create task - owned by library module, not caller */
    retval = rtapi_task_new(thread_task, new, new->priority,
	lib_module_id, HAL_STACKSIZE, uses_fp);
    if (retval < 0) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL_LIB: could not create task for thread %s\n", name);
	return -EINVAL;
    }
    new->task_id = retval;
    /* start task */
    retval = rtapi_task_start(new->task_id, new->period);
    if (retval < 0) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL_LIB: could not start task for thread %s: %d\n", name, retval);
	return -EINVAL;
    }
    /* insert new structure at head of list */
    new->next_ptr = hal_data->thread_list_ptr;
    hal_data->thread_list_ptr = SHMOFF(new);
    /* done, release mutex */
    rtapi_mutex_give(&(hal_data->mutex));

    rtapi_snprintf(buf,sizeof(buf), HAL_PSEUDO_COMP_PREFIX"%s",new->name); // pseudo prefix
    new->comp_id = hal_init(buf);
    if (new->comp_id < 0) {
        rtapi_print_msg(RTAPI_MSG_ERR,
           "HAL: ERROR: fail to create pseudo comp for thread: '%s'\n", new->name);
        return -EINVAL;
    }

    rtapi_snprintf(buf, sizeof(buf), "%s.tmax", new->name);
    new->maxtime = 0;
    if (hal_param_s32_new(buf, HAL_RW, &(new->maxtime), new->comp_id)) {
        rtapi_print_msg(RTAPI_MSG_ERR,
           "HAL: ERROR: fail to create param '%s.tmax'\n", new->name);
        return -EINVAL;
    }

    if (hal_pin_s32_newf(HAL_OUT, &(new->runtime), new->comp_id,"%s.time",new->name)) {
        rtapi_print_msg(RTAPI_MSG_ERR,
           "HAL: ERROR: fail to create pin '%s.time'\n", new->name);
        return -EINVAL;
    }
    *(new->runtime) = 0;
    hal_ready(new->comp_id);

    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: thread created\n");
    return 0;
}

extern int hal_thread_delete(const char *name)
{
    hal_thread_t *thread;
    int *prev, next;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: thread_delete called before init\n");
	return -EINVAL;
    }

    if (hal_data->lock & HAL_LOCK_CONFIG) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: thread_delete called while HAL is locked\n");
	return -EPERM;
    }
    
    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: deleting thread '%s'\n", name);
    /* get mutex before accessing shared data */
    rtapi_mutex_get(&(hal_data->mutex));
    /* search for the signal */
    prev = &(hal_data->thread_list_ptr);
    next = *prev;
    while (next != 0) {
	thread = SHMPTR(next);
	if (strcmp(thread->name, name) == 0) {
	    /* this is the right thread, unlink from list */
	    if (thread->comp_id != 0) {
	        hal_exit(thread->comp_id);
	        thread->comp_id = 0;
	    }
	    *prev = thread->next_ptr;
	    /* and delete it */
	    free_thread_struct(thread);
	    /* done */
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	/* no match, try the next one */
	prev = &(thread->next_ptr);
	next = *prev;
    }
    /* if we get here, we didn't find a match */
    rtapi_mutex_give(&(hal_data->mutex));
    rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: thread '%s' not found\n",
	name);
    return -EINVAL;
}

#endif /* RTAPI */

int hal_add_funct_to_thread(const char *funct_name, const char *thread_name, int position)
{
    hal_thread_t *thread;
    hal_funct_t *funct;
    hal_list_t *list_root, *list_entry;
    int n;
    hal_funct_entry_t *funct_entry;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: add_funct called before init\n");
	return -EINVAL;
    }

    if (hal_data->lock & HAL_LOCK_CONFIG) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: add_funct_to_thread called while HAL is locked\n");
	return -EPERM;
    }

    rtapi_print_msg(RTAPI_MSG_DBG,
	"HAL: adding function '%s' to thread '%s'\n",
	funct_name, thread_name);
    /* get mutex before accessing data structures */
    rtapi_mutex_get(&(hal_data->mutex));
    /* make sure position is valid */
    if (position == 0) {
	/* zero is not allowed */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: bad position: 0\n");
	return -EINVAL;
    }
    /* make sure we were given a function name */
    if (funct_name == 0) {
	/* no name supplied */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: missing function name\n");
	return -EINVAL;
    }
    /* make sure we were given a thread name */
    if (thread_name == 0) {
	/* no name supplied */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: missing thread name\n");
	return -EINVAL;
    }
    /* search function list for the function */
    funct = halpr_find_funct_by_name(funct_name);
    if (funct == 0) {
	/* function not found */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: function '%s' not found\n", funct_name);
	return -EINVAL;
    }
    /* found the function, is it available? */
    if ((funct->users > 0) && (funct->reentrant == 0)) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: function '%s' may only be added to one thread\n", funct_name);
	return -EINVAL;
    }
    /* search thread list for thread_name */
    thread = halpr_find_thread_by_name(thread_name);
    if (thread == 0) {
	/* thread not found */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: thread '%s' not found\n", thread_name);
	return -EINVAL;
    }
    /* ok, we have thread and function, are they compatible? */
    if ((funct->uses_fp) && (!thread->uses_fp)) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: function '%s' needs FP\n", funct_name);
	return -EINVAL;
    }
    /* find insertion point */
    list_root = &(thread->funct_list);
    list_entry = list_root;
    n = 0;
    if (position > 0) {
	/* insertion is relative to start of list */
	while (++n < position) {
	    /* move further into list */
	    list_entry = list_next(list_entry);
	    if (list_entry == list_root) {
		/* reached end of list */
		rtapi_mutex_give(&(hal_data->mutex));
		rtapi_print_msg(RTAPI_MSG_ERR,
		    "HAL: ERROR: position '%d' is too high\n", position);
		return -EINVAL;
	    }
	}
    } else {
	/* insertion is relative to end of list */
	while (--n > position) {
	    /* move further into list */
	    list_entry = list_prev(list_entry);
	    if (list_entry == list_root) {
		/* reached end of list */
		rtapi_mutex_give(&(hal_data->mutex));
		rtapi_print_msg(RTAPI_MSG_ERR,
		    "HAL: ERROR: position '%d' is too low\n", position);
		return -EINVAL;
	    }
	}
	/* want to insert before list_entry, so back up one more step */
	list_entry = list_prev(list_entry);
    }
    /* allocate a funct entry structure */
    funct_entry = alloc_funct_entry_struct();
    if (funct_entry == 0) {
	/* alloc failed */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: insufficient memory for thread->function link\n");
	return -ENOMEM;
    }
    /* init struct contents */
    funct_entry->funct_ptr = SHMOFF(funct);
    funct_entry->arg = funct->arg;
    funct_entry->funct = funct->funct;
    /* add the entry to the list */
    list_add_after((hal_list_t *) funct_entry, list_entry);
    /* update the function usage count */
    funct->users++;
    rtapi_mutex_give(&(hal_data->mutex));
    return 0;
}

int hal_del_funct_from_thread(const char *funct_name, const char *thread_name)
{
    hal_thread_t *thread;
    hal_funct_t *funct;
    hal_list_t *list_root, *list_entry;
    hal_funct_entry_t *funct_entry;

    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: del_funct called before init\n");
	return -EINVAL;
    }

    if (hal_data->lock & HAL_LOCK_CONFIG) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: del_funct_from_thread called while HAL is locked\n");
	return -EPERM;
    }

    rtapi_print_msg(RTAPI_MSG_DBG,
	"HAL: removing function '%s' from thread '%s'\n",
	funct_name, thread_name);
    /* get mutex before accessing data structures */
    rtapi_mutex_get(&(hal_data->mutex));
    /* make sure we were given a function name */
    if (funct_name == 0) {
	/* no name supplied */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: missing function name\n");
	return -EINVAL;
    }
    /* make sure we were given a thread name */
    if (thread_name == 0) {
	/* no name supplied */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR, "HAL: ERROR: missing thread name\n");
	return -EINVAL;
    }
    /* search function list for the function */
    funct = halpr_find_funct_by_name(funct_name);
    if (funct == 0) {
	/* function not found */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: function '%s' not found\n", funct_name);
	return -EINVAL;
    }
    /* found the function, is it in use? */
    if (funct->users == 0) {
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: function '%s' is not in use\n", funct_name);
	return -EINVAL;
    }
    /* search thread list for thread_name */
    thread = halpr_find_thread_by_name(thread_name);
    if (thread == 0) {
	/* thread not found */
	rtapi_mutex_give(&(hal_data->mutex));
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: thread '%s' not found\n", thread_name);
	return -EINVAL;
    }
    /* ok, we have thread and function, does thread use funct? */
    list_root = &(thread->funct_list);
    list_entry = list_next(list_root);
    while (1) {
	if (list_entry == list_root) {
	    /* reached end of list, funct not found */
	    rtapi_mutex_give(&(hal_data->mutex));
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: thread '%s' doesn't use %s\n", thread_name,
		funct_name);
	    return -EINVAL;
	}
	funct_entry = (hal_funct_entry_t *) list_entry;
	if (SHMPTR(funct_entry->funct_ptr) == funct) {
	    /* this funct entry points to our funct, unlink */
	    list_remove_entry(list_entry);
	    /* and delete it */
	    free_funct_entry_struct(funct_entry);
	    /* done */
	    rtapi_mutex_give(&(hal_data->mutex));
	    return 0;
	}
	/* try next one */
	list_entry = list_next(list_entry);
    }
}

int hal_start_threads(void)
{
    /* a trivial function for a change! */
    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: start_threads called before init\n");
	return -EINVAL;
    }

    if (hal_data->lock & HAL_LOCK_RUN) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: start_threads called while HAL is locked\n");
	return -EPERM;
    }


    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: starting threads\n");
    hal_data->threads_running = 1;
    return 0;
}

int hal_stop_threads(void)
{
    /* wow, two in a row! */
    if (hal_data == 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: stop_threads called before init\n");
	return -EINVAL;
    }

    if (hal_data->lock & HAL_LOCK_RUN) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL: ERROR: stop_threads called while HAL is locked\n");
	return -EPERM;
    }

    hal_data->threads_running = 0;
    rtapi_print_msg(RTAPI_MSG_DBG, "HAL: threads stopped\n");
    return 0;
}

/***********************************************************************
*                    PRIVATE FUNCTION CODE                             *
************************************************************************/

hal_list_t *list_prev(hal_list_t * entry)
{
    /* this function is only needed because of memory mapping */
    return SHMPTR(entry->prev);
}

hal_list_t *list_next(hal_list_t * entry)
{
    /* this function is only needed because of memory mapping */
    return SHMPTR(entry->next);
}

void list_init_entry(hal_list_t * entry)
{
    int entry_n;

    entry_n = SHMOFF(entry);
    entry->next = entry_n;
    entry->prev = entry_n;
}

void list_add_after(hal_list_t * entry, hal_list_t * prev)
{
    int entry_n, prev_n, next_n;
    hal_list_t *next;

    /* messiness needed because of memory mapping */
    entry_n = SHMOFF(entry);
    prev_n = SHMOFF(prev);
    next_n = prev->next;
    next = SHMPTR(next_n);
    /* insert the entry */
    entry->next = next_n;
    entry->prev = prev_n;
    prev->next = entry_n;
    next->prev = entry_n;
}

void list_add_before(hal_list_t * entry, hal_list_t * next)
{
    int entry_n, prev_n, next_n;
    hal_list_t *prev;

    /* messiness needed because of memory mapping */
    entry_n = SHMOFF(entry);
    next_n = SHMOFF(next);
    prev_n = next->prev;
    prev = SHMPTR(prev_n);
    /* insert the entry */
    entry->next = next_n;
    entry->prev = prev_n;
    prev->next = entry_n;
    next->prev = entry_n;
}

hal_list_t *list_remove_entry(hal_list_t * entry)
{
    int entry_n;
    hal_list_t *prev, *next;

    /* messiness needed because of memory mapping */
    entry_n = SHMOFF(entry);
    prev = SHMPTR(entry->prev);
    next = SHMPTR(entry->next);
    /* remove the entry */
    prev->next = entry->next;
    next->prev = entry->prev;
    entry->next = entry_n;
    entry->prev = entry_n;
    return next;
}

hal_comp_t *halpr_find_comp_by_name(const char *name)
{
    int next;
    hal_comp_t *comp;

    /* search component list for 'name' */
    next = hal_data->comp_list_ptr;
    while (next != 0) {
	comp = SHMPTR(next);
	if (strcmp(comp->name, name) == 0) {
	    /* found a match */
	    return comp;
	}
	/* didn't find it yet, look at next one */
	next = comp->next_ptr;
    }
    /* if loop terminates, we reached end of list with no match */
    return 0;
}

hal_pin_t *halpr_find_pin_by_name(const char *name)
{
    int next;
    hal_pin_t *pin;
    hal_oldname_t *oldname;

    /* search pin list for 'name' */
    next = hal_data->pin_list_ptr;
    while (next != 0) {
	pin = SHMPTR(next);
	if (strcmp(pin->name, name) == 0) {
	    /* found a match */
	    return pin;
	}
	if (pin->oldname != 0 ) {
	    oldname = SHMPTR(pin->oldname);
	    if (strcmp(oldname->name, name) == 0) {
		/* found a match */
		return pin;
	    }
	}
	/* didn't find it yet, look at next one */
	next = pin->next_ptr;
    }
    /* if loop terminates, we reached end of list with no match */
    return 0;
}

hal_sig_t *halpr_find_sig_by_name(const char *name)
{
    int next;
    hal_sig_t *sig;

    /* search signal list for 'name' */
    next = hal_data->sig_list_ptr;
    while (next != 0) {
	sig = SHMPTR(next);
	if (strcmp(sig->name, name) == 0) {
	    /* found a match */
	    return sig;
	}
	/* didn't find it yet, look at next one */
	next = sig->next_ptr;
    }
    /* if loop terminates, we reached end of list with no match */
    return 0;
}

hal_param_t *halpr_find_param_by_name(const char *name)
{
    int next;
    hal_param_t *param;
    hal_oldname_t *oldname;

    /* search parameter list for 'name' */
    next = hal_data->param_list_ptr;
    while (next != 0) {
	param = SHMPTR(next);
	if (strcmp(param->name, name) == 0) {
	    /* found a match */
	    return param;
	}
	if (param->oldname != 0 ) {
	    oldname = SHMPTR(param->oldname);
	    if (strcmp(oldname->name, name) == 0) {
		/* found a match */
		return param;
	    }
	}
	/* didn't find it yet, look at next one */
	next = param->next_ptr;
    }
    /* if loop terminates, we reached end of list with no match */
    return 0;
}

hal_thread_t *halpr_find_thread_by_name(const char *name)
{
    int next;
    hal_thread_t *thread;

    /* search thread list for 'name' */
    next = hal_data->thread_list_ptr;
    while (next != 0) {
	thread = SHMPTR(next);
	if (strcmp(thread->name, name) == 0) {
	    /* found a match */
	    return thread;
	}
	/* didn't find it yet, look at next one */
	next = thread->next_ptr;
    }
    /* if loop terminates, we reached end of list with no match */
    return 0;
}

hal_funct_t *halpr_find_funct_by_name(const char *name)
{
    int next;
    hal_funct_t *funct;

    /* search function list for 'name' */
    next = hal_data->funct_list_ptr;
    while (next != 0) {
	funct = SHMPTR(next);
	if (strcmp(funct->name, name) == 0) {
	    /* found a match */
	    return funct;
	}
	/* didn't find it yet, look at next one */
	next = funct->next_ptr;
    }
    /* if loop terminates, we reached end of list with no match */
    return 0;
}

hal_comp_t *halpr_find_comp_by_id(int id)
{
    int next;
    hal_comp_t *comp;

    /* search list for 'comp_id' */
    next = hal_data->comp_list_ptr;
    while (next != 0) {
	comp = SHMPTR(next);
	if (comp->comp_id == id) {
	    /* found a match */
	    return comp;
	}
	/* didn't find it yet, look at next one */
	next = comp->next_ptr;
    }
    /* if loop terminates, we reached end of list without finding a match */
    return 0;
}

hal_pin_t *halpr_find_pin_by_owner(hal_comp_t * owner, hal_pin_t * start)
{
    int owner_ptr, next;
    hal_pin_t *pin;

    /* get offset of 'owner' component */
    owner_ptr = SHMOFF(owner);
    /* is this the first call? */
    if (start == 0) {
	/* yes, start at beginning of pin list */
	next = hal_data->pin_list_ptr;
    } else {
	/* no, start at next pin */
	next = start->next_ptr;
    }
    while (next != 0) {
	pin = SHMPTR(next);
	if (pin->owner_ptr == owner_ptr) {
	    /* found a match */
	    return pin;
	}
	/* didn't find it yet, look at next one */
	next = pin->next_ptr;
    }
    /* if loop terminates, we reached end of list without finding a match */
    return 0;
}

hal_param_t *halpr_find_param_by_owner(hal_comp_t * owner,
    hal_param_t * start)
{
    int owner_ptr, next;
    hal_param_t *param;

    /* get offset of 'owner' component */
    owner_ptr = SHMOFF(owner);
    /* is this the first call? */
    if (start == 0) {
	/* yes, start at beginning of param list */
	next = hal_data->param_list_ptr;
    } else {
	/* no, start at next param */
	next = start->next_ptr;
    }
    while (next != 0) {
	param = SHMPTR(next);
	if (param->owner_ptr == owner_ptr) {
	    /* found a match */
	    return param;
	}
	/* didn't find it yet, look at next one */
	next = param->next_ptr;
    }
    /* if loop terminates, we reached end of list without finding a match */
    return 0;
}

hal_funct_t *halpr_find_funct_by_owner(hal_comp_t * owner,
    hal_funct_t * start)
{
    int owner_ptr, next;
    hal_funct_t *funct;

    /* get offset of 'owner' component */
    owner_ptr = SHMOFF(owner);
    /* is this the first call? */
    if (start == 0) {
	/* yes, start at beginning of function list */
	next = hal_data->funct_list_ptr;
    } else {
	/* no, start at next function */
	next = start->next_ptr;
    }
    while (next != 0) {
	funct = SHMPTR(next);
	if (funct->owner_ptr == owner_ptr) {
	    /* found a match */
	    return funct;
	}
	/* didn't find it yet, look at next one */
	next = funct->next_ptr;
    }
    /* if loop terminates, we reached end of list without finding a match */
    return 0;
}

hal_pin_t *halpr_find_pin_by_sig(hal_sig_t * sig, hal_pin_t * start)
{
    int sig_ptr, next;
    hal_pin_t *pin;

    /* get offset of 'sig' component */
    sig_ptr = SHMOFF(sig);
    /* is this the first call? */
    if (start == 0) {
	/* yes, start at beginning of pin list */
	next = hal_data->pin_list_ptr;
    } else {
	/* no, start at next pin */
	next = start->next_ptr;
    }
    while (next != 0) {
	pin = SHMPTR(next);
	if (pin->signal == sig_ptr) {
	    /* found a match */
	    return pin;
	}
	/* didn't find it yet, look at next one */
	next = pin->next_ptr;
    }
    /* if loop terminates, we reached end of list without finding a match */
    return 0;
}

/***********************************************************************
*                     LOCAL FUNCTION CODE                              *
************************************************************************/

#ifdef RTAPI
/* these functions are called when the hal_lib module is insmod'ed
   or rmmod'ed.
*/

#if defined(__KERNEL__) && defined( CONFIG_PROC_FS ) && LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
#include <linux/proc_fs.h>
extern struct proc_dir_entry *rtapi_dir;
static struct proc_dir_entry *hal_dir = 0;
static struct proc_dir_entry *hal_newinst_file = 0;

static int proc_write_newinst(struct file *file,
        const char *buffer, unsigned long count, void *data)
{
    if(hal_data->pending_constructor) {
        rtapi_print_msg(RTAPI_MSG_DBG,
                "HAL: running constructor for %s %s\n",
                hal_data->constructor_prefix,
                hal_data->constructor_arg);
        hal_data->pending_constructor(hal_data->constructor_prefix,
                hal_data->constructor_arg);
        hal_data->pending_constructor = 0;
    }
    return count;
}

static void hal_proc_clean(void) {
    if(hal_newinst_file)
        remove_proc_entry("newinst", hal_dir);
    if(hal_dir)
        remove_proc_entry("hal", rtapi_dir);
    hal_newinst_file = hal_dir = 0;
}
static int hal_proc_init(void) {
    if(!rtapi_dir) return 0;
    hal_dir = create_proc_entry("hal", S_IFDIR, rtapi_dir);
    if(!hal_dir) { hal_proc_clean(); return -1; }
    hal_newinst_file = create_proc_entry("newinst", 0666, hal_dir);
    if(!hal_newinst_file) { hal_proc_clean(); return -1; }
    hal_newinst_file->data = NULL;
    hal_newinst_file->read_proc = NULL;
    hal_newinst_file->write_proc = proc_write_newinst;
    return 0;
}
#else
static int hal_proc_clean(void) { return 0; }
static int hal_proc_init(void) { return 0; }
#endif

int rtapi_app_main(void)
{
    int retval;
    void *mem;

    rtapi_print_msg(RTAPI_MSG_DBG, "HAL_LIB: loading kernel lib\n");
    /* do RTAPI init */
    lib_module_id = rtapi_init("HAL_LIB");
    if (lib_module_id < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR, "HAL_LIB: ERROR: rtapi init failed\n");
	return -EINVAL;
    }
    /* get HAL shared memory block from RTAPI */
    lib_mem_id = rtapi_shmem_new(HAL_KEY, lib_module_id, HAL_SIZE);
    if (lib_mem_id < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL_LIB: ERROR: could not open shared memory\n");
	rtapi_exit(lib_module_id);
	return -EINVAL;
    }
    /* get address of shared memory area */
    retval = rtapi_shmem_getptr(lib_mem_id, &mem);
    if (retval < 0) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL_LIB: ERROR: could not access shared memory\n");
	rtapi_exit(lib_module_id);
	return -EINVAL;
    }
    /* set up internal pointers to shared mem and data structure */
    hal_shmem_base = (char *) mem;
    hal_data = (hal_data_t *) mem;
    /* perform a global init if needed */
    retval = init_hal_data();
    if ( retval ) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL_LIB: ERROR: could not init shared memory\n");
	rtapi_exit(lib_module_id);
	return -EINVAL;
    }
    retval = hal_proc_init();
    if ( retval ) {
	rtapi_print_msg(RTAPI_MSG_ERR,
	    "HAL_LIB: ERROR: could not init /proc files\n");
	rtapi_exit(lib_module_id);
	return -EINVAL;
    }
    /* done */
    rtapi_print_msg(RTAPI_MSG_DBG,
	"HAL_LIB: kernel lib installed successfully\n");
    return 0;
}

void rtapi_app_exit(void)
{
    hal_thread_t *thread;

    rtapi_print_msg(RTAPI_MSG_DBG, "HAL_LIB: removing kernel lib\n");
    hal_proc_clean();
    /* grab mutex before manipulating list */
    rtapi_mutex_get(&(hal_data->mutex));
    /* must remove all threads before unloading this module */
    while (hal_data->thread_list_ptr != 0) {
	/* point to a thread */
	thread = SHMPTR(hal_data->thread_list_ptr);
	/* unlink from list */
	hal_data->thread_list_ptr = thread->next_ptr;
	/* and delete it */
	free_thread_struct(thread);
    }
    /* release mutex */
    rtapi_mutex_give(&(hal_data->mutex));
    /* release RTAPI resources */
    rtapi_shmem_delete(lib_mem_id, lib_module_id);
    rtapi_exit(lib_module_id);
    /* done */
    rtapi_print_msg(RTAPI_MSG_DBG,
	"HAL_LIB: kernel lib removed successfully\n");
}

/* this is the task function that implements threads in realtime */

static void thread_task(void *arg)
{
    hal_thread_t *thread;
    hal_funct_t *funct;
    hal_funct_entry_t *funct_root, *funct_entry;
    long long int start_time, end_time;
    long long int thread_start_time;

    thread = arg;
    while (1) {
	if (hal_data->threads_running > 0) {
	    /* point at first function on function list */
	    funct_root = (hal_funct_entry_t *) & (thread->funct_list);
	    funct_entry = SHMPTR(funct_root->links.next);
	    /* execution time logging */
	    start_time = rtapi_get_clocks();
	    end_time = start_time;
	    thread_start_time = start_time;
	    /* run thru function list */
	    while (funct_entry != funct_root) {
		/* call the function */
		funct_entry->funct(funct_entry->arg, thread->period);
		/* capture execution time */
		end_time = rtapi_get_clocks();
		/* point to function structure */
		funct = SHMPTR(funct_entry->funct_ptr);
		/* update execution time data */
		*(funct->runtime) = (hal_s32_t)(end_time - start_time);
		if ( *(funct->runtime) > funct->maxtime) {
		    funct->maxtime = *(funct->runtime);
		    funct->maxtime_increased = 1;
		} else {
		    funct->maxtime_increased = 0;
		}
		/* point to next next entry in list */
		funct_entry = SHMPTR(funct_entry->links.next);
		/* prepare to measure time for next funct */
		start_time = end_time;
	    }
	    /* update thread execution time */
	    *(thread->runtime) = (hal_s32_t)(end_time - thread_start_time);
	    if ( *(thread->runtime) > thread->maxtime) {
	        thread->maxtime = *(thread->runtime);
	    }
	}
	/* wait until next period */
	rtapi_wait();
    }
}
#endif /* RTAPI */

/* see the declarations of these functions (near top of file) for
   a description of what they do.
*/

static int init_hal_data(void)
{
    /* has the block already been initialized? */
    if (hal_data->version != 0) {
	/* yes, verify version code */
	if (hal_data->version == HAL_VER) {
	    return 0;
	} else {
	    rtapi_print_msg(RTAPI_MSG_ERR,
		"HAL: ERROR: version code mismatch\n");
	    return -1;
	}
    }
    /* no, we need to init it, grab the mutex unconditionally */
    rtapi_mutex_try(&(hal_data->mutex));
    /* set version code so nobody else init's the block */
    hal_data->version = HAL_VER;
    /* initialize everything */
    hal_data->comp_list_ptr = 0;
    hal_data->pin_list_ptr = 0;
    hal_data->sig_list_ptr = 0;
    hal_data->param_list_ptr = 0;
    hal_data->funct_list_ptr = 0;
    hal_data->thread_list_ptr = 0;
    hal_data->base_period = 0;
    hal_data->threads_running = 0;
    hal_data->oldname_free_ptr = 0;
    hal_data->comp_free_ptr = 0;
    hal_data->pin_free_ptr = 0;
    hal_data->sig_free_ptr = 0;
    hal_data->param_free_ptr = 0;
    hal_data->funct_free_ptr = 0;
    hal_data->pending_constructor = 0;
    hal_data->constructor_prefix[0] = 0;
    list_init_entry(&(hal_data->funct_entry_free));
    hal_data->thread_free_ptr = 0;
    hal_data->exact_base_period = 0;
    /* set up for shmalloc_xx() */
    hal_data->shmem_bot = sizeof(hal_data_t);
    hal_data->shmem_top = HAL_SIZE;
    hal_data->lock = HAL_LOCK_NONE;
    /* done, release mutex */
    rtapi_mutex_give(&(hal_data->mutex));
    return 0;
}

static void *shmalloc_up(long int size)
{
    long int tmp_bot;
    void *retval;

    /* deal with alignment requirements */
    tmp_bot = hal_data->shmem_bot;
    if (size >= 8) {
	/* align on 8 byte boundary */
	tmp_bot = (tmp_bot + 7) & (~7);
    } else if (size >= 4) {
	/* align on 4 byte boundary */
	tmp_bot = (tmp_bot + 3) & (~3);
    } else if (size == 2) {
	/* align on 2 byte boundary */
	tmp_bot = (tmp_bot + 1) & (~1);
    }
    /* is there enough memory available? */
    if ((hal_data->shmem_top - tmp_bot) < size) {
	/* no */
	return 0;
    }
    /* memory is available, allocate it */
    retval = SHMPTR(tmp_bot);
    hal_data->shmem_bot = tmp_bot + size;
    hal_data->shmem_avail = hal_data->shmem_top - hal_data->shmem_bot;
    return retval;
}

static void *shmalloc_dn(long int size)
{
    long int tmp_top;
    void *retval;

    /* tentatively allocate memory */
    tmp_top = hal_data->shmem_top - size;
    /* deal with alignment requirements */
    if (size >= 8) {
	/* align on 8 byte boundary */
	tmp_top &= (~7);
    } else if (size >= 4) {
	/* align on 4 byte boundary */
	tmp_top &= (~3);
    } else if (size == 2) {
	/* align on 2 byte boundary */
	tmp_top &= (~1);
    }
    /* is there enough memory available? */
    if (tmp_top < hal_data->shmem_bot) {
	/* no */
	return 0;
    }
    /* memory is available, allocate it */
    retval = SHMPTR(tmp_top);
    hal_data->shmem_top = tmp_top;
    hal_data->shmem_avail = hal_data->shmem_top - hal_data->shmem_bot;
    return retval;
}

hal_comp_t *halpr_alloc_comp_struct(void)
{
    hal_comp_t *p;

    /* check the free list */
    if (hal_data->comp_free_ptr != 0) {
	/* found a free structure, point to it */
	p = SHMPTR(hal_data->comp_free_ptr);
	/* unlink it from the free list */
	hal_data->comp_free_ptr = p->next_ptr;
	p->next_ptr = 0;
    } else {
	/* nothing on free list, allocate a brand new one */
	p = shmalloc_dn(sizeof(hal_comp_t));
    }
    if (p) {
	/* make sure it's empty */
	p->next_ptr = 0;
	p->comp_id = 0;
	p->mem_id = 0;
	p->type = 0;
	p->shmem_base = 0;
	p->name[0] = '\0';
    }
    return p;
}

static hal_pin_t *alloc_pin_struct(void)
{
    hal_pin_t *p;

    /* check the free list */
    if (hal_data->pin_free_ptr != 0) {
	/* found a free structure, point to it */
	p = SHMPTR(hal_data->pin_free_ptr);
	/* unlink it from the free list */
	hal_data->pin_free_ptr = p->next_ptr;
	p->next_ptr = 0;
    } else {
	/* nothing on free list, allocate a brand new one */
	p = shmalloc_dn(sizeof(hal_pin_t));
    }
    if (p) {
	/* make sure it's empty */
	p->next_ptr = 0;
	p->data_ptr_addr = 0;
	p->owner_ptr = 0;
	p->type = 0;
	p->dir = 0;
	p->signal = 0;
	memset(&p->dummysig, 0, sizeof(hal_data_u));
	p->name[0] = '\0';
    }
    return p;
}

static hal_sig_t *alloc_sig_struct(void)
{
    hal_sig_t *p;

    /* check the free list */
    if (hal_data->sig_free_ptr != 0) {
	/* found a free structure, point to it */
	p = SHMPTR(hal_data->sig_free_ptr);
	/* unlink it from the free list */
	hal_data->sig_free_ptr = p->next_ptr;
	p->next_ptr = 0;
    } else {
	/* nothing on free list, allocate a brand new one */
	p = shmalloc_dn(sizeof(hal_sig_t));
    }
    if (p) {
	/* make sure it's empty */
	p->next_ptr = 0;
	p->data_ptr = 0;
	p->type = 0;
	p->readers = 0;
	p->writers = 0;
	p->bidirs = 0;
	p->name[0] = '\0';
    }
    return p;
}

static hal_param_t *alloc_param_struct(void)
{
    hal_param_t *p;

    /* check the free list */
    if (hal_data->param_free_ptr != 0) {
	/* found a free structure, point to it */
	p = SHMPTR(hal_data->param_free_ptr);
	/* unlink it from the free list */
	hal_data->param_free_ptr = p->next_ptr;
	p->next_ptr = 0;
    } else {
	/* nothing on free list, allocate a brand new one */
	p = shmalloc_dn(sizeof(hal_param_t));
    }
    if (p) {
	/* make sure it's empty */
	p->next_ptr = 0;
	p->data_ptr = 0;
	p->owner_ptr = 0;
	p->type = 0;
	p->name[0] = '\0';
    }
    return p;
}

static hal_oldname_t *halpr_alloc_oldname_struct(void)
{
    hal_oldname_t *p;

    /* check the free list */
    if (hal_data->oldname_free_ptr != 0) {
	/* found a free structure, point to it */
	p = SHMPTR(hal_data->oldname_free_ptr);
	/* unlink it from the free list */
	hal_data->oldname_free_ptr = p->next_ptr;
	p->next_ptr = 0;
    } else {
	/* nothing on free list, allocate a brand new one */
	p = shmalloc_dn(sizeof(hal_oldname_t));
    }
    if (p) {
	/* make sure it's empty */
	p->next_ptr = 0;
	p->name[0] = '\0';
    }
    return p;
}

#ifdef RTAPI
static hal_funct_t *alloc_funct_struct(void)
{
    hal_funct_t *p;

    /* check the free list */
    if (hal_data->funct_free_ptr != 0) {
	/* found a free structure, point to it */
	p = SHMPTR(hal_data->funct_free_ptr);
	/* unlink it from the free list */
	hal_data->funct_free_ptr = p->next_ptr;
	p->next_ptr = 0;
    } else {
	/* nothing on free list, allocate a brand new one */
	p = shmalloc_dn(sizeof(hal_funct_t));
    }
    if (p) {
	/* make sure it's empty */
	p->next_ptr = 0;
	p->uses_fp = 0;
	p->owner_ptr = 0;
	p->reentrant = 0;
	p->users = 0;
	p->arg = 0;
	p->funct = 0;
	p->name[0] = '\0';
    }
    return p;
}
#endif /* RTAPI */

static hal_funct_entry_t *alloc_funct_entry_struct(void)
{
    hal_list_t *freelist, *l;
    hal_funct_entry_t *p;

    /* check the free list */
    freelist = &(hal_data->funct_entry_free);
    l = list_next(freelist);
    if (l != freelist) {
	/* found a free structure, unlink from the free list */
	list_remove_entry(l);
	p = (hal_funct_entry_t *) l;
    } else {
	/* nothing on free list, allocate a brand new one */
	p = shmalloc_dn(sizeof(hal_funct_entry_t));
	l = (hal_list_t *) p;
	list_init_entry(l);
    }
    if (p) {
	/* make sure it's empty */
	p->funct_ptr = 0;
	p->arg = 0;
	p->funct = 0;
    }
    return p;
}

#ifdef RTAPI
static hal_thread_t *alloc_thread_struct(void)
{
    hal_thread_t *p;

    /* check the free list */
    if (hal_data->thread_free_ptr != 0) {
	/* found a free structure, point to it */
	p = SHMPTR(hal_data->thread_free_ptr);
	/* unlink it from the free list */
	hal_data->thread_free_ptr = p->next_ptr;
	p->next_ptr = 0;
    } else {
	/* nothing on free list, allocate a brand new one */
	p = shmalloc_dn(sizeof(hal_thread_t));
    }
    if (p) {
	/* make sure it's empty */
	p->next_ptr = 0;
	p->uses_fp = 0;
	p->period = 0;
	p->priority = 0;
	p->task_id = 0;
	list_init_entry(&(p->funct_list));
	p->name[0] = '\0';
    }
    return p;
}
#endif /* RTAPI */

static void free_comp_struct(hal_comp_t * comp)
{
    int *prev, next;
#ifdef RTAPI
    hal_funct_t *funct;
#endif /* RTAPI */
    hal_pin_t *pin;
    hal_param_t *param;

    /* can't delete the component until we delete its "stuff" */
    /* need to check for functs only if a realtime component */
#ifdef RTAPI
    /* search the function list for this component's functs */
    prev = &(hal_data->funct_list_ptr);
    next = *prev;
    while (next != 0) {
	funct = SHMPTR(next);
	if (SHMPTR(funct->owner_ptr) == comp) {
	    /* this function belongs to our component, unlink from list */
	    *prev = funct->next_ptr;
	    /* and delete it */
	    free_funct_struct(funct);
	} else {
	    /* no match, try the next one */
	    prev = &(funct->next_ptr);
	}
	next = *prev;
    }
#endif /* RTAPI */
    /* search the pin list for this component's pins */
    prev = &(hal_data->pin_list_ptr);
    next = *prev;
    while (next != 0) {
	pin = SHMPTR(next);
	if (SHMPTR(pin->owner_ptr) == comp) {
	    /* this pin belongs to our component, unlink from list */
	    *prev = pin->next_ptr;
	    /* and delete it */
	    free_pin_struct(pin);
	} else {
	    /* no match, try the next one */
	    prev = &(pin->next_ptr);
	}
	next = *prev;
    }
    /* search the parameter list for this component's parameters */
    prev = &(hal_data->param_list_ptr);
    next = *prev;
    while (next != 0) {
	param = SHMPTR(next);
	if (SHMPTR(param->owner_ptr) == comp) {
	    /* this param belongs to our component, unlink from list */
	    *prev = param->next_ptr;
	    /* and delete it */
	    free_param_struct(param);
	} else {
	    /* no match, try the next one */
	    prev = &(param->next_ptr);
	}
	next = *prev;
    }
    /* now we can delete the component itself */
    /* clear contents of struct */
    comp->comp_id = 0;
    comp->mem_id = 0;
    comp->type = 0;
    comp->shmem_base = 0;
    comp->name[0] = '\0';
    /* add it to free list */
    comp->next_ptr = hal_data->comp_free_ptr;
    hal_data->comp_free_ptr = SHMOFF(comp);
}

static void unlink_pin(hal_pin_t * pin)
{
    hal_sig_t *sig;
    hal_comp_t *comp;
    void **data_ptr_addr;
    hal_data_u *dummy_addr, *sig_data_addr;

    /* is this pin linked to a signal? */
    if (pin->signal != 0) {
	/* yes, need to unlink it */
	sig = SHMPTR(pin->signal);
	/* make pin's 'data_ptr' point to its dummy signal */
	data_ptr_addr = SHMPTR(pin->data_ptr_addr);
	comp = SHMPTR(pin->owner_ptr);
	dummy_addr = comp->shmem_base + SHMOFF(&(pin->dummysig));
	*data_ptr_addr = dummy_addr;

	/* copy current signal value to dummy */
	sig_data_addr = (hal_data_u *)(hal_shmem_base + sig->data_ptr);
	dummy_addr = (hal_data_u *)(hal_shmem_base + SHMOFF(&(pin->dummysig)));

	switch (pin->type) {
	case HAL_BIT:
	    dummy_addr->b = sig_data_addr->b;
	    break;
	case HAL_S32:
	    dummy_addr->s = sig_data_addr->s;
	    break;
	case HAL_U32:
	    dummy_addr->u = sig_data_addr->u;
	    break;
	case HAL_FLOAT:
	    dummy_addr->f = sig_data_addr->f;
	    break;
	default:
	    rtapi_print_msg(RTAPI_MSG_ERR,
			  "HAL: BUG: pin '%s' has invalid type %d !!\n",
			  pin->name, pin->type);
	}

	/* update the signal's reader/writer counts */
	if ((pin->dir & HAL_IN) != 0) {
	    sig->readers--;
	}
	if (pin->dir == HAL_OUT) {
	    sig->writers--;
	}
	if (pin->dir == HAL_IO) {
	    sig->bidirs--;
	}
	/* mark pin as unlinked */
	pin->signal = 0;
    }
}

static void free_pin_struct(hal_pin_t * pin)
{

    unlink_pin(pin);
    /* clear contents of struct */
    if ( pin->oldname != 0 ) free_oldname_struct(SHMPTR(pin->oldname));
    pin->data_ptr_addr = 0;
    pin->owner_ptr = 0;
    pin->type = 0;
    pin->dir = 0;
    pin->signal = 0;
    memset(&pin->dummysig, 0, sizeof(hal_data_u));
    pin->name[0] = '\0';
    /* add it to free list */
    pin->next_ptr = hal_data->pin_free_ptr;
    hal_data->pin_free_ptr = SHMOFF(pin);
}

static void free_sig_struct(hal_sig_t * sig)
{
    hal_pin_t *pin;

    /* look for pins linked to this signal */
    pin = halpr_find_pin_by_sig(sig, 0);
    while (pin != 0) {
	/* found one, unlink it */
	unlink_pin(pin);
	/* check for another pin linked to the signal */
	pin = halpr_find_pin_by_sig(sig, pin);
    }
    /* clear contents of struct */
    sig->data_ptr = 0;
    sig->type = 0;
    sig->readers = 0;
    sig->writers = 0;
    sig->bidirs = 0;
    sig->name[0] = '\0';
    /* add it to free list */
    sig->next_ptr = hal_data->sig_free_ptr;
    hal_data->sig_free_ptr = SHMOFF(sig);
}

static void free_param_struct(hal_param_t * p)
{
    /* clear contents of struct */
    if ( p->oldname != 0 ) free_oldname_struct(SHMPTR(p->oldname));
    p->data_ptr = 0;
    p->owner_ptr = 0;
    p->type = 0;
    p->name[0] = '\0';
    /* add it to free list (params use the same struct as src vars) */
    p->next_ptr = hal_data->param_free_ptr;
    hal_data->param_free_ptr = SHMOFF(p);
}

static void free_oldname_struct(hal_oldname_t * oldname)
{
    /* clear contents of struct */
    oldname->name[0] = '\0';
    /* add it to free list */
    oldname->next_ptr = hal_data->oldname_free_ptr;
    hal_data->oldname_free_ptr = SHMOFF(oldname);
}

#ifdef RTAPI
static void free_funct_struct(hal_funct_t * funct)
{
    int next_thread;
    hal_thread_t *thread;
    hal_list_t *list_root, *list_entry;
    hal_funct_entry_t *funct_entry;

/*  int next_thread, next_entry;*/

    if (funct->users > 0) {
	/* We can't casually delete the function, there are thread(s) which
	   will call it.  So we must check all the threads and remove any
	   funct_entrys that call this function */
	/* start at root of thread list */
	next_thread = hal_data->thread_list_ptr;
	/* run through thread list */
	while (next_thread != 0) {
	    /* point to thread */
	    thread = SHMPTR(next_thread);
	    /* start at root of funct_entry list */
	    list_root = &(thread->funct_list);
	    list_entry = list_next(list_root);
	    /* run thru funct_entry list */
	    while (list_entry != list_root) {
		/* point to funct entry */
		funct_entry = (hal_funct_entry_t *) list_entry;
		/* test it */
		if (SHMPTR(funct_entry->funct_ptr) == funct) {
		    /* this funct entry points to our funct, unlink */
		    list_entry = list_remove_entry(list_entry);
		    /* and delete it */
		    free_funct_entry_struct(funct_entry);
		} else {
		    /* no match, try the next one */
		    list_entry = list_next(list_entry);
		}
	    }
	    /* move on to the next thread */
	    next_thread = thread->next_ptr;
	}
    }
    /* clear contents of struct */
    funct->uses_fp = 0;
    funct->owner_ptr = 0;
    funct->reentrant = 0;
    funct->users = 0;
    funct->arg = 0;
    funct->funct = 0;
    funct->runtime = 0;
    funct->name[0] = '\0';
    /* add it to free list */
    funct->next_ptr = hal_data->funct_free_ptr;
    hal_data->funct_free_ptr = SHMOFF(funct);
}
#endif /* RTAPI */

static void free_funct_entry_struct(hal_funct_entry_t * funct_entry)
{
    hal_funct_t *funct;

    if (funct_entry->funct_ptr > 0) {
	/* entry points to a function, update the function struct */
	funct = SHMPTR(funct_entry->funct_ptr);
	funct->users--;
    }
    /* clear contents of struct */
    funct_entry->funct_ptr = 0;
    funct_entry->arg = 0;
    funct_entry->funct = 0;
    /* add it to free list */
    list_add_after((hal_list_t *) funct_entry, &(hal_data->funct_entry_free));
}

#ifdef RTAPI
static void free_thread_struct(hal_thread_t * thread)
{
    hal_funct_entry_t *funct_entry;
    hal_list_t *list_root, *list_entry;
/*! \todo Another #if 0 */
#if 0
    int *prev, next;
    char time[HAL_NAME_LEN + 1], tmax[HAL_NAME_LEN + 1];
    hal_param_t *param;
#endif

    /* if we're deleting a thread, we need to stop all threads */
    hal_data->threads_running = 0;
    /* and stop the task associated with this thread */
    rtapi_task_pause(thread->task_id);
    rtapi_task_delete(thread->task_id);
    /* clear contents of struct */
    thread->uses_fp = 0;
    thread->period = 0;
    thread->priority = 0;
    thread->task_id = 0;
    /* clear the function entry list */
    list_root = &(thread->funct_list);
    list_entry = list_next(list_root);
    while (list_entry != list_root) {
	/* entry found, save pointer to it */
	funct_entry = (hal_funct_entry_t *) list_entry;
	/* unlink it, point to the next one */
	list_entry = list_remove_entry(list_entry);
	/* free the removed entry */
	free_funct_entry_struct(funct_entry);
    }
/*! \todo Another #if 0 */
#if 0
/* Currently these don't get created, so we don't have to worry
   about deleting them.  They will come back when the HAL refactor
   is done, at that time this code or something like it will be
   needed.
*/
    /* need to delete <thread>.time and <thread>.tmax params */
    rtapi_snprintf(time, sizeof(time), "%s.time", thread->name);
    rtapi_snprintf(tmax, sizeof(tmax), "%s.tmax", thread->name);
    /* search the parameter list for those parameters */
    prev = &(hal_data->param_list_ptr);
    next = *prev;
    while (next != 0) {
	param = SHMPTR(next);
	/* does this param match either name? */
	if ((strcmp(param->name, time) == 0)
	    || (strcmp(param->name, tmax) == 0)) {
	    /* yes, unlink from list */
	    *prev = param->next_ptr;
	    /* and delete it */
	    free_param_struct(param);
	} else {
	    /* no match, try the next one */
	    prev = &(param->next_ptr);
	}
	next = *prev;
    }
#endif
    thread->name[0] = '\0';
    /* add thread to free list */
    thread->next_ptr = hal_data->thread_free_ptr;
    hal_data->thread_free_ptr = SHMOFF(thread);
}
#endif /* RTAPI */


#ifdef RTAPI
/* only export symbols when we're building a kernel module */

EXPORT_SYMBOL(hal_init);
EXPORT_SYMBOL(hal_ready);
EXPORT_SYMBOL(hal_exit);
EXPORT_SYMBOL(hal_malloc);
EXPORT_SYMBOL(hal_comp_name);

EXPORT_SYMBOL(hal_pin_bit_new);
EXPORT_SYMBOL(hal_pin_float_new);
EXPORT_SYMBOL(hal_pin_u32_new);
EXPORT_SYMBOL(hal_pin_s32_new);
EXPORT_SYMBOL(hal_pin_new);

EXPORT_SYMBOL(hal_pin_bit_newf);
EXPORT_SYMBOL(hal_pin_float_newf);
EXPORT_SYMBOL(hal_pin_u32_newf);
EXPORT_SYMBOL(hal_pin_s32_newf);


EXPORT_SYMBOL(hal_signal_new);
EXPORT_SYMBOL(hal_signal_delete);
EXPORT_SYMBOL(hal_link);
EXPORT_SYMBOL(hal_unlink);

EXPORT_SYMBOL(hal_param_bit_new);
EXPORT_SYMBOL(hal_param_float_new);
EXPORT_SYMBOL(hal_param_u32_new);
EXPORT_SYMBOL(hal_param_s32_new);
EXPORT_SYMBOL(hal_param_new);

EXPORT_SYMBOL(hal_param_bit_newf);
EXPORT_SYMBOL(hal_param_float_newf);
EXPORT_SYMBOL(hal_param_u32_newf);
EXPORT_SYMBOL(hal_param_s32_newf);

EXPORT_SYMBOL(hal_param_bit_set);
EXPORT_SYMBOL(hal_param_float_set);
EXPORT_SYMBOL(hal_param_u32_set);
EXPORT_SYMBOL(hal_param_s32_set);
EXPORT_SYMBOL(hal_param_set);

EXPORT_SYMBOL(hal_set_constructor);

EXPORT_SYMBOL(hal_export_funct);

EXPORT_SYMBOL(hal_create_thread);

EXPORT_SYMBOL(hal_add_funct_to_thread);
EXPORT_SYMBOL(hal_del_funct_from_thread);

EXPORT_SYMBOL(hal_start_threads);
EXPORT_SYMBOL(hal_stop_threads);

EXPORT_SYMBOL(hal_shmem_base);
EXPORT_SYMBOL(halpr_find_comp_by_name);
EXPORT_SYMBOL(halpr_find_pin_by_name);
EXPORT_SYMBOL(halpr_find_sig_by_name);
EXPORT_SYMBOL(halpr_find_param_by_name);
EXPORT_SYMBOL(halpr_find_thread_by_name);
EXPORT_SYMBOL(halpr_find_funct_by_name);
EXPORT_SYMBOL(halpr_find_comp_by_id);

EXPORT_SYMBOL(halpr_find_pin_by_owner);
EXPORT_SYMBOL(halpr_find_param_by_owner);
EXPORT_SYMBOL(halpr_find_funct_by_owner);

EXPORT_SYMBOL(halpr_find_pin_by_sig);

EXPORT_SYMBOL(hal_pin_alias);
EXPORT_SYMBOL(hal_param_alias);
#endif /* rtapi */
