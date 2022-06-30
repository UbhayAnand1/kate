/*
    SPDX-FileCopyrightText: 2014 Dominik Haumann <dhaumann@kde.org>
    SPDX-FileCopyrightText: 2020 Christoph Cullmann <cullmann@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "katetabbar.h"
#include "kateapp.h"
#include "tabmimedata.h"

#include <QApplication>
#include <QDataStream>
#include <QDrag>
#include <QIcon>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QStyleOptionTab>
#include <QStylePainter>
#include <QWheelEvent>

#include <KAcceleratorManager>
#include <KConfigGroup>
#include <KSharedConfig>

#include <KTextEditor/Document>
#include <kcolorscheme.h>

namespace
{
class CloseButton : public QAbstractButton
{
    Q_OBJECT
public:
    explicit CloseButton(QWidget *parent);
    QSize sizeHint() const override
    {
        ensurePolished();
        int width = style()->pixelMetric(QStyle::PM_TabCloseIndicatorWidth, nullptr, this);
        int height = style()->pixelMetric(QStyle::PM_TabCloseIndicatorHeight, nullptr, this);
        return QSize(width, height);
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEvent *event) override
#else
    void enterEvent(QEnterEvent *event) override
#endif
    {
        if (isEnabled())
            update();
        QAbstractButton::enterEvent(event);
    }
    void leaveEvent(QEvent *event) override
    {
        if (isEnabled())
            update();
        QAbstractButton::leaveEvent(event);
    }

    void paintEvent(QPaintEvent *event) override;
};
}

CloseButton::CloseButton(QWidget *parent)
    : QAbstractButton(parent)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setToolTip(tr("Close Tab"));
    resize(sizeHint());
}

void CloseButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QStyleOption opt;
    opt.initFrom(this);
    opt.state |= QStyle::State_AutoRaise;
    if (isEnabled() && underMouse() && !isChecked() && !isDown())
        opt.state |= QStyle::State_Raised;
    if (isChecked())
        opt.state |= QStyle::State_On;
    if (isDown())
        opt.state |= QStyle::State_Sunken;
    if (const QTabBar *tb = qobject_cast<const QTabBar *>(parent())) {
        int index = tb->currentIndex();
        QTabBar::ButtonPosition position = (QTabBar::ButtonPosition)style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, tb);
        if (tb->tabButton(index, position) == this)
            opt.state |= QStyle::State_Selected;
    }

    if (KateTabBar *tb = qobject_cast<KateTabBar *>(parent())) {
        int index = tb->tabAt(pos());
        auto doc = tb->tabDocument(index);
        if (doc && doc->isModified()) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setBrush(opt.palette.windowText());
            QRect rect = opt.rect;
            rect.setSize(opt.rect.size() * 0.6);
            rect.moveCenter(opt.rect.center());
            p.drawEllipse(rect);
            return;
        }
    }

    style()->drawPrimitive(QStyle::PE_IndicatorTabClose, &opt, &p, this);
}

struct KateTabButtonData {
    KTextEditor::Document *doc = nullptr;
};

Q_DECLARE_METATYPE(KateTabButtonData)

/**
 * Creates a new tab bar with the given \a parent.
 */
KateTabBar::KateTabBar(QWidget *parent)
    : QTabBar(parent)
{
    // we want no auto-accelerators here
    KAcceleratorManager::setNoAccel(this);

    // enable document mode, docs tell this will trigger:
    // On macOS this will look similar to the tabs in Safari or Sierra's Terminal.app.
    // this seems reasonable for our document tabs
    setDocumentMode(true);

    // we want drag and drop
    setAcceptDrops(true);

    // allow users to re-arrange the tabs
    setMovable(true);

    // We have custom tab close handling
    setTabsClosable(false);

    // enforce configured limit
    readConfig();

    // handle config changes
    connect(KateApp::self(), &KateApp::configurationChanged, this, &KateTabBar::readConfig);
}

void KateTabBar::readConfig()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig();
    KConfigGroup cgGeneral = KConfigGroup(config, "General");

    // 0 == unlimited, normalized other inputs
    const int tabCountLimit = cgGeneral.readEntry("Tabbar Tab Limit", 0);
    m_tabCountLimit = (tabCountLimit <= 0) ? 0 : tabCountLimit;

    // use scroll buttons if we have no limit
    setUsesScrollButtons(m_tabCountLimit == 0 || cgGeneral.readEntry("Allow Tab Scrolling", true));

    // elide if we have some limit
    setElideMode((m_tabCountLimit == 0 || !cgGeneral.readEntry("Elide Tab Text", false)) ? Qt::ElideNone : Qt::ElideMiddle);

    const std::vector<int> documentTabIndexes = this->documentTabIndexes();

    // if we enforce a limit: purge tabs that violate it
    if (m_tabCountLimit > 0 && ((int)documentTabIndexes.size() > m_tabCountLimit)) {
        // just purge last X tabs, this isn't that clever but happens only on config changes!
        size_t toRemove = documentTabIndexes.size() - (size_t)m_tabCountLimit;
        size_t removed = 0;
        for (auto rit = documentTabIndexes.rbegin(); rit != documentTabIndexes.rend(); ++rit) {
            if (removed == toRemove)
                break;
            removeTab(*rit);
            removed++;
        }
        setCurrentIndex(0);
    }

    // handle tab close button and expansion
    setExpanding(cgGeneral.readEntry("Expand Tabs", false));
    setTabsAreCloseable(cgGeneral.readEntry("Show Tabs Close Button", true));

    // get mouse click rules
    m_doubleClickNewDocument = cgGeneral.readEntry("Tab Double Click New Document", true);
    m_middleClickCloseDocument = cgGeneral.readEntry("Tab Middle Click Close Document", true);
}

void KateTabBar::setTabsAreCloseable(bool closeable)
{
    if (closeable == m_tabsCloseable)
        return;
    m_tabsCloseable = closeable;

    ButtonPosition closeSide = (ButtonPosition)style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, this);
    if (closeable) {
        for (int i = 0; i < count(); ++i) {
            auto *closeButton = new CloseButton(this);
            connect(closeButton, &CloseButton::clicked, this, &KateTabBar::slotCloseTab);
            setTabButton(i, closeSide, closeButton);
        }
    } else {
        for (int i = 0; i < count(); ++i) {
            if (auto btn = tabButton(i, closeSide)) {
                btn->deleteLater();
                setTabButton(i, closeSide, nullptr);
            }
        }
    }
    update();
}

bool KateTabBar::areTabsCloseable() const
{
    return m_tabsCloseable;
}

void KateTabBar::onDocumentModified(KTextEditor::Document *doc)
{
    auto idx = documentIdx(doc);
    if (idx >= 0 && doc->isModified() && !areTabsCloseable()) {
        auto tabText = this->tabText(idx);
        tabText.append(QStringLiteral(" *"));
        setTabText(idx, tabText);
    } else {
        ButtonPosition closeSide = (ButtonPosition)style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, this);
        if (auto b = tabButton(idx, closeSide)) {
            b->update();
        }
    }
}

std::vector<int> KateTabBar::documentTabIndexes() const
{
    std::vector<int> docs;
    const int tabCount = count();
    for (int i = 0; i < tabCount; ++i) {
        if (tabData(i).value<KateTabButtonData>().doc) {
            docs.push_back(i);
        }
    }
    return docs;
}

void KateTabBar::setActive(bool active)
{
    if (active == m_isActive) {
        return;
    }
    m_isActive = active;
    update();
}

bool KateTabBar::isActive() const
{
    return m_isActive;
}

int KateTabBar::prevTab() const
{
    return currentIndex() == 0 ? 0 // first index, keep it here.
                               : currentIndex() - 1;
}

int KateTabBar::nextTab() const
{
    return currentIndex() == count() - 1 ? count() - 1 // last index, keep it here.
                                         : currentIndex() + 1;
}

bool KateTabBar::containsTab(int index) const
{
    return index >= 0 && index < count();
}

QVariant KateTabBar::ensureValidTabData(int idx)
{
    if (!tabData(idx).isValid()) {
        setTabData(idx, QVariant::fromValue(KateTabButtonData{}));
    }
    return tabData(idx);
}

void KateTabBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();

    if (m_doubleClickNewDocument && event->button() == Qt::LeftButton) {
        Q_EMIT newTabRequested();
    }
}

void KateTabBar::mousePressEvent(QMouseEvent *event)
{
    if (!isActive()) {
        Q_EMIT activateViewSpaceRequested();
    }

    if (event->button() == Qt::LeftButton && tabAt(event->pos()) != -1) {
        dragStartPos = event->pos();
    } else {
        dragStartPos = {};
    }

    QTabBar::mousePressEvent(event);

    // handle close for middle mouse button
    if (m_middleClickCloseDocument && event->button() == Qt::MiddleButton) {
        int id = tabAt(event->pos());
        if (id >= 0) {
            Q_EMIT tabCloseRequested(id);
        }
    }
}

void KateTabBar::mouseMoveEvent(QMouseEvent *event)
{
    if (dragStartPos.isNull()) {
        QTabBar::mouseMoveEvent(event);
        return;
    }

    if ((event->pos() - dragStartPos).manhattanLength() < QApplication::startDragDistance()) {
        QTabBar::mouseMoveEvent(event);
        return;
    }

    if (rect().contains(event->pos())) {
        return QTabBar::mouseMoveEvent(event);
    }

    QDrag *drag = new QDrag(this);

    int tab = tabAt(dragStartPos);
    QRect rect = tabRect(tab);

    QPixmap p(rect.size() * this->devicePixelRatioF());
    p.setDevicePixelRatio(this->devicePixelRatioF());
    p.fill(Qt::transparent);

    // For some reason initStyleOption with tabIdx directly
    // wasn't working, so manually set some stuff
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QStyleOptionTabV4 opt;
#else
    QStyleOptionTab opt;
#endif
    opt.text = tabText(tab);
    opt.state = QStyle::State_Selected | QStyle::State_Raised;
    opt.tabIndex = tab;
    opt.position = QStyleOptionTab::OnlyOneTab;
    opt.features = QStyleOptionTab::TabFeature::HasFrame;
    opt.rect = rect;
    // adjust the rect so that it starts at(0,0)
    opt.rect.adjust(-opt.rect.x(), 0, -opt.rect.x(), 0);

    QStylePainter paint(&p, this);
    paint.drawControl(QStyle::CE_TabBarTab, opt);
    paint.end();

    auto parentViewSpace = qobject_cast<KateViewSpace *>(parentWidget());
    if (!parentViewSpace) {
        qWarning() << Q_FUNC_INFO << "parentViewSpace is null";
    }

    auto view = parentViewSpace->currentView();
    if (!view) {
        return;
    }

    QByteArray data;
    KTextEditor::Cursor cp = view->cursorPosition();
    QDataStream ds(&data, QIODevice::WriteOnly);
    ds << cp.line();
    ds << cp.column();
    ds << view->document()->url();

    auto mime = new TabMimeData(parentViewSpace, tabDocument(tab));
    mime->setData(QStringLiteral("application/kate.tab.mimedata"), data);

    drag->setMimeData(mime);
    drag->setPixmap(p);
    QPoint hp;
    hp.setX(dragStartPos.x() - rect.x());
    hp.setY(dragStartPos.y() - rect.y());
    drag->setHotSpot(hp);

    dragStartPos = {};

    drag->exec();
}

void KateTabBar::contextMenuEvent(QContextMenuEvent *ev)
{
    int id = tabAt(ev->pos());
    if (id >= 0) {
        Q_EMIT contextMenuRequest(id, ev->globalPos());
    }
}

void KateTabBar::wheelEvent(QWheelEvent *event)
{
    event->accept();

    // cycle through the tabs
    const int delta = event->angleDelta().x() + event->angleDelta().y();
    const int idx = (delta > 0) ? prevTab() : nextTab();
    setCurrentIndex(idx);
}

void KateTabBar::setTabDocument(int idx, KTextEditor::Document *doc)
{
    QVariant data = ensureValidTabData(idx);
    KateTabButtonData buttonData = data.value<KateTabButtonData>();
    buttonData.doc = doc;
    setTabData(idx, QVariant::fromValue(buttonData));
    // BUG: 441340 We need to escape the & because it is used for accelerators/shortcut mnemonic by default
    QString tabName = doc->documentName();
    tabName.replace(QLatin1Char('&'), QLatin1String("&&"));
    if (doc->isModified()) {
        tabName.append(QStringLiteral(" *"));
    }
    setTabText(idx, tabName);
    setTabToolTip(idx, doc->url().toDisplayString());
}

void KateTabBar::setCurrentDocument(KTextEditor::Document *doc)
{
    // in any case: update lru counter for this document, might add new element to hash
    // we have a tab after this call, too!
    m_docToLruCounterAndHasTab[doc] = std::make_pair(++m_lruCounter, true);

    // do we have a tab for this document?
    // if yes => just set as current one
    const int existingIndex = documentIdx(doc);
    if (existingIndex != -1) {
        setCurrentIndex(existingIndex);
        return;
    }

    // else: if we are still inside the allowed number of tabs or have no limit
    // => create new tab and be done
    if ((m_tabCountLimit == 0) || documentTabIndexes().size() < (size_t)m_tabCountLimit) {
        m_beingAdded = doc;
        insertTab(-1, doc->documentName());
        return;
    }

    // ok, we have already the limit of tabs reached:
    // replace the tab with the lowest lru counter => the least recently used

    // search for the right tab
    quint64 minCounter = static_cast<quint64>(-1);
    int indexToReplace = 0;
    KTextEditor::Document *docToReplace = nullptr;
    for (int idx = 0; idx < count(); idx++) {
        QVariant data = tabData(idx);
        KTextEditor::Document *doc = data.value<KateTabButtonData>().doc;
        if (!data.isValid() || !doc) {
            continue;
        }
        const quint64 currentCounter = m_docToLruCounterAndHasTab[doc].first;
        if (currentCounter <= minCounter) {
            minCounter = currentCounter;
            indexToReplace = idx;
            docToReplace = doc;
        }
    }

    // mark the replace doc as "has no tab"
    m_docToLruCounterAndHasTab[docToReplace].second = false;

    // replace it's data + set it as active
    setTabDocument(indexToReplace, doc);
    setCurrentIndex(indexToReplace);
}

void KateTabBar::slotCloseTab()
{
    QObject *object = sender();
    int tabToClose = -1;
    QTabBar::ButtonPosition closeSide = (QTabBar::ButtonPosition)style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, this);
    for (int i = 0; i < count(); ++i) {
        if (closeSide == QTabBar::LeftSide) {
            if (tabButton(i, closeSide) == object) {
                tabToClose = i;
                break;
            }
        } else {
            if (tabButton(i, closeSide) == object) {
                tabToClose = i;
                break;
            }
        }
    }
    if (tabToClose != -1)
        Q_EMIT tabCloseRequested(tabToClose);
}

void KateTabBar::removeDocument(KTextEditor::Document *doc)
{
    // purge LRU storage, must work
    auto erased = (m_docToLruCounterAndHasTab.erase(doc) == 1);
    if (!erased) {
        qWarning() << Q_FUNC_INFO << "Failed to erase";
    }

    // remove document if needed, we might have no tab for it, if tab count is limited!
    const int idx = documentIdx(doc);
    if (idx == -1) {
        return;
    }

    // if we have some tab limit, replace the removed tab with the next best document that has none!
    if (m_tabCountLimit > 0) {
        quint64 maxCounter = 0;
        KTextEditor::Document *docToReplace = nullptr;
        for (const auto &lru : m_docToLruCounterAndHasTab) {
            // ignore stuff with tabs
            if (lru.second.second) {
                continue;
            }

            // search most recently used one
            if (lru.second.first >= maxCounter) {
                maxCounter = lru.second.first;
                docToReplace = lru.first;
            }
        }

        // any document found? replace the tab we want to close and be done
        if (docToReplace) {
            // mark the replace doc as "has a tab"
            m_docToLruCounterAndHasTab[docToReplace].second = true;

            // replace info for the tab
            setTabDocument(idx, docToReplace);
            setCurrentIndex(idx);
            Q_EMIT currentChanged(idx);
            return;
        }
    }

    // if we arrive here, we just need to purge the tab
    // this happens if we have no limit or no document to replace the current one
    removeTab(idx);
}

int KateTabBar::documentIdx(KTextEditor::Document *doc)
{
    for (int idx = 0; idx < count(); idx++) {
        QVariant data = tabData(idx);
        if (!data.isValid()) {
            continue;
        }
        if (data.value<KateTabButtonData>().doc != doc) {
            continue;
        }
        return idx;
    }
    return -1;
}

KTextEditor::Document *KateTabBar::tabDocument(int idx)
{
    QVariant data = ensureValidTabData(idx);
    KateTabButtonData buttonData = data.value<KateTabButtonData>();

    KTextEditor::Document *doc = nullptr;
    // The tab got activated before the correct finalixation,
    // we need to plug the document before returning.
    if (buttonData.doc == nullptr && m_beingAdded) {
        setTabDocument(idx, m_beingAdded);
        doc = m_beingAdded;
        m_beingAdded = nullptr;
    } else {
        doc = buttonData.doc;
    }

    return doc;
}

void KateTabBar::tabInserted(int idx)
{
    if (m_beingAdded) {
        setTabDocument(idx, m_beingAdded);
    }

    if (areTabsCloseable()) {
        ButtonPosition closeSide = (ButtonPosition)style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, this);
        auto btn = new CloseButton(this);
        connect(btn, &CloseButton::clicked, this, &KateTabBar::slotCloseTab);
        setTabButton(idx, closeSide, btn);
    }

    m_beingAdded = nullptr;
}

QVector<KTextEditor::Document *> KateTabBar::documentList() const
{
    QVector<KTextEditor::Document *> result;
    for (int idx = 0; idx < count(); idx++) {
        QVariant data = tabData(idx);
        if (!data.isValid()) {
            continue;
        }
        if (data.value<KateTabButtonData>().doc)
            result.append(data.value<KateTabButtonData>().doc);
    }
    return result;
}

void KateTabBar::setCurrentWidget(QWidget *widget)
{
    int idx = insertTab(-1, widget->windowTitle());
    setTabData(idx, QVariant::fromValue(widget));
    setCurrentIndex(idx);
}

#include "katetabbar.moc"
