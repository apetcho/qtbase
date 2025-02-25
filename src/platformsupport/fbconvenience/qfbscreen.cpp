// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qfbscreen_p.h"
#include "qfbcursor_p.h"
#include "qfbwindow_p.h"
#include "qfbbackingstore_p.h"

#include <QtGui/QPainter>
#include <QtCore/QCoreApplication>
#include <qpa/qwindowsysteminterface.h>

#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>

QT_BEGIN_NAMESPACE

QFbScreen::QFbScreen()
    : mUpdatePending(false),
      mCursor(0),
      mDepth(16),
      mFormat(QImage::Format_RGB16),
      mPainter(nullptr)
{
}

QFbScreen::~QFbScreen()
{
    delete mPainter;
}

void QFbScreen::initializeCompositor()
{
    mScreenImage = QImage(mGeometry.size(), mFormat);
    scheduleUpdate();
}

bool QFbScreen::event(QEvent *event)
{
    if (event->type() == QEvent::UpdateRequest) {
        doRedraw();
        mUpdatePending = false;
        return true;
    }
    return QObject::event(event);
}

void QFbScreen::addWindow(QFbWindow *window)
{
    Q_ASSERT(window->backingStore());
    mWindowStack.prepend(window);
    if (!mPendingBackingStores.isEmpty()) {
        //check if we have a backing store for this window
        for (int i = 0; i < mPendingBackingStores.size(); ++i) {
            QFbBackingStore *bs = mPendingBackingStores.at(i);
            // this gets called during QWindow::create() at a point where the
            // invariant (window->handle()->window() == window) is broken
            if (bs->window() == window->window()) {
                window->setBackingStore(bs);
                mPendingBackingStores.removeAt(i);
                break;
            }
        }
    }
    setDirty(window->geometry());
    QWindow *w = topWindow();
    QWindowSystemInterface::handleWindowActivated(w);
    topWindowChanged(w);
}

void QFbScreen::removeWindow(QFbWindow *window)
{
    mWindowStack.removeOne(window);
    setDirty(window->geometry());
    QWindow *w = topWindow();
    QWindowSystemInterface::handleWindowActivated(w);
    topWindowChanged(w);
}

void QFbScreen::raise(QFbWindow *window)
{
    int index = mWindowStack.indexOf(window);
    if (index <= 0)
        return;
    mWindowStack.move(index, 0);
    setDirty(window->geometry());
    QWindow *w = topWindow();
    QWindowSystemInterface::handleWindowActivated(w);
    topWindowChanged(w);
}

void QFbScreen::lower(QFbWindow *window)
{
    int index = mWindowStack.indexOf(window);
    if (index == -1 || index == (mWindowStack.size() - 1))
        return;
    mWindowStack.move(index, mWindowStack.size() - 1);
    setDirty(window->geometry());
    QWindow *w = topWindow();
    QWindowSystemInterface::handleWindowActivated(w);
    topWindowChanged(w);
}

QWindow *QFbScreen::topWindow() const
{
    for (QFbWindow *fbw : mWindowStack) {
        if (fbw->window()->type() == Qt::Window || fbw->window()->type() == Qt::Dialog)
            return fbw->window();
    }
    return nullptr;
}

QWindow *QFbScreen::topLevelAt(const QPoint & p) const
{
    for (QFbWindow *fbw : mWindowStack) {
        if (fbw->geometry().contains(p, false) && fbw->window()->isVisible())
            return fbw->window();
    }
    return nullptr;
}

int QFbScreen::windowCount() const
{
    return mWindowStack.size();
}

void QFbScreen::setDirty(const QRect &rect)
{
    const QRect intersection = rect.intersected(mGeometry);
    const QPoint screenOffset = mGeometry.topLeft();
    mRepaintRegion += intersection.translated(-screenOffset); // global to local translation
    scheduleUpdate();
}

void QFbScreen::scheduleUpdate()
{
    if (!mUpdatePending) {
        mUpdatePending = true;
        QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
    }
}

void QFbScreen::setPhysicalSize(const QSize &size)
{
    mPhysicalSize = size;
}

void QFbScreen::setGeometry(const QRect &rect)
{
    delete mPainter;
    mPainter = nullptr;
    mGeometry = rect;
    mScreenImage = QImage(mGeometry.size(), mFormat);
    QWindowSystemInterface::handleScreenGeometryChange(QPlatformScreen::screen(), geometry(), availableGeometry());
    resizeMaximizedWindows();
}

bool QFbScreen::initialize()
{
    return true;
}

QRegion QFbScreen::doRedraw()
{
    const QPoint screenOffset = mGeometry.topLeft();

    QRegion touchedRegion;
    if (mCursor && mCursor->isDirty() && mCursor->isOnScreen()) {
        const QRect lastCursor = mCursor->dirtyRect();
        mRepaintRegion += lastCursor;
    }
    if (mRepaintRegion.isEmpty() && (!mCursor || !mCursor->isDirty()))
        return touchedRegion;

    if (!mPainter)
        mPainter = new QPainter(&mScreenImage);

    const QRect screenRect = mGeometry.translated(-screenOffset);
    for (QRect rect : mRepaintRegion) {
        rect = rect.intersected(screenRect);
        if (rect.isEmpty())
            continue;

        mPainter->setCompositionMode(QPainter::CompositionMode_Source);
        mPainter->fillRect(rect, mScreenImage.hasAlphaChannel() ? Qt::transparent : Qt::black);

        for (int layerIndex = mWindowStack.size() - 1; layerIndex != -1; layerIndex--) {
            if (!mWindowStack[layerIndex]->window()->isVisible())
                continue;

            const QRect windowRect = mWindowStack[layerIndex]->geometry().translated(-screenOffset);
            const QRect windowIntersect = rect.translated(-windowRect.left(), -windowRect.top());
            QFbBackingStore *backingStore = mWindowStack[layerIndex]->backingStore();
            if (backingStore) {
                backingStore->lock();
                mPainter->drawImage(rect, backingStore->image(), windowIntersect);
                backingStore->unlock();
            }
        }
    }

    if (mCursor && (mCursor->isDirty() || mRepaintRegion.intersects(mCursor->lastPainted()))) {
        mPainter->setCompositionMode(QPainter::CompositionMode_SourceOver);
        touchedRegion += mCursor->drawCursor(*mPainter);
    }
    touchedRegion += mRepaintRegion;
    mRepaintRegion = QRegion();

    return touchedRegion;
}

QFbWindow *QFbScreen::windowForId(WId wid) const
{
    for (int i = 0; i < mWindowStack.size(); ++i) {
        if (mWindowStack[i]->winId() == wid)
            return mWindowStack[i];
    }
    return nullptr;
}

QFbScreen::Flags QFbScreen::flags() const
{
    return { };
}

QT_END_NAMESPACE

#include "moc_qfbscreen_p.cpp"
