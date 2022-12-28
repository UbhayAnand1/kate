/*
    SPDX-FileCopyrightText: 2022 Waqar Ahmed <waqar.17a@gmail.com>
    SPDX-License-Identifier: LGPL-2.0-or-later
*/
#include "FormatPlugin.h"

#include "FormatConfig.h"
#include "FormatterFactory.h"
#include "Formatters.h"

#include <KActionCollection>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KSharedConfig>
#include <KTextEditor/View>
#include <KXMLGUIFactory>

#include <QPointer>

K_PLUGIN_FACTORY_WITH_JSON(FormatPluginFactory, "FormatPlugin.json", registerPlugin<FormatPlugin>();)

FormatPlugin::FormatPlugin(QObject *parent, const QVariantList &)
    : KTextEditor::Plugin(parent)
{
    readConfig();
}

QObject *FormatPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    return new FormatPluginView(this, mainWindow);
}

KTextEditor::ConfigPage *FormatPlugin::configPage(int number, QWidget *parent)
{
    if (number == 0) {
        return new FormatConfigPage(this, parent);
    }
    return nullptr;
}

void FormatPlugin::readConfig()
{
    KConfigGroup cg(KSharedConfig::openConfig(), "Formatting");
    formatOnSave = cg.readEntry("FormatOnSave", false);

    formatterForJson = (Formatters)cg.readEntry("FormatterForJson", (int)Formatters::Prettier);
}

FormatPluginView::FormatPluginView(FormatPlugin *plugin, KTextEditor::MainWindow *mainWin)
    : QObject(plugin)
    , m_plugin(plugin)
    , m_mainWindow(mainWin)
{
    KXMLGUIClient::setComponentName(QStringLiteral("formatplugin"), i18n("Formatting"));

    connect(m_plugin, &FormatPlugin::configChanged, this, &FormatPluginView::onConfigChanged);

    auto ac = actionCollection();
    auto a = ac->addAction(QStringLiteral("format_document"), this, &FormatPluginView::format);
    a->setText(i18n("Format Document"));
    connect(mainWin, &KTextEditor::MainWindow::viewChanged, this, &FormatPluginView::onActiveViewChanged);

    m_mainWindow->guiFactory()->addClient(this);
}

FormatPluginView::~FormatPluginView()
{
    disconnect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, &FormatPluginView::onActiveViewChanged);
    m_mainWindow->guiFactory()->removeClient(this);
}

void FormatPluginView::onConfigChanged()
{
    // maybe remove this? Normally this function is emitted when config page is open
    // which means activeView == null. When the user goes back to the document
    // onActiveViewChanged will be called and config will be re-read
}

void FormatPluginView::onActiveViewChanged(KTextEditor::View *v)
{
    if (!v || !v->document()) {
        if (m_activeDoc) {
            disconnect(m_activeDoc, &KTextEditor::Document::documentSavedOrUploaded, this, &FormatPluginView::format);
        }
        m_activeDoc = nullptr;
        return;
    }

    if (v->document() == m_activeDoc) {
        return;
    }

    if (m_activeDoc) {
        disconnect(m_activeDoc, &KTextEditor::Document::documentSavedOrUploaded, this, &FormatPluginView::format);
    }

    m_activeDoc = v->document();
    m_lastChecksum = {};

    if (formatOnSave()) {
        connect(m_activeDoc, &KTextEditor::Document::documentSavedOrUploaded, this, &FormatPluginView::format, Qt::QueuedConnection);
    }
}

void FormatPluginView::format()
{
    if (!m_activeDoc) {
        return;
    }

    // save if modified
    if (m_activeDoc->isModified() && !m_activeDoc->url().toLocalFile().isEmpty()) {
        saveDocument(m_activeDoc);
    }

    if (!m_lastChecksum.isEmpty() && m_activeDoc->checksum() == m_lastChecksum) {
        return;
    }

    auto formatter = formatterForDoc(m_activeDoc, m_plugin);
    if (!formatter) {
        return;
    }

    connect(formatter, &AbstractFormatter::textFormatted, this, &FormatPluginView::onFormattedTextReceived);
    connect(formatter, &AbstractFormatter::textFormatted, formatter, &AbstractFormatter::deleteLater);
    connect(formatter, &AbstractFormatter::textFormattedPatch, this, [this, formatter](KTextEditor::Document *doc, const std::vector<PatchLine> &patch) {
        onFormattedPatchReceived(doc, patch);
        formatter->deleteLater();
    });
    formatter->run(m_activeDoc);
}

void FormatPluginView::onFormattedTextReceived(AbstractFormatter *formatter, KTextEditor::Document *doc, const QByteArray &formattedText)
{
    if (!doc) {
        qWarning() << Q_FUNC_INFO << "invalid null doc";
        return;
    }

    // No formattted => no work to do
    if (formattedText.isEmpty()) {
        return;
    }

    // if the user typed something while the formatter ran, ignore the formatted text
    if (formatter->originalText != doc->text()) {
        // qDebug() << "text changed, ignoring format";
        return;
    }

    // non local file or untitled?
    if (doc->url().toLocalFile().isEmpty()) {
        doc->setText(QString::fromUtf8(formattedText));
        m_lastChecksum.clear();
        return;
    }

    // get a git diff
    const QString patch = diff(doc, formattedText);
    // no difference? => do nothing
    if (patch.isEmpty()) {
        return;
    }

    // create applyable edits
    const std::vector<PatchLine> edits = parseDiff(qobject_cast<KTextEditor::MovingInterface *>(doc), patch);

    // If the edits are too many, just do "setText" as it can be very slow
    if ((int)edits.size() >= (doc->lines() / 2) && doc->lines() > 1000) {
        for (const auto &p : edits) {
            delete p.pos;
        }
        doc->setText(QString::fromUtf8(formattedText));
        saveDocument(doc);
        m_lastChecksum = doc->checksum();
        return;
    }

    onFormattedPatchReceived(doc, edits);
}

void FormatPluginView::onFormattedPatchReceived(KTextEditor::Document *doc, const std::vector<PatchLine> &patch)
{
    applyPatch(doc, patch);
    // finally save the document
    saveDocument(doc);
    m_lastChecksum = doc->checksum();
}

void FormatPluginView::saveDocument(KTextEditor::Document *doc)
{
    if (doc->url().isValid() && doc->isModified()) {
        if (formatOnSave() && doc == m_activeDoc) {
            disconnect(doc, &KTextEditor::Document::documentSavedOrUploaded, this, &FormatPluginView::format);
        }

        doc->documentSave();

        if (formatOnSave() && doc == m_activeDoc) {
            connect(doc, &KTextEditor::Document::documentSavedOrUploaded, this, &FormatPluginView::format, Qt::QueuedConnection);
        }
    }
}

#include "FormatPlugin.moc"
