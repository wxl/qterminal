#if QT_VERSION < 0x050000
#include <QDesktopServices>
#else
#include <QStandardPaths>
#endif

#include <QDebug>

#include "bookmarkswidget.h"
#include "properties.h"
#include "config.h"


class AbstractBookmarkItem
{
public:
    enum ItemType {
        Root = 0,
        Group = 1,
        Command = 2
    };

    AbstractBookmarkItem(AbstractBookmarkItem* parent = 0)
    {
        m_parent = parent;
    }
    ~AbstractBookmarkItem()
    {
        qDeleteAll(m_children);
    }

    ItemType type() { return m_type; }
    QString value() { return m_value; }
    QString display() { return m_display; }

    void addChild(AbstractBookmarkItem* item) { m_children << item; }
    int childCount() { return m_children.count(); }
    QList<AbstractBookmarkItem*> children() { return m_children; }
    AbstractBookmarkItem *child(int number) { return m_children.value(number); }
    AbstractBookmarkItem *parent() { return m_parent; }

    int childNumber() const
    {
        if (m_parent)
            return m_parent->children().indexOf(const_cast<AbstractBookmarkItem*>(this));

        return 0;
    }

protected:
    ItemType m_type;
    AbstractBookmarkItem *m_parent;
    QList<AbstractBookmarkItem*> m_children;
    QString m_value;
    QString m_display;
};

class BookmarkRootItem : public AbstractBookmarkItem
{
public:
    BookmarkRootItem()
        : AbstractBookmarkItem()
    {
        m_type = AbstractBookmarkItem::Root;
        m_value = m_display = "root";
    }
};

class BookmarkCommandItem : public AbstractBookmarkItem
{
public:
    BookmarkCommandItem(const QString &name, const QString &command, AbstractBookmarkItem *parent)
        : AbstractBookmarkItem(parent)
    {
        m_type = AbstractBookmarkItem::Command;
        m_value = command;
        m_display = name;
    }
};


class BookmarkGroupItem : public AbstractBookmarkItem
{
public:
    BookmarkGroupItem(const QString &name, AbstractBookmarkItem *parent)
        : AbstractBookmarkItem(parent)
    {
        m_type = AbstractBookmarkItem::Group;
        m_display = name;
    }
};

class BookmarkLocalGroupItem : public BookmarkGroupItem
{
public:
    BookmarkLocalGroupItem(AbstractBookmarkItem *parent)
        : BookmarkGroupItem(QObject::tr("Local Bookmarks"), parent)
    {
#if QT_VERSION < 0x050000
        QList<QDesktopServices::StandardLocation> locations;
        locations << QDesktopServices::DesktopLocation
                  << QDesktopServices::DocumentsLocation
                  << QDesktopServices::TempLocation
                  << QDesktopServices::HomeLocation
                  << QDesktopServices::MusicLocation
                  << QDesktopServices::PicturesLocation;

        QString path;
        QString name;
        QString cmd;
        QDir d;

        // standard $HOME subdirs
        foreach (QDesktopServices::StandardLocation i, locations)
        {
            path = QDesktopServices::storageLocation(i);
            if (!d.exists(path))
            {
                continue;
            }
            // it works in Qt5, not in Qt4
            // name = QDesktopServices::displayName(i);
            name = path;

            path.replace(" ", "\\ ");
            cmd = "cd " + path;

            addChild(new BookmarkCommandItem(name, cmd, this));
        }
#else
        QList<QStandardPaths::StandardLocation> locations;
        locations << QStandardPaths::DesktopLocation
                  << QStandardPaths::DocumentsLocation
                  << QStandardPaths::TempLocation
                  << QStandardPaths::HomeLocation
                  << QStandardPaths::MusicLocation
                  << QStandardPaths::PicturesLocation;

        QString path;
        QString name;
        QString cmd;
        QDir d;

        // standard $HOME subdirs
        foreach (QStandardPaths::StandardLocation i, locations)
        {
            path = QStandardPaths::writableLocation(i);
            if (!d.exists(path))
            {
                continue;
            }
            name = QStandardPaths::displayName(i);

            path.replace(" ", "\\ ");
            cmd = "cd " + path;

            addChild(new BookmarkCommandItem(name, cmd, this));
        }
#endif

        // system env - include dirs in the tree
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        foreach (QString i, env.keys())
        {
            path = env.value(i);
            if (!d.exists(path) || !QFileInfo(path).isDir())
            {
                continue;
            }
            path.replace(" ", "\\ ");
            cmd = "cd " + path;
            addChild(new BookmarkCommandItem(i, cmd, this));
        }
    }
};

class BookmarkFileGroupItem : public BookmarkGroupItem
{
    // hierarchy handling
    // m_pos to group map. Example: group1.group2=item
    QHash<QString,AbstractBookmarkItem*> m_map;
    // current position level. Example "group1", "group2"
    QStringList m_pos;

public:
    BookmarkFileGroupItem(AbstractBookmarkItem *parent, const QString &fname)
        : BookmarkGroupItem(QObject::tr("Synchronized Bookmarks"), parent)
    {
        QFile f(fname);
        if (!f.open(QIODevice::ReadOnly))
        {
            qDebug() << "Canot open file" << fname;
            // TODO/FIXME: message box
            return;
        }

        QXmlStreamReader xml;
        xml.setDevice(&f);

        while (true)
        {
            xml.readNext();

            switch (xml.tokenType())
            {
            case QXmlStreamReader::StartElement:
            {
                AbstractBookmarkItem *parent = m_map.contains(xmlPos()) ? m_map[xmlPos()] : this;
                QString tag = xml.name().toString();
                if (tag == "group")
                {
                    QString name = xml.attributes().value("name").toString();
                    m_pos.append(name);

                    BookmarkGroupItem *i = new BookmarkGroupItem(name, parent);
                    parent->addChild(i);

                    m_map[xmlPos()] = i;
                }
                else if (tag == "command")
                {
                    QString name = xml.attributes().value("name").toString();
                    QString cmd = xml.attributes().value("value").toString();

                    BookmarkCommandItem *i = new BookmarkCommandItem(name, cmd, parent);
                    parent->addChild(i);
                }
                break;
            }
            case QXmlStreamReader::EndElement:
            {
                QString tag = xml.name().toString();
                if (tag == "group")
                {
                    m_pos.removeLast();
                }
                break;
            }
            case QXmlStreamReader::Invalid:
                qDebug() << "XML error: " << xml.errorString().data()
                         << xml.lineNumber() << xml.columnNumber();
                m_map.clear();
                return;
                break;
            case QXmlStreamReader::EndDocument:
                m_map.clear();
                return;
                break;
            } // switch
        } // while
    } // constructor

    QString xmlPos()
    {
        return m_pos.join(".");
    }
};


BookmarksModel::BookmarksModel(QObject *parent)
    : QAbstractItemModel(parent),
      m_root(0)
{
    setup();
}

void BookmarksModel::setup()
{
    if (m_root)
        delete m_root;
    m_root = new BookmarkRootItem();
    m_root->addChild(new BookmarkLocalGroupItem(m_root));
    m_root->addChild(new BookmarkFileGroupItem(m_root, Properties::Instance()->bookmarksFile));
    beginResetModel();
    endResetModel();
}

BookmarksModel::~BookmarksModel()
{
    if (m_root)
        delete m_root;
}

int BookmarksModel::columnCount(const QModelIndex & /* parent */) const
{
    return 2;
}

QVariant BookmarksModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    switch (role)
    {
    case Qt::DisplayRole:
    case Qt::EditRole:
        return index.column() == 0 ? getItem(index)->display() : getItem(index)->value();
    case Qt::FontRole:
    {
        QFont f;
        if (static_cast<AbstractBookmarkItem*>(index.internalPointer())->type() == AbstractBookmarkItem::Group)
        {
            f.setBold(true);
        }
        return f;
    }
    default:
        return QVariant();
    }
}

AbstractBookmarkItem *BookmarksModel::getItem(const QModelIndex &index) const
{
    if (index.isValid())
    {
        AbstractBookmarkItem *item = static_cast<AbstractBookmarkItem*>(index.internalPointer());
        if (item)
            return item;
    }
    return m_root;
 }

QVariant BookmarksModel::headerData(int section, Qt::Orientation orientation,
                                    int role) const
{
    return QVariant();
}

QModelIndex BookmarksModel::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid() && parent.column() != 0)
        return QModelIndex();

    AbstractBookmarkItem *parentItem = getItem(parent);

    AbstractBookmarkItem *childItem = parentItem->child(row);
    if (childItem)
        return createIndex(row, column, childItem);
    else
        return QModelIndex();
}


QModelIndex BookmarksModel::parent(const QModelIndex &index) const
{
    if (!index.isValid())
        return QModelIndex();

    AbstractBookmarkItem *childItem = getItem(index);
    AbstractBookmarkItem *parentItem = childItem->parent();

    if (parentItem == m_root)
        return QModelIndex();

    return createIndex(parentItem->childNumber(), 0, parentItem);
}

int BookmarksModel::rowCount(const QModelIndex &parent) const
{
    AbstractBookmarkItem *parentItem = getItem(parent);
    return parentItem->childCount();
}

#if 0
bool BookmarksModel::setData(const QModelIndex &index, const QVariant &value,
                             int role)
{
    if (role != Qt::EditRole)
        return false;

    AbstractBookmarkItem *item = getItem(index);
    bool result = item->setData(index.column(), value);

    if (result)
        emit dataChanged(index, index);

    return result;
}
#endif


BookmarksWidget::BookmarksWidget(QWidget *parent)
    : QWidget(parent)
{
    setupUi(this);

    m_model = new BookmarksModel(this);
    treeView->setModel(m_model);
    treeView->header()->hide();

    connect(treeView, SIGNAL(doubleClicked(QModelIndex)),
            this, SLOT(handleCommand(QModelIndex)));
}

BookmarksWidget::~BookmarksWidget()
{
}

void BookmarksWidget::setup()
{
    m_model->setup();

    treeView->expandAll();
    treeView->resizeColumnToContents(0);
    treeView->resizeColumnToContents(1);
}

void BookmarksWidget::handleCommand(const QModelIndex& index)
{
    AbstractBookmarkItem *item = static_cast<AbstractBookmarkItem*>(index.internalPointer());
    if (!item && item->type() != AbstractBookmarkItem::Command)
        return;

    emit callCommand(item->value() + "\n"); // TODO/FIXME: decide how to handle EOL
}