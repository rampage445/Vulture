#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "traverselib.h"
#include "drivewatcher.h"
#include <QtSql>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScreen>
#include <QPoint>
#include <QLabel>
#include <QApplication>
#include <QStyle>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QtConcurrent/QtConcurrent>
#include <QSystemTrayIcon>
#include <QShortcut>
#include <QMenu>
#include <QKeyEvent>
#include <QThread>
#include <QAtomicInt>
#include <QDebug>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>
#include <QFileInfo>
#include <QPushButton>
#include <QTimer>
#include <QDateTime>

#define debugg(msg) qDebug() << msg
using namespace std;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , db(QSqlDatabase::addDatabase("QSQLITE", "db_connection"))
{
    ui->setupUi(this);
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(500, 110);

    db.setDatabaseName(QDir::currentPath() + "/files.db");
    if (!db.open()) {
        qWarning() << "Failed to open database connection:" << db.lastError().text();
    }


    // Main layout
    QWidget *central = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(central);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignTop);

    // Search input
    inputField = new QLineEdit(this);
    inputField->setFixedSize(480, 50);
    inputField->setPlaceholderText("Search...");
    inputField->setStyleSheet(R"(
        QLineEdit {
            border: 2px solid #ccc;
            border-radius: 25px;
            padding-right: 40px;
            padding-left: 15px;
            background-color: white;
            font-size: 18px;
        }
    )");
    layout->addWidget(inputField);

    // Container with fixed width 370 and white background
    QWidget *statusScanWidget = new QWidget(this);
    statusScanWidget->setFixedWidth(370);
    statusScanWidget->setFixedHeight(50);
    statusScanWidget->setStyleSheet("background-color: #d8f999; border-radius: 6px;");

    QHBoxLayout *statusLayout = new QHBoxLayout(statusScanWidget);
    statusLayout->setContentsMargins(0, 0, 0,20);
    statusLayout->setSpacing(0);

    statusLabel = new QLabel(statusScanWidget);
    statusLabel->setText("Last scanned: --");
    statusLabel->setAlignment(Qt::AlignCenter);
    statusLabel->setStyleSheet("color: black; font-size: 15px;");
    statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    statusLayout->addWidget(statusLabel);


    QHBoxLayout *centerLayout = new QHBoxLayout;
    centerLayout->addStretch();
    centerLayout->addWidget(statusScanWidget);
    centerLayout->addStretch();

    layout->addSpacing(15);
    layout->addLayout(centerLayout);


    QPushButton *exitButton = new QPushButton("X", this);
    exitButton->setFixedSize(25, 25);
    exitButton->setToolTip("Exit");
    exitButton->setStyleSheet(R"(
    QPushButton {
        background-color: #E57373;
        color: white;
        border: none;
        border-radius: 12px;
        font-weight: bold;
        font-size: 14px;
    }
    QPushButton:hover {
        background-color: #EF5350;
    }
    QPushButton:pressed {
        background-color: #D32F2F;
    }
    )");


    exitButton->move(width() - exitButton->width() - 15, 1);
    exitButton->raise();
    exitButton->show();

    connect(exitButton, &QPushButton::clicked, qApp, &QCoreApplication::quit);
    setCentralWidget(central);

    // Search icon
    QLabel *iconLabel = new QLabel(inputField);
    QPixmap icon(":/icon/search.png");
    iconLabel->setPixmap(icon.scaled(20, 20, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    iconLabel->setFixedSize(20, 20);
    iconLabel->move(inputField->width() - 35, (inputField->height() - 20) / 2);
    iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    iconLabel->show();

    suggestionList = new QListWidget(this);
    suggestionList->setWindowFlags(Qt::FramelessWindowHint | Qt::ToolTip);
    suggestionList->setFocusPolicy(Qt::NoFocus);
    suggestionList->setAttribute(Qt::WA_ShowWithoutActivating);
    suggestionList->setMouseTracking(true);
    suggestionList->setStyleSheet(R"(
        QListWidget {
            border: 1px solid #aaa;
            border-radius: 6px;
            background-color: white;
        }
        QListWidget::item {
            margin: 2px;
        }
        QListWidget::item:hover {
            background-color: #f5f5f5;
        }
    )");
    suggestionList->hide();

    // Update last scan timestamp from db file
    auto updateLastScanLabel = [=]() {
        QFileInfo dbInfo(db.databaseName());
        if (dbInfo.exists()) {
            QDateTime modified = dbInfo.lastModified();
            statusLabel->setText("Last scanned: " + modified.toString("hh:mm AP d MMMM, yyyy"));
        } else {
            statusLabel->setText("Last scanned: --");
        }
    };
    // updateLastScanLabel();


    // Debounced search logic
    searchWatcher = new QFutureWatcher<QStringList>(this);
    debounceTimer = new QTimer(this);
    debounceTimer->setSingleShot(true);
    int debounceDelayMs = 2000;

    auto searchFileConcurrent = [this](const QString &text) -> QStringList {
        QStringList allPaths;

        {
            QString connectionName = QUuid::createUuid().toString();
            QSqlDatabase threadDb = QSqlDatabase::addDatabase("QSQLITE", connectionName);
            threadDb.setDatabaseName(QDir::currentPath() + "/files.db");

            if (threadDb.open()) {
                QSqlQuery query(threadDb);
                if (query.exec("SELECT path FROM items ORDER BY priority DESC, path ASC;")) {
                    while (query.next()) {
                        allPaths.append(query.value(0).toString());
                    }
                } else {
                    qWarning() << "DB query error in thread:" << query.lastError().text();
                }

                threadDb.close();
            }

            QSqlDatabase::removeDatabase(connectionName);
        }
        int totalThreads = QThread::idealThreadCount();
        int threadsToUse = max(2, totalThreads - 2);
        int chunkSize = allPaths.size() / threadsToUse + 1;

        QList<QStringList> chunks;
        for (int i = 0; i < allPaths.size(); i += chunkSize) {
            chunks.append(allPaths.mid(i, chunkSize));
        }

        QAtomicInt foundCount = 0;
        const int maxResults = 500; //shows 500 results

        auto mapFunc = [text, &foundCount, maxResults](const QStringList &chunk) -> QStringList {
            QStringList filtered;
            for (const QString &pah : chunk) {
                if (foundCount.loadRelaxed() >= maxResults) break;
                QString fname = QFileInfo(pah).fileName();
                if (fname.contains(text, Qt::CaseInsensitive)) {
                    int prev = foundCount.fetchAndAddRelaxed(1);
                    if (prev < maxResults) {
                        filtered.append(pah);
                    } else {
                        break;
                    }
                }
            }
            reverse(filtered.begin(),filtered.end());
            return filtered;
        };


        auto reduceFunc = [](QStringList &result, const QStringList &partial) {
            result.append(partial);
            if (result.size() > 50) {
                result = result.mid(0, 50);
            }
        };

        QStringList results = QtConcurrent::mappedReduced(chunks, mapFunc, reduceFunc, QtConcurrent::UnorderedReduce).result();

        sort(results.begin(), results.end(), [](const QString &a, const QString &b) {
            auto getPriority = [](const QString &path) -> int {
                QFileInfo fi(path);
                if (fi.isDir()) return 2;  // Folders
                QString ext = fi.suffix().toLower();

                // High priority file types
                QStringList popular = { "exe", "jpg", "jpeg", "png", "pdf", "docx", "txt", "xlsx", "pptx", "mp4", "mp3" };
                if (ext == "lnk") return 3; // Shortcuts
                if (popular.contains(ext)) return 0; // Popular files
                return 1; // Other files
            };

            return getPriority(a) < getPriority(b); // lower number = higher priority
        });

        return results;
    };

    // the search will only start once user has finished giving inputs
    connect(inputField, &QLineEdit::textChanged, this, [=](const QString &text) {
        statusLabel->setText("<b style='color:black;'>Searching...</b>");
        lastResults.clear();
        //At least the input length should be 3
        if (text.trimmed().length() < 3) {
            debounceTimer->stop();
            suggestionList->hide();
            updateLastScanLabel();
            return;
        }
        if (searchWatcher->isRunning()) {

            searchWatcher->cancel();
            searchWatcher->waitForFinished();
        }
        debounceTimer->start(debounceDelayMs);
    });

    connect(debounceTimer, &QTimer::timeout, this, [=]() {
        QString text = inputField->text().trimmed();
        //At least the input length should be 3
        if (text.length() < 3) {
            suggestionList->hide();
            return;
        }
        if (searchWatcher->isRunning()) {
            searchWatcher->cancel();
            searchWatcher->waitForFinished();
        }
        if (!text.isEmpty()) {
            QFuture<QStringList> future = QtConcurrent::run(searchFileConcurrent, text);
            searchWatcher->setFuture(future);
        }

    });

    connect(searchWatcher, &QFutureWatcher<QStringList>::finished, this, [=]() {
        QStringList matches = searchWatcher->result();
        lastResults = matches;
        suggestionList->clear();
        if (matches.isEmpty()) {
            QListWidgetItem *item = new QListWidgetItem(suggestionList);
            item->setSizeHint(QSize(inputField->width(), 50));
            QWidget *noResultWidget = new QWidget;
            QHBoxLayout *layout = new QHBoxLayout(noResultWidget);
            layout->setContentsMargins(10, 5, 10, 5);
            QLabel *label = new QLabel("No results found.");
            label->setStyleSheet("font-size:16px; color:gray; font-style:italic;");
            layout->addWidget(label, 0, Qt::AlignLeft | Qt::AlignVCenter);
            suggestionList->addItem(item);
            suggestionList->setItemWidget(item, noResultWidget);
        } else {
            for (const QString &pah : matches) {
                QString path = pah;
                path = path.replace("\\\\", "\\");
                QListWidgetItem *item = new QListWidgetItem(suggestionList);
                item->setSizeHint(QSize(inputField->width(), 60));
                item->setData(Qt::UserRole, path);
                suggestionList->addItem(item);
                suggestionList->setItemWidget(item, new ResultItemWidget(path));
            }
        }
        suggestionList->setFixedWidth(inputField->width());
        int visibleCount = qMin(matches.size(), 6);
        int height = (matches.isEmpty() ? 50 : visibleCount * 60) + 8;
        QPoint inputPos = inputField->mapToGlobal(QPoint(0, 0));
        suggestionList->setFixedHeight(height);
        suggestionList->move(inputPos.x(), inputPos.y() - height - 8);
        suggestionList->show();
        inputField->setFocus();
        statusLabel->clear();
        updateLastScanLabel();

    });

    connect(suggestionList, &QListWidget::itemClicked, this, [=](QListWidgetItem *item) {
       // QString path = item->data(Qt::UserRole).toString();
       // if (!path.isEmpty()) {
            // inputField->setText(path);
       // }
        inputField->setFocus();
    });

    // Context Menu
    suggestionList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(suggestionList, &QListWidget::customContextMenuRequested, this, [=](const QPoint &pos) {
        QListWidgetItem *item = suggestionList->itemAt(pos);
        if (!item) return;
        QString path = item->data(Qt::UserRole).toString();
        if (path.isEmpty()) return;

        QMenu menu;
        menu.addAction("Open", this, [=]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
        menu.addAction("Open file location", this, [=]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        });
        menu.addSeparator();
        menu.addAction("Delete", this, [=]() {
            if (QMessageBox::question(this, "Confirm Delete", "Delete:\n" + path + " ?") == QMessageBox::Yes) {
                QFileInfo fi(path);
                if (fi.isDir()) QDir(path).removeRecursively();
                else QFile(path).remove();
            }
        });

        menu.exec(suggestionList->viewport()->mapToGlobal(pos));
    });

    // Double click on entry(folders,files)
    connect(suggestionList, &QListWidget::itemDoubleClicked, this, [=](QListWidgetItem *item) {
        QString path = item->data(Qt::UserRole).toString();
        if (!path.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
    });

    inputField->installEventFilter(this);


    // System Tray Icon
    QSystemTrayIcon *tray = new QSystemTrayIcon(QIcon(":/icon/logo.png"), this);

    QMenu *trayMenu = new QMenu(this);
    QAction *showAction = new QAction("Show", this);
    connect(showAction, &QAction::triggered, this, [=]() {
        this->show();
        this->raise();
        this->activateWindow();
    });
    trayMenu->addAction(showAction);
    trayMenu->addSeparator();
    QAction *quitAction = new QAction("Quit", this);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
    trayMenu->addAction(quitAction);
    tray->setContextMenu(trayMenu);
    tray->show();

    QShortcut *escShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escShortcut, &QShortcut::activated, this, [=]() {
        this->hide();
        suggestionList->hide();
    });

    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect screenGeometry = screen->availableGeometry();
        move(screenGeometry.bottomRight() - QPoint(width(), height()));
    }

    QTimer::singleShot(0, [=]() {
        inputField->setFocus();
    });

    // Initial Status
    statusLabel->setText("<b style='color:black;'>Scanning...</b>");
    inputField->setReadOnly(true);
    auto traverseWatcher = new QFutureWatcher<void>(this);
    auto traverseFuture = QtConcurrent::run([=]() {
        traverseAll();  // Runs in background
    });
    traverseWatcher->setFuture(traverseFuture);

    // Update label after scan finishes
    connect(traverseWatcher, &QFutureWatcher<void>::finished, this, [=]() {
        updateLastScanLabel();
        inputField->setReadOnly(false);
    });

    // Monitors all drives for changes
    DriveWatch();
}

MainWindow::~MainWindow()
{
    debugg("here destroyed!");
    if (db.isOpen()) {
        db.close();
        QSqlDatabase::removeDatabase("db_connection");
    }
    delete ui;
}

bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::WindowDeactivate) {
        if (suggestionList->isVisible()) {
            suggestionList->hide();
        }
         this->setWindowOpacity(0.7);
    } else if (event->type() == QEvent::WindowActivate) {
        if (!lastResults.isEmpty() && !suggestionList->isVisible()) {
            suggestionList->show();
        }
         this->setWindowOpacity(1.0);
    }
    return QMainWindow::event(event);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == inputField && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            this->hide();
            suggestionList->hide();
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

