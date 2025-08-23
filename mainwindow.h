#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#pragma once
#include <QLineEdit>
#include <QMainWindow>
#include <QListWidget>
#include <QFocusEvent>
#include <QSqlDatabase>
#include <QFutureWatcher>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QFileInfo>
#include <QIcon>
#include <QPixmap>
#include <QFileIconProvider>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    bool event(QEvent *event);
    bool eventFilter(QObject *obj, QEvent *event);
    QLineEdit *inputField;
    QListWidget *suggestionList;


private:
    Ui::MainWindow *ui;
    QSqlDatabase db;
    QLabel *statusLabel;
    QStringList lastResults;
    QFutureWatcher<QStringList> *searchWatcher;
    QTimer *debounceTimer;
    long long int time = 0;

};



class ResultItemWidget : public QWidget {
public:
    ResultItemWidget(const QString &path, QWidget *parent = nullptr) : QWidget(parent) {
        setFixedHeight(48); // Uniform height

        static QFileIconProvider provider;
        QFileInfo fileInfo(path);
        QIcon icon = provider.icon(fileInfo);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(10, 6, 10, 6);
        layout->setSpacing(10); // Exact 10px gap

        QLabel *iconLabel = new QLabel;
        QPixmap pixmap = icon.pixmap(32, 32);
        iconLabel->setPixmap(pixmap);
        iconLabel->setFixedSize(32, 32);
        iconLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        layout->addWidget(iconLabel);
        auto *textLayout = new QVBoxLayout;
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(2);

        QLabel *nameLabel = new QLabel(fileInfo.fileName());
        nameLabel->setStyleSheet("font-weight: 600; font-size: 14px;");
        QLabel *pathLabel = new QLabel(path);
        pathLabel->setStyleSheet("color: #777; font-size: 11px;");

        textLayout->addWidget(nameLabel);
        textLayout->addWidget(pathLabel);
        layout->addLayout(textLayout);
    }
};



#endif // MAINWINDOW_H
