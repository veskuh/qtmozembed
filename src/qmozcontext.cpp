/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-*/
/* vim: set ts=2 sw=2 et tw=79: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#define LOG_COMPONENT "QMozContext"
#include "mozilla/embedlite/EmbedLog.h"

#include <QTimer>
#include <QApplication>
#include <QVariant>
#if (QT_VERSION < QT_VERSION_CHECK(5, 0, 0))
#include <qjson/serializer.h>
#include <qjson/parser.h>
#else
#include <QJsonDocument>
#include <QJsonParseError>
#endif

#include "qmozcontext.h"

#include "nsDebug.h"
#include "mozilla/embedlite/EmbedLiteApp.h"
#include "mozilla/embedlite/EmbedInitGlue.h"
#include "mozilla/embedlite/EmbedLiteView.h"

using namespace mozilla::embedlite;

static QMozContext* protectSingleton = nullptr;

void
GeckoThread::Quit()
{
    if (mEventLoop)
        mEventLoop->quit();
    quit();
    wait();
}

void
GeckoThread::run()
{
    mContext->GetApp()->StartChildThread();
    mEventLoop = new QEventLoop();
    mEventLoop->exec();
    printf("Call Term StopChildThread\n");
    mContext->GetApp()->StopChildThread();
    delete mEventLoop;
    mEventLoop = 0;
}

class QMozContextPrivate : public EmbedLiteAppListener {
public:
    QMozContextPrivate(QMozContext* qq)
    : q(qq)
    , mApp(NULL)
    , mInitialized(false)
    , mThread(new GeckoThread(qq))
    {
    }
    virtual ~QMozContextPrivate() {
        delete mThread;
    }

    virtual bool ExecuteChildThread() {
        if (!getenv("GECKO_THREAD")) {
            LOGT("Execute in child Native thread: %p", mThread);
            mThread->start();
            mThread->setPriority(QThread::LowPriority);
            return true;
        }
        return false;
    }
    // Native thread must be stopped here
    virtual bool StopChildThread() {
        if (mThread) {
            LOGT("Stop Native thread: %p", mThread);
            mThread->Quit();
            return true;
        }
        return false;
    }
    // App Initialized and ready to API call
    virtual void Initialized() {
        mInitialized = true;
        setDefaultPrefs();
        mApp->LoadGlobalStyleSheet("chrome://global/content/embedScrollStyles.css", true);
        Q_EMIT q->onInitialized();
        QListIterator<QString> i(mObserversList);
        while (i.hasNext()) {
            const QString& str = i.next();
            mApp->AddObserver(str.toUtf8().data());
        }
        mObserversList.clear();
        mApp->SendObserve("embed:getdownloadlist", 0);
    }
    // App Destroyed, and ready to delete and program exit
    virtual void Destroyed() {
        LOGT("");
    }
    virtual void OnObserve(const char* aTopic, const PRUnichar* aData) {
        LOGT("aTopic: %s, data: %s", aTopic, NS_ConvertUTF16toUTF8(aData).get());
        NS_ConvertUTF16toUTF8 data(aData);
        bool ok = false;
#if (QT_VERSION < QT_VERSION_CHECK(5, 0, 0))
        QJson::Parser parser;
        QVariant vdata = parser.parse(QByteArray(data.get()), &ok);
#else
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray(data.get()), &error);
        ok = error.error == QJsonParseError::NoError;
        QVariant vdata = doc.toVariant();
#endif

        if (ok) {
            LOGT("mesg:%s, data:%s", aTopic, data.get());
            Q_EMIT q->recvObserve(aTopic, vdata);
        } else {
#if (QT_VERSION < QT_VERSION_CHECK(5, 0, 0))
            LOGT("parse: err:%s, errLine:%i", parser.errorString().toUtf8().data(), parser.errorLine());
#else
            LOGT("parse: err:%s, errLine:%i", error.errorString().toUtf8().data(), error.offset);
#endif
        }
    }
    void setDefaultPrefs()
    {
        if (getenv("DS_UA")) {
            mApp->SetCharPref("general.useragent.override", "Mozilla/5.0 (X11; Linux x86_64; rv:20.0) Gecko/20130124 Firefox/20.0");
        } else if (getenv("CT_UA")) {
            mApp->SetCharPref("general.useragent.override", "Mozilla/5.0 (Linux; Android 4.0.3; Transformer Prime TF201 Build/IML74K) AppleWebKit/535.19 (KHTML, like Gecko) Tablet Chrome/18.0.1025.166 Safari/535.19");
        } else if (getenv("GB_UA")) {
            mApp->SetCharPref("general.useragent.override", "Mozilla/5.0 (Meego; NokiaN9) AppleWebKit/534.13 (KHTML, like Gecko) NokiaBrowser/8.5.0 Mobile Safari/534.13");
        } else {
            const char* customUA = getenv("CUSTOM_UA");
            if (customUA) {
                mApp->SetCharPref("general.useragent.override", customUA);
            }
        }
        mApp->SetBoolPref("layout.build_layers_for_scrollable_views", getenv("USE_SCROLL_VIEWS") != 0);
    }
    bool IsInitialized() { return mApp && mInitialized; }

    virtual uint32_t CreateNewWindowRequested(const uint32_t& chromeFlags, const char* uri, const uint32_t& contextFlags, EmbedLiteView* aParentView)
    {
        LOGT("QtMozEmbedContext new Window requested: parent:%p", aParentView);
        uint32_t retval = QMozContext::GetInstance()->newWindow(QString(), aParentView ? aParentView->GetUniqueID() : 0);
        return retval;
    }

    QList<QString> mObserversList;
private:
    QMozContext* q;
    EmbedLiteApp* mApp;
    bool mInitialized;
    friend class QMozContext;
    friend class GeckoThread;
    GeckoThread* mThread;
};

QMozContext::QMozContext(QObject* parent)
    : QObject(parent)
    , d(new QMozContextPrivate(this))
{
    Q_ASSERT(protectSingleton == nullptr);
    protectSingleton = this;
    LOGT("Create new Context: %p, parent:%p", (void*)this, (void*)parent);
    setenv("BUILD_GRE_HOME", BUILD_GRE_HOME, 1);
    LoadEmbedLite();
    d->mApp = XRE_GetEmbedLite();
    d->mApp->SetListener(d);
    QObject::connect(qApp, SIGNAL(lastWindowClosed()), this, SLOT(onLastWindowClosed()));
    QTimer::singleShot(0, this, SLOT(runEmbedding()));
}

QMozContext::~QMozContext()
{
    protectSingleton = nullptr;
    delete d;
}

void
QMozContext::sendObserve(const QString& aTopic, const QVariant& variant)
{
    if (!d->mApp)
        return;

#if (QT_VERSION < QT_VERSION_CHECK(5, 0, 0))
    QJson::Serializer serializer;
    QByteArray array = serializer.serialize(variant);
#else
    QJsonDocument doc = QJsonDocument::fromVariant(variant);
    QByteArray array = doc.toJson();
#endif

    d->mApp->SendObserve(aTopic.toUtf8().data(), NS_ConvertUTF8toUTF16(array.constData()).get());
}

void
QMozContext::addComponentManifest(const QString& manifestPath)
{
    if (!d->mApp)
        return;
    d->mApp->AddManifestLocation(manifestPath.toUtf8().data());
}

void
QMozContext::addObserver(const QString& aTopic)
{
    if (!d->IsInitialized()) {
        d->mObserversList.append(aTopic);
        return;
    }

    d->mApp->AddObserver(aTopic.toUtf8().data());
}

QMozContext*
QMozContext::GetInstance()
{
    static QMozContext* lsSingleton = nullptr;
    if (!lsSingleton) {
        lsSingleton = new QMozContext();
        NS_ASSERTION(lsSingleton, "not initialized");
    }
    return lsSingleton;
}

void QMozContext::runEmbedding()
{
    d->mApp->Start(EmbedLiteApp::EMBED_THREAD);
}

bool
QMozContext::initialized()
{
    return d->mInitialized;
}

EmbedLiteApp*
QMozContext::GetApp()
{
    return d->mApp;
}

void QMozContext::onLastWindowClosed()
{
    GetApp()->Stop();
}

quint32
QMozContext::newWindow(const QString& url, const quint32& parentId)
{
    quint32 retval = Q_EMIT(this, newWindowRequested(url, parentId));
    return retval;
}

void
QMozContext::setIsAccelerated(bool aIsAccelerated)
{
    if (!d->mApp)
        return;

    d->mApp->SetIsAccelerated(aIsAccelerated);
}

QmlMozContext::QmlMozContext(QObject* parent)
  : QObject(parent)
{
}

QObject*
QmlMozContext::getChild() const
{
    return QMozContext::GetInstance();
}

void
QmlMozContext::setPref(const QString& aName, const QVariant& aPref)
{
    LOGT("name:%s, type:%i", aName.toUtf8().data(), aPref.type());
    mozilla::embedlite::EmbedLiteApp* mApp = QMozContext::GetInstance()->GetApp();
    switch (aPref.type()) {
    case QVariant::String:
        mApp->SetCharPref(aName.toUtf8().data(), aPref.toString().toUtf8().data());
        break;
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
        mApp->SetIntPref(aName.toUtf8().data(), aPref.toInt());
        break;
    case QVariant::Bool:
        mApp->SetBoolPref(aName.toUtf8().data(), aPref.toBool());
        break;
    case QMetaType::Float:
    case QMetaType::Double:
        bool ok;
        if (aPref.canConvert<int>()) {
            mApp->SetIntPref(aName.toUtf8().data(), aPref.toInt());
        } else {
            mApp->SetCharPref(aName.toUtf8().data(), aPref.toString().toUtf8().data());
        }
        break;
    default:
        LOGT("Unknown pref type: %i", aPref.type());
    }
}

void
QmlMozContext::newWindow(const QString& url)
{
    QMozContext::GetInstance()->newWindow(url, 0);
}