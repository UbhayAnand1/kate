/***************************************************************************
 *   This file is part of Kate build plugin
 *   SPDX-FileCopyrightText: 2014 Kåre Särs <kare.sars@iki.fi>
 *
 *   SPDX-License-Identifier: LGPL-2.0-or-later
 ***************************************************************************/

#include "MatchModel.h"
#include <KLocalizedString>
#include <QDebug>
#include <QTimer>
#include <QRegularExpression>
#include <QFileInfo>
#include <QDir>
#include <algorithm> // std::count_if

#include <ktexteditor/movinginterface.h>
#include <ktexteditor/movingrange.h>

static const quintptr InfoItemId = 0xFFFFFFFF;
static const quintptr FileItemId = 0x7FFFFFFF;

// Model indexes
// - (0, 0, InfoItemId) (row, column, internalId)
//   | - (0, 0, FileItemId)
//   |    | - (0, 0, 0)
//   |    | - (1, 0, 0)
//   | - (1, 0, FileItemId)
//   |    | - (0, 0, 1)
//   |    | - (1, 0, 1)

static QUrl localFileDirUp(const QUrl &url)
{
    if (!url.isLocalFile())
        return url;

    // else go up
    return QUrl::fromLocalFile(QFileInfo(url.toLocalFile()).dir().absolutePath());
}

MatchModel::MatchModel(QObject *parent)
    : QAbstractItemModel(parent)
{
    m_infoUpdateTimer.setInterval(500); // FIXME why does this delay not work?
    m_infoUpdateTimer.setSingleShot(true);
    connect(&m_infoUpdateTimer, &QTimer::timeout, this, [this]() {
        dataChanged(createIndex(0, 0, InfoItemId), createIndex(0, 0, InfoItemId), QVector<int>(Qt::DisplayRole));
    });
}
MatchModel::~MatchModel()
{
}

void MatchModel::setSearchPlace(MatchModel::SearchPlaces searchPlace)
{
    m_searchPlace = searchPlace;
    if (!m_infoUpdateTimer.isActive()) m_infoUpdateTimer.start();
}

void MatchModel::setSearchState(MatchModel::SearchState searchState)
{
    m_searchState = searchState;
    if (!m_infoUpdateTimer.isActive()) m_infoUpdateTimer.start();
}

void MatchModel::setBaseSearchPath(const QString &baseSearchPath)
{
    m_resultBaseDir = baseSearchPath;
    if (!m_infoUpdateTimer.isActive()) m_infoUpdateTimer.start();
}

void MatchModel::setProjectName(const QString &projectName)
{
    m_projectName = projectName;
    if (!m_infoUpdateTimer.isActive()) m_infoUpdateTimer.start();
}


QString MatchModel::infoHtmlString() const
{
    if (m_matchFiles.isEmpty()) {
        return QString();
    }

    int matchesTotal = 0;
    int checkedTotal = 0;
    for (const auto &matchFile: qAsConst(m_matchFiles)) {
        matchesTotal += matchFile.matches.size();
        checkedTotal += std::count_if(matchFile.matches.begin(), matchFile.matches.end(), [](const MatchModel::Match &match) {return match.checked;} );
    }

    if (m_searchState == Searching) {
        QString searchUrl = m_lastMatchUrl.toDisplayString();

        if (searchUrl.size() > 73) {
            return i18np("<b><i>One match found, searching: ...%2</b>", "<b><i>%1 matches found, searching: ...%2</b>", matchesTotal, searchUrl.right(70));
        } else {
            return i18np("<b><i>One match found, searching: %2</b>", "<b><i>%1 matches found, searching: %2</b>", matchesTotal, searchUrl);
        }
    }

    QString checkedStr = i18np("One checked", "%1 checked", checkedTotal);

    switch (m_searchPlace) {
        case CurrentFile:
            return i18np("<b><i>One match (%2) found in file</i></b>", "<b><i>%1 matches (%2) found in current file</i></b>", matchesTotal, checkedStr);
        case MatchModel::OpenFiles:
            return i18np("<b><i>One match (%2) found in open files</i></b>", "<b><i>%1 matches (%2) found in open files</i></b>", matchesTotal, checkedStr);
            break;
        case MatchModel::Folder:
            return i18np("<b><i>One match (%3) found in folder %2</i></b>", "<b><i>%1 matches (%3) found in folder %2</i></b>", matchesTotal, m_resultBaseDir, checkedStr);
            break;
        case MatchModel::Project: {
            return i18np("<b><i>One match (%4) found in project %2 (%3)</i></b>", "<b><i>%1 matches (%4) found in project %2 (%3)</i></b>", matchesTotal, m_projectName, m_resultBaseDir, checkedStr);
            break;
        }
        case MatchModel::AllProjects: // "in Open Projects"
            return i18np("<b><i>One match (%3) found in all open projects (common parent: %2)</i></b>", "<b><i>%1 matches (%3) found in all open projects (common parent: %2)</i></b>", matchesTotal, m_resultBaseDir, checkedStr);
            break;
    }

    return QString();
}


void MatchModel::clear()
{
    beginResetModel();
    m_matchFiles.clear();
    m_matchFileIndexHash.clear();
    endResetModel();
}

/** This function returns the row index of the specified file.
 * If the file does not exist in the model, the file will be added to the model. */
int MatchModel::matchFileRow(const QUrl& fileUrl) const
{
    return m_matchFileIndexHash.value(fileUrl, -1);
}

static const int totalContectLen = 150;

/** This function is used to add a match to a new file */
void MatchModel::addMatches(const QUrl &fileUrl, const QVector<KateSearchMatch> &searchMatches)
{
    m_lastMatchUrl = fileUrl;
    // update match/search info
    if (!m_infoUpdateTimer.isActive()) m_infoUpdateTimer.start();

    if (m_matchFiles.isEmpty()) {
        beginInsertRows(QModelIndex(), 0, 0);
        endInsertRows();
    }

    if (searchMatches.isEmpty()) {
        return;
    }

    int fileIndex = matchFileRow(fileUrl);
    if (fileIndex == -1) {
        fileIndex = m_matchFiles.size();
        m_matchFileIndexHash.insert(fileUrl, fileIndex);
        beginInsertRows(createIndex(0,0,InfoItemId), fileIndex, fileIndex);
        // We are always starting the insert at the end, so we could optimize by delaying/grouping the signaling of the updates
        m_matchFiles.append(MatchFile());
        m_matchFiles[fileIndex].fileUrl = fileUrl;
        endInsertRows();
    }

    int matchIndex = m_matchFiles[fileIndex].matches.size();
    beginInsertRows(createIndex(fileIndex, 0 , FileItemId), matchIndex, matchIndex + searchMatches.size()-1);
    for (const auto &sMatch: searchMatches) {
        MatchModel::Match match;
        match.matchLen = sMatch.matchLen;
        match.range = sMatch.matchRange;

        int matchStartColumn = match.range.start().column();
        int matchEndColumn = match.range.end().column();

        int contextLen = totalContectLen - sMatch.matchLen;
        int preLen = qMin(contextLen/3, matchStartColumn);
        int postLen = contextLen - preLen;

        match.preMatchStr = sMatch.lineContent.mid(matchStartColumn-preLen, preLen);
        if (matchStartColumn > preLen) match.preMatchStr.prepend(QLatin1String("..."));

        match.postMatchStr = sMatch.lineContent.mid(matchEndColumn, postLen);
        if (matchEndColumn+preLen < sMatch.lineContent.size()) match.postMatchStr.append(QLatin1String("..."));

        match.matchStr = sMatch.lineContent.mid(matchStartColumn, sMatch.matchLen);

        m_matchFiles[fileIndex].matches.append(match);
    }
    endInsertRows();
}

void MatchModel::setMatchColors(const QColor &foreground, const QColor &background, const QColor &replaseBackground)
{
    m_foregroundColor = foreground;
    m_searchBackgroundColor = background;
    m_replaceHighlightColor = replaseBackground;
}

MatchModel::Match *MatchModel::matchFromIndex(const QModelIndex &matchIndex)
{
    if (!isMatch(matchIndex)) {
        qDebug() << "Not a valid match index";
        return nullptr;
    }

    int fileRow = matchIndex.internalId();
    int matchRow = matchIndex.row();

    return &m_matchFiles[fileRow].matches[matchRow];
}

KTextEditor::Range MatchModel::matchRange(const QModelIndex &matchIndex) const
{
    if (!isMatch(matchIndex)) {
        qDebug() << "Not a valid match index";
        return KTextEditor::Range();
    }
    int fileRow = matchIndex.internalId();
    int matchRow = matchIndex.row();
    return m_matchFiles[fileRow].matches[matchRow].range;
}


/** This function is used to replace a match */
bool MatchModel::replaceMatch(KTextEditor::Document *doc, const QModelIndex &matchIndex, const QRegularExpression &regExp, const QString &replaceString)
{
    if (!doc) {
        qDebug() << "No doc";
        return false;
    }

    Match *matchItem = matchFromIndex(matchIndex);

    if (!matchItem) {
        qDebug() << "Not a valid index";
        return false;
    }

    // don't replace an already replaced item
    if (!matchItem->replaceText.isEmpty()) {
        // qDebug() << "not replacing already replaced item";
        return false;
    }

    // Check that the text has not been modified and still matches + get captures for the replace
    QString matchLines = doc->text(matchItem->range);
    QRegularExpressionMatch match = regExp.match(matchLines);
    if (match.capturedStart() != 0) {
        qDebug() << matchLines << "Does not match" << regExp.pattern();
        return false;
    }

    // Modify the replace string according to this match
    QString replaceText = replaceString;
    replaceText.replace(QLatin1String("\\\\"), QLatin1String("¤Search&Replace¤"));

    // allow captures \0 .. \9
    for (int j = qMin(9, match.lastCapturedIndex()); j >= 0; --j) {
        QString captureLX = QStringLiteral("\\L\\%1").arg(j);
        QString captureUX = QStringLiteral("\\U\\%1").arg(j);
        QString captureX = QStringLiteral("\\%1").arg(j);
        replaceText.replace(captureLX, match.captured(j).toLower());
        replaceText.replace(captureUX, match.captured(j).toUpper());
        replaceText.replace(captureX, match.captured(j));
    }

    // allow captures \{0} .. \{9999999}...
    for (int j = match.lastCapturedIndex(); j >= 0; --j) {
        QString captureLX = QStringLiteral("\\L\\{%1}").arg(j);
        QString captureUX = QStringLiteral("\\U\\{%1}").arg(j);
        QString captureX = QStringLiteral("\\{%1}").arg(j);
        replaceText.replace(captureLX, match.captured(j).toLower());
        replaceText.replace(captureUX, match.captured(j).toUpper());
        replaceText.replace(captureX, match.captured(j));
    }

    replaceText.replace(QLatin1String("\\n"), QLatin1String("\n"));
    replaceText.replace(QLatin1String("\\t"), QLatin1String("\t"));
    replaceText.replace(QLatin1String("¤Search&Replace¤"), QLatin1String("\\"));

    // Replace the string
    doc->replaceText(matchItem->range, replaceText);

    // update the range
    int newEndLine = matchItem->range.start().line() + replaceText.count(QLatin1Char('\n'));
    int lastNL = replaceText.lastIndexOf(QLatin1Char('\n'));
    int newEndColumn = lastNL == -1 ? matchItem->range.start().column() + replaceText.length() : replaceText.length() - lastNL - 1;
    matchItem->range.setEnd(KTextEditor::Cursor{newEndLine, newEndColumn});

    // Convert replace text back to "html"
    replaceText.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    replaceText.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    matchItem->replaceText = replaceText;
    return true;
}

/** This function is used to replace a match */
bool MatchModel::replaceSingleMatch(KTextEditor::Document *doc, const QModelIndex &matchIndex, const QRegularExpression &regExp, const QString &replaceString)
{
    if (!doc) {
        qDebug() << "No doc";
        return false;
    }

    if (!isMatch(matchIndex)) {
        qDebug() << "This should not be possible";
        return false;
    }

    if (matchIndex.internalId() == InfoItemId || matchIndex.internalId() == FileItemId) {
        qDebug() << "You cannot replace a file or the info item";
        return false;
    }

    // Create a vector of moving ranges for updating the tree-view after replace
    QVector<KTextEditor::MovingRange *> matchRanges;
    KTextEditor::MovingInterface *miface = qobject_cast<KTextEditor::MovingInterface *>(doc);

    // Only add items after "matchIndex"
    int fileRow = matchIndex.internalId();
    int matchRow = matchIndex.row();

    QVector<Match> &matches = m_matchFiles[fileRow].matches;

    for (int i = matchRow+1; i < matches.size(); ++i) {
        KTextEditor::MovingRange *mr = miface->newMovingRange(matches[i].range);
        matchRanges.append(mr);
    }

    // The first range in the vector is for this match
    if (!replaceMatch(doc, matchIndex, regExp, replaceString)) {
        return false;
    }

    // Update the items after the matchIndex
    for (int i = matchRow+1; i < matches.size(); ++i) {
        Q_ASSERT(!matchRanges.isEmpty());
        KTextEditor::MovingRange *mr = matchRanges.takeFirst();
        matches[i].range = mr->toRange();
        delete mr;
    }
    Q_ASSERT(matchRanges.isEmpty());

    dataChanged(createIndex(matchRow, 0, fileRow), createIndex(matches.size()-1, 0, fileRow));

    return true;
}

// /** Replace all matches that have been checked */
// void MatchModel::replaceChecked(const QRegularExpression &regexp, const QString &replace)
// {
// }

QString MatchModel::matchToHtmlString(const Match &match) const
{
    QString pre =match.preMatchStr.toHtmlEscaped();

    QString matchStr = match.matchStr;
    matchStr.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    matchStr = matchStr.toHtmlEscaped();
    QString replaceStr = match.replaceText.toHtmlEscaped();
    if (!replaceStr.isEmpty()) {
        matchStr = QLatin1String("<i><s>") + matchStr + QLatin1String("</s></i> ");
    }
    matchStr = QStringLiteral("<span style=\"background-color:%1; color:%2;\">%3</span>")
    .arg(m_searchBackgroundColor.name(), m_foregroundColor.name(), matchStr);

    if (!replaceStr.isEmpty()) {
        matchStr += QStringLiteral("<span style=\"background-color:%1; color:%2;\">%3</span>")
        .arg(m_replaceHighlightColor.name(), m_foregroundColor.name(), replaceStr);
    }
    QString post = match.postMatchStr.toHtmlEscaped();

    // (line:col)[space][space] ...Line text pre [highlighted match] Line text post....
    QString displayText = QStringLiteral("(<b>%1:%2</b>) &nbsp;").arg(match.range.start().line() + 1).arg(match.range.start().column() + 1) + pre + matchStr + post;

    return displayText;
}

QString MatchModel::fileItemToHtmlString(const MatchFile &matchFile) const
{
    QString path = matchFile.fileUrl.isLocalFile() ? localFileDirUp(matchFile.fileUrl).path() : matchFile.fileUrl.url();
    if (!path.isEmpty() && !path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }

    QString tmpStr = QStringLiteral("%1<b>%2: %3</b>").arg(path, matchFile.fileUrl.fileName()).arg(matchFile.matches.size());

    return tmpStr;
}


bool MatchModel::isMatch(const QModelIndex &itemIndex) const
{
    if (!itemIndex.isValid()) return false;
    if (itemIndex.internalId() == InfoItemId) return false;
    if (itemIndex.internalId() == FileItemId) return false;

    return true;
}

QModelIndex MatchModel::fileIndex(const QUrl &url) const
{
    int row = matchFileRow(url);
    if (row == -1) return QModelIndex();
    return createIndex(row, 0, FileItemId);
}

QModelIndex MatchModel::firstMatch() const
{
    if (m_matchFiles.isEmpty()) return QModelIndex();

    return createIndex(0, 0, static_cast<quintptr>(0));
}

QModelIndex MatchModel::lastMatch() const
{
    if (m_matchFiles.isEmpty()) return QModelIndex();
    const MatchFile &matchFile = m_matchFiles.constLast();
    return createIndex(matchFile.matches.size()-1, 0, m_matchFiles.size()-1);
}

QModelIndex MatchModel::firstFileMatch(const QUrl &url) const
{
    int row = matchFileRow(url);
    if (row == -1) return QModelIndex();

    // if a file is in the vector it has a match
    return createIndex(0, 0, row);
}

QModelIndex MatchModel::closestMatchAfter(const QUrl &url, const KTextEditor::Cursor &cursor) const
{
    int row = matchFileRow(url);
    if (row < 0) return QModelIndex();
    if (row >= m_matchFiles.size()) return QModelIndex();
    if (!cursor.isValid()) return QModelIndex();

    // if a file is in the vector it has a match
    const MatchFile &matchFile = m_matchFiles[row];

    int i=0;
    for (; i<matchFile.matches.size()-1; ++i) {
        if (matchFile.matches[i].range.end() >= cursor) {
            break;
        }
    }

    return createIndex(i, 0, row);
}

QModelIndex MatchModel::closestMatchBefore(const QUrl &url, const KTextEditor::Cursor &cursor) const
{
    int row = matchFileRow(url);
    if (row < 0) return QModelIndex();
    if (row >= m_matchFiles.size()) return QModelIndex();
    if (!cursor.isValid()) return QModelIndex();

    // if a file is in the vector it has a match
    const MatchFile &matchFile = m_matchFiles[row];

    int i=matchFile.matches.size()-1;
    for (; i>=0; --i) {
        if (matchFile.matches[i].range.start() <= cursor) {
            break;
        }
    }

    return createIndex(i, 0, row);
}

QModelIndex MatchModel::nextMatch(const QModelIndex &itemIndex) const
{
    if (!itemIndex.isValid()) return firstMatch();

    int fileRow = itemIndex.internalId() < FileItemId ? itemIndex.internalId() : itemIndex.row();
    if (fileRow < 0 || fileRow >= m_matchFiles.size()) {
        return QModelIndex();
    }

    int matchRow = itemIndex.internalId() < FileItemId ? itemIndex.row() : 0;
    matchRow++;
    if (matchRow >= m_matchFiles[fileRow].matches.size()) {
        fileRow++;
        matchRow = 0;
    }

    if (fileRow >= m_matchFiles.size()) {
        fileRow = 0;
    }
    return createIndex(matchRow, 0, fileRow);
}

QModelIndex MatchModel::prevMatch(const QModelIndex &itemIndex) const
{
    if (!itemIndex.isValid()) return lastMatch();

    int fileRow = itemIndex.internalId() < FileItemId ? itemIndex.internalId() : itemIndex.row();
    if (fileRow < 0 || fileRow >= m_matchFiles.size()) {
        return QModelIndex();
    }

    int matchRow = itemIndex.internalId() < FileItemId ? itemIndex.row() : 0;
    matchRow--;
    if (matchRow < 0) {
        fileRow--;
    }
    if (fileRow < 0) {
        fileRow = m_matchFiles.size()-1;
    }
    if (matchRow < 0) {
        matchRow = m_matchFiles[fileRow].matches.size()-1;
    }
    return createIndex(matchRow, 0, fileRow);
}

QVariant MatchModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    if (index.column() < 0 || index.column() > 1) {
        return QVariant();
    }

    int fileRow = index.internalId() == InfoItemId ? -1 : index.internalId() == FileItemId ? index.row() : (int)index.internalId();
    int matchRow = index.internalId() == InfoItemId || index.internalId() == FileItemId ? -1 : index.row();

    if (fileRow == -1) {
        // Info Item
        switch (role) {
            case Qt::DisplayRole:
                return infoHtmlString();
            case Qt::CheckStateRole:
                return m_infoCheckState;
        }
        return QVariant();
    }

    if (fileRow < 0 || fileRow >= m_matchFiles.size()) {
        qDebug() << "Should be a file (or the info item in the near future)" << fileRow;
        return QVariant();
    }

    if (matchRow < 0) {
        // File item
        switch (role) {
            case Qt::DisplayRole:
                return fileItemToHtmlString(m_matchFiles[fileRow]);
            case Qt::CheckStateRole:
                return m_matchFiles[fileRow].checkState;
            case FileUrlRole:
                return m_matchFiles[fileRow].fileUrl;
        }
    }
    else if (matchRow < m_matchFiles[fileRow].matches.size()) {
        // Match
        const Match &match = m_matchFiles[fileRow].matches[matchRow];
        switch (role) {
            case Qt::DisplayRole:
                return matchToHtmlString(match);
            case Qt::CheckStateRole:
                return match.checked ? Qt::Checked : Qt::Unchecked;
            case FileUrlRole:
                return m_matchFiles[fileRow].fileUrl;
            case StartLineRole:
                return match.range.start().line();
            case StartColumnRole:
                return match.range.start().column();
            case EndLineRole:
                return match.range.end().line();
            case EndColumnRole:
                return match.range.end().column();
            case MatchLenRole:
                return match.matchLen;
            case PreMatchRole:
                return match.preMatchStr;
            case MatchRole:
                return match.matchStr;
            case PostMatchRole:
                return match.postMatchStr;
            case ReplacedRole:
                return !match.replaceText.isEmpty();
            case ReplaceTextRole:
                return match.replaceText;
        }
    }
    else {
        qDebug() << "bad index";
        return QVariant();
    }


    return QVariant();
}

bool MatchModel::setFileChecked(int fileRow, bool checked)
{
    if (fileRow < 0 || fileRow >= m_matchFiles.size()) return false;
    QVector<Match> &matches = m_matchFiles[fileRow].matches;
    for (int i = 0; i < matches.size(); ++i) {
        matches[i].checked = checked;
    }
    m_matchFiles[fileRow].checkState = checked ? Qt::Checked : Qt::Unchecked;
    QModelIndex rootFileIndex = index(fileRow, 0, createIndex(0, 0, InfoItemId));
    dataChanged(index(0, 0, rootFileIndex), index(matches.count()-1, 0, rootFileIndex), QVector<int>(Qt::CheckStateRole));
    dataChanged(rootFileIndex, rootFileIndex, QVector<int>(Qt::CheckStateRole));
    return true;
}


bool MatchModel::setData(const QModelIndex &itemIndex, const QVariant &, int role)
{
    if (role != Qt::CheckStateRole)
        return false;
    if (!itemIndex.isValid())
        return false;
    if (itemIndex.column() != 0)
        return false;

    // Check/un-check the File Item and it's children
    if (itemIndex.internalId() == InfoItemId) {
        bool checked = m_infoCheckState != Qt::Checked;
        for (int i=0; i<m_matchFiles.size(); ++i) {
            setFileChecked(i, checked);
        }
        m_infoCheckState = checked ? Qt::Checked : Qt::Unchecked;
        QModelIndex infoIndex = createIndex(0, 0, InfoItemId);
        dataChanged(infoIndex, infoIndex, QVector<int>(Qt::CheckStateRole));
        return true;
    }


    if (itemIndex.internalId() == FileItemId) {
        int fileRrow = itemIndex.row();
        if (fileRrow < 0 || fileRrow >= m_matchFiles.size()) return false;
        bool checked = m_matchFiles[fileRrow].checkState != Qt::Checked; // we toggle the current value
        setFileChecked(fileRrow, checked);

        // compare file items
        Qt::CheckState checkState = m_matchFiles[0].checkState;
        for (int i=1; i<m_matchFiles.size(); ++i) {
            if (checkState != m_matchFiles[i].checkState) {
                checkState = Qt::PartiallyChecked;
                break;
            }
        }
        m_infoCheckState = checkState;
        QModelIndex infoIndex = createIndex(0, 0, InfoItemId);
        dataChanged(infoIndex, infoIndex, QVector<int>(Qt::CheckStateRole));
        return true;
    }

    int rootRow = itemIndex.internalId();
    if (rootRow < 0 || rootRow >= m_matchFiles.size())
        return false;

    int row = itemIndex.row();
    QVector<Match> &matches = m_matchFiles[rootRow].matches;
    if (row < 0 || row >= matches.size())
        return false;

    // we toggle the current value
    matches[row].checked = !matches[row].checked;

    int checkedCount = std::count_if(matches.begin(), matches.end(), [](const MatchModel::Match &match) {return match.checked;});

    if (checkedCount == matches.size()) {
        m_matchFiles[rootRow].checkState = Qt::Checked;
    }
    else if (checkedCount == 0) {
        m_matchFiles[rootRow].checkState = Qt::Unchecked;
    }
    else {
        m_matchFiles[rootRow].checkState = Qt::PartiallyChecked;
    }

    QModelIndex rootFileIndex = index(rootRow, 0);
    dataChanged(rootFileIndex, rootFileIndex, QVector<int>(Qt::CheckStateRole));
    dataChanged(index(row, 0, rootFileIndex), index(row, 0, rootFileIndex), QVector<int>(Qt::CheckStateRole));
    return true;
}

Qt::ItemFlags MatchModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }

    if (index.column() == 0) {
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
    }

    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

int MatchModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        return m_matchFiles.isEmpty() ? 0 : 1;
    }

    if (parent.internalId() == InfoItemId) {
        return m_matchFiles.size();
    }

    if (parent.internalId() != FileItemId) {
        // matches do not have children
        return 0;
    }

    // If we get here parent.internalId() == FileItemId
    int row = parent.row();
    if (row < 0 || row >= m_matchFiles.size()) {
        return 0;
    }

    return m_matchFiles[row].matches.size();
}

int MatchModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QModelIndex MatchModel::index(int row, int column, const QModelIndex &parent) const
{
    // Create the Info Item
    if (!parent.isValid()) {
        return createIndex(0, 0, InfoItemId);
    }

    // File Item
    if (parent.internalId() == InfoItemId) {
        return createIndex(row, column, FileItemId);
    }

    // Match Item
    if (parent.internalId() == FileItemId) {
        return createIndex(row, column, parent.row());
    }

    // Parent is a match which does not have children
    return QModelIndex();
}

QModelIndex MatchModel::parent(const QModelIndex &child) const
{
    if (child.internalId() == InfoItemId) {
        return QModelIndex();
    }

    if (child.internalId() == FileItemId) {
        return createIndex(0, 0, InfoItemId);
    }

    return createIndex(child.internalId(), 0, FileItemId);
}
