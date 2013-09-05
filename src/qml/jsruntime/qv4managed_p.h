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
#ifndef QMLJS_MANAGED_H
#define QMLJS_MANAGED_H

#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtCore/QDebug>
#include "qv4global_p.h"
#include "qv4value_def_p.h"

QT_BEGIN_NAMESPACE

namespace QV4 {

#define Q_MANAGED_CHECK \
    template <typename T> inline void qt_check_for_QMANAGED_macro(const T &_q_argument) const \
    { int i = qYouForgotTheQ_MANAGED_Macro(this, &_q_argument); i = i + 1; }

template <typename T>
inline int qYouForgotTheQ_MANAGED_Macro(T, T) { return 0; }

template <typename T1, typename T2>
inline void qYouForgotTheQ_MANAGED_Macro(T1, T2) {}

#define Q_MANAGED \
    public: \
        Q_MANAGED_CHECK \
        static const QV4::ManagedVTable static_vtbl;

struct GCDeletable
{
    GCDeletable() : next(0), lastCall(false) {}
    virtual ~GCDeletable() {}
    GCDeletable *next;
    bool lastCall;
};

struct CallData
{
    // below is to be compatible with Value. Initialize tag to 0
#if Q_BYTE_ORDER != Q_LITTLE_ENDIAN
    uint tag;
#endif
    int argc;
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    uint tag;
#endif

    Value thisObject;
    Value args[1];
};

struct ManagedVTable
{
    Value (*call)(Managed *, const CallData &data);
    Value (*construct)(Managed *, const CallData &data);
    void (*markObjects)(Managed *);
    void (*destroy)(Managed *);
    void (*collectDeletables)(Managed *, GCDeletable **deletable);
    bool (*hasInstance)(Managed *, const Value &value);
    Value (*get)(Managed *, String *name, bool *hasProperty);
    Value (*getIndexed)(Managed *, uint index, bool *hasProperty);
    void (*put)(Managed *, String *name, const Value &value);
    void (*putIndexed)(Managed *, uint index, const Value &value);
    PropertyAttributes (*query)(const Managed *, String *name);
    PropertyAttributes (*queryIndexed)(const Managed *, uint index);
    bool (*deleteProperty)(Managed *m, String *name);
    bool (*deleteIndexedProperty)(Managed *m, uint index);
    void (*getLookup)(Managed *m, Lookup *l, Value *result);
    void (*setLookup)(Managed *m, Lookup *l, const Value &v);
    bool (*isEqualTo)(Managed *m, Managed *other);
    Property *(*advanceIterator)(Managed *m, ObjectIterator *it, String **name, uint *index, PropertyAttributes *attributes);
    const char *className;
};

#define DEFINE_MANAGED_VTABLE(classname) \
const QV4::ManagedVTable classname::static_vtbl =    \
{                                               \
    call,                                       \
    construct,                                  \
    markObjects,                                \
    destroy,                                    \
    0,                                          \
    hasInstance,                                \
    get,                                        \
    getIndexed,                                 \
    put,                                        \
    putIndexed,                                 \
    query,                                      \
    queryIndexed,                               \
    deleteProperty,                             \
    deleteIndexedProperty,                      \
    getLookup,                                  \
    setLookup,                                  \
    isEqualTo,                                  \
    advanceIterator,                            \
    #classname                                  \
}

#define DEFINE_MANAGED_VTABLE_WITH_DELETABLES(classname) \
const QV4::ManagedVTable classname::static_vtbl =    \
{                                               \
    call,                                       \
    construct,                                  \
    markObjects,                                \
    destroy,                                    \
    collectDeletables,                          \
    hasInstance,                                \
    get,                                        \
    getIndexed,                                 \
    put,                                        \
    putIndexed,                                 \
    query,                                      \
    queryIndexed,                               \
    deleteProperty,                             \
    deleteIndexedProperty,                      \
    getLookup,                                  \
    setLookup,                                  \
    isEqualTo,                                  \
    advanceIterator,                            \
    #classname                                  \
}

struct Q_QML_EXPORT Managed
{
private:
    void *operator new(size_t);
    Managed(const Managed &other);
    void operator = (const Managed &other);

protected:
    Managed(InternalClass *internal)
        : _data(0), vtbl(&static_vtbl), internalClass(internal)
    { inUse = 1; extensible = 1; hasAccessorProperty = 0; }

public:
    void *operator new(size_t size, MemoryManager *mm);
    void operator delete(void *ptr);
    void operator delete(void *ptr, MemoryManager *mm);

    inline void mark() {
        if (markBit)
            return;
        markBit = 1;
        if (vtbl->markObjects)
            vtbl->markObjects(this);
    }

    enum Type {
        Type_Invalid,
        Type_String,
        Type_Object,
        Type_ArrayObject,
        Type_FunctionObject,
        Type_BooleanObject,
        Type_NumberObject,
        Type_StringObject,
        Type_DateObject,
        Type_RegExpObject,
        Type_ErrorObject,
        Type_ArgumentsObject,
        Type_JSONObject,
        Type_MathObject,
        Type_ForeachIteratorObject,
        Type_RegExp,

        Type_QmlSequence
    };

    ExecutionEngine *engine() const;

    template <typename T>
    T *as() {
#if !defined(QT_NO_QOBJECT_CHECK)
        reinterpret_cast<T *>(this)->qt_check_for_QMANAGED_macro(*reinterpret_cast<T *>(this));
#endif
        return vtbl == &T::static_vtbl ? static_cast<T *>(this) : 0;
    }
    template <typename T>
    const T *as() const {
#if !defined(QT_NO_QOBJECT_CHECK)
        reinterpret_cast<T *>(this)->qt_check_for_QMANAGED_macro(*reinterpret_cast<T *>(const_cast<Managed *>(this)));
#endif
        return vtbl == &T::static_vtbl ? static_cast<const T *>(this) : 0;
    }

    ArrayObject *asArrayObject() { return type == Type_ArrayObject ? reinterpret_cast<ArrayObject *>(this) : 0; }
    FunctionObject *asFunctionObject() { return type == Type_FunctionObject ? reinterpret_cast<FunctionObject *>(this) : 0; }
    BooleanObject *asBooleanObject() { return type == Type_BooleanObject ? reinterpret_cast<BooleanObject *>(this) : 0; }
    NumberObject *asNumberObject() { return type == Type_NumberObject ? reinterpret_cast<NumberObject *>(this) : 0; }
    StringObject *asStringObject() { return type == Type_StringObject ? reinterpret_cast<StringObject *>(this) : 0; }
    DateObject *asDateObject() { return type == Type_DateObject ? reinterpret_cast<DateObject *>(this) : 0; }
    ErrorObject *asErrorObject() { return type == Type_ErrorObject ? reinterpret_cast<ErrorObject *>(this) : 0; }
    ArgumentsObject *asArgumentsObject() { return type == Type_ArgumentsObject ? reinterpret_cast<ArgumentsObject *>(this) : 0; }

    bool isListType() const { return type == Type_QmlSequence; }

    bool isArrayObject() const { return type == Type_ArrayObject; }
    bool isStringObject() const { return type == Type_StringObject; }

    QString className() const;

    Managed **nextFreeRef() {
        return reinterpret_cast<Managed **>(this);
    }
    Managed *nextFree() {
        return *reinterpret_cast<Managed **>(this);
    }
    void setNextFree(Managed *m) {
        *reinterpret_cast<Managed **>(this) = m;
    }

    inline bool hasInstance(const Value &v) {
        return vtbl->hasInstance(this, v);
    }
    Value construct(const CallData &d);
    Value call(const CallData &d);
    Value get(String *name, bool *hasProperty = 0);
    Value getIndexed(uint index, bool *hasProperty = 0);
    void put(String *name, const Value &value)
    { vtbl->put(this, name, value); }
    void putIndexed(uint index, const Value &value)
    { vtbl->putIndexed(this, index, value); }
    PropertyAttributes query(String *name) const
    { return vtbl->query(this, name); }
    PropertyAttributes queryIndexed(uint index) const
    { return vtbl->queryIndexed(this, index); }

    bool deleteProperty(String *name)
    { return vtbl->deleteProperty(this, name); }
    bool deleteIndexedProperty(uint index)
    { return vtbl->deleteIndexedProperty(this, index); }
    void getLookup(Lookup *l, Value *result)
    { vtbl->getLookup(this, l, result); }
    void setLookup(Lookup *l, const Value &v)
    { vtbl->setLookup(this, l, v); }

    bool isEqualTo(Managed *other)
    { return vtbl->isEqualTo(this, other); }
    Property *advanceIterator(ObjectIterator *it, String **name, uint *index, PropertyAttributes *attributes)
    { return vtbl->advanceIterator(this, it, name, index, attributes); }

    static void destroy(Managed *that) { that->_data = 0; }
    static bool hasInstance(Managed *that, const Value &value);
    static Value construct(Managed *m, const CallData &d);
    static Value call(Managed *m, const CallData &);
    static void getLookup(Managed *m, Lookup *, Value *);
    static void setLookup(Managed *m, Lookup *l, const Value &v);
    static bool isEqualTo(Managed *m, Managed *other);

    uint internalType() const {
        return type;
    }

    union {
        uint _data;
        struct {
            uint markBit :  1;
            uint inUse   :  1;
            uint extensible : 1; // used by Object
            uint isNonStrictArgumentsObject : 1;
            uint isBuiltinFunction : 1; // used by FunctionObject
            uint needsActivation : 1; // used by FunctionObject
            uint usesArgumentsObject : 1; // used by FunctionObject
            uint strictMode : 1; // used by FunctionObject
            uint type : 8;
            mutable uint subtype : 3;
            uint bindingKeyFlag : 1;
            uint hasAccessorProperty : 1;
            uint unused : 11;
        };
    };

protected:

    static const ManagedVTable static_vtbl;

    const ManagedVTable *vtbl;
public:
    InternalClass *internalClass;

private:
    friend class MemoryManager;
    friend struct Identifiers;
    friend struct ObjectIterator;
};

// ### Not a good placement
template<typename T>
inline T *Value::as() const { Managed *m = isObject() ? managed() : 0; return m ? m->as<T>() : 0; }


}


QT_END_NAMESPACE

#endif