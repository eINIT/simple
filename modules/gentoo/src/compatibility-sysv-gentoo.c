/*
 *  compatibility-sysv-gentoo.c
 *  einit
 *
 *  Created by Magnus Deininger on 10/11/2006.
 *  Copyright 2006, 2007 Magnus Deininger. All rights reserved.
 *
 */

/*
Copyright (c) 2006, 2007, Magnus Deininger
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

#define _MODULE

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <einit/module.h>
#include <einit/config.h>
#include <einit/bitch.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <fcntl.h>
#include <einit-modules/exec.h>
#include <einit-modules/parse-sh.h>

#include "../gentoo-baselayout-1.13.0-alpha13/rc.h"

#ifdef POSIXREGEX
#include <regex.h>
#endif

#define EXPECTED_EIV 1

#if EXPECTED_EIV != EINIT_VERSION
#warning "This module was developed for a different version of eINIT, you might experience problems"
#endif

const struct smodule self = {
 .eiversion = EINIT_VERSION,
 .eibuild   = BUILDNUMBER,
 .version   = 1,
 .mode      = EINIT_MOD_LOADER,
 .options   = 0,
 .name      = "System-V Compatibility: Gentoo Support",
 .rid       = "compatibility-sysv-gentoo",
 .si        = {
  .provides = NULL,
  .requires = NULL,
  .after    = NULL,
  .before   = NULL
 }
};

#ifdef POSIXREGEX
struct stree *service_group_transformations = NULL;

struct service_group_transformation {
 char *out;
 regex_t *pattern;
};
#endif


char  do_service_tracking = 0,
     *service_tracking_path = NULL,
      is_gentoo_system = 0,
     *init_d_exec_scriptlet = NULL;
time_t profile_env_mtime = 0;

int scanmodules (struct lmodule *);
int init_d_enable (char *, struct einit_event *);
int init_d_disable (char *, struct einit_event *);
int init_d_reset (char *, struct einit_event *);
int init_d_reload (char *, struct einit_event *);
int configure (struct lmodule *);
int cleanup (struct lmodule *);
void sh_add_environ_callback (char **, uint8_t);
void parse_gentoo_runlevels (char *, struct cfgnode *, char);
void einit_event_handler (struct einit_event *);
void ipc_event_handler (struct einit_event *);
int cleanup_after_module (struct lmodule *);

#define SVCDIR "/lib/rcscripts/init.d"

char svcdir_init_done = 0;

/* functions that module tend to need */
int configure (struct lmodule *irr) {
 exec_configure (irr);
 parse_sh_configure (irr);

 event_listen (EVENT_SUBSYSTEM_EINIT, einit_event_handler);
 event_listen (EVENT_SUBSYSTEM_IPC, ipc_event_handler);
}

int cleanup (struct lmodule *irr) {
 event_ignore (EVENT_SUBSYSTEM_IPC, ipc_event_handler);
 event_ignore (EVENT_SUBSYSTEM_EINIT, einit_event_handler);

 parse_sh_cleanup (irr);
 exec_cleanup(irr);
}

void sh_add_environ_callback (char **data, uint8_t status) {
 char *x, *y;

// if (data)
//  puts (set2str(' ', data));

 if (status == PA_NEW_CONTEXT) {
  if (data && (x = data[0])) {
   if ((!strcmp (data[0], "export") || (x = data[1])) && (y = strchr (x, '='))) {
// if we get here, we got ourselves a variable definition
    struct cfgnode nnode;
    memset (&nnode, 0, sizeof(struct cfgnode));
    char *narb[4] = { "id", x, "s", (y+1) }, *yt = NULL;

    *y = 0; y++;

// exception for the PATH and ROOTPATH variable (gentoo usually mangles that around in /etc/profile)
    if (!strcmp (x, "PATH")) {
     return;
    } else if (!strcmp (x, "ROOTPATH")) {
     x = narb[1] = "PATH";
     yt = emalloc (strlen (y) + 30);
     *yt = 0;
     strcat (yt, "/sbin:/bin:/usr/sbin:/usr/bin");
     strcat (yt, y);
     narb[3] = yt;
    }

    nnode.id = estrdup ("configuration-environment-global");
    nnode.arbattrs = (char **)setdup ((void **)&narb, SET_TYPE_STRING);
    nnode.svalue = nnode.arbattrs[3];
    nnode.source = self.rid;
//    nnode.source_file = "/etc/profile.env";

    cfg_addnode (&nnode);

    if (yt) {
     free (yt);
     yt = NULL;
    }
//    puts (x);
//    puts (y);
   }
  }
 }
}

/* parse gentoo runlevels and add them as nodes to the main configuration */
void parse_gentoo_runlevels (char *path, struct cfgnode *currentmode, char exclusive) {
 DIR *dir = NULL;
 struct dirent *de = NULL;
 uint32_t plen;
 char *tmp = NULL;

 if (!path) return;
 plen = strlen (path) +2;

 if (dir = eopendir (path)) {
  struct stat st;
  char **nservices = NULL;

  while (de = ereaddir (dir)) {
   uint32_t xplen = plen + strlen (de->d_name);

   if (de->d_name[0] == '.') continue;

   tmp = (char *)emalloc (xplen);
   *tmp = 0;
   strcat (tmp, path);
   strcat (tmp, de->d_name);

   if (!stat (tmp, &st) && S_ISDIR (st.st_mode)) {
    struct cfgnode newnode;
    char **arbattrs = NULL;
    char **base = NULL;

    if (strcmp (de->d_name, "boot"))
     base = (char **)setadd ((void **)base, (void *)"boot", SET_TYPE_STRING);

// if not exclusive, merge current mode base with the new base
    if (!exclusive) {
     if ((currentmode = cfg_findnode (de->d_name, EI_NODETYPE_MODE, NULL)) && currentmode->arbattrs) {
      char **curmodebase = NULL;

      uint32_t i = 0;
      for (; currentmode->arbattrs[i]; i+=2) {
       if (!strcmp (currentmode->arbattrs[i], "base")) {
        curmodebase = str2set (':', currentmode->arbattrs[i+1]);
        break;
       }
      }

      if (curmodebase) {
       if (!base) base = curmodebase;
       else {
         for (i = 0; curmodebase[i]; i++) {
         if (!inset ((void **)base, (void *)curmodebase[i], SET_TYPE_STRING)) {
          base = (char **)setadd ((void **)base, (void *)curmodebase[i], SET_TYPE_STRING);
         }
        }
        free (curmodebase);
       }
      }
     }

     currentmode = NULL;
    }

//    fprintf (stderr, " >> new mode: %s\n", de->d_name);

    memset (&newnode, 0, sizeof(struct cfgnode));

    arbattrs = (char **)setadd ((void **)arbattrs, (void *)"id", SET_TYPE_STRING);
    arbattrs = (char **)setadd ((void **)arbattrs, (void *)de->d_name, SET_TYPE_STRING);
    if (base) {
     char *nbase = set2str(':', base);
     if (nbase) {
      arbattrs = (char **)setadd ((void **)arbattrs, (void *)"base", SET_TYPE_STRING);
      arbattrs = (char **)setadd ((void **)arbattrs, (void *)nbase, SET_TYPE_STRING);
      free (nbase);
     }
    }

    newnode.nodetype = EI_NODETYPE_MODE;
    newnode.id = estrdup(arbattrs[1]);
    newnode.source   = self.rid;
    newnode.arbattrs = arbattrs;

    cfg_addnode (&newnode);

    if (currentmode = cfg_findnode (newnode.id, EI_NODETYPE_MODE, NULL)) {
     tmp[xplen-2] = '/';
     tmp[xplen-1] = 0;
     parse_gentoo_runlevels (tmp, currentmode, exclusive);
     tmp[xplen-2] = 0;
     currentmode = NULL;
    }
   } else {
    nservices = (char **) setadd ((void **)nservices, (void *)de->d_name, SET_TYPE_STRING);
   }
  }

  if (nservices) {
#ifdef POSIXREGEX
   if (service_group_transformations) {
    struct stree *cur = service_group_transformations;

    while (cur) {
     struct service_group_transformation *trans =
       (struct service_group_transformation *)cur->value;

     if (trans) {
      char **workingset = (char **)setdup ((void **)nservices, SET_TYPE_STRING);
      char **new_services_for_group = NULL;

      if (workingset) {
       ssize_t ci = 0;
       for (; workingset[ci]; ci++) {
        if (!regexec (trans->pattern, workingset[ci], 0, NULL, 0)) {
         ssize_t ip = 0;
         nservices = strsetdel(nservices, workingset[ci]);
         for (; workingset[ci][ip]; ip++) {
          if (workingset[ci][ip] == '.') workingset[ci][ip] = '-';
         }

         new_services_for_group = (char **)setadd((void **)new_services_for_group, (void *)workingset[ci], SET_TYPE_STRING);
        }
       }
       free (workingset);
      }

      if (new_services_for_group) {
       char *cfgid = emalloc (strlen(cur->key) + 16);
       char curgroup_has_seq_attribute = 0;
       char **arbattrs = NULL;
       struct cfgnode *curgroup = NULL;
       struct cfgnode newnode;

       *cfgid = 0;
       strcat (cfgid, "services-alias-");
       strcat (cfgid, cur->key);

       curgroup = cfg_getnode (cfgid, NULL);
       if (curgroup && curgroup->arbattrs) {
        uint32_t y = 0;
        for (; curgroup->arbattrs[y]; y+= 2) {
         if (!strcmp (curgroup->arbattrs[y], "group")) {
          uint32_t z = 0;
          char **groupmembers = str2set (':', curgroup->arbattrs[y+1]);

          if (groupmembers) {
           for (; groupmembers[z]; z++) {
            if (!inset ((void **)new_services_for_group, (void *)groupmembers[z], SET_TYPE_STRING)) {
             new_services_for_group = (char **)setadd ((void **)new_services_for_group, (void *)groupmembers[z], SET_TYPE_STRING);
            }
           }
           free (groupmembers);
          }

         } else if (!strcmp (curgroup->arbattrs[y], "seq")) {
          curgroup_has_seq_attribute = 1;
          arbattrs = (char **)setadd ((void **)arbattrs, (void *)curgroup->arbattrs[y], SET_TYPE_STRING);
          arbattrs = (char **)setadd ((void **)arbattrs, (void *)curgroup->arbattrs[y+1], SET_TYPE_STRING);
         } else {
          arbattrs = (char **)setadd ((void **)arbattrs, (void *)curgroup->arbattrs[y], SET_TYPE_STRING);
          arbattrs = (char **)setadd ((void **)arbattrs, (void *)curgroup->arbattrs[y+1], SET_TYPE_STRING);
         }
        }
       }

       arbattrs = (char **)setadd ((void **)arbattrs, (void *)"group", SET_TYPE_STRING);
       arbattrs = (char **)setadd ((void **)arbattrs, (void *)set2str (':', new_services_for_group), SET_TYPE_STRING);

       if (!curgroup_has_seq_attribute) {
        arbattrs = (char **)setadd ((void **)arbattrs, (void *)"seq", SET_TYPE_STRING);
        arbattrs = (char **)setadd ((void **)arbattrs, (void *)"most", SET_TYPE_STRING);
       }

       memset (&newnode, 0, sizeof(struct cfgnode));

       newnode.nodetype = EI_NODETYPE_CONFIG;
//       newnode.mode     = currentmode;
       newnode.id       = cfgid;
       newnode.source   = self.rid;
       newnode.arbattrs = arbattrs;

       cfg_addnode (&newnode);

       if (!inset ((void **)nservices, (void *)cur->key, SET_TYPE_STRING))
        nservices = (char **)setadd((void **)nservices, (void *)cur->key, SET_TYPE_STRING);
      }
     }

     cur = streenext(cur);
    }
   }
#endif

   if (nservices && currentmode) {
    char **arbattrs = NULL;
    char **critical;
    struct cfgnode newnode;
    uint32_t y = 0;

    for (; nservices[y]; y++) {
     uint32_t x = 0;
     for (; nservices[y][x]; x++) {
      if (nservices[y][x] == '.') nservices[y][x] = '-';
     }
    }

    if (!exclusive) {
     uint32_t i = 0;
     char **curmodeena = str2set (':', cfg_getstring ("enable/services", currentmode));

     if (curmodeena) {
      for (; curmodeena[i]; i++) {
       if (!inset ((void **)nservices, (void *)curmodeena[i], SET_TYPE_STRING)) {
        nservices = (char **)setadd ((void **)nservices, (void *)curmodeena[i], SET_TYPE_STRING);
       }
      }

//      fprintf (stderr, " >> DEBUG: mode \"%s\": before merge: %s;\n    now: %s\n", currentmode->id, set2str(' ', curmodeena), set2str(' ', nservices));

      free (curmodeena);
     }
    }

    memset (&newnode, 0, sizeof(struct cfgnode));

    arbattrs = (char **)setadd ((void **)arbattrs, (void *)"services", SET_TYPE_STRING);
    arbattrs = (char **)setadd ((void **)arbattrs, (void *)set2str (':', nservices), SET_TYPE_STRING);

    if (critical = str2set (':', cfg_getstring ("enable/critical", currentmode))) {
     arbattrs = (char **)setadd ((void **)arbattrs, (void *)"critical", SET_TYPE_STRING);
     arbattrs = (char **)setadd ((void **)arbattrs, (void *)critical, SET_TYPE_STRING);
    }

    newnode.nodetype = EI_NODETYPE_CONFIG;
    newnode.mode     = currentmode;
    newnode.id       = estrdup("mode-enable");
    newnode.source   = self.rid;
    newnode.arbattrs = arbattrs;

    cfg_addnode (&newnode);
   } else {
    fputs (" >> wtf?.\n", stderr);
   }

   if (nservices)
    free (nservices);
  }

  eclosedir (dir);
 }
}

void einit_event_handler (struct einit_event *ev) {
 if (ev->type == EVE_UPDATE_CONFIGURATION) {
  struct stat st;

  init_d_exec_scriptlet = cfg_getstring("configuration-compatibility-sysv-distribution-gentoo-init.d-scriptlets/execute", NULL);

  char *cs = cfg_getstring("configuration-compatibility-sysv-distribution", NULL);
  if (is_gentoo_system || (cs && ((!strcmp("gentoo", cs)) || ((!strcmp("auto", cs) && !stat("/etc/gentoo-release", &st)))))) {
   if (!is_gentoo_system) {
    is_gentoo_system = 1;
    fputs (" >> gentoo system detected\n", stderr);
    ev->chain_type = EVE_CONFIGURATION_UPDATE;
   }
/* env.d data */
   struct cfgnode *node = cfg_getnode ("configuration-compatibility-sysv-distribution-gentoo-parse-env.d", NULL);
   char *bpath = NULL;

   if (node && node->flag) {
    if (!stat ("/etc/profile.env", &st) && (st.st_mtime > profile_env_mtime)) {
     char *data = readfile ("/etc/profile.env");
     profile_env_mtime = st.st_mtime;

     if (data) {
//      puts ("compatibility-sysv-gentoo: updating configuration with env.d");
      parse_sh (data, sh_add_environ_callback);

      free (data);
     }
     ev->chain_type = EVE_CONFIGURATION_UPDATE;
    }
   }

/* runlevels */
   if (bpath = cfg_getpath ("configuration-compatibility-sysv-distribution-gentoo-runlevels/path")) {
    parse_gentoo_runlevels (bpath, NULL, parse_boolean (cfg_getstring ("configuration-compatibility-sysv-distribution-gentoo-runlevels/exclusive", NULL)));
// need to add checks here for updated configuration
   }

/* service tracker */
   node = cfg_getnode ("configuration-compatibility-sysv-distribution-gentoo-softlevel-tracker", NULL);
   if (do_service_tracking = (node && node->flag)) {
    service_tracking_path = cfg_getpath ("configuration-compatibility-sysv-distribution-gentoo-softlevel-tracker/path");
    if (!service_tracking_path) do_service_tracking = 0;
   }
  }
#ifdef POSIXREGEX
 } else if (ev->type == EVE_CONFIGURATION_UPDATE) {
  struct cfgnode *node = NULL;
  struct stree *new_transformations = NULL, *ca;

  while ((node = cfg_findnode ("configuration-compatibility-sysv-distribution-gentoo-service-group", 0, node))) {
   if (node->arbattrs) {
    struct service_group_transformation new_transformation;
    ssize_t sti = 0;
    char have_pattern = 0;

    memset (&new_transformation, 0, sizeof(struct service_group_transformation));

    for (; node->arbattrs[sti]; sti+=2) {
     if (!strcmp (node->arbattrs[sti], "put-into")) {
      new_transformation.out = node->arbattrs[sti+1];
     } else if (!strcmp (node->arbattrs[sti], "service")) {
      regex_t *buffer = emalloc (sizeof (regex_t));

      if ((have_pattern = !(eregcomp (buffer, node->arbattrs[sti+1])))) {
       new_transformation.pattern = buffer;
      }
     }
    }

    if (have_pattern && new_transformation.out) {
     new_transformations =
       streeadd (new_transformations, new_transformation.out, (void *)(&new_transformation), sizeof(new_transformation), new_transformation.pattern);
    }
   }
  }

  ca = service_group_transformations;
  service_group_transformations = new_transformations;
  if (ca)
   streefree (ca);
#endif
 } else if (ev->type == EVE_PLAN_UPDATE) { // set active "soft mode"
  if (do_service_tracking && ev->string) {
   char tmp[256];
   int slfile;

   snprintf (tmp, 256, "updating softlevel to %s\n", ev->string);
   notice (4, tmp);

   snprintf (tmp, 256, "%ssoftlevel", service_tracking_path);

   if ((slfile = open (tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644)) > 0) {
    write (slfile, amode->id, strlen(ev->string));
    write (slfile, "\n", 1);
    close (slfile);
   } else {
    perror (" >> creating softlevel file");
   }
  }
 }
}

void ipc_event_handler (struct einit_event *ev) {
 if (ev && ev->set && ev->set[0] && ev->set[1] && !strcmp(ev->set[0], "examine") && !strcmp(ev->set[1], "configuration")) {
  if (!cfg_getstring("configuration-compatibility-sysv-distribution-gentoo-init.d/path", NULL)) {
   fputs ("NOTICE: CV \"configuration-compatibility-sysv-distribution-gentoo-init.d/path\":\n  Not found: Gentoo Init Scripts will not be processed. (not a problem)\n", (FILE *)ev->para);
   ev->task++;
  } else if (!cfg_getstring("configuration-compatibility-sysv-distribution-gentoo-init.d-scriptlets/svcdir-init", NULL)) {
   fputs ("NOTICE: CV \"configuration-compatibility-sysv-distribution-gentoo-init.d-scriptlets/svcdir-init\":\n  Not found: Things might go haywire.\n", (FILE *)ev->para);
   ev->task++;
  }

  ev->flag = 1;
 }
}

/* gentoo init.d support functions */

int cleanup_after_module (struct lmodule *this) {
#if 0
 if (this->module) {
 if (this->module->provides)
 free (this->module->provides);
 if (this->module->requires)
 free (this->module->requires);
 if (this->module->notwith)
 free (this->module->notwith);
 free (this->module);
}

 if (this->param) {
 free (this->param);
}
#endif
}

int scanmodules (struct lmodule *modchain) {
 DIR *dir;
 struct dirent *de;
 char *nrid = NULL,
      *init_d_path = cfg_getpath ("configuration-compatibility-sysv-distribution-gentoo-init.d/path"),
      *tmp = NULL;
 uint32_t plen;
 struct smodule *modinfo;
#ifdef POSIXREGEX
 char *spattern;
 regex_t allowpattern, disallowpattern;
 unsigned char haveallowpattern = 0, havedisallowpattern = 0;
#endif

 if (!init_d_path || !is_gentoo_system) {
//  fprintf (stderr, " >> not parsing gentoo scripts: 0x%x, 0x%x, 0x%x\n", init_d_path, init_d_dependency_scriptlet, is_gentoo_system);
  return 0;
 }/* else {
  fprintf (stderr, " >> parsing gentoo scripts\n");
 }*/

 if (!svcdir_init_done && (gmode != EINIT_GMODE_IPCONLY) && (gmode != EINIT_GMODE_SANDBOX)) {
  char *cmd = cfg_getstring ("configuration-compatibility-sysv-distribution-gentoo-init.d-scriptlets/svcdir-init", NULL);

  if (cmd) pexec(cmd, NULL, 0, 0, NULL, NULL, NULL, NULL);
  svcdir_init_done = 1;
 }

#ifdef POSIXREGEX
 if (spattern = cfg_getstring ("configuration-compatibility-sysv-distribution-gentoo-init.d/pattern-allow", NULL)) {
  haveallowpattern = !eregcomp (&allowpattern, spattern);
 }

 if (spattern = cfg_getstring ("configuration-compatibility-sysv-distribution-gentoo-init.d/pattern-disallow", NULL)) {
  havedisallowpattern = !eregcomp (&disallowpattern, spattern);
 }
#endif

 plen = strlen (init_d_path) +1;

 if (dir = eopendir (init_d_path)) {
// load gentoo's default dependency information using librc.so's functions
  rc_depinfo_t *gentoo_deptree = rc_load_deptree(NULL);
  if (!gentoo_deptree) {
   fputs (" >> unable to load gentoo's dependency information.\n", stderr);
  }

#ifdef DEBUG
  puts (" >> reading directory");
#endif
  while (de = ereaddir (dir)) {
   char doop = 1;
   rc_depinfo_t *depinfo;

//   puts (de->d_name);

// filter .- and *.sh-files (or apply regex patterns)
#ifdef POSIXREGEX
   if (haveallowpattern && regexec (&allowpattern, de->d_name, 0, NULL, 0)) continue;
   if (havedisallowpattern && !regexec (&disallowpattern, de->d_name, 0, NULL, 0)) continue;
#else
   if (de->d_name[0] == '.') continue;

   uint32_t xl = strlen(de->d_name);
   if ((xl > 3) && (de->d_name[xl-3] == '.') &&
        (de->d_name[xl-2] == 's') && (de->d_name[xl-1] == 'h')) continue;
#endif

   tmp = (char *)emalloc (plen + strlen (de->d_name));
   struct stat sbuf;
   struct lmodule *lm;
   *tmp = 0;
   strcat (tmp, init_d_path);
   strcat (tmp, de->d_name);
   if (!stat (tmp, &sbuf) && S_ISREG (sbuf.st_mode)) {
    char  tmpx[1024];

    modinfo = emalloc (sizeof (struct smodule));
    memset (modinfo, 0, sizeof(struct smodule));
// make sure this module is only a last resort:
    modinfo->options |= EINIT_MOD_DEPRECATED;

    nrid = emalloc (8 + strlen(de->d_name));
    *nrid = 0;
    strcat (nrid, "gentoo-");
    strcat (nrid, de->d_name);

    snprintf (tmpx, 1024, "Gentoo-Style init.d Script (%s)", de->d_name);
    modinfo->name = estrdup (tmpx);
    modinfo->rid = estrdup(nrid);

    if (depinfo = get_depinfo (gentoo_deptree, de->d_name)) {
     rc_deptype_t *dependencies;
     if (dependencies = get_deptype(depinfo, "ineed")) {
      char *serv = estrdup (dependencies->services);
      ssize_t i = 0; for (; serv[i]; i++) { if (serv[i] == '.') serv[i] = '-'; }
      modinfo->si.requires = str2set (' ', serv);
      free (serv);
     }
     if (dependencies = get_deptype(depinfo, "iprovide")) {
      char *serv = estrdup (dependencies->services);
      ssize_t i = 0; for (; serv[i]; i++) { if (serv[i] == '.') serv[i] = '-'; }
      modinfo->si.provides = str2set (' ', serv);
      free (serv);
     }
     if (dependencies = get_deptype(depinfo, "ibefore")) {
      char *serv = estrdup (dependencies->services);
      ssize_t i = 0; for (; serv[i]; i++) { if (serv[i] == '.') serv[i] = '-'; }
      modinfo->si.before = str2set (' ', serv);
      free (serv);
     }
     if (dependencies = get_deptype(depinfo, "after")) {
      char *serv = estrdup (dependencies->services);
      ssize_t i = 0; for (; serv[i]; i++) { if (serv[i] == '.') serv[i] = '-'; }
      char **tmp = str2set (' ', serv);
      if (tmp) {
       char **bef = modinfo->si.after;
       modinfo->si.after = (char **)setcombine((void **)bef, (void **)tmp, SET_TYPE_STRING);
       if (bef) free (bef);
       free (tmp);
      }
      free (serv);
     }
     if (dependencies = get_deptype(depinfo, "iuse")) {
      char *serv = estrdup (dependencies->services);
      ssize_t i = 0; for (; serv[i]; i++) { if (serv[i] == '.') serv[i] = '-'; }
      char **tmp = str2set (' ', serv);
      if (tmp) {
       char **bef = modinfo->si.after;
       modinfo->si.after = (char **)setcombine((void **)bef, (void **)tmp, SET_TYPE_STRING);
       if (bef) free (bef);
       free (tmp);
      }
      free (serv);
     }
/*     if (dependencies = get_deptype(depinfo, "iafter")) {
      dependencies->services;
     }*/
    } else {
     fprintf (stderr, " >> no dependency information for service \"%s\".\n", de->d_name);
    }

    char *serv = estrdup (nrid + 7);
    ssize_t i = 0; for (; serv[i]; i++) { if (serv[i] == '.') serv[i] = '-'; }
    modinfo->si.after = str2set (' ', serv);
    modinfo->si.provides = (char **)setadd ((void **)modinfo->si.provides, (void *)serv, SET_TYPE_STRING);
    free (serv);

    modinfo->si.requires = (char **)setadd ((void **)modinfo->si.requires, (void *)"utmp", SET_TYPE_STRING);
    modinfo->si.requires = (char **)setadd ((void **)modinfo->si.requires, (void *)"initctl", SET_TYPE_STRING);

    struct lmodule *lm = modchain;
    while (lm) {
     if (lm->source && !strcmp(lm->source, tmp)) {
      lm->param = (void *)estrdup (tmp);
      lm->enable = (int (*)(void *, struct einit_event *))init_d_enable;
      lm->disable = (int (*)(void *, struct einit_event *))init_d_disable;
      lm->reset = (int (*)(void *, struct einit_event *))init_d_reset;
      lm->reload = (int (*)(void *, struct einit_event *))init_d_reload;
      lm->cleanup = cleanup_after_module;
      lm->module = modinfo;

      lm = mod_update (lm);
      doop = 0;
      break;
     }
     lm = lm->next;
    }

    if (doop) {
     struct lmodule *new = mod_add (NULL, modinfo);
     if (new) {
      new->source = estrdup (tmp);
      new->param = (void *)estrdup (tmp);
      new->enable = (int (*)(void *, struct einit_event *))init_d_enable;
      new->disable = (int (*)(void *, struct einit_event *))init_d_disable;
      new->reset = (int (*)(void *, struct einit_event *))init_d_reset;
      new->reload = (int (*)(void *, struct einit_event *))init_d_reload;
      new->cleanup = cleanup_after_module;
     }
    }

   }
   if (nrid) {
    free (nrid);
    nrid = NULL;
   }
   if (tmp) {
    free (tmp);
    tmp = NULL;
   }
  }

  rc_free_deptree (gentoo_deptree);

  eclosedir (dir);
 }

#ifdef POSIXREGEX
 if (haveallowpattern) { haveallowpattern = 0; regfree (&allowpattern); }
 if (havedisallowpattern) { havedisallowpattern = 0; regfree (&disallowpattern); }
#endif
}

// int __pexec_function (char *command, char **variables, uid_t uid, gid_t gid, char *user, char *group, char **local_environment, struct einit_event *status);

int init_d_enable (char *init_script, struct einit_event *status) {
 char *variables[7] = {
  "script-path", init_script,
  "script-name", init_script,
  "action", "start", NULL },
  *cmdscript = NULL,
  *xrev = NULL;

 if (!init_script || !init_d_exec_scriptlet) return STATUS_FAIL;
 if (xrev = strrchr(init_script, '/')) variables[3] = xrev+1;

 cmdscript = apply_variables (init_d_exec_scriptlet, variables);

 return pexec (cmdscript, NULL, 0, 0, NULL, NULL, NULL, status);
}

int init_d_disable (char *init_script, struct einit_event *status) {
 char *variables[7] = {
  "script-path", init_script,
  "script-name", init_script,
  "action", "stop", NULL },
  *cmdscript = NULL,
  *xrev = NULL;

  if (!init_script || !init_d_exec_scriptlet) return STATUS_FAIL;
  if (xrev = strrchr(init_script, '/')) variables[3] = xrev+1;

  cmdscript = apply_variables (init_d_exec_scriptlet, variables);

  return pexec (cmdscript, NULL, 0, 0, NULL, NULL, NULL, status);
}

int init_d_reset (char *init_script, struct einit_event *status) {
 char *variables[7] = {
  "script-path", init_script,
  "script-name", init_script,
  "action", "restart", NULL },
  *cmdscript = NULL,
  *xrev = NULL;

  if (!init_script || !init_d_exec_scriptlet) return STATUS_FAIL;
  if (xrev = strrchr(init_script, '/')) variables[3] = xrev+1;

  cmdscript = apply_variables (init_d_exec_scriptlet, variables);

  return pexec (cmdscript, NULL, 0, 0, NULL, NULL, NULL, status);
}

int init_d_reload (char *init_script, struct einit_event *status) {
 char *variables[7] = {
  "script-path", init_script,
  "script-name", init_script,
  "action", "reload", NULL },
  *cmdscript = NULL,
  *xrev = NULL;

  if (!init_script || !init_d_exec_scriptlet) return STATUS_FAIL;
  if (xrev = strrchr(init_script, '/')) variables[3] = xrev+1;

  cmdscript = apply_variables (init_d_exec_scriptlet, variables);

  return pexec (cmdscript, NULL, 0, 0, NULL, NULL, NULL, status);
}

// no enable/disable functions: this is a passive module
