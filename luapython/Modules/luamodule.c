#include <string.h>
#include <stdlib.h>
#include "Python.h"

#include "lua.h"
#include "lualib.h"

/*
** The Lua part implementation of this module.
** This code is run whenever a Lua state is opened, to setup an environment
** suitable for calling Python functions.
** '_LuaPy_callPythonFunction' and '_LuaPy_setErrorMessage' are Lua functions
** defined in this module.
*/
#define LUA_init_state                                                          \
" _PYTHON = {}                                                                " \
" _PYTHON.function_tag = newtag()                                             " \
"                                                                             " \
/* returns a Lua table that represents a Python function */                     \
" _PYTHON.newPythonFunction = function (PyFuncName, PyFuncHandle)             " \
" local f = {}                                                                " \
"    settag(f, _PYTHON.function_tag)                                          " \
"    f.language     = 'python'                                                " \
"    f.PyFuncName   = PyFuncName                                              " \
"    f.PyFuncHandle = PyFuncHandle                                            " \
"    return f                                                                 " \
" end                                                                         " \
"                                                                             " \
" _PYTHON.callPythonFunction = _LuaPy_callPythonFunction                      " \
" _LuaPy_callPythonFunction = nil                                             " \
"                                                                             " \
" _PYTHON.setErrorMessage = _LuaPy_setErrorMessage                            " \
" _LuaPy_setErrorMessage = nil                                                " \
"                                                                             " \
/* intercepts a call to a Python function and passes it to C code */            \
" settagmethod(_PYTHON.function_tag, 'function', function (Pyfunc, ...)       " \
"    if (tag(Pyfunc) ~= _PYTHON.function_tag) then                            " \
"       error('LuaPy: Trying to call a non-Python function as one!')          " \
"    end                                                                      " \
"    tinsert(arg, 1, Pyfunc)                                                  " \
"    return call(_PYTHON.callPythonFunction, arg)                             " \
" end)                                                                        " \
"                                                                             " \
/* intercepts Lua errors and passes it to C code */                             \
" _ALERT = function (error_message)                                           " \
"    _PYTHON.setErrorMessage(error_message)                                   " \
" end                                                                         " \



typedef enum {True=1, False=0} BOOLEAN;

typedef enum {Nothing, Number, String, Function, Pointer, Container, Undefined} VALUE_TYPES;

/* For error communication between Lua and Python */
/* (used by 'Lua_callPythonFunction' and 'Py_lua') */
static BOOLEAN LuaPy_Python_error_ocurred;

static void luaPushPythonFunction(lua_State *L, char *PyFuncName, PyObject *PyFuncHandle);


#define ASSERT(condition, message)                       \
{                                                        \
   if (!(condition)) {                                   \
      fprintf(stderr, "assertion failed: %s", message);  \
      exit(1);                                           \
   }                                                     \
}                                                        \



/**************************************************/
/*** SECTION: Python auxiliar functions (begin) ***/
/**************************************************/

/*
** Maps Python specific types to common types between Lua and Python
*/
static VALUE_TYPES pyGetType(PyObject *value) {
   ASSERT(value, "pyGetType: null value!");
   if (value == Py_None)        return Nothing;
   if (PyNumber_Check(value))   return Number;
   if (PyString_Check(value))   return String;
   if (PyCallable_Check(value)) return Function;
   if (PyCObject_Check(value))  return Pointer;
   if (PyTuple_Check(value))    return Container;
   return Undefined;
}


/*
** Converts 'value' to a tuple, if it isn't already one
*/
static PyObject *pyTryTuple(PyObject *value) {
PyObject *tuple;
   tuple = value;
   if (pyGetType(value) != Container) {
      tuple = PyTuple_New(1);
      PyTuple_SetItem(tuple, 0, value);
   }
   return tuple;
}

/*
** Converts 'tuple' to a single value, if the tuple has only 1 element
*/
static PyObject *pyTryValue(PyObject *tuple) {
PyObject *value;
int tuple_size;
   value = tuple;
   if (pyGetType(tuple) == Container) {
      tuple_size = PyTuple_Size(tuple);
      if (tuple_size == 1) {
         value = PyTuple_GetItem(tuple, 0);
      }
   }
   return value;
}



/*
** Used to put in the Lua stack the elements of a Python tuple.
** This function could be used to pass to Lua the parameters passed
** to a Python function, if calling Lua functions directly from Python
** was implemented, but now it is only used to pass to Lua the returns
** of a Python function.
** If a Python function returns a single value or a tuple, it is passed
** as parameters to Lua. Ony other combination of tuples is not yet implemented.
*/
static BOOLEAN pythonValues_to_luaStack(lua_State *L, PyObject *value, int *_size) {
int n;
int size;
PyObject *tuple;
PyObject *item;
VALUE_TYPES type;
BOOLEAN succeed;

   tuple = pyTryTuple(value);
   size = PyTuple_Size(tuple);

   if (_size) {
      *_size = size;
   }

   succeed = (size > 0)?True:False;

   /* values: Python --> Lua */
   for (n = 0; n < size; n++) {
      item = PyTuple_GetItem(tuple, n);
      type = pyGetType(item);

      switch (type) {

         case (Nothing):
            lua_pushnil(L);
            break;

         case (Number):
            if (PyInt_Check(item)) {
               lua_pushnumber(L, PyInt_AsLong(item));
            } else if (PyFloat_Check(item)) {
               lua_pushnumber(L, PyFloat_AsDouble(item));
            } else {
               lua_pushnil(L);
               PyErr_SetString(PyExc_Exception, "LuaPy: Unknown number type");
               succeed = False;
            }
            break;

         case (String):
            lua_pushstring(L, PyString_AS_STRING(item));
            break;

         case (Function):
            /* ** FALTA IMPLEMENTAR O DECREF CORRESPONDENTE NO GC DE LUA! ** */
            Py_INCREF(item);
            luaPushPythonFunction(L, "unnamed Python function", item);
            break;

         case (Pointer):
            /* ** FALTA IMPLEMENTAR O DECREF CORRESPONDENTE NO GC DE LUA! ** */
            Py_INCREF(item);
            lua_pushuserdata(L, PyCObject_AsVoidPtr(item));
            break;

         case (Container):
            lua_pushnil(L);
            PyErr_SetString(PyExc_Exception, "LuaPy: passing Python tuples to Lua (as a table) is not implemented. Sorry.");
            succeed = False;
            break;

         default:
            lua_pushnil(L);
            PyErr_SetString(PyExc_Exception, "LuaPy: can't translate an unknown type of parameter or return value from Python to Lua");
            succeed = False;
      }

   }

   return succeed;
}


/************************************************/
/*** SECTION: Python auxiliar functions (end) ***/
/************************************************/



/***********************************************/
/*** SECTION: Lua auxiliar functions (begin) ***/
/***********************************************/


/*
** Maps Lua specific types to common types between Lua and Python
*/
static VALUE_TYPES luaGetType(lua_State *L, int stack_pos) {
   if (lua_isnil(L, stack_pos))      return Nothing;
   if (lua_isnumber(L, stack_pos))   return Number;
   if (lua_isstring(L, stack_pos))   return String;
   if (lua_isfunction(L, stack_pos)) return Function;
   if (lua_isuserdata(L, stack_pos)) return Pointer;
   if (lua_istable(L, stack_pos))    return Container;
   return Undefined;
}


/*
** Puts a "Python function" in the Lua stack for later use.
** To Lua, a "Python function" is a table returned by the function
** '_PYTHON.newPythonFunction(PyFuncName, PyFuncHandle)'
*/
static void luaPushPythonFunction(lua_State *L, char *PyFuncName, PyObject *PyFuncHandle) {

   lua_getglobal(L, "_PYTHON");
   lua_pushstring(L, "newPythonFunction");
   lua_gettable(L, -2);

   /* remove "_PYTHON" from the stack */
   lua_remove(L, -2);

   /* call '_PYTHON.newPythonFunction(PyFuncName, PyFuncHandle)' */
   lua_pushstring(L, PyFuncName);
   lua_pushuserdata(L, PyFuncHandle);

   /* puts the Python "function" (a Lua table) in the stack */
   lua_call(L, 2, 1);

}


/*
** sets the Lua global variable 'PyFuncName' to be the Python function 'PyFuncHandle'
*/
static void luaRegisterPythonFunction(lua_State *L, char *PyFuncName, PyObject *PyFuncHandle) {

   luaPushPythonFunction(L, PyFuncName, PyFuncHandle);
   lua_setglobal(L, PyFuncName);
   /* ** FALTA IMPLEMENTAR O DECREF CORRESPONDENTE NO GC DE LUA! ** */
   Py_INCREF(PyFuncHandle);
}


/*
** returns the Lua tag number stored in the Lua global variable
** '_PYTHON.function_tag'
*/
static int luaGetPythonFunctionTag(lua_State *L) {
int tag;

   lua_getglobal(L, "_PYTHON");
   lua_pushstring(L, "function_tag");
   lua_gettable(L, -2);

   tag = lua_tonumber(L, -1);

   /* remove "_PYTHON" and the tag value from the stack */
   lua_pop(L, 2);

   return tag;
}


/*
** given a Lua table returned from '_PYTHON.newPythonFunction' at the stack position
** 'table_index', returns the handler to the Python function.
*/
static void *luaGetPythonFunctionFromLuaTable(lua_State *L, int table_index) {
void *pyFuncHandle;

   lua_pushstring(L, "PyFuncHandle");
   lua_gettable(L, table_index);

   pyFuncHandle = lua_touserdata(L, -1);

   /* remove "PyFuncHandle" from the stack */
   lua_pop(L, 1);

   if (pyFuncHandle == NULL) {
      lua_error(L, "LuaPy: The Lua copy of the Python function has been corrupted!");
   }
   return pyFuncHandle;
}



/*
** Used to create a Python Tuple from the parameters of a Lua function and/or
** to create a Python Tuple from the return values of a Lua function.
** Because the multiple return values are tied together as a Tuple, Python never
** sees Lua functions returning more than 1 value: the Tuple. All values are inside it.
*/
static PyObject *luaStack_to_pythonTuple(lua_State *L, int stack_start, int nelements) {
int n;
PyObject *tuple;
int item;
VALUE_TYPES type;
BOOLEAN succeed;

   tuple = PyTuple_New(nelements);

   succeed = True;

   /* values: Lua --> Python */
   for (n = 0; n < nelements; n++) {
      item = n+stack_start;
      type = luaGetType(L, item);

      switch (type) {

         case (Nothing):
            PyTuple_SetItem(tuple, n, Py_None);
            break;

         case (Number):
            PyTuple_SetItem(tuple, n, PyFloat_FromDouble(lua_tonumber(L, item)));
            break;

         case (String):
            PyTuple_SetItem(tuple, n, PyString_FromString(lua_tostring(L, item)));
            break;

         case (Function):
            PyTuple_SetItem(tuple, n, Py_None);
            PyErr_SetString(PyExc_Exception, "LuaPy: passing Lua functions to Python is not implemented. Sorry.");
            succeed = False;
            break;

         case (Pointer):
            PyTuple_SetItem(tuple, n, PyCObject_FromVoidPtr(lua_touserdata(L, item), NULL));
            break;

         case (Container): {
         PyObject *function;
            if ((lua_tag(L, item) == luaGetPythonFunctionTag(L))) {
               /* lua table that represents a python function --> python function */
               function = luaGetPythonFunctionFromLuaTable(L, item);
               /* ** FALTA IMPLEMENTAR O DECREF CORRESPONDENTE NO GC DE LUA! ** */
               Py_INCREF(function);
               PyTuple_SetItem(tuple, n, function);

            } else {
               PyTuple_SetItem(tuple, n, Py_None);
               PyErr_SetString(PyExc_Exception, "LuaPy: passing Lua tables to Python (as a tuples) is not implemented. Sorry.");
               succeed = False;
            }
            break;
         }

         default:
            PyTuple_SetItem(tuple, n, Py_None);
            PyErr_SetString(PyExc_Exception, "LuaPy: can't translate an unknown type of parameter or return value from Lua to Python");
            succeed = False;
      }

   }

   if (succeed) {
      return tuple;
   } else {
      return NULL;
   }
}

/*********************************************/
/*** SECTION: Lua auxiliar functions (end) ***/
/*********************************************/



/*********************************************/
/*** SECTION: code called from Lua (begin) ***/
/*********************************************/


/*
** Lua prototype: "result1, result2, ..., resultn <-- _PYTHON.callPythonFunction(python_function_table)"
** this function is implemented to be the "function" tag method of Python function -- they are represented
** in Lua as tables.
** Calls a Python function (the 1st argument) passing all other arguments as parameters to it.
** The 1st argument is a Lua table with the same tag of the Lua variable "_PYTHON.function_tag"
** (see 'LUA_init_state') which has the fields 'PyFuncName' and 'PyFuncHandle'.
*/
static int Lua_callPythonFunction(lua_State *L) {
int nparams;
int nreturns;
PyObject *PyFuncHandle;
PyObject *args;
PyObject *result = NULL;

   nparams = lua_gettop(L);

   if (!lua_istable(L, 1)) {
      lua_error(L, "LuaPy: trying to call a non-callable Python Object!!");
   }

   PyFuncHandle = luaGetPythonFunctionFromLuaTable(L, 1);

   if (!PyCallable_Check(PyFuncHandle)) {
      lua_error(L, "LuaPy: trying to call a non-callable Python Object!");
   }

   args = luaStack_to_pythonTuple(L, 2, nparams-1);
   if (args) {
      result = PyEval_CallObject(PyFuncHandle, args);
      Py_DECREF(args);
   } else {
      result = NULL;
   }

   nreturns = 0;
   if (result == NULL) {
      LuaPy_Python_error_ocurred = True;
   } else {
      LuaPy_Python_error_ocurred = False;
      if (pythonValues_to_luaStack(L, result, &nreturns) == False) {
         LuaPy_Python_error_ocurred = True;
      }
      Py_DECREF(result);
   }
   return nreturns;
}


/*
** Lua prototype: "nil <-- _PYTHON.setErrorMessage(message)"
** this function is implemented to substitute the Lua '_ALERT' function.
** Receives a Lua error message (1st argument) and tells it to Python.
*/
static int Lua_setErrorMessage(lua_State *L) {
const char *lua_message;
char *custom_message;
char *custom_prefix = "Lua ";

   lua_message = lua_tostring(L, 1);
   custom_message = malloc(strlen(custom_prefix) + strlen(lua_message) + sizeof('\0'));
   sprintf(custom_message, "%s%s", custom_prefix, lua_message);
   PyErr_SetString(PyExc_Exception, custom_message);
   return 0;
}


/*******************************************/
/*** SECTION: code called from Lua (end) ***/
/*******************************************/



/************************************************/
/*** SECTION: code called from Python (begin) ***/
/************************************************/


/*
** Python prototype: "lua_state <-- lua_open([stack_size])"
** opens a Lua State and returns it to Python
** accepts an optional parameter informing the size of the stack Lua should use
** (measured in number of elements)
*/
static PyObject *Py_lua_open(PyObject *self, PyObject *args) {
lua_State *L;
int stack_size = 0;
PyObject *result = NULL;

   if (PyArg_ParseTuple(args, "|i:lua_open", &stack_size)) {
      L = lua_open(stack_size);
      lua_baselibopen(L);
      lua_iolibopen(L);
      lua_strlibopen(L);
      lua_mathlibopen(L);
      lua_dblibopen(L);

      lua_register(L, "_LuaPy_callPythonFunction", Lua_callPythonFunction);
      lua_register(L, "_LuaPy_setErrorMessage",    Lua_setErrorMessage);
      lua_dostring(L, LUA_init_state);
      result = Py_BuildValue("i", L);
   }
   return result;
}


/*
** Python prototype: "none <-- lua_close(lua_state)"
** closes the 'lua_state'
*/
static PyObject *Py_lua_close(PyObject *self, PyObject *args) {
PyObject *result = NULL;
lua_State *L;

   if (PyArg_ParseTuple(args, "i:lua_close", &L)) {
      lua_close(L);
      Py_INCREF(Py_None);
      result = Py_None;
   }
   return result;
}


/*
** Python prototype: "<tuple|value> <-- lua(lua_code)"
** executes the Lua "dostring" function, returning all return values to Python.
** The Lua return values can be of the type "number", "string", "userdata" or
** "python_function" only (the others are not yet implemented).
** If only 1 value is returned from Lua, it is passed as a single value to
** Python, but if Lua returns 0 or more than 1 values, a tuple is passed to
** Python.
*/
static PyObject *Py_lua(PyObject *self, PyObject *args) {
lua_State *L;
char *lua_code;
int lua_return_code;
int nreturns;
PyObject *result;

   if (PyArg_ParseTuple(args, "is:lua", &L, &lua_code)) {
      lua_return_code = lua_dostring(L, lua_code);
      /* treat errors that Lua can't give its own message */
      if (lua_return_code == LUA_ERRMEM) {
         PyErr_NoMemory();
         return NULL;
      } else if (lua_return_code == LUA_ERRERR) {
         PyErr_SetString(PyExc_Exception, "LuaPy: error in the user-defined Lua error function!");
         return NULL;
      } else if (lua_return_code != 0) {
         /* Lua error ocurred. Set error condition in Python */
         /* (Lua has already told Python what happened -- via 'Lua_setErrorMessage') */
         return NULL;
      }

      nreturns = lua_gettop(L);
      result = pyTryValue(luaStack_to_pythonTuple(L, 1, nreturns));
      /* remove Lua return values from the stack */
      lua_pop(L, nreturns);

   } else {
      return NULL;
   }

   /* watch out the 'LuaPy_Python_error_ocurred' condition, which can change due to */
   /* the execution of 'lua_dostring' */
   if (LuaPy_Python_error_ocurred == False) {
      return result;
   } else {
      return NULL;
   }
}


/*
** Python prototype: "none <-- lua_register(python_function)"
** register (in Lua) a Python function, so it can be called as a normal Lua
** function.
*/
static PyObject *Py_lua_register(PyObject *self, PyObject *args) {
lua_State *L;
char *PyFuncName;
PyObject *PyFuncHandle;
PyObject *result = NULL;

   if (PyArg_ParseTuple(args, "isO:lua_register", &L, &PyFuncName, &PyFuncHandle)) {
      if (!PyCallable_Check(PyFuncHandle)) {
         PyErr_SetString(PyExc_TypeError, "LuaPy: parameter 2 must be callable");
         return NULL;
      }

      luaRegisterPythonFunction(L, PyFuncName, PyFuncHandle);

      Py_INCREF(Py_None);
      result = Py_None;
   }
   return result;
}


/**********************************************/
/*** SECTION: code called from Python (end) ***/
/**********************************************/




static PyMethodDef LuaPy_methods[] = {
        {"lua_open",           Py_lua_open,        METH_VARARGS},
        {"lua_close",          Py_lua_close,       METH_VARARGS},
        {"lua",                Py_lua,             METH_VARARGS},
        {"lua_register",       Py_lua_register,    METH_VARARGS},
        {NULL,      NULL}
};


DL_EXPORT(void) initlua(void)
{
   (void)Py_InitModule("lua", LuaPy_methods);
}
