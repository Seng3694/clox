#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "object.h"
#include "memory.h"
#include "vm.h"

VM vm;

static Value clockNative(int argCount, Value *args)
{
    return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static void resetStack()
{
    vm.stackTop = vm.stack;
    vm.frameCount = 0;
    vm.openUpvalues = NULL;
}

static void runtimeError(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm.frameCount - 1; i >= 0; i--)
    {
        CallFrame *frame = &vm.frames[i];
        ObjFunction *func = frame->closure->function;
        size_t instruction = frame->ip - func->chunk.code - 1;
        fprintf(stderr, "[line %d] in ", func->chunk.lines[instruction]);
        if (func->name == NULL)
        {
            fprintf(stderr, "script\n");
        }
        else
        {
            fprintf(stderr, "%s()\n", func->name->chars);
        }
    }

    resetStack();
}

static void defineNative(const char *name, NativeFn func)
{
    push(OBJ_VAL(copyString(name, (int)strlen(name))));
    push(OBJ_VAL(newNative(func)));
    tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
    pop();
    pop();
}

void initVM()
{
    resetStack();
    vm.objects = NULL;
    vm.bytesAllocated = 0;
    vm.nextGC = 1024 * 1024;
    vm.grayCount = 0;
    vm.grayCapacity = 0;
    vm.grayStack = NULL;
    initTable(&vm.globals);
    initTable(&vm.strings);

    vm.initString = NULL;
    vm.initString = copyString("init", 4);

    defineNative("clock", clockNative);
}

void freeVM()
{
    freeTable(&vm.globals);
    freeTable(&vm.strings);
    vm.initString = NULL;
    freeObjects();
}

void push(Value value)
{
    *vm.stackTop = value;
    vm.stackTop++;
}
Value pop()
{
    vm.stackTop--;
    return *vm.stackTop;
}

static Value peek(int distance)
{
    return vm.stackTop[-1 - distance];
}

static bool call(ObjClosure *closure, int argCount)
{
    if (argCount != closure->function->arity)
    {
        runtimeError(
            "Expected %d arguments but got %d.", closure->function->arity,
            argCount);
        return false;
    }

    if (vm.frameCount == FRAMES_MAX)
    {
        runtimeError("Stack overflow.");
        return false;
    }

    CallFrame *frame = &vm.frames[vm.frameCount++];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
    frame->slots = vm.stackTop - argCount - 1;
    return true;
}

static bool callValue(Value callee, int argCount)
{
    if (IS_OBJ(callee))
    {
        switch (OBJ_TYPE(callee))
        {
        case OBJ_BOUND_METHOD: {
            ObjBoundMethod *bound = AS_BOUND_METHOD(callee);
            vm.stackTop[-argCount - 1] = bound->receiver;
            return call(bound->method, argCount);
        }
        case OBJ_CLASS: {
            ObjClass *klass = AS_CLASS(callee);
            vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));
            Value initializer;
            if (tableGet(&klass->methods, vm.initString, &initializer))
            {
                return call(AS_CLOSURE(initializer), argCount);
            }
            else if (argCount != 0)
            {
                runtimeError("Expected 0 arguments but got %d.", argCount);
                return false;
            }
            return true;
        }
        case OBJ_CLOSURE:
            return call(AS_CLOSURE(callee), argCount);
        case OBJ_NATIVE: {
            NativeFn native = AS_NATIVE(callee);
            Value result = native(argCount, vm.stackTop - argCount);
            vm.stackTop -= argCount + 1;
            push(result);
            return true;
        }

        default:
            break; // non-callable obj type
        }
    }
    runtimeError("Can only call functions and classes.");
    return false;
}

static bool invokeFromClass(ObjClass *klass, ObjString *name, int argCount)
{
    Value method;
    if (!tableGet(&klass->methods, name, &method))
    {
        runtimeError("Undefined property '%s'.", name->chars);
        return false;
    }
    return call(AS_CLOSURE(method), argCount);
}

static bool invoke(ObjString *name, int argCount)
{
    Value receiver = peek(argCount);

    if (!IS_INSTANCE(receiver))
    {
        runtimeError("Only instances have methods.");
        return false;
    }

    ObjInstance *instance = AS_INSTANCE(receiver);

    Value value;
    if (tableGet(&instance->fields, name, &value))
    {
        vm.stackTop[-argCount - 1] = value;
        return callValue(value, argCount);
    }

    return invokeFromClass(instance->klass, name, argCount);
}

static bool bindMethod(ObjClass *klass, ObjString *name)
{
    Value method;
    if (!tableGet(&klass->methods, name, &method))
    {
        runtimeError("Undefined property '%s'.", name->chars);
        return false;
    }

    ObjBoundMethod *bound = newBoundMethod(peek(0), AS_CLOSURE(method));

    pop();
    push(OBJ_VAL(bound));
    return true;
}

static ObjUpvalue *captureUpvalue(Value *local)
{
    ObjUpvalue *prevUpvalue = NULL;
    ObjUpvalue *upvalue = vm.openUpvalues;

    while (upvalue != NULL && upvalue->location > local)
    {
        prevUpvalue = upvalue;
        upvalue = upvalue->next;
    }

    if (upvalue != NULL && upvalue->location == local)
    {
        return upvalue;
    }

    ObjUpvalue *createdUpvalue = newUpvalue(local);
    createdUpvalue->next = upvalue;

    if (prevUpvalue == NULL)
    {
        vm.openUpvalues = createdUpvalue;
    }
    else
    {
        prevUpvalue->next = createdUpvalue;
    }

    return createdUpvalue;
}

static void closeUpvalues(Value *last)
{
    while (vm.openUpvalues != NULL && vm.openUpvalues->location >= last)
    {
        ObjUpvalue *upvalue = vm.openUpvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        vm.openUpvalues = upvalue->next;
    }
}

static void defineMethod(ObjString *name)
{
    Value method = peek(0);
    ObjClass *klass = AS_CLASS(peek(1));
    tableSet(&klass->methods, name, method);
    pop();
}

static bool isFalsey(Value value)
{
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate()
{
    ObjString *b = AS_STRING(peek(0));
    ObjString *a = AS_STRING(peek(1));

    int length = a->length + b->length;
    char *chars = ALLOCATE(char, length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    ObjString *result = takeString(chars, length);
    pop();
    pop();
    push(OBJ_VAL(result));
}

static InterpretResult run()
{
    CallFrame *frame = &vm.frames[vm.frameCount - 1];

    static void *dispatchTable[] = {
        &&instr_op_constant,      &&instr_op_nil,
        &&instr_op_true,          &&instr_op_false,
        &&instr_op_get_upvalue,   &&instr_op_set_upvalue,
        &&instr_op_get_property,  &&instr_op_set_property,
        &&instr_op_get_super,     &&instr_op_equal,
        &&instr_op_pop,           &&instr_op_get_local,
        &&instr_op_set_local,     &&instr_op_get_global,
        &&instr_op_define_global, &&instr_op_set_global,
        &&instr_op_greater,       &&instr_op_less,
        &&instr_op_add,           &&instr_op_subtract,
        &&instr_op_multiply,      &&instr_op_divide,
        &&instr_op_not,           &&instr_op_negate,
        &&instr_op_print,         &&instr_op_jump,
        &&instr_op_jump_if_false, &&instr_op_loop,
        &&instr_op_call,          &&instr_op_invoke,
        &&instr_op_super_invoke,  &&instr_op_closure,
        &&instr_op_close_upvalue, &&instr_op_return,
        &&instr_op_class,         &&instr_op_inherit,
        &&instr_op_method};

#define READ_BYTE() (*frame->ip++)
#define READ_CONSTANT()                                                       \
    (frame->closure->function->chunk.constants.values[READ_BYTE()])
#define READ_SHORT()                                                          \
    (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_STRING() AS_STRING(READ_CONSTANT())
#define BINARY_OP(valueType, op)                                              \
    do                                                                        \
    {                                                                         \
        if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1)))                       \
        {                                                                     \
            runtimeError("Operands must be numbers.");                        \
            return INTERPRET_RUNTIME_ERROR;                                   \
        }                                                                     \
        double b = AS_NUMBER(pop());                                          \
        double a = AS_NUMBER(pop());                                          \
        push(valueType(a op b));                                              \
    } while (false)

#ifdef DEBUG_TRACE_EXECUTION
#define PRINT_TRACE()                                                         \
    do                                                                        \
    {                                                                         \
        printf("          ");                                                 \
        for (Value *slot = vm.stack; slot < vm.stackTop; slot++)              \
        {                                                                     \
            printf("[ ");                                                     \
            printValue(*slot);                                                \
            printf(" ]");                                                     \
        }                                                                     \
        printf("\n");                                                         \
                                                                              \
        disassembleInstruction(                                               \
            &frame->closure->function->chunk,                                 \
            (int)(frame->ip - frame->closure->function->chunk.code));         \
    } while (false)
#else
#define PRINT_TRACE()
#endif

#ifdef DEBUG_TRACE_EXECUTION
#define DISPATCH()                                                            \
    do                                                                        \
    {                                                                         \
        PRINT_TRACE();                                                        \
        goto *dispatchTable[*frame->ip++];                                    \
    } while (false)
#else
#define DISPATCH() goto *dispatchTable[*frame->ip++]
#endif

    DISPATCH();
    for (;;)
    {
    instr_op_constant : {
        Value constant = READ_CONSTANT();
        push(constant);
        DISPATCH();
    }
    instr_op_nil:
        push(NIL_VAL);
        DISPATCH();
    instr_op_true:
        push(BOOL_VAL(true));
        DISPATCH();
    instr_op_false:
        push(BOOL_VAL(false));
        DISPATCH();
    instr_op_pop:
        pop();
        DISPATCH();
    instr_op_get_local : {
        uint8_t slot = READ_BYTE();
        push(frame->slots[slot]);
        DISPATCH();
    }
    instr_op_set_local : {
        uint8_t slot = READ_BYTE();
        frame->slots[slot] = peek(0);
        DISPATCH();
    }
    instr_op_get_global : {
        ObjString *name = READ_STRING();
        Value value;
        if (!tableGet(&vm.globals, name, &value))
        {
            runtimeError("Undefined variable '%s'.", name->chars);
            return INTERPRET_RUNTIME_ERROR;
        }
        push(value);
        DISPATCH();
    }
    instr_op_define_global : {
        ObjString *name = READ_STRING();
        tableSet(&vm.globals, name, peek(0));
        pop();
        DISPATCH();
    }
    instr_op_set_global : {
        ObjString *name = READ_STRING();
        if (tableSet(&vm.globals, name, peek(0)))
        {
            tableDelete(&vm.globals, name);
            runtimeError("Undefined variable '%s'.", name->chars);
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    instr_op_set_upvalue : {
        uint8_t slot = READ_BYTE();
        *frame->closure->upvalues[slot]->location = peek(0);
        DISPATCH();
    }
    instr_op_get_property : {
        if (!IS_INSTANCE(peek(0)))
        {
            runtimeError("Only instances have properties.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjInstance *instance = AS_INSTANCE(peek(0));
        ObjString *name = READ_STRING();

        Value value;
        if (tableGet(&instance->fields, name, &value))
        {
            pop(); // instance
            push(value);
            DISPATCH();
        }

        if (!bindMethod(instance->klass, name))
        {
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    instr_op_set_property : {
        if (!IS_INSTANCE(peek(1)))
        {
            runtimeError("Only instances have fields.");
            return INTERPRET_RUNTIME_ERROR;
        }

        ObjInstance *instance = AS_INSTANCE(peek(1));
        tableSet(&instance->fields, READ_STRING(), peek(0));
        Value value = pop();
        pop();
        push(value);
        DISPATCH();
    }
    instr_op_get_super : {
        ObjString *name = READ_STRING();
        ObjClass *superclass = AS_CLASS(pop());

        if (!bindMethod(superclass, name))
        {
            return INTERPRET_RUNTIME_ERROR;
        }

        DISPATCH();
    }
    instr_op_equal : {
        Value b = pop();
        Value a = pop();
        push(BOOL_VAL(valuesEqual(a, b)));
        DISPATCH();
    }
    instr_op_get_upvalue : {
        uint8_t slot = READ_BYTE();
        push(*frame->closure->upvalues[slot]->location);
        DISPATCH();
    }
    instr_op_greater:
        BINARY_OP(BOOL_VAL, >);
        DISPATCH();
    instr_op_less:
        BINARY_OP(BOOL_VAL, <);
        DISPATCH();
    instr_op_add : {
        if (IS_STRING(peek(0)) && IS_STRING(peek(1)))
        {
            concatenate();
        }
        else if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1)))
        {
            double b = AS_NUMBER(pop());
            double a = AS_NUMBER(pop());
            push(NUMBER_VAL(a + b));
        }
        else
        {
            runtimeError("Operands must be two numbers or two strings.");
            return INTERPRET_RUNTIME_ERROR;
        }
        DISPATCH();
    }
    instr_op_subtract:
        BINARY_OP(NUMBER_VAL, -);
        DISPATCH();
    instr_op_multiply:
        BINARY_OP(NUMBER_VAL, *);
        DISPATCH();
    instr_op_divide:
        BINARY_OP(NUMBER_VAL, /);
        DISPATCH();
    instr_op_not:
        push(BOOL_VAL(isFalsey(pop())));
        DISPATCH();
    instr_op_negate:
        if (!IS_NUMBER(peek(0)))
        {
            runtimeError("Operand must be a number.");
            return INTERPRET_RUNTIME_ERROR;
        }
        push(NUMBER_VAL(-AS_NUMBER(pop())));
        DISPATCH();
    instr_op_print : {
        printValue(pop());
        printf("\n");
        DISPATCH();
    }
    instr_op_jump : {
        uint16_t offset = READ_SHORT();
        frame->ip += offset;
        DISPATCH();
    }
    instr_op_jump_if_false : {
        uint16_t offset = READ_SHORT();
        if (isFalsey(peek(0)))
            frame->ip += offset;
        DISPATCH();
    }
    instr_op_loop : {
        uint16_t offset = READ_SHORT();
        frame->ip -= offset;
        DISPATCH();
    }
    instr_op_call : {
        int argCount = READ_BYTE();
        if (!callValue(peek(argCount), argCount))
        {
            return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        DISPATCH();
    }
    instr_op_invoke : {
        ObjString *method = READ_STRING();
        int argCount = READ_BYTE();
        if (!invoke(method, argCount))
        {
            return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        DISPATCH();
    }
    instr_op_super_invoke : {
        ObjString *method = READ_STRING();
        int argCount = READ_BYTE();
        ObjClass *superclass = AS_CLASS(pop());
        if (!invokeFromClass(superclass, method, argCount))
        {
            return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm.frames[vm.frameCount - 1];
        DISPATCH();
    }
    instr_op_closure : {
        ObjFunction *function = AS_FUNCTION(READ_CONSTANT());
        ObjClosure *closure = newClosure(function);
        push(OBJ_VAL(closure));
        for (int i = 0; i < closure->upvalueCount; i++)
        {
            uint8_t isLocal = READ_BYTE();
            uint8_t index = READ_BYTE();
            if (isLocal)
            {
                closure->upvalues[i] = captureUpvalue(frame->slots + index);
            }
            else
            {
                closure->upvalues[i] = frame->closure->upvalues[index];
            }
        }
        DISPATCH();
    }
    instr_op_close_upvalue : {
        closeUpvalues(vm.stackTop - 1);
        pop();
        DISPATCH();
    }
    instr_op_return : {
        Value result = pop();
        closeUpvalues(frame->slots);
        vm.frameCount--;
        if (vm.frameCount == 0)
        {
            pop();
            return INTERPRET_OK;
        }

        vm.stackTop = frame->slots;
        push(result);
        frame = &vm.frames[vm.frameCount - 1];
        DISPATCH();
    }
    instr_op_class : {
        push(OBJ_VAL(newClass(READ_STRING())));
        DISPATCH();
    }
    instr_op_inherit : {
        Value superclass = peek(1);
        if (!IS_CLASS(superclass))
        {
            runtimeError("Superclass must be a class.");
            return INTERPRET_RUNTIME_ERROR;
        }
        ObjClass *subclass = AS_CLASS(peek(0));
        tableAddAll(&AS_CLASS(superclass)->methods, &subclass->methods);
        pop(); // subclass
        DISPATCH();
    }
    instr_op_method:
        defineMethod(READ_STRING());
        DISPATCH();
    }

#undef PRINT_TRACE
#undef DISPATCH
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

InterpretResult interpret(const char *source)
{
    ObjFunction *function = compile(source);
    if (function == NULL)
        return INTERPRET_COMPILE_ERROR;

    push(OBJ_VAL(function));

    ObjClosure *closure = newClosure(function);
    pop();
    push(OBJ_VAL(closure));
    call(closure, 0);

    return run();
}
