/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/
#ifndef QV4ENGINE_H
#define QV4ENGINE_H

#include "qv4global_p.h"
#include "private/qv4isel_p.h"
#include "qv4managed_p.h"
#include "qv4context_p.h"
#include "qv4typedvalue_p.h"
#include "qv4internalclass_p.h"
#include <private/qintrusivelist_p.h>

namespace WTF {
class BumpPointerAllocator;
class PageAllocation;
}

QT_BEGIN_NAMESPACE

class QV8Engine;
class QQmlError;
class QJSEngine;
class QQmlEngine;

namespace QV4 {
namespace Debugging {
class Debugger;
} // namespace Debugging
namespace Profiling {
class Profiler;
} // namespace Profiling
namespace CompiledData {
struct CompilationUnit;
}

#define CHECK_STACK_LIMITS(v4) \
    if ((v4->jsStackTop <= v4->jsStackLimit) && (reinterpret_cast<quintptr>(&v4) >= v4->cStackLimit || v4->recheckCStackLimits())) {}  \
    else \
        return v4->throwRangeError(QStringLiteral("Maximum call stack size exceeded."))


struct Q_QML_EXPORT ExecutionEngine
{
private:
    friend struct ExecutionContextSaver;
    friend struct ExecutionContext;
    friend struct Heap::ExecutionContext;
public:
    Heap::ExecutionContext *current;
    Heap::ExecutionContext *currentContext() const { return current; }

    Value *jsStackTop;
    quint32 hasException;
    Heap::GlobalContext *m_rootContext;
    Heap::GlobalContext *rootContext() const { return m_rootContext; }

    MemoryManager *memoryManager;
    ExecutableAllocator *executableAllocator;
    ExecutableAllocator *regExpAllocator;
    QScopedPointer<EvalISelFactory> iselFactory;


    Value *jsStackLimit;
    quintptr cStackLimit;

    WTF::BumpPointerAllocator *bumperPointerAllocator; // Used by Yarr Regex engine.

    enum { JSStackLimit = 4*1024*1024 };
    WTF::PageAllocation *jsStack;
    Value *jsStackBase;

    void pushForGC(Heap::Base *m) {
        *jsStackTop = m;
        ++jsStackTop;
    }
    Heap::Base *popForGC() {
        --jsStackTop;
        return jsStackTop->heapObject();
    }
    Value *jsAlloca(int nValues) {
        Value *ptr = jsStackTop;
        jsStackTop = ptr + nValues;
        memset(ptr, 0, nValues*sizeof(Value));
        return ptr;
    }

    IdentifierTable *identifierTable;

    QV4::Debugging::Debugger *debugger;
    QV4::Profiling::Profiler *profiler;

    Object *globalObject;

    Function *globalCode;

    QJSEngine *jsEngine() const;
    QQmlEngine *qmlEngine() const;
    QV8Engine *v8Engine;

    enum JSObjects {
        ObjectProto,
        ArrayProto,
        StringProto,
        NumberProto,
        BooleanProto,
        DateProto,
        FunctionProto,
        RegExpProto,
        ErrorProto,
        EvalErrorProto,
        RangeErrorProto,
        ReferenceErrorProto,
        SyntaxErrorProto,
        TypeErrorProto,
        URIErrorProto,
        VariantProto,
        SequenceProto,
        ArrayBufferProto,
        DataViewProto,

        Object_Ctor,
        String_Ctor,
        Number_Ctor,
        Boolean_Ctor,
        Array_Ctor,
        Function_Ctor,
        Date_Ctor,
        RegExp_Ctor,
        Error_Ctor,
        EvalError_Ctor,
        RangeError_Ctor,
        ReferenceError_Ctor,
        SyntaxError_Ctor,
        TypeError_Ctor,
        URIError_Ctor,
        ArrayBuffer_Ctor,
        DataView_Ctor,

        Eval_Function,
        ThrowerObject,
        NJSObjects
    };
    Value *jsObjects;
    enum { NTypedArrayTypes = 9 }; // == TypedArray::NValues, avoid header dependency

    FunctionObject *objectCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + Object_Ctor); }
    FunctionObject *stringCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + String_Ctor); }
    FunctionObject *numberCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + Number_Ctor); }
    FunctionObject *booleanCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + Boolean_Ctor); }
    FunctionObject *arrayCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + Array_Ctor); }
    FunctionObject *functionCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + Function_Ctor); }
    FunctionObject *dateCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + Date_Ctor); }
    FunctionObject *regExpCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + RegExp_Ctor); }
    FunctionObject *errorCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + Error_Ctor); }
    FunctionObject *evalErrorCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + EvalError_Ctor); }
    FunctionObject *rangeErrorCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + RangeError_Ctor); }
    FunctionObject *referenceErrorCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + ReferenceError_Ctor); }
    FunctionObject *syntaxErrorCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + SyntaxError_Ctor); }
    FunctionObject *typeErrorCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + TypeError_Ctor); }
    FunctionObject *uRIErrorCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + URIError_Ctor); }
    FunctionObject *arrayBufferCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + ArrayBuffer_Ctor); }
    FunctionObject *dataViewCtor() const { return reinterpret_cast<FunctionObject *>(jsObjects + DataView_Ctor); }
    FunctionObject *typedArrayCtors;

    Object *objectPrototype() const { return reinterpret_cast<Object *>(jsObjects + ObjectProto); }
    Object *arrayPrototype() const { return reinterpret_cast<Object *>(jsObjects + ArrayProto); }
    Object *stringPrototype() const { return reinterpret_cast<Object *>(jsObjects + StringProto); }
    Object *numberPrototype() const { return reinterpret_cast<Object *>(jsObjects + NumberProto); }
    Object *booleanPrototype() const { return reinterpret_cast<Object *>(jsObjects + BooleanProto); }
    Object *datePrototype() const { return reinterpret_cast<Object *>(jsObjects + DateProto); }
    Object *functionPrototype() const { return reinterpret_cast<Object *>(jsObjects + FunctionProto); }
    Object *regExpPrototype() const { return reinterpret_cast<Object *>(jsObjects + RegExpProto); }
    Object *errorPrototype() const { return reinterpret_cast<Object *>(jsObjects + ErrorProto); }
    Object *evalErrorPrototype() const { return reinterpret_cast<Object *>(jsObjects + EvalErrorProto); }
    Object *rangeErrorPrototype() const { return reinterpret_cast<Object *>(jsObjects + RangeErrorProto); }
    Object *referenceErrorPrototype() const { return reinterpret_cast<Object *>(jsObjects + ReferenceErrorProto); }
    Object *syntaxErrorPrototype() const { return reinterpret_cast<Object *>(jsObjects + SyntaxErrorProto); }
    Object *typeErrorPrototype() const { return reinterpret_cast<Object *>(jsObjects + TypeErrorProto); }
    Object *uRIErrorPrototype() const { return reinterpret_cast<Object *>(jsObjects + URIErrorProto); }
    Object *variantPrototype() const { return reinterpret_cast<Object *>(jsObjects + VariantProto); }
    Object *sequencePrototype() const { return reinterpret_cast<Object *>(jsObjects + SequenceProto); }

    Object *arrayBufferPrototype() const { return reinterpret_cast<Object *>(jsObjects + ArrayBufferProto); }
    Object *dataViewPrototype() const { return reinterpret_cast<Object *>(jsObjects + DataViewProto); }
    Object *typedArrayPrototype;

    InternalClassPool *classPool;
    InternalClass *emptyClass;

    InternalClass *arrayClass;

    InternalClass *functionClass;
    InternalClass *simpleScriptFunctionClass;
    InternalClass *protoClass;

    InternalClass *regExpExecArrayClass;

    InternalClass *argumentsObjectClass;
    InternalClass *strictArgumentsObjectClass;

    EvalFunction *evalFunction() const { return reinterpret_cast<EvalFunction *>(jsObjects + Eval_Function); }
    FunctionObject *thrower() const { return reinterpret_cast<FunctionObject *>(jsObjects + ThrowerObject); }

    Property *argumentsAccessors;
    int nArgumentsAccessors;

    StringValue id_empty;
    StringValue id_undefined;
    StringValue id_null;
    StringValue id_true;
    StringValue id_false;
    StringValue id_boolean;
    StringValue id_number;
    StringValue id_string;
    StringValue id_object;
    StringValue id_function;
    StringValue id_length;
    StringValue id_prototype;
    StringValue id_constructor;
    StringValue id_arguments;
    StringValue id_caller;
    StringValue id_callee;
    StringValue id_this;
    StringValue id___proto__;
    StringValue id_enumerable;
    StringValue id_configurable;
    StringValue id_writable;
    StringValue id_value;
    StringValue id_get;
    StringValue id_set;
    StringValue id_eval;
    StringValue id_uintMax;
    StringValue id_name;
    StringValue id_index;
    StringValue id_input;
    StringValue id_toString;
    StringValue id_destroy;
    StringValue id_valueOf;
    StringValue id_byteLength;
    StringValue id_byteOffset;
    StringValue id_buffer;
    StringValue id_lastIndex;

    QSet<CompiledData::CompilationUnit*> compilationUnits;

    quint32 m_engineId;

    RegExpCache *regExpCache;

    // Scarce resources are "exceptionally high cost" QVariant types where allowing the
    // normal JavaScript GC to clean them up is likely to lead to out-of-memory or other
    // out-of-resource situations.  When such a resource is passed into JavaScript we
    // add it to the scarceResources list and it is destroyed when we return from the
    // JavaScript execution that created it.  The user can prevent this behavior by
    // calling preserve() on the object which removes it from this scarceResource list.
    class ScarceResourceData {
    public:
        ScarceResourceData(const QVariant &data = QVariant()) : data(data) {}
        QVariant data;
        QIntrusiveListNode node;
    };
    QIntrusiveList<ScarceResourceData, &ScarceResourceData::node> scarceResources;

    // Normally the JS wrappers for QObjects are stored in the QQmlData/QObjectPrivate,
    // but any time a QObject is wrapped a second time in another engine, we have to do
    // bookkeeping.
    MultiplyWrappedQObjectMap *m_multiplyWrappedQObjects;

    ExecutionEngine(EvalISelFactory *iselFactory = 0);
    ~ExecutionEngine();

    void enableDebugger();
    void enableProfiler();

    Heap::ExecutionContext *pushGlobalContext();
    void pushContext(CallContext *context);
    Heap::ExecutionContext *popContext();

    Heap::Object *newObject();
    Heap::Object *newObject(InternalClass *internalClass, Object *prototype);

    Heap::String *newString(const QString &s = QString());
    Heap::String *newIdentifier(const QString &text);

    Heap::Object *newStringObject(const Value &value);
    Heap::Object *newNumberObject(double value);
    Heap::Object *newBooleanObject(bool b);

    Heap::ArrayObject *newArrayObject(int count = 0);
    Heap::ArrayObject *newArrayObject(const QStringList &list);
    Heap::ArrayObject *newArrayObject(InternalClass *ic, Object *prototype);

    Heap::ArrayBuffer *newArrayBuffer(const QByteArray &array);

    Heap::DateObject *newDateObject(const Value &value);
    Heap::DateObject *newDateObject(const QDateTime &dt);

    Heap::RegExpObject *newRegExpObject(const QString &pattern, int flags);
    Heap::RegExpObject *newRegExpObject(RegExp *re, bool global);
    Heap::RegExpObject *newRegExpObject(const QRegExp &re);

    Heap::Object *newErrorObject(const Value &value);
    Heap::Object *newSyntaxErrorObject(const QString &message, const QString &fileName, int line, int column);
    Heap::Object *newSyntaxErrorObject(const QString &message);
    Heap::Object *newReferenceErrorObject(const QString &message);
    Heap::Object *newReferenceErrorObject(const QString &message, const QString &fileName, int lineNumber, int columnNumber);
    Heap::Object *newTypeErrorObject(const QString &message);
    Heap::Object *newRangeErrorObject(const QString &message);
    Heap::Object *newURIErrorObject(const Value &message);

    Heap::Object *newVariantObject(const QVariant &v);

    Heap::Object *newForEachIteratorObject(Object *o);

    Heap::Object *qmlContextObject() const;

    StackTrace stackTrace(int frameLimit = -1) const;
    StackFrame currentStackFrame() const;
    QUrl resolvedUrl(const QString &file);

    void requireArgumentsAccessors(int n);

    void markObjects();

    void initRootContext();

    InternalClass *newClass(const InternalClass &other);

    QmlExtensions *qmlExtensions();

    bool recheckCStackLimits();

    // Exception handling
    Value *exceptionValue;
    StackTrace exceptionStackTrace;

    ReturnedValue throwError(const Value &value);
    ReturnedValue catchException(StackTrace *trace = 0);

    ReturnedValue throwError(const QString &message);
    ReturnedValue throwSyntaxError(const QString &message);
    ReturnedValue throwSyntaxError(const QString &message, const QString &fileName, int lineNumber, int column);
    ReturnedValue throwTypeError();
    ReturnedValue throwTypeError(const QString &message);
    ReturnedValue throwReferenceError(const Value &value);
    ReturnedValue throwReferenceError(const QString &value, const QString &fileName, int lineNumber, int column);
    ReturnedValue throwRangeError(const Value &value);
    ReturnedValue throwRangeError(const QString &message);
    ReturnedValue throwURIError(const Value &msg);
    ReturnedValue throwUnimplemented(const QString &message);

    // Use only inside catch(...) -- will re-throw if no JS exception
    QQmlError catchExceptionAsQmlError();

    // variant conversions
    QVariant toVariant(const QV4::Value &value, int typeHint, bool createJSValueForObjects = true);
    QV4::ReturnedValue fromVariant(const QVariant &);

    QVariantMap variantMapFromJS(QV4::Object *o);

    bool metaTypeFromJS(const Value &value, int type, void *data);
    QV4::ReturnedValue metaTypeToJS(int type, const void *data);

private:
    QmlExtensions *m_qmlExtensions;
};

inline void ExecutionEngine::pushContext(CallContext *context)
{
    Q_ASSERT(current && context && context->d());
    context->d()->parent = current;
    current = context->d();
}

inline Heap::ExecutionContext *ExecutionEngine::popContext()
{
    Q_ASSERT(current->parent);
    current = current->parent;
    Q_ASSERT(current);
    return current;
}

inline
Heap::ExecutionContext::ExecutionContext(ExecutionEngine *engine, ContextType t)
    : engine(engine)
    , parent(engine->currentContext())
    , outer(0)
    , lookups(0)
    , compilationUnit(0)
    , type(t)
    , strictMode(false)
    , lineNumber(-1)
{
    engine->current = this;
}


inline
void Heap::Base::mark(QV4::ExecutionEngine *engine)
{
    Q_ASSERT(inUse());
    if (isMarked())
        return;
    setMarkBit();
    engine->pushForGC(this);
}

inline void Value::mark(ExecutionEngine *e)
{
    if (!val)
        return;
    Managed *m = as<Managed>();
    if (m)
        m->d()->mark(e);
}



} // namespace QV4

QT_END_NAMESPACE

#endif // QV4ENGINE_H
