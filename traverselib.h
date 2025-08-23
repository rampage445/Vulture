#ifndef TRAVERSELIB_H
#define TRAVERSELIB_H

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDir>
#include <QString>
#include <windows.h>
#include <dirent.h>
#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>
#include <atomic>
#include <set>

#define LOG(msg) cout << msg << endl;

using namespace std;

int getPriorityFromPath(const string& path);

namespace TraverseInternal {

    queue<string> taskQueue;
    mutex queueMutex;
    condition_variable cv;
    atomic<bool> done{false};

    struct FileItem {
        string path;
        string type;
        int priority;
    };

    vector<FileItem> itms;

    mutex itmsMutex;

    atomic<int> activeWorkers{0};

    void enqueueDirectory(const string& path) {
        lock_guard<mutex> lock(queueMutex);
        taskQueue.push(path);
        cv.notify_one();
    }

    void processDirectory(const string& path) {
        DIR* dir = opendir(path.c_str());
        if (!dir) return;

        struct dirent* d = nullptr;
        while ((d = readdir(dir)) != nullptr) {
            if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..") || strchr(d->d_name, '$')) continue;

            string newPath = path + "\\" + d->d_name;
            DWORD attr = GetFileAttributesA(newPath.c_str());

            bool isDir = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));

            {
                lock_guard<mutex> lock(itmsMutex);
                itms.push_back({ newPath, isDir ? "d" : "f", getPriorityFromPath(newPath) });
            }

            if (isDir) {
                enqueueDirectory(newPath);
            }
        }

        closedir(dir);
    }

    void workerThread() {
        while (true) {
            string task;

            {
                unique_lock<mutex> lock(queueMutex);
                cv.wait(lock, [] { return done || !taskQueue.empty(); });

                if (done && taskQueue.empty()) return;

                task = taskQueue.front();
                taskQueue.pop();
                activeWorkers++;
            }

            processDirectory(task);

            {
                lock_guard<mutex> lock(queueMutex);
                activeWorkers--;
                if (taskQueue.empty() && activeWorkers == 0) {
                    done = true;
                    cv.notify_all();
                }
            }
        }
    }

    void batchInsertToDB(QSqlDatabase& db, const vector<FileItem>& items) {
        QSqlQuery query(db);
        db.transaction();

        for (const auto& item : items) {
            query.prepare("INSERT OR IGNORE INTO items (path, type, priority) VALUES (:path, :type, :priority)");
            query.bindValue(":path", QString::fromStdString(item.path));
            query.bindValue(":type", QString::fromStdString(item.type));
            query.bindValue(":priority", item.priority);
            query.exec();
        }

        db.commit();
    }


    void traverseAllDrives(unsigned int numThreads) {
        LOG("Traversing...");

        char driveList[MAX_PATH];
        if (GetLogicalDriveStringsA(MAX_PATH, driveList) == 0) return;

        set<char> drivesToScan = {'C','D','E'}; //filter
        char* drive = driveList;

        while (*drive) {
            char driveLetter = toupper(drive[0]);
            //if (drivesToScan.count(driveLetter)) {
                string rootPath = string(1, driveLetter) + ":\\";
                enqueueDirectory(rootPath);
            //}
            drive += strlen(drive) + 1;
        }

        vector<thread> threads;
        for (unsigned int i = 0; i < numThreads; ++i)
            threads.emplace_back(workerThread);

        for (auto& t : threads)
            if (t.joinable()) t.join();
    }

}

QString getSystemBootTime() {
#if defined(_WIN32) || defined(_WIN64)
    ULONGLONG uptimeMillis = GetTickCount64();
    QDateTime currentTime = QDateTime::currentDateTimeUtc();
    QDateTime bootTime = currentTime.addMSecs(-static_cast<qint64>(uptimeMillis));
    return bootTime.toString(Qt::ISODate);
#else
    return QString();
#endif
}

bool shouldScan(QSqlDatabase& db) {
    if (!db.open())
        return true;

    QSqlQuery query(db);

    query.exec(R"(
        CREATE TABLE IF NOT EXISTS scan_metadata (
            id INTEGER PRIMARY KEY,
            last_scan_time TEXT,
            last_boot_time TEXT,
            scan_status TEXT
        )
    )");

    query.exec("SELECT last_boot_time, scan_status FROM scan_metadata WHERE id = 1");

    QString currentBootTime = getSystemBootTime();

    if (query.next()) {
        QString lastBootTime = query.value(0).toString();
        QString lastStatus = query.value(1).toString();

        if (currentBootTime != lastBootTime)
            return true; // Rebooted

        if (lastStatus != "complete")
            return true; // Last scan failed
    } else {
        return true; // scan needed
    }

    return false; // skip scan
}


void traverseAll() {
    using namespace TraverseInternal;

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "traverse_connection");
    db.setDatabaseName(QDir::currentPath() + "/files.db");

    if (!shouldScan(db)) {
        qDebug() << "Skipping scan...";
        return;
    }

    auto start = chrono::high_resolution_clock::now();

    unsigned int numThreads = thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 2;
    unsigned int usableThreads = (numThreads > 2) ? numThreads - 2 : numThreads;

    traverseAllDrives(usableThreads);

    if (db.open()) {
        QSqlQuery query(db);
        query.exec(R"(
            CREATE TABLE IF NOT EXISTS items (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                path TEXT NOT NULL UNIQUE,
                type TEXT NOT NULL,
                priority INTEGER DEFAULT 0
            )
        )");

        {
            lock_guard<mutex> lock(itmsMutex);
            batchInsertToDB(db, itms);
        }

        QString bootTime = getSystemBootTime();
        query.prepare(R"(
            INSERT OR REPLACE INTO scan_metadata
            (id, last_scan_time, last_boot_time, scan_status)
            VALUES (1, :scanTime, :bootTime, 'complete')
        )");
        query.bindValue(":scanTime", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        query.bindValue(":bootTime", bootTime);
        query.exec();

        db.close();
    }

    auto end = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::seconds>(end - start);
    cout << "Execution time: " << duration.count() << " seconds" << endl;
}

int getPriorityFromPath(const string& path) {
    string lowercasePath = path;
    transform(lowercasePath.begin(), lowercasePath.end(), lowercasePath.begin(), ::tolower);

    const vector<string> highPriorityKeywords = {
        "\\documents",
        "\\desktop",
        "\\downloads",
        "\\pictures",
        "\\videos",
        "\\music",
        "d:\\",
        "e:\\","f:\\","g:\\","h:\\","i:\\","j:\\","k:\\","x:\\","y:\\","z:\\"
    };

    for (const auto& keyword : highPriorityKeywords) {
        if (lowercasePath.find(keyword) != string::npos)
            return 1; // User file
    }

    return 0; // System or unknown
}


#endif // TRAVERSELIB_H
