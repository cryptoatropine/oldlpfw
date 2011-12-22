/*
---Provided for reference: ---
key_t ftok(const char* path, int id)
int msgget(key_t key, int msgflg)
int msgsnd(int msqid, const void* msgp, size_t msgsz, int msgflg)
ssize_t msgrcv(int msqid, void* msgp, size_t msgsz, long msgtyp, int msgflg)
*/

#include <sys/ipc.h>
#include <sys/msg.h> 
#include <string.h> //strerror, strcpy
#include <errno.h> 
#include "common/includes.h" //for msg_struct
#include "/usr/include/python2.6/Python.h"

static PyObject * ipc_wrapper_ftok(PyObject *self, PyObject *args){
  char* path;
  int id;
 if (!PyArg_ParseTuple(args, "si", &path, &id)) return NULL;
 int retval = ftok(path, id);
 if (retval == -1){return (PyObject*)Py_BuildValue("s",strerror(errno));}
 return (PyObject*)Py_BuildValue("i", retval );
}

static PyObject * ipc_wrapper_msgget(PyObject *self, PyObject *args){
  key_t key;
  int msgflg;
 if (!PyArg_ParseTuple(args, "ii", &key, &msgflg)) return NULL;
  int retval = msgget(key, msgflg);
 if (retval == -1){return (PyObject*)Py_BuildValue("s",strerror(errno));}
 return (PyObject*)Py_BuildValue("i", retval );
}

static char *keywords[] = {"msqid","command","perms","flags","path","pid",NULL};

static PyObject * ipc_wrapper_msgsnd(PyObject *self, PyObject *args, PyObject *pKwds){
  int msqid;
  msg_struct msg;
  int msgflg = 0; //optional argument
  char *perms = ""; //optional argument
  char *path = ""; //optional argument
  char *pid = ""; //optional argument

 //the frontend uses only command field almost always, and very rarely uses perms when adding a rule, and path+pid when deleting a rule; all other fields can remain uninitialized
 if (!PyArg_ParseTupleAndKeywords(args, pKwds, "ii|siss", keywords, &msqid, &msg.item.command, &perms, &msgflg, &path, &pid)) return NULL;
strcpy(msg.item.perms, perms);
strcpy(msg.item.path, path);
strcpy(msg.item.pid, pid);
msg.type = 1; //all messages will have type == 1
#ifdef DEBUG
printf("Doing msgsnd\n");
#endif
int retval = msgsnd(msqid, &msg, sizeof(msg.item), msgflg);
#ifdef DEBUG
printf("Done msgsnd\n");
#endif
 if (retval == -1){return (PyObject*)Py_BuildValue("s",strerror(errno));}
 return (PyObject*)Py_BuildValue("i", retval );
}

static PyObject * ipc_wrapper_msgrcv(PyObject *self, PyObject *args){
 int msqid;
  msg_struct msg;
  int msgflg = 0;//optional argument
 if (!PyArg_ParseTuple(args, "i|i", &msqid, &msgflg)) return NULL;
#ifdef DEBUG
 printf("before msgrcv\n");
#endif
  int retval = msgrcv(msqid, &msg, sizeof(msg.item), 0, msgflg);
#ifdef DEBUG
  printf("after msgrcv %d\n", retval);
#endif
 if (retval == -1){return (PyObject*)Py_BuildValue("s",strerror(errno));}
 // return (PyObject*)Py_BuildValue("i", retval );
 return (PyObject*)Py_BuildValue("issscc", msg.item.command, msg.item.path, msg.item.pid, msg.item.perms, msg.item.is_active, msg.item.first_instance);
}

typedef struct
{
    long type;
    int rulesexp[RULES_EXPORT][3];
} mymsg;

static PyObject * ipc_wrapper_msgrcv_traffic(PyObject *self, PyObject *args){
 int msqid;
  mymsg msg;
 if (!PyArg_ParseTuple(args, "i", &msqid)) return NULL;
#ifdef DEBUG
 printf("before msgrcv\n");
#endif
  int retval = msgrcv(msqid, &msg, sizeof(msg.rulesexp), 0, 0);
#ifdef DEBUG
  printf("after msgrcv %d\n", retval);
#endif
 if (retval == -1){return (PyObject*)Py_BuildValue("s",strerror(errno));}
 PyObject *tuple;
 tuple = PyTuple_New( RULES_EXPORT * 3 * sizeof(u_int32_t));
 int i = 0;
 for (i = 0; msg.rulesexp[i]; ++i)
 {
     PyTuple_SetItem(tuple, i*3, PyInt_FromLong(msg.rulesexp[i][0]));
     PyTuple_SetItem(tuple, i*3+1, PyInt_FromLong(msg.rulesexp[i][1]));
     PyTuple_SetItem(tuple, i*3+2, PyInt_FromLong(msg.rulesexp[i][2]));
 }
 return tuple;
}


static PyMethodDef
ipc_wrapperMethods[] = {
     { "ftok", ipc_wrapper_ftok, METH_VARARGS },
     { "msgget", ipc_wrapper_msgget, METH_VARARGS },
     { "msgsnd", ipc_wrapper_msgsnd, METH_VARARGS | METH_KEYWORDS },
     { "msgrcv", ipc_wrapper_msgrcv, METH_VARARGS },
     { "msgrcv_traffic", ipc_wrapper_msgrcv_traffic, METH_VARARGS },
     { NULL, NULL },
};

 void initipc_wrapper() {
	  Py_InitModule("ipc_wrapper", ipc_wrapperMethods);
    }
    
    
