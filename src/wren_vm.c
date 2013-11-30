#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "wren.h"
#include "wren_common.h"
#include "wren_compiler.h"
#include "wren_vm.h"
#include "wren_core.h"

WrenVM* wrenNewVM(WrenReallocateFn reallocateFn)
{
  // TODO(bob): Get rid of explicit malloc() here.
  WrenVM* vm = reallocateFn(NULL, 0, sizeof(WrenVM));
  initSymbolTable(&vm->methods);
  initSymbolTable(&vm->globalSymbols);

  // TODO(bob): Get rid of explicit malloc() here.
  vm->fiber = malloc(sizeof(Fiber));
  vm->fiber->stackSize = 0;
  vm->fiber->numFrames = 0;
  vm->totalAllocated = 0;

  // TODO(bob): Make this configurable.
  vm->nextGC = 1024 * 1024 * 10;
  vm->first = NULL;

  vm->pinned = NULL;

  // Clear out the global variables. This ensures they are NULL before being
  // initialized in case we do a garbage collection before one gets initialized.
  for (int i = 0; i < MAX_SYMBOLS; i++)
  {
    vm->globals[i] = NULL_VAL;
  }

  wrenInitializeCore(vm);

  return vm;
}

void wrenFreeVM(WrenVM* vm)
{
  clearSymbolTable(&vm->methods);
  clearSymbolTable(&vm->globalSymbols);
  free(vm);
}

int wrenInterpret(WrenVM* vm, const char* source)
{
  ObjFn* fn = wrenCompile(vm, source);
  if (fn == NULL) return 1;

  // TODO(bob): Return error code on runtime errors.
  interpret(vm, fn);
  return 0;
}

static void collectGarbage(WrenVM* vm);

// A generic allocation that handles all memory changes, like so:
//
// - To allocate new memory, [memory] is NULL and [oldSize] is zero.
//
// - To attempt to grow an existing allocation, [memory] is the memory,
//   [oldSize] is its previous size, and [newSize] is the desired size.
//   It returns [memory] if it was able to grow it in place, or a new pointer
//   if it had to move it.
//
// - To shrink memory, [memory], [oldSize], and [newSize] are the same as above
//   but it will always return [memory]. If [newSize] is zero, the memory will
//   be freed and `NULL` will be returned.
//
// - To free memory, [newSize] will be zero.
void* wrenReallocate(WrenVM* vm, void* memory, size_t oldSize, size_t newSize)
{
  ASSERT(memory == NULL || oldSize > 0, "Cannot take unsized previous memory.");

  vm->totalAllocated += newSize - oldSize;

#ifdef DEBUG_GC_STRESS
  if (newSize > oldSize)
  {
    collectGarbage(vm);
  }
#else
  if (vm->totalAllocated > vm->nextGC)
  {
#ifdef TRACE_MEMORY
    size_t before = vm->totalAllocated;
#endif
    collectGarbage(vm);
    vm->nextGC = vm->totalAllocated * 3 / 2;

#ifdef TRACE_MEMORY
    printf("GC %ld before, %ld after (%ld collected), next at %ld\n",
           before, vm->totalAllocated, before - vm->totalAllocated, vm->nextGC);
#endif
  }
#endif

  if (newSize == 0)
  {
    ASSERT(memory != NULL, "Must have pointer to free.");
    free(memory);
    return NULL;
  }

  // TODO(bob): Let external code provide allocator.
  return realloc(memory, newSize);
}

static void markValue(Value value);

static void markFn(ObjFn* fn)
{
  // Don't recurse if already marked. Avoids getting stuck in a loop on cycles.
  if (fn->obj.flags & FLAG_MARKED) return;
  fn->obj.flags |= FLAG_MARKED;

  // Mark the constants.
  for (int i = 0; i < fn->numConstants; i++)
  {
    markValue(fn->constants[i]);
  }
}

static void markClass(ObjClass* classObj)
{
  // Don't recurse if already marked. Avoids getting stuck in a loop on cycles.
  if (classObj->obj.flags & FLAG_MARKED) return;
  classObj->obj.flags |= FLAG_MARKED;

  // The metaclass.
  if (classObj->metaclass != NULL) markClass(classObj->metaclass);

  // The superclass.
  if (classObj->superclass != NULL) markClass(classObj->superclass);

  // Method function objects.
  for (int i = 0; i < MAX_SYMBOLS; i++)
  {
    if (classObj->methods[i].type == METHOD_BLOCK)
    {
      markFn(classObj->methods[i].fn);
    }
  }
}

static void markInstance(ObjInstance* instance)
{
  // Don't recurse if already marked. Avoids getting stuck in a loop on cycles.
  if (instance->obj.flags & FLAG_MARKED) return;
  instance->obj.flags |= FLAG_MARKED;

  markClass(instance->classObj);

  // Mark the fields.
  for (int i = 0; i < instance->classObj->numFields; i++)
  {
    markValue(instance->fields[i]);
  }
}

static void markList(ObjList* list)
{
  Value* elements = list->elements;
  for (int i = 0; i < list->count; i++)
  {
    markValue(elements[i]);
  }
}

static void markObj(Obj* obj)
{
#ifdef TRACE_MEMORY
  static int indent = 0;
  indent++;
  for (int i = 0; i < indent; i++) printf("  ");
  printf("mark ");
  wrenPrintValue(OBJ_VAL(obj));
  printf("\n");
#endif

  // Traverse the object's fields.
  switch (obj->type)
  {
    case OBJ_CLASS: markClass((ObjClass*)obj); break;
    case OBJ_FN: markFn((ObjFn*)obj); break;
    case OBJ_INSTANCE: markInstance((ObjInstance*)obj); break;
    case OBJ_LIST: markList((ObjList*)obj); break;
    case OBJ_STRING:
      // Just mark the string itself.
      obj->flags |= FLAG_MARKED;
      break;
  }

#ifdef TRACE_MEMORY
  indent--;
#endif
}

void markValue(Value value)
{
  if (!IS_OBJ(value)) return;
  markObj(AS_OBJ(value));
}

static void* deallocate(WrenVM* vm, void* memory, size_t oldSize)
{
  return wrenReallocate(vm, memory, oldSize, 0);
}

static void freeObj(WrenVM* vm, Obj* obj)
{
#ifdef TRACE_MEMORY
  printf("free ");
  wrenPrintValue(OBJ_VAL(obj));
#endif

  // Free any additional heap data allocated by the object.
  size_t size;

  switch (obj->type)
  {
    case OBJ_CLASS:
      size = sizeof(ObjClass);
      break;

    case OBJ_FN:
    {
      // TODO(bob): Don't hardcode array sizes.
      size = sizeof(ObjFn);
      ObjFn* fn = (ObjFn*)obj;
      deallocate(vm, fn->bytecode, sizeof(Code) * 1024);
      deallocate(vm, fn->constants, sizeof(Value) * 256);
      break;
    }

    case OBJ_INSTANCE:
    {
      size = sizeof(ObjInstance);

      // Include the size of the field array.
      ObjInstance* instance = (ObjInstance*)obj;
      size += sizeof(Value) * instance->classObj->numFields;
      break;
    }

    case OBJ_LIST:
    {
      size = sizeof(ObjList);
      ObjList* list = (ObjList*)obj;
      if (list->elements != NULL)
      {
        deallocate(vm, list->elements, sizeof(Value) * list->count);
      }
      break;
    }

    case OBJ_STRING:
    {
      size = sizeof(ObjString);
      ObjString* string = (ObjString*)obj;
      // TODO(bob): O(n) calculation here is lame!
      deallocate(vm, string->value, strlen(string->value));
      break;
    }
  }

  deallocate(vm, obj, size);
}

static void collectGarbage(WrenVM* vm)
{
  // Mark all reachable objects.
#ifdef TRACE_MEMORY
  printf("-- gc --\n");
#endif

  // Global variables.
  for (int i = 0; i < vm->globalSymbols.count; i++)
  {
    // Check for NULL to handle globals that have been defined (at compile time)
    // but not yet initialized.
    if (!IS_NULL(vm->globals[i])) markValue(vm->globals[i]);
  }

  // Pinned objects.
  PinnedObj* pinned = vm->pinned;
  while (pinned != NULL)
  {
    markObj(pinned->obj);
    pinned = pinned->previous;
  }

  // Stack functions.
  for (int k = 0; k < vm->fiber->numFrames; k++)
  {
    markFn(vm->fiber->frames[k].fn);
  }

  // Stack variables.
  for (int l = 0; l < vm->fiber->stackSize; l++)
  {
    markValue(vm->fiber->stack[l]);
  }

  // Collect any unmarked objects.
  Obj** obj = &vm->first;
  while (*obj != NULL)
  {
    if (!((*obj)->flags & FLAG_MARKED))
    {
      // This object wasn't reached, so remove it from the list and free it.
      Obj* unreached = *obj;
      *obj = unreached->next;
      freeObj(vm, unreached);
    }
    else
    {
      // This object was reached, so unmark it (for the next GC) and move on to
      // the next.
      (*obj)->flags &= ~FLAG_MARKED;
      obj = &(*obj)->next;
    }
  }
}

void initSymbolTable(SymbolTable* symbols)
{
  symbols->count = 0;
}

void clearSymbolTable(SymbolTable* symbols)
{
  for (int i = 0; i < symbols->count; i++)
  {
    free(symbols->names[i]);
  }
}

void truncateSymbolTable(SymbolTable* symbols, int count)
{
  ASSERT(count <= symbols->count, "Cannot truncate to larger size.");
  for (int i = count; i < symbols->count; i++)
  {
    free(symbols->names[i]);
  }
  symbols->count = count;
}

int addSymbolUnchecked(SymbolTable* symbols, const char* name, size_t length)
{
  // TODO(bob): Get rid of explicit malloc here.
  symbols->names[symbols->count] = malloc(length + 1);
  strncpy(symbols->names[symbols->count], name, length);
  symbols->names[symbols->count][length] = '\0';

  return symbols->count++;
}

int addSymbol(SymbolTable* symbols, const char* name, size_t length)
{
  // If already present, return an error.
  if (findSymbol(symbols, name, length) != -1) return -1;

  return addSymbolUnchecked(symbols, name, length);
}

int ensureSymbol(SymbolTable* symbols, const char* name, size_t length)
{
  // See if the symbol is already defined.
  int existing = findSymbol(symbols, name, length);
  if (existing != -1) return existing;

  // New symbol, so add it.
  return addSymbolUnchecked(symbols, name, length);
}

int findSymbol(SymbolTable* symbols, const char* name, size_t length)
{
  // See if the symbol is already defined.
  // TODO(bob): O(n). Do something better.
  for (int i = 0; i < symbols->count; i++)
  {
    if (strlen(symbols->names[i]) == length &&
        strncmp(symbols->names[i], name, length) == 0) return i;
  }

  return -1;
}

const char* getSymbolName(SymbolTable* symbols, int symbol)
{
  return symbols->names[symbol];
}

Value findGlobal(WrenVM* vm, const char* name)
{
  int symbol = findSymbol(&vm->globalSymbols, name, strlen(name));
  // TODO(bob): Handle failure.
  return vm->globals[symbol];
}

Value interpret(WrenVM* vm, ObjFn* fn)
{
  Fiber* fiber = vm->fiber;
  callFunction(fiber, fn, 0);

  // These macros are designed to only be invoked within this function.
  // TODO(bob): Check for stack overflow.
  #define PUSH(value) (fiber->stack[fiber->stackSize++] = value)
  #define POP()       (fiber->stack[--fiber->stackSize])
  #define PEEK()      (fiber->stack[fiber->stackSize - 1])
  #define READ_ARG()  (frame->ip++, bytecode[ip++])

  // Hoist these into local variables. They are accessed frequently in the loop
  // but change less frequently. Keeping them in locals and updating them when
  // a call frame has been pushed or pop gives a large speed boost.
  CallFrame* frame = &fiber->frames[fiber->numFrames - 1];
  int ip = frame->ip;
  unsigned char* bytecode = frame->fn->bytecode;

  // Use this before a CallFrame is pushed to store the local variables back
  // into the current one.
  #define STORE_FRAME() frame->ip = ip

  // Use this after a CallFrame has been pushed or popped to refresh the local
  // variables.
  #define LOAD_FRAME() \
      frame = &fiber->frames[fiber->numFrames - 1]; \
      ip = frame->ip; \
      bytecode = frame->fn->bytecode \

  #ifdef COMPUTED_GOTOS

  static void* dispatchTable[] = {
    &&code_CONSTANT,
    &&code_NULL,
    &&code_FALSE,
    &&code_TRUE,
    &&code_CLASS,
    &&code_SUBCLASS,
    &&code_METHOD_INSTANCE,
    &&code_METHOD_STATIC,
    &&code_METHOD_CTOR,
    &&code_LIST,
    &&code_LOAD_LOCAL,
    &&code_STORE_LOCAL,
    &&code_LOAD_GLOBAL,
    &&code_STORE_GLOBAL,
    &&code_LOAD_FIELD,
    &&code_STORE_FIELD,
    &&code_DUP,
    &&code_POP,
    &&code_CALL_0,
    &&code_CALL_1,
    &&code_CALL_2,
    &&code_CALL_3,
    &&code_CALL_4,
    &&code_CALL_5,
    &&code_CALL_6,
    &&code_CALL_7,
    &&code_CALL_8,
    &&code_CALL_9,
    &&code_CALL_10,
    &&code_CALL_11,
    &&code_CALL_12,
    &&code_CALL_13,
    &&code_CALL_14,
    &&code_CALL_15,
    &&code_CALL_16,
    &&code_JUMP,
    &&code_LOOP,
    &&code_JUMP_IF,
    &&code_AND,
    &&code_OR,
    &&code_IS,
    &&code_END
  };

  #define INTERPRET_LOOP    DISPATCH();
  #define CASE_CODE(name)   code_##name
  #define DISPATCH()        goto *dispatchTable[instruction = bytecode[ip++]]

  #else

  #define INTERPRET_LOOP    for (;;) switch (instruction = bytecode[ip++])
  #define CASE_CODE(name)   case CODE_##name
  #define DISPATCH()        break

  #endif

  Code instruction;
  INTERPRET_LOOP
  {
    CASE_CODE(CONSTANT):
      PUSH(frame->fn->constants[READ_ARG()]);
      DISPATCH();

    CASE_CODE(NULL):  PUSH(NULL_VAL); DISPATCH();
    CASE_CODE(FALSE): PUSH(FALSE_VAL); DISPATCH();
    CASE_CODE(TRUE):  PUSH(TRUE_VAL); DISPATCH();

    CASE_CODE(CLASS):
    CASE_CODE(SUBCLASS):
    {
      int isSubclass = instruction == CODE_SUBCLASS;
      int numFields = READ_ARG();

      ObjClass* superclass;
      if (isSubclass)
      {
        // TODO(bob): Handle the superclass not being a class object!
        superclass = AS_CLASS(POP());
      }
      else
      {
        // Implicit Object superclass.
        superclass = vm->objectClass;
      }

      ObjClass* classObj = wrenNewClass(vm, superclass, numFields);

      // Assume the first class being defined is Object.
      if (vm->objectClass == NULL)
      {
        vm->objectClass = classObj;
      }

      // Define a "new" method on the metaclass.
      // TODO(bob): Can this be inherited?
      int newSymbol = ensureSymbol(&vm->methods, "new", strlen("new"));
      classObj->metaclass->methods[newSymbol].type = METHOD_CTOR;
      classObj->metaclass->methods[newSymbol].fn = NULL;

      PUSH(OBJ_VAL(classObj));
      DISPATCH();
    }

    CASE_CODE(LIST):
    {
      int numElements = READ_ARG();
      ObjList* list = wrenNewList(vm, numElements);
      for (int i = 0; i < numElements; i++)
      {
        list->elements[i] = fiber->stack[fiber->stackSize - numElements + i];
      }

      // Discard the elements.
      fiber->stackSize -= numElements;

      PUSH(OBJ_VAL(list));
      DISPATCH();
    }

    CASE_CODE(METHOD_INSTANCE):
    CASE_CODE(METHOD_STATIC):
    CASE_CODE(METHOD_CTOR):
    {
      int type = instruction;
      int symbol = READ_ARG();
      int constant = READ_ARG();
      ObjClass* classObj = AS_CLASS(PEEK());

      switch (type)
      {
        case CODE_METHOD_INSTANCE:
          classObj->methods[symbol].type = METHOD_BLOCK;
          break;

        case CODE_METHOD_STATIC:
          // Statics are defined on the metaclass.
          classObj = classObj->metaclass;
          classObj->methods[symbol].type = METHOD_BLOCK;
          break;

        case CODE_METHOD_CTOR:
          // Constructors are like statics.
          classObj = classObj->metaclass;
          classObj->methods[symbol].type = METHOD_CTOR;
          break;
      }

      ObjFn* body = AS_FN(frame->fn->constants[constant]);
      classObj->methods[symbol].fn = body;
      DISPATCH();
    }

    CASE_CODE(LOAD_LOCAL):
    {
      int local = READ_ARG();
      PUSH(fiber->stack[frame->stackStart + local]);
      DISPATCH();
    }

    CASE_CODE(STORE_LOCAL):
    {
      int local = READ_ARG();
      fiber->stack[frame->stackStart + local] = PEEK();
      DISPATCH();
    }

    CASE_CODE(LOAD_GLOBAL):
    {
      int global = READ_ARG();
      PUSH(vm->globals[global]);
      DISPATCH();
    }

    CASE_CODE(STORE_GLOBAL):
    {
      int global = READ_ARG();
      vm->globals[global] = PEEK();
      DISPATCH();
    }

    CASE_CODE(LOAD_FIELD):
    {
      int field = READ_ARG();
      // TODO(bob): We'll have to do something better here to handle functions
      // inside methods.
      Value receiver = fiber->stack[frame->stackStart];
      ASSERT(IS_INSTANCE(receiver), "Receiver should be instance.");
      ObjInstance* instance = AS_INSTANCE(receiver);
      ASSERT(field < instance->classObj->numFields, "Out of bounds field.");
      PUSH(instance->fields[field]);
      DISPATCH();
    }

    CASE_CODE(STORE_FIELD):
    {
      int field = READ_ARG();
      // TODO(bob): We'll have to do something better here to handle functions
      // inside methods.
      Value receiver = fiber->stack[frame->stackStart];
      ASSERT(IS_INSTANCE(receiver), "Receiver should be instance.");
      ObjInstance* instance = AS_INSTANCE(receiver);
      ASSERT(field < instance->classObj->numFields, "Out of bounds field.");
      instance->fields[field] = PEEK();
      DISPATCH();
    }

    CASE_CODE(DUP): PUSH(PEEK()); DISPATCH();
    CASE_CODE(POP): POP(); DISPATCH();

    CASE_CODE(CALL_0):
    CASE_CODE(CALL_1):
    CASE_CODE(CALL_2):
    CASE_CODE(CALL_3):
    CASE_CODE(CALL_4):
    CASE_CODE(CALL_5):
    CASE_CODE(CALL_6):
    CASE_CODE(CALL_7):
    CASE_CODE(CALL_8):
    CASE_CODE(CALL_9):
    CASE_CODE(CALL_10):
    CASE_CODE(CALL_11):
    CASE_CODE(CALL_12):
    CASE_CODE(CALL_13):
    CASE_CODE(CALL_14):
    CASE_CODE(CALL_15):
    CASE_CODE(CALL_16):
    {
      // Add one for the implicit receiver argument.
      int numArgs = instruction - CODE_CALL_0 + 1;
      int symbol = READ_ARG();

      Value receiver = fiber->stack[fiber->stackSize - numArgs];
      ObjClass* classObj = wrenGetClass(vm, receiver);
      Method* method = &classObj->methods[symbol];
      switch (method->type)
      {
        case METHOD_NONE:
          printf("Receiver ");
          wrenPrintValue(receiver);
          printf(" does not implement method \"%s\".\n",
                 vm->methods.names[symbol]);
          // TODO(bob): Throw an exception or halt the fiber or something.
          exit(1);
          break;

        case METHOD_PRIMITIVE:
        {
          Value* args = &fiber->stack[fiber->stackSize - numArgs];
          Value result = method->primitive(vm, args);

          fiber->stack[fiber->stackSize - numArgs] = result;

          // Discard the stack slots for the arguments (but leave one for
          // the result).
          fiber->stackSize -= numArgs - 1;
          break;
        }

        case METHOD_FIBER:
        {
          STORE_FRAME();
          Value* args = &fiber->stack[fiber->stackSize - numArgs];
          method->fiberPrimitive(vm, fiber, args);
          LOAD_FRAME();
          break;
        }

        case METHOD_BLOCK:
          STORE_FRAME();
          callFunction(fiber, method->fn, numArgs);
          LOAD_FRAME();
          break;

        case METHOD_CTOR:
        {
          Value instance = wrenNewInstance(vm, AS_CLASS(receiver));

          // Store the new instance in the receiver slot so that it can be
          // "this" in the body of the constructor and returned by it.
          fiber->stack[fiber->stackSize - numArgs] = instance;

          if (method->fn == NULL)
          {
            // Default constructor, so no body to call. Just discard the
            // stack slots for the arguments (but leave one for the instance).
            fiber->stackSize -= numArgs - 1;
          }
          else
          {
            // Invoke the constructor body.
            STORE_FRAME();
            callFunction(fiber, method->fn, numArgs);
            LOAD_FRAME();
          }
          break;
        }
      }
      DISPATCH();
    }

    CASE_CODE(JUMP):
    {
      int offset = READ_ARG();
      ip += offset;
      DISPATCH();
    }

    CASE_CODE(LOOP):
    {
      // The loop body's result is on the top of the stack. Since we are
      // looping and running the body again, discard it.
      POP();

      // Jump back to the top of the loop.
      int offset = READ_ARG();
      ip -= offset;
      DISPATCH();
    }

    CASE_CODE(JUMP_IF):
    {
      int offset = READ_ARG();
      Value condition = POP();

      // False is the only falsey value.
      if (IS_FALSE(condition)) ip += offset;
      DISPATCH();
    }

    CASE_CODE(AND):
    {
      int offset = READ_ARG();
      Value condition = PEEK();

      // False is the only falsey value.
      if (!IS_FALSE(condition))
      {
        // Discard the condition and evaluate the right hand side.
        POP();
      }
      else
      {
        // Short-circuit the right hand side.
        ip += offset;
      }
      DISPATCH();
    }

    CASE_CODE(OR):
    {
      int offset = READ_ARG();
      Value condition = PEEK();

      // False is the only falsey value.
      if (IS_FALSE(condition))
      {
        // Discard the condition and evaluate the right hand side.
        POP();
      }
      else
      {
        // Short-circuit the right hand side.
        ip += offset;
      }
      DISPATCH();
    }

    CASE_CODE(IS):
    {
      // TODO(bob): What if classObj is not a class?
      ObjClass* expected = AS_CLASS(POP());
      Value obj = POP();

      ObjClass* actual = wrenGetClass(vm, obj);
      int isInstance = 0;

      // Walk the superclass chain looking for the class.
      while (actual != NULL)
      {
        if (actual == expected)
        {
          isInstance = 1;
          break;
        }
        actual = actual->superclass;
      }
      PUSH(BOOL_VAL(isInstance));
      DISPATCH();
    }

    CASE_CODE(END):
    {
      Value result = POP();
      fiber->numFrames--;

      // If we are returning from the top-level block, just return the value.
      if (fiber->numFrames == 0) return result;

      // Store the result of the block in the first slot, which is where the
      // caller expects it.
      fiber->stack[frame->stackStart] = result;

      // Discard the stack slots for the call frame (leaving one slot for the
      // result).
      fiber->stackSize = frame->stackStart + 1;
      LOAD_FRAME();
      DISPATCH();
    }
  }
}

void callFunction(Fiber* fiber, ObjFn* fn, int numArgs)
{
  fiber->frames[fiber->numFrames].fn = fn;
  fiber->frames[fiber->numFrames].ip = 0;
  fiber->frames[fiber->numFrames].stackStart = fiber->stackSize - numArgs;

  // TODO(bob): Check for stack overflow.
  fiber->numFrames++;
}

void pinObj(WrenVM* vm, Obj* obj, PinnedObj* pinned)
{
  pinned->obj = obj;
  pinned->previous = vm->pinned;
  vm->pinned = pinned;
}

void unpinObj(WrenVM* vm)
{
  vm->pinned = vm->pinned->previous;
}