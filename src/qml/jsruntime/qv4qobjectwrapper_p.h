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

#ifndef QV4QOBJECTWRAPPER_P_H
#define QV4QOBJECTWRAPPER_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <QtCore/qglobal.h>
#include <QtCore/qmetatype.h>
#include <QtCore/qpair.h>
#include <QtCore/qhash.h>
#include <private/qhashedstring_p.h>
#include <private/qqmldata_p.h>
#include <private/qqmlpropertycache_p.h>
#include <private/qintrusivelist_p.h>

#include <private/qv4value_p.h>
#include <private/qv4functionobject_p.h>

QT_BEGIN_NAMESPACE

class QObject;
class QQmlData;
class QQmlPropertyCache;
class QQmlPropertyData;

namespace QV4 {
struct QObjectSlotDispatcher;

namespace Heap {

struct QObjectWrapper : Object {
    QObjectWrapper(QV4::ExecutionEngine *engine, QObject *object);
    QPointer<QObject> object;
};

struct QObjectMethod : FunctionObject {
    QObjectMethod(QV4::ExecutionContext *scope);
    QPointer<QObject> object;
    QQmlRefPointer<QQmlPropertyCache> propertyCache;
    int index;
    Value qmlGlobal;

    Value valueTypeWrapper;

    const QMetaObject *metaObject();
};

struct QmlSignalHandler : Object {
    QmlSignalHandler(QV4::ExecutionEngine *engine, QObject *object, int signalIndex);
    QPointer<QObject> object;
    int signalIndex;
};

}

struct Q_QML_EXPORT QObjectWrapper : public Object
{
    V4_OBJECT2(QObjectWrapper, Object)

    enum RevisionMode { IgnoreRevision, CheckRevision };

    static void initializeBindings(ExecutionEngine *engine);

    QObject *object() const { return d()->object.data(); }

    ReturnedValue getQmlProperty(QQmlContextData *qmlContext, String *name, RevisionMode revisionMode, bool *hasProperty = 0, bool includeImports = false) const;
    static ReturnedValue getQmlProperty(ExecutionEngine *engine, QQmlContextData *qmlContext, QObject *object, String *name, RevisionMode revisionMode, bool *hasProperty = 0);

    static bool setQmlProperty(ExecutionEngine *engine, QQmlContextData *qmlContext, QObject *object, String *name, RevisionMode revisionMode, const Value &value);

    static ReturnedValue wrap(ExecutionEngine *engine, QObject *object);

    using Object::get;

    static ReturnedValue getProperty(QObject *object, ExecutionContext *ctx, int propertyIndex, bool captureRequired);
    void setProperty(ExecutionContext *ctx, int propertyIndex, const Value &value);

protected:
    static bool isEqualTo(Managed *that, Managed *o);

private:
    static ReturnedValue getProperty(QObject *object, ExecutionContext *ctx, QQmlPropertyData *property, bool captureRequired = true);
    static void setProperty(QObject *object, ExecutionContext *ctx, QQmlPropertyData *property, const Value &value);

    static ReturnedValue create(ExecutionEngine *engine, QObject *object);

    QQmlPropertyData *findProperty(ExecutionEngine *engine, QQmlContextData *qmlContext, String *name, RevisionMode revisionMode, QQmlPropertyData *local) const;

    static ReturnedValue get(const Managed *m, String *name, bool *hasProperty);
    static void put(Managed *m, String *name, const Value &value);
    static PropertyAttributes query(const Managed *, String *name);
    static void advanceIterator(Managed *m, ObjectIterator *it, Heap::String **name, uint *index, Property *p, PropertyAttributes *attributes);
    static void markObjects(Heap::Base *that, QV4::ExecutionEngine *e);
    static void destroy(Heap::Base *that);

    static ReturnedValue method_connect(CallContext *ctx);
    static ReturnedValue method_disconnect(CallContext *ctx);
};

struct QQmlValueTypeWrapper;

struct Q_QML_EXPORT QObjectMethod : public QV4::FunctionObject
{
    V4_OBJECT2(QObjectMethod, QV4::FunctionObject)
    V4_NEEDS_DESTROY

    enum { DestroyMethod = -1, ToStringMethod = -2 };

    static ReturnedValue create(QV4::ExecutionContext *scope, QObject *object, int index, const Value &qmlGlobal = Primitive::undefinedValue());
    static ReturnedValue create(QV4::ExecutionContext *scope, const QQmlValueTypeWrapper *valueType, int index, const Value &qmlGlobal = Primitive::undefinedValue());

    int methodIndex() const { return d()->index; }
    QObject *object() const { return d()->object.data(); }

    QV4::ReturnedValue method_toString(QV4::ExecutionContext *ctx) const;
    QV4::ReturnedValue method_destroy(QV4::ExecutionContext *ctx, const Value *args, int argc) const;

    static ReturnedValue call(const Managed *, CallData *callData);

    ReturnedValue callInternal(CallData *callData) const;

    static void markObjects(Heap::Base *that, QV4::ExecutionEngine *e);
};

struct QmlSignalHandler : public QV4::Object
{
    V4_OBJECT2(QmlSignalHandler, QV4::Object)
    V4_NEEDS_DESTROY

    int signalIndex() const { return d()->signalIndex; }
    QObject *object() const { return d()->object.data(); }
};

class MultiplyWrappedQObjectMap : public QObject,
                                  private QHash<QObject*, Object*>
{
    Q_OBJECT
public:
    typedef QHash<QObject*, Object*>::ConstIterator ConstIterator;
    typedef QHash<QObject*, Object*>::Iterator Iterator;

    ConstIterator begin() const { return QHash<QObject*, Object*>::constBegin(); }
    Iterator begin() { return QHash<QObject*, Object*>::begin(); }
    ConstIterator end() const { return QHash<QObject*, Object*>::constEnd(); }
    Iterator end() { return QHash<QObject*, Object*>::end(); }

    void insert(QObject *key, Object *value);
    Object *value(QObject *key) const { return QHash<QObject*, Object*>::value(key, 0); }
    Iterator erase(Iterator it);
    void remove(QObject *key);

private Q_SLOTS:
    void removeDestroyedObject(QObject*);
};

}

QT_END_NAMESPACE

#endif // QV4QOBJECTWRAPPER_P_H


