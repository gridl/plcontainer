#ifndef PLPGJ_MESSAGE_CALLREQ_H
#define PLPGJ_MESSAGE_CALLREQ_H

#include "message_base.h"

typedef struct {
    char *src;  /* source code of the procedure */
    char *name; /* name of procedure */
} procedure;

typedef struct {
    char *name;
    char *value;
    char *type;
} argument;

typedef struct call_req {
    base_message_content;
    procedure proc;
    int       nargs;
    argument *args;
} call_req, *callreq;

/*
  Frees a callreq and all subfields of the struct, this function
  assumes ownership of all pointers in the struct and substructs
*/
void free_callreq(callreq req);

#endif