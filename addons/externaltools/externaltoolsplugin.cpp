/* This file is part of the KDE project
 *
 *  Copyright 2019 Dominik Haumann <dhaumann@kde.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */
#include "externaltoolsplugin.h"

#include "kateexternaltoolsview.h"
#include "kateexternaltool.h"
#include "kateexternaltoolscommand.h"
#include "katemacroexpander.h"
#include "katetoolrunner.h"
#include "kateexternaltoolsconfigwidget.h"

#include <KTextEditor/Editor>
#include <KTextEditor/Document>
#include <KTextEditor/View>
#include <KActionCollection>
#include <KLocalizedString>
#include <QAction>
#include <kparts/part.h>

#include <KAboutData>
#include <KAuthorized>
#include <KConfig>
#include <KConfigGroup>
#include <KPluginFactory>
#include <KXMLGUIFactory>

K_PLUGIN_FACTORY_WITH_JSON(KateExternalToolsFactory, "externaltoolsplugin.json",
                           registerPlugin<KateExternalToolsPlugin>();)

KateExternalToolsPlugin::KateExternalToolsPlugin(QObject* parent, const QList<QVariant>&)
    : KTextEditor::Plugin(parent)
{
    reload();
}

KateExternalToolsPlugin::~KateExternalToolsPlugin()
{
    delete m_command;
    m_command = nullptr;
}

QObject* KateExternalToolsPlugin::createView(KTextEditor::MainWindow* mainWindow)
{
    KateExternalToolsPluginView* view = new KateExternalToolsPluginView(mainWindow, this);
    connect(this, &KateExternalToolsPlugin::externalToolsChanged, view, &KateExternalToolsPluginView::rebuildMenu);
    return view;
}

void KateExternalToolsPlugin::reload()
{
    delete m_command;
    m_command = nullptr;
    m_commands.clear();
    qDeleteAll(m_tools);
    m_tools.clear();

    KConfig _config(QStringLiteral("externaltools"), KConfig::NoGlobals, QStandardPaths::ApplicationsLocation);
    KConfigGroup config(&_config, "Global");
    const int toolCount = config.readEntry("tools", 0);

    for (int i = 0; i < toolCount; ++i) {
        config = KConfigGroup(&_config, QStringLiteral("Tool %1").arg(i));

        auto t = new KateExternalTool();
        t->load(config);
        m_tools.push_back(t);

        // FIXME test for a command name first!
        if (t->hasexec && (!t->cmdname.isEmpty())) {
            m_commands.push_back(t->cmdname);
        }
    }

    if (KAuthorized::authorizeAction(QStringLiteral("shell_access"))) {
        m_command = new KateExternalToolsCommand(this);
    }

    Q_EMIT externalToolsChanged();
}

QStringList KateExternalToolsPlugin::commands() const
{
    return m_commands;
}

const KateExternalTool* KateExternalToolsPlugin::toolForCommand(const QString& cmd) const
{
    for (auto tool : m_tools) {
        if (tool->cmdname == cmd) {
            return tool;
        }
    }
    return nullptr;
}

const QVector<KateExternalTool*> & KateExternalToolsPlugin::tools() const
{
    return m_tools;
}

void KateExternalToolsPlugin::runTool(const KateExternalTool& tool, KTextEditor::View* view)
{
    // expand the macros in command if any,
    // and construct a command with an absolute path
    auto mw = view->mainWindow();

    // save documents if requested
    if (tool.saveMode == KateExternalTool::SaveMode::CurrentDocument) {
        // only save if modified, to avoid unnecessary recompiles
        if (view->document()->isModified()) {
            view->document()->save();
        }
    } else if (tool.saveMode == KateExternalTool::SaveMode::AllDocuments) {
        foreach (KXMLGUIClient* client, mw->guiFactory()->clients()) {
            if (QAction* a = client->actionCollection()->action(QStringLiteral("file_save_all"))) {
                a->trigger();
                break;
            }
        }
    }

    // clear previous toolview data
    auto pluginView = viewForMainWindow(mw);
    pluginView->clearToolView();

    // copy tool
    std::unique_ptr<KateExternalTool> copy(new KateExternalTool(tool));

    MacroExpander macroExpander(view);

    if (!macroExpander.expandMacrosShellQuote(copy->executable)) {
        pluginView->showToolView();
        pluginView->addToolStatus(i18n("Failed to expand executable '%1'.", copy->executable), copy.get());
        return;
    }

    if (!macroExpander.expandMacrosShellQuote(copy->arguments)) {
        pluginView->showToolView();
        pluginView->addToolStatus(i18n("Failed to expand argument '%1'.", copy->arguments), copy.get());
        return;
    }

    if (!macroExpander.expandMacrosShellQuote(copy->workingDir)) {
        pluginView->showToolView();
        pluginView->addToolStatus(i18n("Failed to expand working directory '%1'.", copy->workingDir), copy.get());
        return;
    }

    if (!macroExpander.expandMacrosShellQuote(copy->input)) {
        pluginView->showToolView();
        pluginView->addToolStatus(i18n("Failed to expand input '%1'.", copy->input), copy.get());
        return;
    }

    // Allocate runner on heap such that it lives as long as the child
    // process is running and does not block the main thread.
    auto runner = new KateToolRunner(std::move(copy), view, this);

    // use QueuedConnection, since handleToolFinished deletes the runner
    connect(runner, &KateToolRunner::toolFinished, this, &KateExternalToolsPlugin::handleToolFinished, Qt::QueuedConnection);
    runner->run();
}

void KateExternalToolsPlugin::handleToolFinished(KateToolRunner* runner, int exitCode, bool crashed)
{
    if (exitCode != 0 && runner->view()) {
        auto pluginView = viewForMainWindow(runner->view()->mainWindow());
        pluginView->createToolView();
        pluginView->addToolStatus(i18n("External tool %1 finished with non-zero exit code: %2", runner->tool()->name, exitCode), runner->tool());
        pluginView->showToolView();
    }

    if (crashed && runner->view()) {
        auto pluginView = viewForMainWindow(runner->view()->mainWindow());
        pluginView->createToolView();
        pluginView->addToolStatus(i18n("External tool crashed: %1", runner->tool()->name), runner->tool());
        pluginView->showToolView();
    }

    auto view = runner->view();
    if (crashed && view) {
        viewForMainWindow(view->mainWindow())->addToolStatus(i18n("External tool crashed: %1", runner->tool()->name), runner->tool());
    }

    if (view && !runner->outputData().isEmpty()) {
        switch (runner->tool()->outputMode) {
            case KateExternalTool::OutputMode::InsertAtCursor: {
                KTextEditor::Document::EditingTransaction transaction(view->document());
                view->removeSelection();
                view->insertText(runner->outputData());
                break;
            }
            case KateExternalTool::OutputMode::ReplaceSelectedText: {
                KTextEditor::Document::EditingTransaction transaction(view->document());
                view->removeSelectionText();
                view->insertText(runner->outputData());
                break;
            }
            case KateExternalTool::OutputMode::ReplaceCurrentDocument: {
                KTextEditor::Document::EditingTransaction transaction(view->document());
                view->document()->clear();
                view->insertText(runner->outputData());
                break;
            }
            case KateExternalTool::OutputMode::AppendToCurrentDocument: {
                view->document()->insertText(view->document()->documentEnd(), runner->outputData());
                break;
            }
            case KateExternalTool::OutputMode::InsertInNewDocument: {
                auto mainWindow = view->mainWindow();
                auto newView = mainWindow->openUrl({});
                newView->insertText(runner->outputData());
                mainWindow->activateView(newView->document());
                break;
            }
            default:
                break;
        }
    }

    if (view && runner->tool()->reload) {
        // updates-enabled trick: avoid some flicker
        const bool wereUpdatesEnabled = view->updatesEnabled();
        view->setUpdatesEnabled(false);
        view->document()->documentReload();
        view->setUpdatesEnabled(wereUpdatesEnabled);
    }

    if (runner->tool()->outputMode == KateExternalTool::OutputMode::DisplayInPane) {
        auto pluginView = viewForMainWindow(view->mainWindow());
        pluginView->createToolView();
        pluginView->setOutputData(runner->outputData());
        pluginView->showToolView();
    }

    if (!runner->errorData().isEmpty()) {
        auto pluginView = viewForMainWindow(view->mainWindow());
        pluginView->createToolView();
        pluginView->addToolStatus(i18n("Data written to stderr:\n%1", runner->errorData()), runner->tool());
        pluginView->showToolView();
    }

    delete runner;
}

int KateExternalToolsPlugin::configPages() const
{
    return 1;
}

KTextEditor::ConfigPage* KateExternalToolsPlugin::configPage(int number, QWidget* parent)
{
    if (number == 0) {
        return new KateExternalToolsConfigWidget(parent, this);
    }
    return nullptr;
}

void KateExternalToolsPlugin::registerPluginView(KateExternalToolsPluginView * view)
{
    Q_ASSERT(!m_views.contains(view));
    m_views.push_back(view);
}

void KateExternalToolsPlugin::unregisterPluginView(KateExternalToolsPluginView * view)
{
    Q_ASSERT(m_views.contains(view));
    m_views.removeAll(view);
}

KateExternalToolsPluginView* KateExternalToolsPlugin::viewForMainWindow(KTextEditor::MainWindow* mainWindow) const
{
    for (auto view : m_views) {
        if (view->mainWindow() == mainWindow) {
            return view;
        }
    }
    return nullptr;
}

#include "externaltoolsplugin.moc"

// kate: space-indent on; indent-width 4; replace-tabs on;
