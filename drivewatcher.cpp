#include <windows.h>
#include <QString>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDir>
#include <QSqlError>
#include <QDebug>
#include <QFileInfo>
#include <QDateTime>
#include <QTimer>
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
using namespace std;

enum class FileStatus { Created, Deleted, Renamed };

struct FileChange {
    wstring path;
    FileStatus status;
    QChar type;
    QDateTime timestamp;
};

mutex g_mutex;
vector<FileChange> g_pendingInserts;

wstring GetFileName(const FILE_NOTIFY_INFORMATION* fni) {
    return wstring(fni->FileName, fni->FileNameLength / sizeof(WCHAR));
}

// Get all system drives
vector<wstring> GetAllDrives() {
    DWORD driveMask = GetLogicalDrives();
    vector<wstring> drives;
    for (int i = 0; i < 26; ++i) {
        if (driveMask & (1 << i)) {
            wchar_t letter = L'A' + i;
            wstring driveRoot = wstring(1, letter) + L":\\";
            if (GetDriveType(driveRoot.c_str()) != DRIVE_REMOVABLE) 
                drives.push_back(driveRoot);
        }
    }
    return drives;
}

// Determine if path is file or directory
QChar DetectFileType(const wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return '?';
    return (attr & FILE_ATTRIBUTE_DIRECTORY) ? 'd' : 'f';
}

// Insert queued "created" items into DB after delay
void StartInsertWorker(const QString& connName) {
    thread([connName]() {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(QDir::currentPath() + "/files.db");

        if (!db.open()) {
            qWarning() << "InsertWorker DB open failed:" << db.lastError().text();
            return;
        }

        QSqlQuery query(db);

        while (true) {
            this_thread::sleep_for(chrono::milliseconds(500));

            vector<FileChange> toInsert;

            {
                lock_guard<mutex> lock(g_mutex);
                QDateTime now = QDateTime::currentDateTime();

                auto it = g_pendingInserts.begin();
                while (it != g_pendingInserts.end()) {
                    /*
                    Assume user has just created a folder , before pushing the folder path to db,
                    it waits for 15 seconds, user can change the folder name during this period.
                    */
                    if (it->timestamp.msecsTo(now) >= 15000) {
                        toInsert.push_back(*it);
                        it = g_pendingInserts.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            for (const FileChange& change : toInsert) {
                QString path = QString::fromStdWString(change.path);
                QString type = QString(change.type);

                query.prepare("INSERT OR REPLACE INTO items (path, type) VALUES (:path, :type)");
                query.bindValue(":path", path);
                query.bindValue(":type", type);

                if (!query.exec()) {
                    qWarning() << "Delayed Insert Failed:" << query.lastError().text();
                }
            }
        }

        db.close();
        QSqlDatabase::removeDatabase(connName);
    }).detach();
}

// Monitor a single drive for changes
void MonitorDrive(const wstring& rootPath) {
    HANDLE hDir = CreateFileW(
        rootPath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
        );

    if (hDir == INVALID_HANDLE_VALUE) {
        qWarning() << "Failed to open directory handle for" << QString::fromStdWString(rootPath);
        return;
    }

    QString connName = "Watcher_" + QString::number((quintptr)GetCurrentThreadId());
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
    db.setDatabaseName(QDir::currentPath() + "/files.db");

    if (!db.open()) {
        qWarning() << "Failed to open DB in MonitorDrive:" << db.lastError().text();
        return;
    }

    QSqlQuery query(db);
    char buffer[8192];
    DWORD bytesReturned;

    while (true) {
        if (ReadDirectoryChangesW(
                hDir,
                buffer,
                sizeof(buffer),
                TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
                &bytesReturned,
                nullptr,
                nullptr)) {

            FILE_NOTIFY_INFORMATION* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
            wstring oldName;

            do {
                wstring relative = GetFileName(fni);
                wstring fullPath = rootPath + relative;
                FileChange change;

                switch (fni->Action) {
                case FILE_ACTION_ADDED:
                    change = {
                        fullPath,
                        FileStatus::Created,
                        DetectFileType(fullPath),
                        QDateTime::currentDateTime()
                    };

                    {
                        lock_guard<mutex> lock(g_mutex);
                        g_pendingInserts.push_back(change);
                    }
                    break;

                case FILE_ACTION_REMOVED:
                    query.prepare("DELETE FROM items WHERE path = :path");
                    query.bindValue(":path", QString::fromStdWString(fullPath));
                    if (!query.exec())
                        qWarning() << "Delete failed:" << query.lastError().text();
                    break;

                case FILE_ACTION_RENAMED_OLD_NAME:
                    oldName = fullPath;
                    break;

                case FILE_ACTION_RENAMED_NEW_NAME:
                    // Remove old name
                    query.prepare("DELETE FROM items WHERE path = :path");
                    query.bindValue(":path", QString::fromStdWString(oldName));
                    if (!query.exec())
                        qWarning() << "Rename delete failed:" << query.lastError().text();

                    // Insert new name immediately
                    query.prepare("INSERT OR REPLACE INTO items (path, type) VALUES (:path, :type)");
                    query.bindValue(":path", QString::fromStdWString(fullPath));
                    query.bindValue(":type", QString(DetectFileType(fullPath)));
                    if (!query.exec())
                        qWarning() << "Rename insert failed:" << query.lastError().text();
                    break;

                default:
                    break;
                }

                if (fni->NextEntryOffset == 0)
                    break;

                fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                    reinterpret_cast<BYTE*>(fni) + fni->NextEntryOffset
                    );

            } while (true);

        } else {
            qWarning() << "ReadDirectoryChangesW failed. Exiting thread.";
            break;
        }
    }

    CloseHandle(hDir);
    db.close();
    QSqlDatabase::removeDatabase(connName);
}

void DriveWatch() {
    auto drives = GetAllDrives();
    StartInsertWorker("InsertWorker");

    for (const auto& drive : drives) {
        thread(MonitorDrive, drive).detach();
    }
}
