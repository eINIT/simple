/***************************************************************************
 *            module.c
 *
 *  Mon Feb  6 15:27:39 2006
 *  Copyright  2006  Magnus Deininger
 *  dma05@web.de
 ****************************************************************************/
/*
Copyright (c) 2006, Magnus Deininger
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
    * Neither the name of the project nor the names of its contributors may be
	  used to endorse or promote products derived from this software without
	  specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <unistd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <dlfcn.h>
#include <string.h>
#include <einit/bitch.h>
#include <einit/config.h>
#include <einit/module.h>
#include <einit/utility.h>
/*
 dynamic linker functions (POSIX)

 void *dlopen(const char *filename, int flag);
 char *dlerror(void);
 void *dlsym(void *handle, const char *symbol);
 int dlclose(void *handle);
*/

#include <pthread.h>

int mod_comment (struct mfeedback *status);

struct lmodule *mlist = NULL;
struct lmodule mdefault = {
	NULL, NULL, NULL, mod_comment, NULL, 0, NULL, NULL, NULL
};
int mcount = 0;

char **provided = NULL;
char **required = NULL;

int mod_scanmodules () {
 DIR *dir;
 struct dirent *entry;
 char *tmp;
 int mplen;
 void *sohandle;
 struct lmodule *cmod = NULL, *nmod;
 char *modulepath = cfg_getpath ("module-path");
 if (!modulepath) return -1;

 mplen = strlen (modulepath) +1;
 dir = opendir (modulepath);
 if (dir != NULL) {
  while (entry = readdir (dir)) {
   if (entry->d_name[0] == '.') continue;
   tmp = (char *)emalloc ((mplen + strlen (entry->d_name))*sizeof (char));
	struct stat sbuf;
	struct smodule *modinfo;
    int (*func)(void *);
    *tmp = 0;
    strcat (tmp, modulepath);
    strcat (tmp, entry->d_name);
	dlerror ();
    if (stat (tmp, &sbuf) || !S_ISREG (sbuf.st_mode)) {
	 free (tmp);
	 continue;
	}

	sohandle = dlopen (tmp, RTLD_NOW);
	if (sohandle == NULL) {
	 puts (dlerror ());
	 free (tmp);
	 continue;
	}
	modinfo = (struct smodule *)dlsym (sohandle, "self");
	if (modinfo != NULL) {
     mod_add (sohandle, NULL, NULL, NULL, modinfo);
	 if (modinfo->mode & EINIT_MOD_LOADER) {
      func = (int (*)(void *)) dlsym (sohandle, "scanmodules");
	 }
    }
	
	free (tmp);
  }
  closedir (dir);
 } else {
  fputs ("couldn't open module directory\n", stderr);
  return bitch(BTCH_ERRNO);
 }
 return 1;
}

void mod_freedesc (struct lmodule *m) {
 if (m->next != NULL)
  mod_freedesc (m->next);
 if (m->cleanup)
  m->cleanup (m);
 if (m->sohandle)
  dlclose (m->sohandle);
 free (m);
}

int mod_freemodules () {
 if (mlist != NULL)
  mod_freedesc (mlist);
 mlist = NULL;
 return 1;
}

int mod_add (void *sohandle, int (*enable)(void *, struct mfeedback *), int (*disable)(void *, struct mfeedback *), void *param, struct smodule *module) {
 struct lmodule *nmod, *cur;
 int (*scanfunc)(struct lmodule *);
 int (*comment) (struct mfeedback *);
 int (*ftload)  (void *, struct mfeedback *);
 int (*configfunc)(struct lmodule *);

 nmod = ecalloc (1, sizeof (struct lmodule));

 if (mlist == NULL) {
  mlist = nmod;
 } else {
  cur = mlist;
  while (cur->next)
   cur = cur->next;
  cur->next = nmod;
 }
 mcount++;

 nmod->sohandle = sohandle;
 nmod->module = module;
 nmod->param = param;
 nmod->enable = enable;
 nmod->disable = disable;

// this will do additional initialisation functions for certain module-types
 if (module && sohandle) {
// EINIT_MOD_LOADER modules will usually want to provide a function to scan
//  for modules so they can be included in the dependency chain
  if (module->mode & EINIT_MOD_LOADER) {
   scanfunc = (int (*)(struct lmodule *)) dlsym (sohandle, "scanmodules");
   if (scanfunc != NULL) {
    scanfunc (mlist);
   }
   else bitch(BTCH_ERRNO + BTCH_DL);
  }
// EINIT_MOD_FEEDBACK-type modules will usually want to provide a comment()-
//  function in order to provide feedback about how a module is loading...
  if (module->mode & EINIT_MOD_FEEDBACK) {
   comment = (int (*)(struct mfeedback *)) dlsym (sohandle, "comment");
   if (comment != NULL) {
    nmod->comment = comment;
   }
   else bitch(BTCH_ERRNO + BTCH_DL);
  }
// we need to scan for load and unload functions if NULL was supplied for these
  if (enable == NULL) {
   ftload = (int (*)(void *, struct mfeedback *)) dlsym (sohandle, "enable");
   if (ftload != NULL) {
    nmod->enable = ftload;
   }
  }
  if (disable == NULL) {
   ftload = (int (*)(void *, struct mfeedback *)) dlsym (sohandle, "disable");
   if (ftload != NULL) {
    nmod->disable = ftload;
   }
  }
// look for and execute any configure() functions in modules
  configfunc = (int (*)(struct lmodule *)) dlsym (sohandle, "configure");
  if (configfunc != NULL) {
   configfunc (nmod);
  }
// look for any cleanup() functions
  configfunc = (int (*)(struct lmodule *)) dlsym (sohandle, "cleanup");
  if (configfunc != NULL) {
   nmod->cleanup = configfunc;
  }
 }

 return 0;
}

struct lmodule *mod_find (char *rid, unsigned int modeflags) {
 struct lmodule *cur = mlist;
 if (mlist == NULL)
  return NULL;

 if (rid) {
  while (!cur->module || !cur->module->rid ||
    (modeflags && (cur->module->mode ^ modeflags)) ||
    strcmp(rid, cur->module->rid)) {
   if (!cur->next) return NULL;
   cur = cur->next;
  }
 } else {
  while (!cur->module ||
    (modeflags && (cur->module->mode ^ modeflags))) {
   if (!cur->next) return NULL;
   cur = cur->next;
  }
 }

 return cur;
}

int mod (unsigned int task, struct lmodule *module) {
 struct mfeedback *fb;
 pthread_t *th;
 char providefeedback;
 struct smodule *t;
 int ti;
 unsigned int ret;
 if (!module) return 0;
/* wait if the module is already being processed in a different thread */
 while (module->status & STATUS_WORKING)
  sleep (1);

 module->status = module->status | STATUS_WORKING;

/* check if the task requested has already been done (or if it can be done at all) */
 if ((task & MOD_ENABLE) && (!module->enable || (module->status & STATUS_ENABLED))) {
  wontload:
  module->status ^= STATUS_WORKING;
//  module->status = STATUS_ENABLED;
  return STATUS_IDLE;
 }
 if ((task & MOD_DISABLE) && (!module->disable || (module->status & STATUS_DISABLED)))
  goto wontload;

 th = ecalloc (1, sizeof (pthread_t));

 if (task & MOD_ENABLE) {
   if (t = module->module) {
    if (t->requires) for (ti = 0; t->requires[ti]; ti++)
     if (!strinset (provided, t->requires[ti])) {
      module->status ^= STATUS_WORKING;
      return STATUS_FAIL_REQ;
     }
   }
 } else if (task & MOD_DISABLE) {
   if (t = module->module) {
    if (t->provides) for (ti = 0; t->provides[ti]; ti++)
     if (strinset (required, t->provides[ti])) {
      module->status ^= STATUS_WORKING;
      return STATUS_FAIL_REQ;
     }
   }
 }

 fb = (struct mfeedback *)ecalloc (1, sizeof (struct mfeedback));
 fb->module = module;
 fb->task = task | MOD_FEEDBACK_SHOW;
 fb->status = STATUS_WORKING;
 status_update (fb);

 if (task & MOD_ENABLE) {
   ret = module->enable (module->param, fb);
   if (ret & STATUS_OK) {
    if (t = module->module) {
     if (t->provides)
      provided = (char **)setcombine ((void **)provided, (void **)t->provides);
     if (t->requires)
      required = (char **)setcombine ((void **)required, (void **)t->requires);
    }
	module->status = STATUS_ENABLED;
   }
 } else if (task & MOD_DISABLE) {
   ret = module->disable (module->param, fb);
   if (ret & STATUS_OK) {
    if (t = module->module) {
     if (t->provides) for (ti = 0; t->provides[ti]; ti++)
      provided = strsetdel (provided, t->provides[ti]);
     if (t->requires) for (ti = 0; t->requires[ti]; ti++)
      required = strsetdel (required, t->requires[ti]);
    }
	module->status = STATUS_DISABLED;
   }
 }

 fb->status = module->status;
 status_update (fb);
 free (fb);

 return module->status;
}

int mod_comment (struct mfeedback *status) {
 return 0;
}

/* helper functions for mod_plan should go right here */

int mod_plan_sort_by_preference (struct lmodule **cand, char *atom) {
 char *pstring = emalloc ((8 + strlen (atom)) * sizeof (char));
 char **preftab;
 struct cfgnode *node;
 unsigned int tn = 0, cn = 0, ci;
 pstring[0] = 0;
 strcat (pstring, "prefer-");
 strcat (pstring, atom);
 node = cfg_findnode (pstring, 0, NULL);
 if (!node || !node->svalue) return 0;
 free (pstring);
 pstring = estrdup (node->svalue);
 preftab = str2set (':', pstring);
 if (!preftab) {
  free (pstring);
  return bitch (BTCH_ERRNO);
 }

 for (; preftab[tn]; tn++) {
  for (ci = 0; cand[ci]; ci++) {
   if (cand[ci]->module && cand[ci]->module->rid &&
       !strcmp (cand[ci]->module->rid, preftab[tn])) {
    struct lmodule *tmp = cand[cn];
    cand[cn] = cand[ci];
	cand[ci] = tmp;
	cn++;
	break;
   }
  }
 }

 free (pstring);
 free (preftab);
 return 0;
}

struct uhash *mod_plan_wpw (struct mloadplan *plan, struct uhash *hash) {
 struct uhash *ihash = hash;
 int i;

 if (!plan) return NULL;
 if (plan->left && plan->left[0]) {
  for (i = 0; plan->left[i]; i++)
   ihash = mod_plan_wpw(plan->left[i], ihash);
 }
 if (plan->right && plan->right[0]) {
  for (i = 0; plan->right[i]; i++)
   ihash = mod_plan_wpw(plan->right[i], ihash);
 }
 if (plan->orphaned && plan->orphaned[0]) {
  for (i = 0; plan->orphaned[i]; i++)
   ihash = mod_plan_wpw(plan->orphaned[i], ihash);
 }
 if (plan->mod) {
  if (plan->mod->module) {
   struct smodule *mod = plan->mod->module;
   if (mod->rid)
    ihash = hashadd (ihash, mod->rid, plan);
   if (mod->provides && mod->provides[0]) {
    for (i = 0; mod->provides[i]; i++)
     ihash = hashadd (ihash, mod->provides[i], (void *)plan);
   }
  }
 }

 return ihash;
}

struct uhash *mod_plan_wrw (struct mloadplan *plan, struct uhash *hash) {
 struct uhash *ihash = hash;
 int i;

 if (!plan) return NULL;
 if (plan->left && plan->left[0]) {
  for (i = 0; plan->left[i]; i++)
   ihash = mod_plan_wrw(plan->left[i], ihash);
 }
 if (plan->right && plan->right[0]) {
  for (i = 0; plan->right[i]; i++)
   ihash = mod_plan_wrw(plan->right[i], ihash);
 }
 if (plan->orphaned && plan->orphaned[0]) {
  for (i = 0; plan->orphaned[i]; i++)
   ihash = mod_plan_wrw(plan->orphaned[i], ihash);
 }
 if (plan->mod) {
  if (plan->mod->module) {
   struct smodule *mod = plan->mod->module;
   if (mod->requires && mod->requires[0]) {
    for (i = 0; mod->requires[i]; i++)
     ihash = hashadd (ihash, mod->requires[i], (void *)plan);
   }
  }
 }

 return ihash;
}

struct mloadplan *mod_plan_restructure (struct mloadplan *plan) {
 struct uhash *hash_prov;
 struct uhash *hash_req;
 struct uhash *c;
 struct uhash *d;
 struct mloadplan **orphans = NULL;
 struct mloadplan **curpl = NULL;
 unsigned int i, j;
 unsigned char pass = 0, adds;
 if (!plan) return NULL;
 else if (!plan->mod && !plan->right && !plan->left && !plan->orphaned) {
  free (plan);
  return NULL;
 }

 hash_prov = mod_plan_wpw (plan, NULL);
 hash_req = mod_plan_wrw (plan, NULL);

 d = hash_prov;
 
 while (d) {
  if (d->value) {
   struct mloadplan *v = (struct mloadplan *)d->value;
   if (v->right) free (v->right);
   v->right = NULL;
//   if (v->left) free (v->left);
//   v->left = NULL;
  }
  d = hashnext (d);
 }

 d = hash_prov;
 while (d) {
  if (d->value) {
   struct mloadplan *v = (struct mloadplan *)d->value;

//   printf ("%s: %i\n", v->mod->module->rid, v->task);

   if (v->mod && v->mod->module) {
    if (((v->task & MOD_ENABLE) && (v->mod->module->requires)) ||
        ((v->task & MOD_DISABLE) && ((v->mod->module->provides)))) {
     char **req = NULL;
     if (v->task & MOD_ENABLE) req = v->mod->module->requires;
     if (v->task & MOD_DISABLE) req = v->mod->module->provides;

     for (j = 0; req[j]; j++) {
      adds = 0;
      if (v->task & MOD_ENABLE) c = hash_prov;
      if (v->task & MOD_DISABLE) c = hash_req;
      while (c && (c = hashfind (c, req[j]))) {
       struct mloadplan *e = c->value;
       adds++;
       e->right = (struct mloadplan **)setadd ((void **)e->right, (void *)v);
       c = c->next;
      }
      if (adds) {
       plan->orphaned = (struct mloadplan **)setdel ((void **)plan->orphaned, (void*)v);
      } else if (v->task & MOD_ENABLE) {
       if (!strinset (plan->unavailable, req[j]) && !strinset (plan->unsatisfied, req[j]))
        plan->unsatisfied = (char **)setadd ((void **)plan->unsatisfied, (void *)req[j]);
      } else if (v->task & MOD_DISABLE) {
       plan->right = (struct mloadplan **)setadd ((void **)plan->right, (void *)v);
       plan->orphaned = (struct mloadplan **)setdel ((void **)plan->orphaned, (void*)v);
      }
     }
    } else {
     plan->right = (struct mloadplan **)setadd ((void **)plan->right, (void *)v);
     plan->orphaned = (struct mloadplan **)setdel ((void **)plan->orphaned, (void*)v);
    }
   }
  }
  d = hashnext (d);
 }

 hashfree (hash_prov);
 hashfree (hash_req);

 return plan;
}

/* end helper functions */

struct mloadplan *mod_plan (struct mloadplan *plan, char **atoms, unsigned int task) {
 struct lmodule *curmod;
 struct mloadplan **nplancand = NULL;
 int si = 0;
 if (!atoms && !(task & MOD_DISABLE_UNSPEC)) return NULL;

 if (!plan) {
  plan = (struct mloadplan *)ecalloc (1, sizeof (struct mloadplan));
  plan->task = task;
 }

 if (task & MOD_DISABLE_UNSPEC) {
  curmod = mlist;
  while (curmod) {
   struct smodule *tmp = curmod->module;
   if (curmod->status & STATUS_ENABLED) {
    if (tmp) {
	 if ((tmp->mode & EINIT_MOD_FEEDBACK) && !(task & MOD_DISABLE_UNSPEC_FEEDBACK) ||
         (tmp->rid && strinset (atoms, tmp->rid)))
      goto skipcurmod;
     if (tmp->provides && atoms) {
      for (si = 0; atoms[si]; si++)
	   if (strinset (tmp->provides, atoms[si]))
        goto skipcurmod;
     }
    }
    struct mloadplan *cplan = (struct mloadplan *)ecalloc (1, sizeof (struct mloadplan));
    cplan->task = MOD_DISABLE;
    cplan->mod = curmod;
    plan->orphaned = (struct mloadplan **)setadd ((void **)plan->orphaned, (void *)cplan);
   }
   skipcurmod:
   curmod = curmod->next;
  }
 }

 if (atoms) for (si = 0; atoms[si]; si++) {
  struct lmodule **cand = (struct lmodule **)ecalloc (mcount+1, sizeof (struct lmodule *));
  struct mloadplan *cplan = NULL;
  struct mloadplan *tcplan = NULL;
  struct mloadplan **planl = NULL;
  unsigned int cc = 0, npcc;
  curmod = mlist;

  while (curmod) {
   struct smodule *tmp = curmod->module;
   if (tmp &&
	   (tmp->rid && !strcmp (tmp->rid, atoms[si])) ||
	   (tmp->provides && strinset (tmp->provides, atoms[si]))) {
	cand[cc] = curmod;
    cc++;
   }
   curmod = curmod->next;
  }
//  printf ("looking for \"%s\": %i candidate(s)\n", atoms[si], cc);

  if (cc) {
   if (mod_plan_sort_by_preference (cand, atoms[si])) {
    return NULL;
   }
   cplan = (struct mloadplan *)ecalloc (1, sizeof (struct mloadplan));
   cplan->task = task;
   cplan->mod = cand[0];

   nplancand = (struct mloadplan **)setadd ((void **)nplancand, (void *)cplan);

   if (cc > 1) {
	unsigned int icc = 1;
    planl = (struct mloadplan **)ecalloc (cc, sizeof (struct mloadplan *));
	cplan->left = planl;
	for (; icc < cc; icc++) {
     tcplan = (struct mloadplan *)ecalloc (1, sizeof (struct mloadplan));
     tcplan->task = task;
     tcplan->mod = cand[icc];
     cplan->left[icc-1] = tcplan;
    }
   }

   if (plan && plan->unsatisfied) {
    plan->unsatisfied = strsetdel (plan->unsatisfied, atoms[si]);
   }
  } else {
   if (plan && plan->unsatisfied && strinset (plan->unsatisfied, atoms [si])) {
    char *tmpa = estrdup (atoms[si]);
    printf ("can't satisfy atom: %s\n", atoms[si]);
    plan->unsatisfied = strsetdel (plan->unsatisfied, atoms[si]);
    plan->unavailable = (char **)setadd ((void **)plan->unavailable, (void *)tmpa);
    return plan;
   }
  }

  free (cand);
 }

 plan->orphaned = (struct mloadplan **)setcombine ((void **)plan->orphaned, (void **)nplancand);

 plan = mod_plan_restructure(plan);
 if (plan && plan->unsatisfied && plan->unsatisfied[0])
  mod_plan (plan, plan->unsatisfied, task);

 return plan;
}

unsigned int mod_plan_commit (struct mloadplan *plan) {
 unsigned int i;
 pthread_t **childthreads = NULL;
 if (!plan) return 1;
 if (!plan->mod) i = STATUS_OK;
 else i = mod(plan->task, plan->mod);
 if (i & STATUS_OK) {
  if (plan->right)
   for (i = 0; plan->right[i]; i++) {
    pthread_t *th = ecalloc (1, sizeof (pthread_t));
    if (!pthread_create (th, NULL, (void * (*)(void *))mod_plan_commit, (void*)plan->right[i])) {
     childthreads = (pthread_t **)setadd ((void **)childthreads, (void *)th);
    }
   }
 } else if (plan->left) {
  for (i = 0; plan->left[i]; i++) {
   pthread_t *th = ecalloc (1, sizeof (pthread_t));
   if (!pthread_create (th, NULL, (void * (*)(void *))mod_plan_commit, (void*)plan->left[i])) {
    childthreads = (pthread_t **)setadd ((void **)childthreads, (void *)th);
   }
  }
 }

 if (childthreads) {
  for (i = 0; childthreads[i]; i++)
   pthread_join (*(childthreads[i]), NULL);
  free (childthreads);
 }

 return STATUS_OK;
 panic:
  bitch (BTCH_ERRNO);
  return STATUS_FAIL;
}


/* collect all elements in the plan */
struct uhash *mod_plan_ga (struct mloadplan *plan, struct uhash *hash) {
 struct uhash *ihash = hash;
 int i;

 if (!plan) return NULL;
 if (plan->left && plan->left[0]) {
  for (i = 0; plan->left[i]; i++)
   ihash = mod_plan_ga(plan->left[i], ihash);
 }
 if (plan->right && plan->right[0]) {
  for (i = 0; plan->right[i]; i++)
   ihash = mod_plan_ga(plan->right[i], ihash);
 }
 if (plan->orphaned && plan->orphaned[0]) {
  for (i = 0; plan->orphaned[i]; i++)
   ihash = mod_plan_ga(plan->orphaned[i], ihash);
 }
 ihash = hashadd (ihash, "tmp", plan);

 return ihash;
}

/* free all elements in the plan */
int mod_plan_free (struct mloadplan *plan) {
 struct uhash *hash_prov;
 struct uhash *d;

 hash_prov = mod_plan_ga (plan, NULL);

 d = hash_prov;
 while (d) {
  if (d->value) {
   struct mloadplan *v = (struct mloadplan *)d->value;
   if (v->right) free (v->right);
   if (v->left) free (v->left);
   if (v->orphaned) free (v->orphaned);
   free (d->value);
   d->value = NULL;
  }
  d = hashnext (d);
 }

 hashfree (hash_prov);
}

#ifdef DEBUG
/* debugging functions: only available if DEBUG is set (obviously...) */
void mod_ls () {
 struct lmodule *cur = mlist;
 do {
  if (cur->module != NULL) {
   if (cur->module->rid)
	fputs (cur->module->rid, stdout);
   if (cur->module->name)
	printf (" (%s)", cur->module->name, stdout);
   puts ("");
  } else
   puts ("(NULL)");
  cur = cur->next;
 } while (cur != NULL);
}

void mod_plan_ls (struct mloadplan *plan) {
 char *rid = "n/a", *name = "unknown", *action;
 static int recursion;
 unsigned char pass = 0;
 struct mloadplan **cur;
 int i;
 if (!recursion) puts ("committing this plan will...");
 if (!plan) return;
 if (plan->mod) {
  if (plan->mod->module) {
   if (plan->mod->module->rid)
	rid = plan->mod->module->rid;
   if (plan->mod->module->name)
    name = plan->mod->module->name;
  }
 }
 for (i = 0; i < recursion; i++)
  fputs (" ", stdout);
 switch (plan->task) {
  case MOD_ENABLE:
   action = "enable"; break;
  case MOD_DISABLE:
   action = "disable"; break;
  default:
   action = "do something with..."; break;
 }
 printf ("%s %s (%s)\n", action, rid, name);
 while (pass < 3) {
  recursion++;
  switch (pass) {
   case 0:
    if (plan->left && plan->left[0]) {
     for (i = 0; i < recursion; i++)
      fputs (" ", stdout);
	 cur = plan->left;
     puts ("on failure {");
	 break;
	}
	pass++;
   case 1:
    if (plan->right && plan->right[0]) {
     for (i = 0; i < recursion; i++)
      fputs (" ", stdout);
	 cur = plan->right;
     puts ("on success {");
	 break;
    }
	pass++;
   case 2:
    if (plan->orphaned && plan->orphaned[0]) {
     for (i = 0; i < recursion; i++)
      fputs (" ", stdout);
	 cur = plan->orphaned;
     puts ("orphans {");
	 break;
    }
	pass++;
   default:
    recursion--;
    goto unsat;
  }

  recursion++;
  for (i = 0; cur[i]; i++) {
   if (cur[i] != plan)
    mod_plan_ls (cur[i]);
   else {
    puts ("Circular dependency detected, aborting.");
	recursion-=2;
    return;
   }
  }
  recursion--;
  for (i = 0; i < recursion; i++)
   fputs (" ", stdout);
  puts ("}");

  pass++;
  recursion--;
 }

 unsat:

 if (plan->unsatisfied && plan->unsatisfied[0]) {
  for (i = -1; i < recursion; i++)
   fputs (" ", stdout);
  for (i = 0; plan->unsatisfied[i]; i++)
   printf ("unsatisfied dependency: %s\n", plan->unsatisfied[i]);
 }

 if (plan->unavailable && plan->unavailable[0]) {
  for (i = -1; i < recursion; i++)
   fputs (" ", stdout);
  for (i = 0; plan->unavailable[i]; i++)
   printf ("unavailable dependency: %s\n", plan->unavailable[i]);
 }
}
#endif
