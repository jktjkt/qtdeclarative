/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/
#ifndef QMLJS_ENVIRONMENT_H
#define QMLJS_ENVIRONMENT_H

#include "qv4global_p.h"
#include "qv4runtime_p.h"

QT_BEGIN_NAMESPACE

namespace QV4 {

struct Object;
struct ExecutionEngine;
struct DeclarativeEnvironment;
struct Lookup;

namespace CompiledData {
struct CompilationUnit;
struct Function;
};

struct CallContext;

struct Q_QML_EXPORT ExecutionContext
{
    enum Type {
        Type_GlobalContext = 0x1,
        Type_CatchContext = 0x2,
        Type_WithContext = 0x3,
        Type_SimpleCallContext = 0x4,
        Type_CallContext = 0x5,
        Type_QmlContext = 0x6
    };

    Type type;
    bool strictMode;
    bool marked;

    Value thisObject;

    ExecutionEngine *engine;
    ExecutionContext *parent;
    ExecutionContext *outer;
    Lookup *lookups;
    String **runtimeStrings;
    CompiledData::CompilationUnit *compilationUnit;
    const CompiledData::Function *compiledFunction;
    ExecutionContext *next; // used in the GC

    struct EvalCode
    {
        Function *function;
        EvalCode *next;
    };
    EvalCode *currentEvalCode;

    const uchar **interpreterInstructionPointer;

    void initBaseContext(Type type, ExecutionEngine *engine, ExecutionContext *parentContext)
    {
        this->type = type;
        strictMode = false;
        marked = false;
        thisObject = Value::undefinedValue();
        this->engine = engine;
        parent = parentContext;
        outer = 0;
        lookups = 0;
        runtimeStrings = 0;
        compilationUnit = 0;
        compiledFunction = 0;
        currentEvalCode = 0;
        interpreterInstructionPointer = 0;
    }

    String * const *formals() const;
    unsigned int formalCount() const;
    String * const *variables() const;
    unsigned int variableCount() const;

    void createMutableBinding(String *name, bool deletable);

    void Q_NORETURN throwError(const Value &value);
    void Q_NORETURN throwError(const QString &message);
    void Q_NORETURN throwSyntaxError(const QString &message);
    void Q_NORETURN throwSyntaxError(const QString &message, const QString &fileName, int line, int column);
    void Q_NORETURN throwTypeError();
    void Q_NORETURN throwTypeError(const QString &message);
    void Q_NORETURN throwReferenceError(Value value);
    void Q_NORETURN throwReferenceError(const QString &value, const QString &fileName, int line, int column);
    void Q_NORETURN throwRangeError(Value value);
    void Q_NORETURN throwURIError(Value msg);
    void Q_NORETURN throwUnimplemented(const QString &message);

    void setProperty(String *name, const Value &value);
    Value getProperty(String *name);
    Value getPropertyNoThrow(String *name);
    Value getPropertyAndBase(String *name, Object **base);
    void inplaceBitOp(String *name, const QV4::Value &value, BinOp op);
    bool deleteProperty(String *name);

    inline Value argument(unsigned int index = 0);

    void mark();

    inline CallContext *asCallContext();
    inline const CallContext *asCallContext() const;
};

struct SimpleCallContext : public ExecutionContext
{
    void initSimpleCallContext(ExecutionEngine *engine);
    FunctionObject *function;
    Value *arguments;
    unsigned int realArgumentCount;
    unsigned int argumentCount;
};

struct CallContext : public SimpleCallContext
{
    void initCallContext(ExecutionContext *parentContext, FunctionObject *function, Value *args, int argc,
                         const Value &thisObject);
    void initQmlContext(ExecutionContext *parentContext, Object *qml, QV4::FunctionObject *function);
    bool needsOwnArguments() const;

    Value *locals;
    Object *activation;
};

struct GlobalContext : public ExecutionContext
{
    void initGlobalContext(ExecutionEngine *e);

    Object *global;
};

struct CatchContext : public ExecutionContext
{
    void initCatchContext(ExecutionContext *p, String *exceptionVarName, const QV4::Value &exceptionValue);

    String *exceptionVarName;
    Value exceptionValue;
};

struct WithContext : public ExecutionContext
{
    Object *withObject;

    void initWithContext(ExecutionContext *p, Object *with);
};

inline Value ExecutionContext::argument(unsigned int index)
{
    if (type >= Type_SimpleCallContext) {
        CallContext *ctx = static_cast<CallContext *>(this);
        if (index < ctx->argumentCount)
            return ctx->arguments[index];
    }
    return Value::undefinedValue();
}

inline CallContext *ExecutionContext::asCallContext()
{
    return type >= Type_CallContext ? static_cast<CallContext *>(this) : 0;
}

inline const CallContext *ExecutionContext::asCallContext() const
{
    return type >= Type_CallContext ? static_cast<const CallContext *>(this) : 0;
}

/* Function *f, int argc */
#define requiredMemoryForExecutionContect(f, argc) \
    sizeof(CallContext) + sizeof(Value) * (f->varCount + qMax((uint)argc, f->formalParameterCount))
#define requiredMemoryForQmlExecutionContect(f) \
    sizeof(CallContext) + sizeof(Value) * (f->locals.size())
#define stackContextSize (sizeof(CallContext) + 32*sizeof(Value))

} // namespace QV4

QT_END_NAMESPACE

#endif