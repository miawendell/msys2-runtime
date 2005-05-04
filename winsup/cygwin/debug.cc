/* debug.cc

   Copyright 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005 Red Hat, Inc.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <malloc.h>
#include "sync.h"
#include "sigproc.h"
#include "pinfo.h"
#include "perprocess.h"
#include "security.h"
#include "cygerrno.h"
#ifdef DEBUGGING
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#endif

#undef CloseHandle

#ifdef DEBUGGING
/* Here lies extra debugging routines which help track down internal
   Cygwin problems when compiled with -DDEBUGGING . */
#include <stdlib.h>
#define NFREEH (sizeof (cygheap->debug.freeh) / sizeof (cygheap->debug.freeh[0]))

class lock_debug
{
  static muto locker;
  bool acquired;
 public:
  lock_debug () : acquired (0)
  {
    if (locker.name && !exit_state)
      acquired = !!locker.acquire (INFINITE);
  }
  void unlock ()
  {
    if (locker.name && acquired)
      {
	locker.release ();
	acquired = false;
      }
  }
  ~lock_debug () {unlock ();}
  friend void debug_init ();
};

muto NO_COPY lock_debug::locker;

static bool __stdcall mark_closed (const char *, int, HANDLE, const char *, bool);

void
debug_init ()
{
  lock_debug::locker.init ("debug_lock");
}

/* Find a registered handle in the linked list of handles. */
static handle_list * __stdcall
find_handle (HANDLE h)
{
  handle_list *hl;
  for (hl = &cygheap->debug.starth; hl->next != NULL; hl = hl->next)
    if (hl->next->h == h)
      goto out;
  cygheap->debug.endh = hl;
  hl = NULL;

out:
  return hl;
}

void
verify_handle (const char *func, int ln, HANDLE h)
{
  handle_list *hl = find_handle (h);
  if (!hl)
    return;
  system_printf ("%s:%d - multiple attempts to add handle %p", func, ln, h);

  system_printf (" previously allocated by %s:%d(%s<%p>) winpid %d",
		 hl->func, hl->ln, hl->name, hl->h, hl->pid);
}

void
setclexec (HANDLE oh, HANDLE nh, bool not_inheriting)
{
  handle_list *hl = find_handle (oh);
  if (hl)
    {
      hl = hl->next;
      hl->inherited = !not_inheriting;
      hl->h = nh;
    }
}

/* Create a new handle record */
static handle_list * __stdcall
newh ()
{
  handle_list *hl;
  lock_debug here;

  for (hl = cygheap->debug.freeh; hl < cygheap->debug.freeh + NFREEH; hl++)
    if (hl->name == NULL)
      return hl;

  return NULL;
}

/* Add a handle to the linked list of known handles. */
void __stdcall
add_handle (const char *func, int ln, HANDLE h, const char *name, bool inh)
{
  handle_list *hl;
  lock_debug here;

  if (!cygheap)
    return;

  if ((hl = find_handle (h)))
    {
      hl = hl->next;
      if (hl->name == name && hl->func == func && hl->ln == ln)
	return;
      system_printf ("%s:%d - multiple attempts to add handle %s<%p>", func,
		     ln, name, h);
      system_printf (" previously allocated by %s:%d(%s<%p>) winpid %d",
		     hl->func, hl->ln, hl->name, hl->h, hl->pid);
      return;
    }

  if ((hl = newh ()) == NULL)
    {
      here.unlock ();
      debug_printf ("couldn't allocate memory for %s(%d): %s(%p)",
		    func, ln, name, h);
      return;
    }
  hl->h = h;
  hl->name = name;
  hl->func = func;
  hl->ln = ln;
  hl->next = NULL;
  hl->inherited = inh;
  hl->pid = GetCurrentProcessId ();
  cygheap->debug.endh->next = hl;
  cygheap->debug.endh = hl;
  debug_printf ("protecting handle '%s', inherited flag %d", hl->name, hl->inherited);

  return;
}

static void __stdcall
delete_handle (handle_list *hl)
{
  handle_list *hnuke = hl->next;
  debug_printf ("nuking handle '%s' (%p)", hnuke->name, hnuke->h);
  hl->next = hl->next->next;
  memset (hnuke, 0, sizeof (*hnuke));
}

void
debug_fixup_after_fork_exec ()
{
  /* No lock needed at this point */
  handle_list *hl;
  for (hl = &cygheap->debug.starth; hl->next != NULL; /* nothing */)
    if (hl->next->inherited)
      hl = hl->next;
    else
      delete_handle (hl);	// removes hl->next
}

static bool __stdcall
mark_closed (const char *func, int ln, HANDLE h, const char *name, bool force)
{
  handle_list *hl;
  lock_debug here;

  if (!cygheap)
    return true;

  if ((hl = find_handle (h)) && !force)
    {
      hl = hl->next;
      here.unlock ();	// race here
      system_printf ("attempt to close protected handle %s:%d(%s<%p>) winpid %d",
		     hl->func, hl->ln, hl->name, hl->h, hl->pid);
      system_printf (" by %s:%d(%s<%p>)", func, ln, name, h);
      return false;
    }

  handle_list *hln;
  if (hl && (hln = hl->next) && strcmp (name, hln->name))
    {
      system_printf ("closing protected handle %s:%d(%s<%p>)",
		     hln->func, hln->ln, hln->name, hln->h);
      system_printf (" by %s:%d(%s<%p>)", func, ln, name, h);
    }

  if (hl)
    delete_handle (hl);

  return true;
}

/* Close a known handle.  Complain if !force and closing a known handle or
   if the name of the handle being closed does not match the registered name. */
bool __stdcall
close_handle (const char *func, int ln, HANDLE h, const char *name, bool force)
{
  bool ret;
  lock_debug here;

  if (!mark_closed (func, ln, h, name, force))
    return false;

  ret = CloseHandle (h);

#if 0 /* Uncomment to see CloseHandle failures */
  if (!ret)
    small_printf ("CloseHandle(%s) failed %s:%d\n", name, func, ln);
#endif
  return ret;
}
#endif /*DEBUGGING*/
